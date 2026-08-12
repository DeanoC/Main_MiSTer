#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "offload.h"
#include "profiling.h"

#include <pthread.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <atomic>

static constexpr uint32_t QUEUE_SIZE = 8;
static constexpr uint64_t ADMISSION_CLOSED = UINT64_C(1) << 63;
static constexpr uint64_t ADMISSION_COUNT_MASK = (UINT64_C(1) << 16) - 1;
static constexpr uint64_t ADMISSION_GENERATION_MASK =
	~(ADMISSION_CLOSED | ADMISSION_COUNT_MASK);
static constexpr uint64_t ADMISSION_GENERATION_STEP = UINT64_C(1) << 16;

enum OffloadState
{
	OFFLOAD_IDLE,
	OFFLOAD_RUNNING,
	OFFLOAD_STOPPING
};

enum ResourceBits
{
	RESOURCE_MUTEX = 1u << 0,
	RESOURCE_COND_WORK = 1u << 1,
	RESOURCE_COND_AVAILABLE = 1u << 2,
	RESOURCE_COND_STOPPED = 1u << 3,
	RESOURCE_ATTR = 1u << 4,
	RESOURCE_THREAD = 1u << 5
};

struct Work
{
	std::function<void()> handler;
};

static pthread_t s_thread_handle;
static pthread_cond_t s_cond_work, s_cond_available, s_cond_stopped;
static pthread_mutex_t s_queue_lock;
static pthread_attr_t s_thread_attr;
static Work s_queue[QUEUE_SIZE];
static uint32_t s_queue_head, s_queue_tail;
static bool s_quit;
static bool s_worker_exited;
static std::atomic<int> s_state(OFFLOAD_IDLE);
static std::atomic<uint64_t> s_admission(ADMISSION_CLOSED);
static unsigned s_resources;

#ifdef OFFLOAD_TESTING
static unsigned s_fail_acquisition;
static unsigned s_acquisition_step;
static OffloadTestReleaseEvent s_release_events[8];
static unsigned s_release_event_count;
static unsigned s_fail_attr_destroy;
static std::atomic<bool> s_hold_worker_exit(false);
static std::atomic<bool> s_worker_exit_held(false);
static std::atomic<unsigned> s_waiting_submitters(0);
static std::atomic<bool> s_hold_submitter_before_lock(false);
static std::atomic<bool> s_submitter_before_lock_held(false);
static std::atomic<bool> s_hold_submitter_before_admission(false);
static std::atomic<bool> s_submitter_before_admission_held(false);

static void record_release(OffloadTestReleaseEvent event)
{
	if (s_release_event_count < sizeof(s_release_events) / sizeof(s_release_events[0]))
		s_release_events[s_release_event_count++] = event;
}

static bool inject_acquisition_failure()
{
	++s_acquisition_step;
	return s_fail_acquisition == s_acquisition_step;
}
#else
static bool inject_acquisition_failure()
{
	return false;
}
#endif

static int destroy_thread_attr()
{
#ifdef OFFLOAD_TESTING
	if (s_fail_attr_destroy) {
		--s_fail_attr_destroy;
		return EBUSY;
	}
#endif
	return pthread_attr_destroy(&s_thread_attr);
}

uint64_t offload_now_ms()
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return static_cast<uint64_t>(now.tv_sec) * 1000u +
		static_cast<uint64_t>(now.tv_nsec / 1000000u);
}

