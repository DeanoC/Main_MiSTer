/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "scheduler.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *events[8];
static unsigned event_count;
static bool fpga_ready = true;

static void record(const char *event)
{
	assert(event_count < sizeof(events) / sizeof(events[0]));
	events[event_count++] = event;
}

int is_fpga_ready(int quick)
{
	assert(quick == 1);
	record("is_fpga_ready");
	return fpga_ready ? 1 : 0;
}

void fpga_wait_to_reset()
{
	assert(false);
}

void user_io_poll()
{
	record("user_io_poll");
}

void frame_timer()
{
	record("frame_timer");
}

int input_poll(int getchar)
{
	assert(getchar == 0);
	record("input_poll(0)");
	return 0;
}

void video_poll()
{
	record("video_poll");
}

void HandleUI()
{
	record("HandleUI");
}

void OsdUpdate()
{
	record("OsdUpdate");
}

static void expect_event(unsigned index, const char *event)
{
	assert(index < event_count);
	assert(strcmp(events[index], event) == 0);
}

static void verify_legacy_scheduler()
{
	assert(scheduler_init());
	assert(scheduler_step());
	assert(event_count == 5);
	expect_event(0, "is_fpga_ready");
	expect_event(1, "user_io_poll");
	expect_event(2, "frame_timer");
	expect_event(3, "input_poll(0)");
	expect_event(4, "video_poll");

	assert(scheduler_step());
	assert(event_count == 7);
	expect_event(5, "HandleUI");
	expect_event(6, "OsdUpdate");
#if defined(__linux__)
	assert(scheduler_stop());
#endif
}

static void verify_native_scheduler()
{
	assert(scheduler_init_mode(SCHEDULER_MODE_NATIVE_HEADLESS));
	assert(scheduler_step());
	assert(event_count == 4);
	expect_event(0, "is_fpga_ready");
	expect_event(1, "user_io_poll");
	expect_event(2, "frame_timer");
	expect_event(3, "video_poll");

	event_count = 0;
	assert(scheduler_step());
	assert(event_count == 4);
	assert(!scheduler_failed());

	fpga_ready = false;
	event_count = 0;
	assert(!scheduler_step());
	assert(scheduler_failed());
	assert(event_count == 1);
	expect_event(0, "is_fpga_ready");
#if defined(__linux__)
	assert(scheduler_stop());
#endif
}

static void run_in_child(void (*test)())
{
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		test();
		_Exit(0);
	}
	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}

int main()
{
	run_in_child(verify_legacy_scheduler);
	run_in_child(verify_native_scheduler);
	return 0;
}
