/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime_internal.hpp"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MISTER_RUNTIME_WRAPPER_FIXTURE

struct MisterRuntime {
	MisterStatus status;
};

static MisterRuntime fixture_runtime;
static MisterPlatform fixture_platform = {
	MISTER_RUNTIME_ABI_VERSION, sizeof(MisterPlatform), 0, nullptr,
	nullptr, nullptr, nullptr, nullptr
};

static const char *fixture_mode()
{
	const char *mode = getenv("MISTER_WRAPPER_MODE");
	return mode ? mode : "success";
}

static void trace(const char *event)
{
	const char *path = getenv("MISTER_WRAPPER_TRACE");
	if (!path) return;
	FILE *file = fopen(path, "a");
	assert(file != nullptr);
	fputs(event, file);
	fclose(file);
}

const MisterPlatform *MisterRuntime_LegacyPlatform(void)
{
	return &fixture_platform;
}

extern "C" MisterRuntime *MisterRuntime_Create(const MisterPlatform *)
{
	if (strcmp(fixture_mode(), "create") == 0) return nullptr;
	memset(&fixture_runtime, 0, sizeof(fixture_runtime));
	fixture_runtime.status.abi_version = MISTER_RUNTIME_ABI_VERSION;
	fixture_runtime.status.struct_size = sizeof(MisterStatus);
	fixture_runtime.status.state = MISTER_RUNTIME_CREATED;
	return &fixture_runtime;
}

extern "C" bool MisterRuntime_Start(MisterRuntime *runtime)
{
	if (strcmp(fixture_mode(), "start") == 0) {
		runtime->status.state = MISTER_RUNTIME_FAILED;
		runtime->status.primary_error = MISTER_RUNTIME_ERROR_PLATFORM_START;
		return false;
	}
	runtime->status.state = MISTER_RUNTIME_READY;
	return true;
}

extern "C" bool MisterRuntime_Load(MisterRuntime *runtime, const MisterLaunch *launch)
{
	if (launch->core_path[0]) printf("Core path: %s\n", launch->core_path);
	if (launch->xml_path) printf("XML path: %s\n", launch->xml_path);
	if (strcmp(fixture_mode(), "fpga") == 0) {
		printf("\nGPI[31]==1. FPGA is uninitialized or incompatible core loaded.\n");
		printf("Quitting. Bye bye...\n");
		exit(0);
	}
	if (strcmp(fixture_mode(), "load") == 0) {
		runtime->status.state = MISTER_RUNTIME_FAILED;
		runtime->status.primary_error = MISTER_RUNTIME_ERROR_PLATFORM_LOAD;
		return false;
	}
	runtime->status.state = MISTER_RUNTIME_RUNNING;
	return true;
}

extern "C" void MisterRuntime_Tick(MisterRuntime *runtime)
{
	if (strcmp(fixture_mode(), "tick") == 0) {
		runtime->status.state = MISTER_RUNTIME_FAILED;
		runtime->status.primary_error = MISTER_RUNTIME_ERROR_PLATFORM_TICK;
		return;
	}
	exit(0);
}

extern "C" MisterStatus MisterRuntime_Status(const MisterRuntime *runtime)
{
	if (runtime->status.state == MISTER_RUNTIME_EXIT_REQUIRED) {
		trace("status-exit\n");
	}
	return runtime->status;
}

extern "C" void MisterRuntime_Stop(MisterRuntime *runtime)
{
	trace("stop\n");
	runtime->status.state = MISTER_RUNTIME_EXIT_REQUIRED;
}

extern "C" bool MisterRuntime_Destroy(MisterRuntime **)
{
	trace("destroy\n");
	return true;
}

#else

#include <sys/wait.h>
#include <unistd.h>

static void read_file(const char *path, char *buffer, size_t length)
{
	FILE *file = fopen(path, "r");
	if (!file) {
		buffer[0] = '\0';
		return;
	}
	size_t read_count = fread(buffer, 1, length - 1, file);
	buffer[read_count] = '\0';
	fclose(file);
}

static int run_fixture(const char *fixture, const char *mode, char *const argv[],
	char *stdout_text, size_t stdout_length, char *stderr_text, size_t stderr_length,
	char *trace_path)
{
	int stdout_pipe[2];
	int stderr_pipe[2];
	assert(pipe(stdout_pipe) == 0);
	assert(pipe(stderr_pipe) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		assert(setenv("MISTER_WRAPPER_MODE", mode, 1) == 0);
		assert(setenv("MISTER_WRAPPER_TRACE", trace_path, 1) == 0);
		assert(dup2(stdout_pipe[1], STDOUT_FILENO) >= 0);
		assert(dup2(stderr_pipe[1], STDERR_FILENO) >= 0);
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		close(stderr_pipe[0]);
		close(stderr_pipe[1]);
		execv(fixture, argv);
		_exit(127);
	}
	close(stdout_pipe[1]);
	close(stderr_pipe[1]);
	ssize_t stdout_read = read(stdout_pipe[0], stdout_text, stdout_length - 1);
	ssize_t stderr_read = read(stderr_pipe[0], stderr_text, stderr_length - 1);
	assert(stdout_read >= 0 && stderr_read >= 0);
	stdout_text[stdout_read] = '\0';
	stderr_text[stderr_read] = '\0';
	close(stdout_pipe[0]);
	close(stderr_pipe[0]);
	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	return WEXITSTATUS(status);
}