static void sleep_until_next_probe(uint64_t deadline_ms)
{
	const uint64_t now = offload_now_ms();
	if (now >= deadline_ms)
		return;
	uint64_t remaining = deadline_ms - now;
	if (remaining > 1)
		remaining = 1;
	struct timespec delay;
	delay.tv_sec = 0;
	delay.tv_nsec = static_cast<long>(remaining * 1000000u);
	while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static void mark_worker_exited()
{
	if (pthread_mutex_lock(&s_queue_lock) != 0)
		return;
	s_worker_exited = true;
	pthread_cond_broadcast(&s_cond_stopped);
	pthread_mutex_unlock(&s_queue_lock);
}

static void complete_worker_exit()
{
#ifdef OFFLOAD_TESTING
	s_worker_exit_held.store(true);
	while (s_hold_worker_exit.load()) {
		struct timespec delay = {0, 1000000};
		nanosleep(&delay, nullptr);
	}
	s_worker_exit_held.store(false);
#endif
	mark_worker_exited();
}

static void *worker_thread(void *)
{
	for (;;) {
		if (pthread_mutex_lock(&s_queue_lock) != 0)
			break;
		while (s_queue_head == s_queue_tail && !s_quit) {
			if (pthread_cond_wait(&s_cond_work, &s_queue_lock) != 0) {
				s_quit = true;
				break;
			}
		}
		if (s_quit && s_queue_head == s_queue_tail) {
			pthread_mutex_unlock(&s_queue_lock);
			complete_worker_exit();
			return nullptr;
		}

		Work *current_work = &s_queue[s_queue_tail % QUEUE_SIZE];
		pthread_mutex_unlock(&s_queue_lock);
		current_work->handler();
		current_work->handler = nullptr;

		if (pthread_mutex_lock(&s_queue_lock) != 0)
			break;
		++s_queue_tail;
		pthread_cond_signal(&s_cond_available);
		pthread_mutex_unlock(&s_queue_lock);
	}
	complete_worker_exit();
	return nullptr;
}

static bool destroy_initialized_resources()
{
	bool ok = true;
	if (s_resources & RESOURCE_ATTR) {
		if (destroy_thread_attr() == 0) {
			s_resources &= ~RESOURCE_ATTR;
#ifdef OFFLOAD_TESTING
			record_release(OFFLOAD_TEST_RELEASE_ATTR);
#endif
		} else ok = false;
	}
	if (s_resources & RESOURCE_COND_STOPPED) {
		if (pthread_cond_destroy(&s_cond_stopped) == 0) {
			s_resources &= ~RESOURCE_COND_STOPPED;
#ifdef OFFLOAD_TESTING
			record_release(OFFLOAD_TEST_RELEASE_COND_STOPPED);
#endif
		} else ok = false;
	}
	if (s_resources & RESOURCE_COND_AVAILABLE) {
		if (pthread_cond_destroy(&s_cond_available) == 0) {
			s_resources &= ~RESOURCE_COND_AVAILABLE;
#ifdef OFFLOAD_TESTING
			record_release(OFFLOAD_TEST_RELEASE_COND_AVAILABLE);
#endif
		} else ok = false;
	}
	if (s_resources & RESOURCE_COND_WORK) {
		if (pthread_cond_destroy(&s_cond_work) == 0) {
			s_resources &= ~RESOURCE_COND_WORK;
#ifdef OFFLOAD_TESTING
			record_release(OFFLOAD_TEST_RELEASE_COND_WORK);
#endif
		} else ok = false;
	}
	if (s_resources & RESOURCE_MUTEX) {
		if (pthread_mutex_destroy(&s_queue_lock) == 0) {
			s_resources &= ~RESOURCE_MUTEX;
#ifdef OFFLOAD_TESTING
			record_release(OFFLOAD_TEST_RELEASE_MUTEX);
#endif
		} else ok = false;
	}
	return ok;
}

static void unwind_start_failure()
{
	destroy_initialized_resources();
	s_queue_head = s_queue_tail = 0;
	s_quit = false;
	s_worker_exited = false;
	s_state.store(s_resources == 0 ? OFFLOAD_IDLE : OFFLOAD_STOPPING);
}

static OffloadStatus request_stop();

bool offload_start_native()
{
	const uint64_t closed_admission = s_admission.load();
	if (s_state.load() != OFFLOAD_IDLE || s_resources != 0 ||
		!(closed_admission & ADMISSION_CLOSED) ||
		(closed_admission & ADMISSION_COUNT_MASK) != 0)
		return false;
	const uint64_t generation = closed_admission & ADMISSION_GENERATION_MASK;
	if (generation == ADMISSION_GENERATION_MASK)
		return false;
#ifdef OFFLOAD_TESTING
	s_acquisition_step = 0;
#endif
	s_queue_head = s_queue_tail = 0;
	s_quit = false;
	s_worker_exited = false;

	if (inject_acquisition_failure() || pthread_mutex_init(&s_queue_lock, nullptr) != 0)
		return false;
	s_resources |= RESOURCE_MUTEX;
	if (inject_acquisition_failure() || pthread_cond_init(&s_cond_work, nullptr) != 0) {
		unwind_start_failure();
		return false;
	}
	s_resources |= RESOURCE_COND_WORK;
	if (inject_acquisition_failure() || pthread_cond_init(&s_cond_available, nullptr) != 0) {
		unwind_start_failure();
		return false;
	}
	s_resources |= RESOURCE_COND_AVAILABLE;
	if (inject_acquisition_failure() || pthread_cond_init(&s_cond_stopped, nullptr) != 0) {
		unwind_start_failure();
		return false;
	}
	s_resources |= RESOURCE_COND_STOPPED;
	if (inject_acquisition_failure() || pthread_attr_init(&s_thread_attr) != 0) {
		unwind_start_failure();
		return false;
	}
	s_resources |= RESOURCE_ATTR;

#if defined(__linux__)
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	if (inject_acquisition_failure() ||
		pthread_attr_setaffinity_np(&s_thread_attr, sizeof(set), &set) != 0) {
		unwind_start_failure();
		return false;
	}
#endif

	if (inject_acquisition_failure() ||
		pthread_create(&s_thread_handle, &s_thread_attr, worker_thread, nullptr) != 0) {
		unwind_start_failure();
		return false;
	}
	s_resources |= RESOURCE_THREAD;
	const int attr_result = destroy_thread_attr();
	if (attr_result == 0) {
		s_resources &= ~RESOURCE_ATTR;
#ifdef OFFLOAD_TESTING
		record_release(OFFLOAD_TEST_RELEASE_ATTR);
#endif
	}
	if (attr_result != 0) {
		s_state.store(OFFLOAD_RUNNING);
		request_stop();
		return false;
	}
	s_state.store(OFFLOAD_RUNNING);
	s_admission.store(generation + ADMISSION_GENERATION_STEP);
	return true;
}

static OffloadStatus request_stop()
{
	const int state = s_state.load();
	if (state == OFFLOAD_IDLE)
		return OFFLOAD_STATUS_OK;
	if (state != OFFLOAD_RUNNING && state != OFFLOAD_STOPPING)
		return OFFLOAD_STATUS_INVALID_STATE;
	if (state == OFFLOAD_STOPPING)
		return OFFLOAD_STATUS_OK;
	s_admission.fetch_or(ADMISSION_CLOSED);
	if (pthread_mutex_lock(&s_queue_lock) != 0)
		return OFFLOAD_STATUS_SYSTEM_ERROR;
	s_state.store(OFFLOAD_STOPPING);
	s_quit = true;
	const int work_signal_result = pthread_cond_broadcast(&s_cond_work);
	const int available_signal_result = pthread_cond_broadcast(&s_cond_available);
	const int unlock_result = pthread_mutex_unlock(&s_queue_lock);
	return work_signal_result == 0 && available_signal_result == 0 && unlock_result == 0 ?
		OFFLOAD_STATUS_OK : OFFLOAD_STATUS_SYSTEM_ERROR;
}

static bool wait_for_submitters(uint64_t absolute_deadline_ms)
{
	while ((s_admission.load() & ADMISSION_COUNT_MASK) != 0) {
		if (offload_now_ms() >= absolute_deadline_ms)
			return false;
		sleep_until_next_probe(absolute_deadline_ms);
	}
	return true;
}

static bool admit_submitter()
{
	uint64_t admission = s_admission.load();
	const uint64_t generation = admission & ADMISSION_GENERATION_MASK;
#ifdef OFFLOAD_TESTING
	s_submitter_before_admission_held.store(true);
	while (s_hold_submitter_before_admission.load()) {
		struct timespec delay = {0, 1000000};
		nanosleep(&delay, nullptr);
	}
	s_submitter_before_admission_held.store(false);
#endif
	for (;;) {
		if ((admission & ADMISSION_CLOSED) ||
			(admission & ADMISSION_GENERATION_MASK) != generation ||
			(admission & ADMISSION_COUNT_MASK) == ADMISSION_COUNT_MASK)
			return false;
		if (s_admission.compare_exchange_weak(admission, admission + 1))
			return true;
	}
}

static void release_submitter()
{
	s_admission.fetch_sub(1);
}

static OffloadStatus finish_cleanup_after_join(uint64_t absolute_deadline_ms)
{
	if (!wait_for_submitters(absolute_deadline_ms))
		return OFFLOAD_STATUS_DEADLINE;
	const bool destroyed = destroy_initialized_resources();
	if (destroyed && s_resources == 0) {
		s_queue_head = s_queue_tail = 0;
		s_quit = false;
		s_worker_exited = false;
		s_state.store(OFFLOAD_IDLE);
		return OFFLOAD_STATUS_OK;
	}
	return OFFLOAD_STATUS_SYSTEM_ERROR;
}

static OffloadStatus finish_join(uint64_t absolute_deadline_ms)
{
	s_resources &= ~RESOURCE_THREAD;
#ifdef OFFLOAD_TESTING
	record_release(OFFLOAD_TEST_RELEASE_THREAD);
#endif
	return finish_cleanup_after_join(absolute_deadline_ms);
}

OffloadStatus offload_stop_native(uint64_t absolute_deadline_ms)
{
	const OffloadStatus requested = request_stop();
	if (requested != OFFLOAD_STATUS_OK || s_state.load() == OFFLOAD_IDLE)
		return requested;
	if (!(s_resources & RESOURCE_THREAD))
		return finish_cleanup_after_join(absolute_deadline_ms);

	for (;;) {
#if defined(__linux__)
		const int join_result = pthread_tryjoin_np(s_thread_handle, nullptr);
		if (join_result == 0)
			return finish_join(absolute_deadline_ms);
		if (join_result != EBUSY)
			return OFFLOAD_STATUS_SYSTEM_ERROR;
#else
		if (pthread_mutex_lock(&s_queue_lock) != 0)
			return OFFLOAD_STATUS_SYSTEM_ERROR;
		const bool exited = s_worker_exited;
		if (pthread_mutex_unlock(&s_queue_lock) != 0)
			return OFFLOAD_STATUS_SYSTEM_ERROR;
		if (exited) {
			if (pthread_join(s_thread_handle, nullptr) != 0)
				return OFFLOAD_STATUS_SYSTEM_ERROR;
			return finish_join(absolute_deadline_ms);
		}
#endif
		if (offload_now_ms() >= absolute_deadline_ms)
			return OFFLOAD_STATUS_DEADLINE;
		sleep_until_next_probe(absolute_deadline_ms);
	}
}

bool offload_add_work_native(std::function<void()> handler)
{
	PROFILE_FUNCTION();
	if (!admit_submitter())
		return false;
#ifdef OFFLOAD_TESTING
	s_submitter_before_lock_held.store(true);
	while (s_hold_submitter_before_lock.load()) {
		struct timespec delay = {0, 1000000};
		nanosleep(&delay, nullptr);
	}
	s_submitter_before_lock_held.store(false);
#endif
	if (!handler || s_state.load() != OFFLOAD_RUNNING) {
		release_submitter();
		return false;
	}
	if (pthread_mutex_lock(&s_queue_lock) != 0) {
		release_submitter();
		return false;
	}
	while ((s_queue_head - s_queue_tail) == QUEUE_SIZE &&
		s_state.load() == OFFLOAD_RUNNING) {
#ifdef OFFLOAD_TESTING
		s_waiting_submitters.fetch_add(1);
#endif
		const int wait_result = pthread_cond_wait(&s_cond_available, &s_queue_lock);
#ifdef OFFLOAD_TESTING
		s_waiting_submitters.fetch_sub(1);
#endif
		if (wait_result != 0) {
			pthread_mutex_unlock(&s_queue_lock);
			release_submitter();
			return false;
		}
	}
	if (s_state.load() != OFFLOAD_RUNNING) {
		pthread_mutex_unlock(&s_queue_lock);
		release_submitter();
		return false;
	}
	Work *work = &s_queue[s_queue_head % QUEUE_SIZE];
	work->handler = handler;
	++s_queue_head;
	const int signal_result = pthread_cond_signal(&s_cond_work);
	const int unlock_result = pthread_mutex_unlock(&s_queue_lock);
	release_submitter();
	return signal_result == 0 && unlock_result == 0;
}

void offload_start()
{
	offload_start_native();
}

void offload_stop()
{
	if (request_stop() != OFFLOAD_STATUS_OK || s_state.load() == OFFLOAD_IDLE)
		return;
	if (!(s_resources & RESOURCE_THREAD)) {
		finish_cleanup_after_join(UINT64_MAX);
		return;
	}
	printf("Waiting for offloaded work to finish...");
	if (pthread_join(s_thread_handle, nullptr) == 0)
		finish_join(UINT64_MAX);
	printf("Done\n");
}

void offload_add_work(std::function<void()> handler)
{
	offload_add_work_native(handler);
}

#ifdef OFFLOAD_TESTING
void offload_test_fail_acquisition(unsigned step)
{
	s_fail_acquisition = step;
}

unsigned offload_test_acquisition_steps()
{
#if defined(__linux__)
	return 7;
#else
	return 6;
#endif
}

unsigned offload_test_resource_mask()
{
	return s_resources;
}

void offload_test_reset_release_events()
{
	s_release_event_count = 0;
}

unsigned offload_test_release_event_count()
{
	return s_release_event_count;
}

OffloadTestReleaseEvent offload_test_release_event(unsigned index)
{
	return index < s_release_event_count ? s_release_events[index] :
		OFFLOAD_TEST_RELEASE_MUTEX;
}

void offload_test_fail_attr_destroy(unsigned calls)
{
	s_fail_attr_destroy = calls;
}

void offload_test_hold_worker_exit(bool hold)
{
	s_hold_worker_exit.store(hold);
}

bool offload_test_worker_exit_held()
{
	return s_worker_exit_held.load();
}

unsigned offload_test_waiting_submitters()
{
	return s_waiting_submitters.load();
}

void offload_test_hold_submitter_before_lock(bool hold)
{
	s_hold_submitter_before_lock.store(hold);
}

bool offload_test_submitter_before_lock_held()
{
	return s_submitter_before_lock_held.load();
}

bool offload_test_worker_joined()
{
	return !(s_resources & RESOURCE_THREAD);
}

bool offload_test_worker_exited()
{
	if (!(s_resources & RESOURCE_MUTEX) || pthread_mutex_lock(&s_queue_lock) != 0)
		return false;
	const bool exited = s_worker_exited;
	pthread_mutex_unlock(&s_queue_lock);
	return exited;
}

void offload_test_hold_submitter_before_admission(bool hold)
{
	s_hold_submitter_before_admission.store(hold);
}

bool offload_test_submitter_before_admission_held()
{
	return s_submitter_before_admission_held.load();
}
#endif
