/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "scheduler.h"

#include <assert.h>
#include <string.h>

static const char *events[8];
static unsigned event_count;

static void record(const char *event)
{
	assert(event_count < sizeof(events) / sizeof(events[0]));
	events[event_count++] = event;
}

int is_fpga_ready(int quick)
{
	assert(quick == 1);
	record("is_fpga_ready");
	return 1;
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

int main()
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
	return 0;
}