static void expect_success_args(const char *fixture, char *const argv[],
	const char *expected_stdout)
{
	char stdout_text[256];
	char stderr_text[256];
	char trace_path[] = "/tmp/mister-runtime-wrapper-trace.XXXXXX";
	int trace_fd = mkstemp(trace_path);
	assert(trace_fd >= 0);
	close(trace_fd);
	assert(run_fixture(fixture, "success", argv, stdout_text, sizeof(stdout_text),
		stderr_text, sizeof(stderr_text), trace_path) == 0);
	assert(strcmp(stdout_text, expected_stdout) == 0);
	assert(strcmp(stderr_text, "") == 0);
	char trace_text[64];
	read_file(trace_path, trace_text, sizeof(trace_text));
	assert(strcmp(trace_text, "") == 0);
	assert(unlink(trace_path) == 0);
}

static void expect_failure(const char *fixture, const char *mode,
	uint32_t primary_error, char *const argv[])
{
	char stdout_text[256];
	char stderr_text[256];
	char trace_path[] = "/tmp/mister-runtime-wrapper-trace.XXXXXX";
	int trace_fd = mkstemp(trace_path);
	assert(trace_fd >= 0);
	close(trace_fd);
	assert(run_fixture(fixture, mode, argv, stdout_text, sizeof(stdout_text),
		stderr_text, sizeof(stderr_text), trace_path) == 1);
	assert(strcmp(stdout_text, "") == 0);
	char expected[128];
	snprintf(expected, sizeof(expected),
		"MiSTer runtime failed: state=%u primary_error=%u cleanup_error=0\n",
		MISTER_RUNTIME_FAILED, primary_error);
	assert(strcmp(stderr_text, expected) == 0);
	char trace_text[64];
	read_file(trace_path, trace_text, sizeof(trace_text));
	assert(strcmp(trace_text, "stop\nstatus-exit\n") == 0);
	assert(unlink(trace_path) == 0);
}

int main(int argc, char *argv[])
{
	assert(argc == 2);
	const char *fixture = argv[1];
	char *no_path[] = { const_cast<char *>(fixture), nullptr };
	char *empty_core[] = { const_cast<char *>(fixture), const_cast<char *>(""), nullptr };
	char *empty_core_xml[] = { const_cast<char *>(fixture), const_cast<char *>(""), const_cast<char *>("core.xml"), nullptr };
	char *core_only[] = { const_cast<char *>(fixture), const_cast<char *>("core.rbf"), nullptr };
	char *core_xml[] = { const_cast<char *>(fixture), const_cast<char *>("core.rbf"), const_cast<char *>("core.xml"), nullptr };
	char *extra[] = { const_cast<char *>(fixture), const_cast<char *>("core.rbf"), const_cast<char *>("core.xml"), const_cast<char *>("ignored"), nullptr };
	expect_success_args(fixture, no_path, "");
	expect_success_args(fixture, empty_core, "Core path: \n");
	expect_success_args(fixture, empty_core_xml, "Core path: \nXML path: core.xml\n");
	expect_success_args(fixture, core_only, "Core path: core.rbf\n");
	expect_success_args(fixture, core_xml, "Core path: core.rbf\nXML path: core.xml\n");
	expect_success_args(fixture, extra, "Core path: core.rbf\nXML path: core.xml\n");
	char stdout_text[256];
	char stderr_text[256];
	{
		char trace_path[] = "/tmp/mister-runtime-wrapper-trace.XXXXXX";
		int trace_fd = mkstemp(trace_path);
		assert(trace_fd >= 0);
		close(trace_fd);
		assert(run_fixture(fixture, "fpga", empty_core_xml, stdout_text, sizeof(stdout_text),
			stderr_text, sizeof(stderr_text), trace_path) == 0);
		assert(strcmp(stdout_text,
			"Core path: \nXML path: core.xml\n\nGPI[31]==1. FPGA is uninitialized or incompatible core loaded.\n"
			"Quitting. Bye bye...\n") == 0);
		assert(strcmp(stderr_text, "") == 0);
		char trace_text[64];
		read_file(trace_path, trace_text, sizeof(trace_text));
		assert(strcmp(trace_text, "") == 0);
		assert(unlink(trace_path) == 0);
	}

	{
		char trace_path[] = "/tmp/mister-runtime-wrapper-trace.XXXXXX";
		int trace_fd = mkstemp(trace_path);
		assert(trace_fd >= 0);
		close(trace_fd);
		assert(run_fixture(fixture, "create", no_path, stdout_text, sizeof(stdout_text),
			stderr_text, sizeof(stderr_text), trace_path) == 1);
		assert(strcmp(stdout_text, "") == 0);
		assert(strcmp(stderr_text, "MiSTer runtime create failed\n") == 0);
		char trace_text[64];
		read_file(trace_path, trace_text, sizeof(trace_text));
		assert(strcmp(trace_text, "") == 0);
		assert(unlink(trace_path) == 0);
	}

	expect_failure(fixture, "start", MISTER_RUNTIME_ERROR_PLATFORM_START, no_path);
	expect_failure(fixture, "load", MISTER_RUNTIME_ERROR_PLATFORM_LOAD, no_path);
	expect_failure(fixture, "tick", MISTER_RUNTIME_ERROR_PLATFORM_TICK, no_path);
	return 0;
}

#endif
