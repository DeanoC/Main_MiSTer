#include "scheduler.h"
#include <stdio.h>
#include "libco.h"
#include "menu.h"
#include "user_io.h"
#include "input.h"
#include "frame_timer.h"
#include "fpga_io.h"
#include "osd.h"
#include "profiling.h"
#include "video.h"

static cothread_t co_scheduler = nullptr;
static cothread_t co_poll = nullptr;
static cothread_t co_ui = nullptr;
static cothread_t co_last = nullptr;
static SchedulerMode scheduler_mode = SCHEDULER_MODE_LEGACY;
static bool scheduler_failure = false;

static void scheduler_reset_globals(void)
{
	co_scheduler = nullptr;
	co_poll = nullptr;
	co_ui = nullptr;
	co_last = nullptr;
	scheduler_mode = SCHEDULER_MODE_LEGACY;
	scheduler_failure = false;
}

static void scheduler_wait_fpga_ready(void)
{
	while (!is_fpga_ready(1))
	{
		fpga_wait_to_reset();
	}
}

static void scheduler_co_poll(void)
{
	for (;;)
	{
		if (scheduler_mode == SCHEDULER_MODE_NATIVE_HEADLESS)
		{
			if (!is_fpga_ready(1))
			{
				scheduler_failure = true;
				scheduler_yield();
				continue;
			}
		}
		else
		{
			scheduler_wait_fpga_ready();
		}

		{
			SPIKE_SCOPE("co_poll", 1000);
			user_io_poll();
			frame_timer();
			if (scheduler_mode == SCHEDULER_MODE_LEGACY)
				input_poll(0);
			video_poll();
		}

		scheduler_yield();
	}
}

static void scheduler_co_ui(void)
{
	for (;;)
	{
		{
			SPIKE_SCOPE("co_ui", 1000);
			HandleUI();
			OsdUpdate();
		}

		scheduler_yield();
	}
}

static void scheduler_schedule(void)
{
	if (scheduler_mode == SCHEDULER_MODE_NATIVE_HEADLESS)
	{
		co_last = co_poll;
		co_switch(co_poll);
		return;
	}

	if (co_last == co_poll)
	{
		co_last = co_ui;
		co_switch(co_ui);
	}
	else
	{
		co_last = co_poll;
		co_switch(co_poll);
	}
}

bool scheduler_init(void)
{
	return scheduler_init_mode(SCHEDULER_MODE_LEGACY);
}

bool scheduler_init_mode(SchedulerMode mode)
{
	const unsigned int co_stack_size = 262144 * sizeof(void*);
	if (mode != SCHEDULER_MODE_LEGACY && mode != SCHEDULER_MODE_NATIVE_HEADLESS)
		return false;
	if (co_scheduler || co_poll || co_ui)
		return false;

	co_scheduler = co_active();
	co_last = nullptr;
	scheduler_mode = mode;
	scheduler_failure = false;
	if (!co_scheduler)
	{
		scheduler_reset_globals();
		return false;
	}

	co_poll = co_create(co_stack_size, scheduler_co_poll);
	if (!co_poll)
	{
		scheduler_reset_globals();
		return false;
	}

	if (mode == SCHEDULER_MODE_NATIVE_HEADLESS)
		return true;

	co_ui = co_create(co_stack_size, scheduler_co_ui);
	if (!co_ui)
	{
		co_delete(co_poll);
		scheduler_reset_globals();
		return false;
	}

	return true;
}

bool scheduler_step(void)
{
	if (!co_scheduler || !co_poll || scheduler_failure) return false;
	if (scheduler_mode == SCHEDULER_MODE_LEGACY && !co_ui) return false;

	scheduler_schedule();
	return !scheduler_failure;
}

void scheduler_run(void)
{
	while (scheduler_step())
	{
	}
}

void scheduler_yield(void)
{
	co_switch(co_scheduler);
}

bool scheduler_stop(void)
{
	if (!co_scheduler)
	{
		scheduler_reset_globals();
		return true;
	}
	if (co_active() != co_scheduler)
		return false;
	if (co_ui)
	{
		co_delete(co_ui);
		co_ui = nullptr;
	}
	if (co_poll)
	{
		co_delete(co_poll);
		co_poll = nullptr;
	}
	scheduler_reset_globals();
	return true;
}

bool scheduler_failed(void)
{
	return scheduler_failure;
}
