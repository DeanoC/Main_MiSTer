// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "offload.h"

#include <assert.h>
#include <stdint.h>
#include <unistd.h>

#include <atomic>
#include <thread>

static std::atomic<bool> worker_entered(false);
static std::atomic<bool> release_worker(false);

static void wait_for_worker()
{
	for (unsigned i = 0; i < 2000 && !worker_entered.load(); ++i)
		usleep(1000);
	assert(worker_entered.load());
}

static void verify_each_acquisition_failure_unwinds()
{
	const unsigned steps = offload_test_acquisition_steps();
	assert(steps >= 6);
	for (unsigned step = 1; step <= steps; ++step) {
		offload_test_reset_release_events();
		offload_test_fail_acquisition(step);
		assert(!offload_start_native());
		assert(offload_test_resource_mask() == 0);
		const unsigned acquired_before_failure = step - 1;
		unsigned event = 0;
		if (acquired_before_failure >= 5)
			assert(offload_test_release_event(event++) == OFFLOAD_TEST_RELEASE_ATTR);
		if (acquired_before_failure >= 4)
			assert(offload_test_release_event(event++) == OFFLOAD_TEST_RELEASE_COND_STOPPED);
		if (acquired_before_failure >= 3)
			assert(offload_test_release_event(event++) == OFFLOAD_TEST_RELEASE_COND_AVAILABLE);
		if (acquired_before_failure >= 2)
			assert(offload_test_release_event(event++) == OFFLOAD_TEST_RELEASE_COND_WORK);
		if (acquired_before_failure >= 1)
			assert(offload_test_release_event(event++) == OFFLOAD_TEST_RELEASE_MUTEX);
		assert(offload_test_release_event_count() == event);
		assert(!offload_add_work_native([] {}));
		assert(offload_stop_native(offload_now_ms()) == OFFLOAD_STATUS_OK);
	}
	offload_test_fail_acquisition(0);
}

static void verify_bounded_stop_retains_referenced_resources()
{
	assert(offload_start_native());
	assert(!offload_start_native());
	assert(offload_add_work_native([] {
		worker_entered.store(true);
		while (!release_worker.load()) usleep(1000);
	}));
	wait_for_worker();

	const unsigned live_mask = offload_test_resource_mask();
	assert(live_mask != 0);
	assert(offload_stop_native(offload_now_ms()) == OFFLOAD_STATUS_DEADLINE);
	assert(offload_test_resource_mask() == live_mask);
	assert(!offload_add_work_native([] {}));
	assert(!offload_start_native());

	release_worker.store(true);
	assert(offload_stop_native(offload_now_ms() + 2000) == OFFLOAD_STATUS_OK);
	assert(offload_test_resource_mask() == 0);
	assert(offload_stop_native(offload_now_ms()) == OFFLOAD_STATUS_OK);
}

static void verify_reverse_release_and_repeated_cycle()
{
	offload_test_reset_release_events();
	assert(offload_start_native());
	assert(offload_test_release_event_count() == 1);
	assert(offload_test_release_event(0) == OFFLOAD_TEST_RELEASE_ATTR);
	offload_test_reset_release_events();
	assert(offload_stop_native(offload_now_ms() + 2000) == OFFLOAD_STATUS_OK);
	assert(offload_test_release_event_count() == 5);
	assert(offload_test_release_event(0) == OFFLOAD_TEST_RELEASE_THREAD);
	assert(offload_test_release_event(1) == OFFLOAD_TEST_RELEASE_COND_STOPPED);
	assert(offload_test_release_event(2) == OFFLOAD_TEST_RELEASE_COND_AVAILABLE);
	assert(offload_test_release_event(3) == OFFLOAD_TEST_RELEASE_COND_WORK);
	assert(offload_test_release_event(4) == OFFLOAD_TEST_RELEASE_MUTEX);
	assert(offload_test_resource_mask() == 0);

	assert(offload_start_native());
	std::atomic<unsigned> calls(0);
	assert(offload_add_work_native([&calls] { calls.fetch_add(1); }));
	assert(offload_stop_native(offload_now_ms() + 2000) == OFFLOAD_STATUS_OK);
	assert(calls.load() == 1);
	assert(offload_test_resource_mask() == 0);
}

