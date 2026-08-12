/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "scheduler.h"
#include "libco.h"

#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

static int active_fiber;
static int worker_fibers[2];
static cothread_t active_return = &active_fiber;
static unsigned create_calls;
static unsigned delete_calls;
static unsigned failing_create;
static cothread_t deleted_fibers[2];

extern "C" cothread_t co_active()
{
	return active_return;
}

extern "C" cothread_t co_create(unsigned int, void (*)(void))
{
	++create_calls;
	return create_calls == failing_create ? nullptr : &worker_fibers[create_calls - 1];
}

extern "C" void co_delete(cothread_t fiber)
{
	assert(fiber != &active_fiber);
	assert(delete_calls < 2);
	deleted_fibers[delete_calls] = fiber;
	++delete_calls;
}

extern "C" void co_switch(cothread_t)
{
	assert(false);
}

int is_fpga_ready(int)
{
	assert(false);
	return 0;
}

void fpga_wait_to_reset()
{
	assert(false);
}

void user_io_poll()
{
	assert(false);
}

void frame_timer()
{
	assert(false);
}

int input_poll(int)
{
	assert(false);
	return 0;
}

void video_poll()
{
	assert(false);
}

void HandleUI()
{
	assert(false);
}

void OsdUpdate()
{
	assert(false);
}

static void verify_failed_initialization(unsigned failed_create,
	unsigned expected_deletes)
{
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		failing_create = failed_create;
		assert(!scheduler_step());
		assert(!scheduler_init());
		assert(create_calls == failed_create);
		assert(delete_calls == expected_deletes);
		assert(!scheduler_step());
		failing_create = 0;
		create_calls = 0;
		assert(scheduler_init_mode(SCHEDULER_MODE_NATIVE_HEADLESS));
		assert(scheduler_stop());
		_Exit(0);
	}

	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}

static void verify_missing_borrowed_scheduler()
{
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		active_return = nullptr;
		assert(!scheduler_init_mode(SCHEDULER_MODE_NATIVE_HEADLESS));
		assert(create_calls == 0);
		assert(delete_calls == 0);
		assert(scheduler_stop());
		active_return = &active_fiber;
		assert(scheduler_init_mode(SCHEDULER_MODE_NATIVE_HEADLESS));
		assert(scheduler_stop());
		_Exit(0);
	}

	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}

static void verify_native_lifecycle()
{
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		assert(scheduler_init_mode(SCHEDULER_MODE_NATIVE_HEADLESS));
		assert(create_calls == 1);
		assert(delete_calls == 0);
		active_return = &worker_fibers[0];
		assert(!scheduler_stop());
		assert(delete_calls == 0);
		active_return = &active_fiber;
		assert(scheduler_stop());
		assert(delete_calls == 1);
		assert(deleted_fibers[0] == &worker_fibers[0]);
		assert(scheduler_stop());
		assert(delete_calls == 1);
		assert(!scheduler_step());
		_Exit(0);
	}

	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}

static void verify_legacy_reverse_teardown()
{
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		assert(scheduler_init());
		assert(create_calls == 2);
		assert(scheduler_stop());
		assert(delete_calls == 2);
		assert(deleted_fibers[0] == &worker_fibers[1]);
		assert(deleted_fibers[1] == &worker_fibers[0]);
		_Exit(0);
	}

	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}

int main()
{
	verify_failed_initialization(1, 0);
	verify_failed_initialization(2, 1);
	verify_missing_borrowed_scheduler();
	verify_native_lifecycle();
	verify_legacy_reverse_teardown();
	return 0;
}
