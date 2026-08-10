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
static unsigned create_calls;
static unsigned delete_calls;
static unsigned failing_create;

extern "C" cothread_t co_active()
{
	return &active_fiber;
}

extern "C" cothread_t co_create(unsigned int, void (*)(void))
{
	++create_calls;
	return create_calls == failing_create ? nullptr : &worker_fibers[create_calls - 1];
}

extern "C" void co_delete(cothread_t fiber)
{
	assert(fiber != &active_fiber);
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
	return 0;
}