static void verify_attr_destroy_failure_uses_bounded_stop()
{
	offload_test_reset_release_events();
	offload_test_fail_attr_destroy(1);
	offload_test_hold_worker_exit(true);
	assert(!offload_start_native());
	for (unsigned i = 0; i < 2000 && !offload_test_worker_exit_held(); ++i)
		usleep(1000);
	assert(offload_test_worker_exit_held());
	const unsigned retained = offload_test_resource_mask();
	assert(retained != 0);
	assert(!offload_add_work_native([] {}));
	assert(offload_stop_native(offload_now_ms()) == OFFLOAD_STATUS_DEADLINE);
	assert(offload_test_resource_mask() == retained);

	offload_test_hold_worker_exit(false);
	assert(offload_stop_native(offload_now_ms() + 2000) == OFFLOAD_STATUS_OK);
	assert(offload_test_release_event_count() == 6);
	assert(offload_test_release_event(0) == OFFLOAD_TEST_RELEASE_THREAD);
	assert(offload_test_release_event(1) == OFFLOAD_TEST_RELEASE_ATTR);
	assert(offload_test_release_event(2) == OFFLOAD_TEST_RELEASE_COND_STOPPED);
	assert(offload_test_release_event(3) == OFFLOAD_TEST_RELEASE_COND_AVAILABLE);
	assert(offload_test_release_event(4) == OFFLOAD_TEST_RELEASE_COND_WORK);
	assert(offload_test_release_event(5) == OFFLOAD_TEST_RELEASE_MUTEX);
	assert(offload_test_resource_mask() == 0);
}

static void verify_stop_rejects_blocked_submitter()
{
	worker_entered.store(false);
	release_worker.store(false);
	assert(offload_start_native());
	assert(offload_add_work_native([] {
		worker_entered.store(true);
		while (!release_worker.load()) usleep(1000);
	}));
	wait_for_worker();
	for (unsigned i = 1; i < 8; ++i)
		assert(offload_add_work_native([] {}));

	std::atomic<bool> submit_returned(false);
	std::atomic<bool> submit_result(true);
	std::thread submitter([&] {
		submit_result.store(offload_add_work_native([] {}));
		submit_returned.store(true);
	});
	for (unsigned i = 0; i < 2000 && offload_test_waiting_submitters() != 1; ++i)
		usleep(1000);
	assert(offload_test_waiting_submitters() == 1);
	assert(offload_stop_native(offload_now_ms()) == OFFLOAD_STATUS_DEADLINE);
	for (unsigned i = 0; i < 2000 && !submit_returned.load(); ++i)
		usleep(1000);
	assert(submit_returned.load());
	assert(!submit_result.load());
	submitter.join();

	release_worker.store(true);
	assert(offload_stop_native(offload_now_ms() + 2000) == OFFLOAD_STATUS_OK);
	assert(offload_test_resource_mask() == 0);
}

static void verify_cleanup_waits_for_prelock_submitter()
{
	offload_test_hold_submitter_before_lock(true);
	assert(offload_start_native());
	std::atomic<bool> submit_result(true);
	std::thread submitter([&] {
		submit_result.store(offload_add_work_native([] {}));
	});
	for (unsigned i = 0; i < 2000 && !offload_test_submitter_before_lock_held(); ++i)
		usleep(1000);
	assert(offload_test_submitter_before_lock_held());

	assert(offload_stop_native(offload_now_ms()) == OFFLOAD_STATUS_DEADLINE);
	for (unsigned i = 0; i < 2000 && !offload_test_worker_joined() &&
		!offload_test_worker_exited(); ++i)
		usleep(1000);
	assert(offload_test_worker_joined() || offload_test_worker_exited());
	assert(offload_stop_native(offload_now_ms() + 50) == OFFLOAD_STATUS_DEADLINE);
	assert(offload_test_worker_joined());
	assert(offload_test_resource_mask() != 0);
	offload_test_hold_submitter_before_lock(false);
	submitter.join();
	assert(!submit_result.load());
	assert(offload_stop_native(offload_now_ms() + 2000) == OFFLOAD_STATUS_OK);
	assert(offload_test_resource_mask() == 0);
}

static void verify_stale_submitter_cannot_cross_restart()
{
	assert(offload_start_native());
	offload_test_hold_submitter_before_admission(true);
	std::atomic<bool> submit_result(true);
	std::thread stale_submitter([&] {
		submit_result.store(offload_add_work_native([] {}));
	});
	for (unsigned i = 0; i < 2000 &&
		!offload_test_submitter_before_admission_held(); ++i)
		usleep(1000);
	assert(offload_test_submitter_before_admission_held());

	assert(offload_stop_native(offload_now_ms() + 2000) == OFFLOAD_STATUS_OK);
	assert(offload_start_native());
	offload_test_hold_submitter_before_admission(false);
	stale_submitter.join();
	assert(!submit_result.load());
	assert(offload_stop_native(offload_now_ms() + 2000) == OFFLOAD_STATUS_OK);
}

int main()
{
	verify_each_acquisition_failure_unwinds();
	verify_bounded_stop_retains_referenced_resources();
	verify_reverse_release_and_repeated_cycle();
	verify_attr_destroy_failure_uses_bounded_stop();
	verify_stop_rejects_blocked_submitter();
	verify_cleanup_waits_for_prelock_submitter();
	verify_stale_submitter_cannot_cross_restart();
	return 0;
}
