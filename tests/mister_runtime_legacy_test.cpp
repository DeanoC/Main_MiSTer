/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime_internal.hpp"

#include <assert.h>
#include <sched.h>
#include <string.h>
#include <sys/types.h>

extern "C" pid_t fork(void);
extern "C" pid_t waitpid(pid_t, int *, int);
extern "C" void _Exit(int) __attribute__((__noreturn__));

static const char *events[16];
static unsigned event_count;
static unsigned offload_stop_calls;
static bool scheduler_init_result = true;
static bool scheduler_step_result = true;

static void record(const char *event)
{
	assert(event_count < sizeof(events) / sizeof(events[0]));
	events[event_count++] = event;
}

#if !defined(__APPLE__)
int sched_setaffinity(pid_t, size_t, const cpu_set_t *)
{
	record("affinity");
	return 0;
}
#endif

void offload_start()
{
	record("offload_start");
}

void offload_stop()
{
	++offload_stop_calls;
}

int fpga_io_init()
{
	record("fpga_io_init");
	return 0;
}

int is_fpga_ready(int quick)
{
	assert(quick == 1);
	record("is_fpga_ready");
	return 1;
}

void FindStorage()
{
	record("FindStorage");
}

void user_io_init(const char *core_path, const char *xml_path)
{
	assert(strcmp(core_path, "core.rbf") == 0);
	assert(strcmp(xml_path, "core.xml") == 0);
	record("user_io_init");
}

bool scheduler_init()
{
	record("scheduler_init");
	return scheduler_init_result;
}

bool scheduler_step()
{
	record("scheduler_step");
	return scheduler_step_result;
}

const char *version = "$VER:260810";

static void expect_event(unsigned index, const char *expected)
{
	assert(index < event_count);
	assert(strcmp(events[index], expected) == 0);
}

static MisterLaunch test_launch()
{
	MisterLaunch launch = {};
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION;
	launch.struct_size = sizeof(launch);
	launch.core_path = "core.rbf";
	launch.xml_path = "core.xml";
	return launch;
}

static void expect_exit_required_failure(MisterRuntime *runtime, uint32_t error)
{
	MisterStatus status = MisterRuntime_Status(runtime);
	assert(status.state == MISTER_RUNTIME_FAILED);
	assert(status.primary_error == error);
	assert(status.cleanup_error == MISTER_RUNTIME_ERROR_NONE);
	MisterRuntime_Stop(runtime);
	status = MisterRuntime_Status(runtime);
	assert(status.state == MISTER_RUNTIME_EXIT_REQUIRED);
	assert(!MisterRuntime_Destroy(&runtime));
	assert(runtime != nullptr);
}

static void expect_exit_required_invalid_state(MisterRuntime *runtime)
{
	MisterStatus status = MisterRuntime_Status(runtime);
	assert(status.state == MISTER_RUNTIME_RUNNING);
	assert(status.last_error == MISTER_RUNTIME_ERROR_INVALID_STATE);
	assert(status.primary_error == MISTER_RUNTIME_ERROR_NONE);
	MisterRuntime_Stop(runtime);
	status = MisterRuntime_Status(runtime);
	assert(status.state == MISTER_RUNTIME_EXIT_REQUIRED);
	assert(!MisterRuntime_Destroy(&runtime));
	assert(runtime != nullptr);
}

static int run_isolated_case(const char *case_name)
{
	const MisterPlatform *platform = MisterRuntime_LegacyPlatform();
	MisterLaunch launch = test_launch();
	assert(platform != nullptr);

	if (strcmp(case_name, "second-start") == 0) {
		MisterRuntime *first = MisterRuntime_Create(platform);
		MisterRuntime *second = MisterRuntime_Create(platform);
		assert(first != nullptr && second != nullptr);
		assert(MisterRuntime_Start(first));
		assert(!MisterRuntime_Start(second));
		expect_exit_required_failure(second, MISTER_RUNTIME_ERROR_PLATFORM_START);
		return 0;
	}
	if (strcmp(case_name, "second-load") == 0) {
		MisterRuntime *runtime = MisterRuntime_Create(platform);
		assert(runtime != nullptr);
		assert(MisterRuntime_Start(runtime));
		assert(MisterRuntime_Load(runtime, &launch));
		assert(!MisterRuntime_Load(runtime, &launch));
		expect_exit_required_invalid_state(runtime);
		return 0;
	}
	if (strcmp(case_name, "scheduler-init-failure") == 0) {
		scheduler_init_result = false;
		MisterRuntime *runtime = MisterRuntime_Create(platform);
		assert(runtime != nullptr);
		assert(MisterRuntime_Start(runtime));
		assert(!MisterRuntime_Load(runtime, &launch));
		expect_exit_required_failure(runtime, MISTER_RUNTIME_ERROR_PLATFORM_LOAD);
		return 0;
	}
	if (strcmp(case_name, "scheduler-step-failure") == 0) {
		scheduler_step_result = false;
		MisterRuntime *runtime = MisterRuntime_Create(platform);
		assert(runtime != nullptr);
		assert(MisterRuntime_Start(runtime));
		assert(MisterRuntime_Load(runtime, &launch));
		MisterRuntime_Tick(runtime);
		expect_exit_required_failure(runtime, MISTER_RUNTIME_ERROR_PLATFORM_TICK);
		return 0;
	}
	assert(false);
	return 1;
}

static void run_case_in_process(const char *case_name)
{
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		_Exit(run_isolated_case(case_name));
	}
	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(status == 0);
}

int main(int argc, char *argv[])
{
	if (argc == 2) return run_isolated_case(argv[1]);
	assert(argc == 1);
	run_case_in_process("second-start");
	run_case_in_process("second-load");
	run_case_in_process("scheduler-init-failure");
	run_case_in_process("scheduler-step-failure");
	const MisterPlatform *platform = MisterRuntime_LegacyPlatform();
	assert(platform != nullptr);
	assert(platform->abi_version == MISTER_RUNTIME_ABI_VERSION);
	assert(platform->struct_size == sizeof(*platform));
	assert(platform->capability_flags == MISTER_PLATFORM_CAP_PROCESS_CONTROL_ESCAPE);
	assert(platform->context == nullptr);
	assert(platform->start(nullptr));

#if !defined(__APPLE__)
	assert(event_count == 3);
	expect_event(0, "affinity");
	expect_event(1, "offload_start");
	expect_event(2, "fpga_io_init");
#else
	assert(event_count == 2);
	expect_event(0, "offload_start");
	expect_event(1, "fpga_io_init");
#endif

	MisterLaunch launch = test_launch();
	assert(platform->load(nullptr, &launch));
	assert(event_count >= 4);
	expect_event(event_count - 4, "is_fpga_ready");
	expect_event(event_count - 3, "FindStorage");
	expect_event(event_count - 2, "user_io_init");
	expect_event(event_count - 1, "scheduler_init");
	assert(platform->tick(nullptr));
	expect_event(event_count - 1, "scheduler_step");
	assert(platform->stop(nullptr) == MISTER_PLATFORM_EXIT_REQUIRED);
	assert(offload_stop_calls == 0);
	return 0;
}
