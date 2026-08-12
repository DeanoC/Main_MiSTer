#ifndef OFFLOAD_H
#define OFFLOAD_H

#include <stddef.h>
#include <stdint.h>
#include <functional>

void offload_start();
void offload_stop();

void offload_add_work(std::function<void()> work);

enum OffloadStatus
{
	OFFLOAD_STATUS_OK,
	OFFLOAD_STATUS_INVALID_STATE,
	OFFLOAD_STATUS_DEADLINE,
	OFFLOAD_STATUS_SYSTEM_ERROR
};

bool offload_start_native();
OffloadStatus offload_stop_native(uint64_t absolute_deadline_ms);
bool offload_add_work_native(std::function<void()> work);
uint64_t offload_now_ms();

#ifdef OFFLOAD_TESTING
enum OffloadTestReleaseEvent
{
	OFFLOAD_TEST_RELEASE_ATTR,
	OFFLOAD_TEST_RELEASE_THREAD,
	OFFLOAD_TEST_RELEASE_COND_STOPPED,
	OFFLOAD_TEST_RELEASE_COND_AVAILABLE,
	OFFLOAD_TEST_RELEASE_COND_WORK,
	OFFLOAD_TEST_RELEASE_MUTEX
};

void offload_test_fail_acquisition(unsigned step);
unsigned offload_test_acquisition_steps();
unsigned offload_test_resource_mask();
void offload_test_reset_release_events();
unsigned offload_test_release_event_count();
OffloadTestReleaseEvent offload_test_release_event(unsigned index);
void offload_test_fail_attr_destroy(unsigned calls);
void offload_test_hold_worker_exit(bool hold);
bool offload_test_worker_exit_held();
unsigned offload_test_waiting_submitters();
void offload_test_hold_submitter_before_lock(bool hold);
bool offload_test_submitter_before_lock_held();
bool offload_test_worker_joined();
bool offload_test_worker_exited();
void offload_test_hold_submitter_before_admission(bool hold);
bool offload_test_submitter_before_admission_held();
#endif

#endif
