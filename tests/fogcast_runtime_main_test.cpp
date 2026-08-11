/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fogcast/backend_fence.hpp"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

std::string TemporaryDirectory() {
	char pattern[] = "/tmp/fogcast-main-XXXXXX";
	char* path = mkdtemp(pattern);
	assert(path);
	assert(chmod(path, 0700) == 0);
	return path;
}

pid_t Spawn(const std::string& binary, const std::vector<std::string>& arguments,
	const char* failure = 0, const char* platform_failure = 0,
	const char* launch_marker = 0, const char* launch_release = 0,
	bool block_drain_signals_before_exec = false) {
	const pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		if (block_drain_signals_before_exec) {
			sigset_t blocked;
			assert(sigemptyset(&blocked) == 0);
			assert(sigaddset(&blocked, SIGINT) == 0);
			assert(sigaddset(&blocked, SIGTERM) == 0);
			assert(sigprocmask(SIG_BLOCK, &blocked, 0) == 0);
		}
		if (failure) setenv("FOGCAST_TEST_SIGNAL_FAIL", failure, 1);
		if (platform_failure) setenv("FOGCAST_TEST_PLATFORM", platform_failure, 1);
		if (launch_marker) setenv("FOGCAST_TEST_LAUNCH_MARKER", launch_marker, 1);
		if (launch_release) setenv("FOGCAST_TEST_LAUNCH_RELEASE", launch_release, 1);
		std::vector<char*> values;
		values.push_back(const_cast<char*>(binary.c_str()));
		for (size_t i = 0; i != arguments.size(); ++i)
			values.push_back(const_cast<char*>(arguments[i].c_str()));
		values.push_back(0);
		execv(binary.c_str(), values.data());
		_Exit(127);
	}
	return child;
}

struct CapturedProcess { pid_t pid; int output; };

CapturedProcess SpawnCaptured(const std::string& binary,
	const std::vector<std::string>& arguments) {
	int descriptors[2];
	assert(pipe(descriptors) == 0);
	const pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		close(descriptors[0]);
		assert(dup2(descriptors[1], STDOUT_FILENO) >= 0);
		assert(dup2(descriptors[1], STDERR_FILENO) >= 0);
		close(descriptors[1]);
		std::vector<char*> values;
		values.push_back(const_cast<char*>(binary.c_str()));
		for (size_t i = 0; i != arguments.size(); ++i)
			values.push_back(const_cast<char*>(arguments[i].c_str()));
		values.push_back(0);
		execv(binary.c_str(), values.data());
		_Exit(127);
	}
	close(descriptors[1]);
	CapturedProcess result = {child, descriptors[0]};
	return result;
}

int Wait(pid_t child) {
	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	return status;
}

void AssertExitedNonzero(pid_t child) {
	const int status = Wait(child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) != 0);
}

bool Exists(const std::string& path) {
	struct stat value;
	return lstat(path.c_str(), &value) == 0;
}

bool WaitForPath(const std::string& path) {
	for (unsigned int attempt = 0; attempt != 500; ++attempt) {
		if (Exists(path)) return true;
		usleep(1000);
	}
	return false;
}

int Connect(const std::string& path) {
	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	assert(path.size() < sizeof(address.sun_path));
	strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
	for (unsigned int attempt = 0; attempt != 500; ++attempt) {
		const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
		assert(descriptor >= 0);
		if (connect(descriptor, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) == 0)
			return descriptor;
		const int connect_error = errno;
		close(descriptor);
		assert(connect_error == ECONNREFUSED || connect_error == ENOENT);
		usleep(1000);
	}
	assert(false);
	return -1;
}

void WriteAll(int descriptor, const std::string& bytes) {
	size_t used = 0;
	while (used != bytes.size()) {
		const ssize_t count = write(descriptor, bytes.data() + used, bytes.size() - used);
		assert(count > 0);
		used += static_cast<size_t>(count);
	}
}

std::string Frame(const std::string& request) {
	std::string frame(4, '\0');
	frame[0] = static_cast<char>(request.size() >> 24);
	frame[1] = static_cast<char>(request.size() >> 16);
	frame[2] = static_cast<char>(request.size() >> 8);
	frame[3] = static_cast<char>(request.size());
	return frame + request;
}

std::string Exchange(int descriptor, const std::string& request) {
	WriteAll(descriptor, Frame(request));
	char prefix[4];
	size_t used = 0;
	while (used != sizeof(prefix)) {
		const ssize_t count = read(descriptor, prefix + used, sizeof(prefix) - used);
		assert(count > 0);
		used += static_cast<size_t>(count);
	}
	const uint32_t length = (static_cast<uint32_t>(static_cast<unsigned char>(prefix[0])) << 24) |
		(static_cast<uint32_t>(static_cast<unsigned char>(prefix[1])) << 16) |
		(static_cast<uint32_t>(static_cast<unsigned char>(prefix[2])) << 8) |
		static_cast<uint32_t>(static_cast<unsigned char>(prefix[3]));
	std::string response(length, '\0');
	used = 0;
	while (used != response.size()) {
		const ssize_t count = read(descriptor, &response[used], response.size() - used);
		assert(count > 0);
		used += static_cast<size_t>(count);
	}
	return response;
}

std::string Query(int descriptor, const char* operation = "hello", uint64_t request_id = 1) {
	return Exchange(descriptor, std::string("{\"protocol\":1,\"request_id\":") +
		std::to_string(request_id) + ",\"operation\":\"" + operation + "\",\"body\":{}}");
}

std::string LaunchRequest(uint64_t sequence) {
	return std::string("{\"protocol\":1,\"request_id\":10,\"operation_id\":") +
		"\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"operation\":\"launch\",\"candidate\":{" +
		"\"session\":\"55555555555555555555555555555555\",\"generation\":7}," +
		"\"precondition\":{\"sequence\":" + std::to_string(sequence) + ",\"owner\":null}," +
		"\"body\":{\"game_id\":\"sonic\",\"system\":\"megadrive\"," +
		"\"expected_core\":\"MegaDrive\",\"cache_lease_id\":\"22222222222222222222222222222222\"," +
		"\"content\":{\"sha256\":\"3333333333333333333333333333333333333333333333333333333333333333\"," +
		"\"size\":1,\"extension\":\"md\"}}}";
}

std::string StopRequest(uint64_t sequence) {
	const std::string owner = "{\"session\":\"55555555555555555555555555555555\"," +
		std::string("\"generation\":7,\"mode\":\"fpga_native\"}");
	return std::string("{\"protocol\":1,\"request_id\":12,\"operation_id\":") +
		"\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"operation\":\"stop\",\"owner\":" + owner +
		",\"precondition\":{\"sequence\":" + std::to_string(sequence) + ",\"owner\":" + owner +
		"},\"body\":{}}";
}

uint64_t SnapshotSequence(const std::string& response) {
	const std::string marker = "\"sequence\":";
	size_t position = response.find(marker);
	assert(position != std::string::npos);
	position += marker.size();
	uint64_t value = 0;
	assert(position < response.size() && response[position] >= '0' && response[position] <= '9');
	while (position < response.size() && response[position] >= '0' && response[position] <= '9')
		value = value * 10 + static_cast<uint64_t>(response[position++] - '0');
	return value;
}

void CreateRegular(const std::string& path) {
	const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
}

bool LockHeld(const std::string& path) {
	const int descriptor = open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	assert(descriptor >= 0);
	const int result = flock(descriptor, LOCK_EX | LOCK_NB);
	const int lock_error = errno;
	close(descriptor);
	return result != 0 && (lock_error == EWOULDBLOCK || lock_error == EAGAIN);
}

void RemoveRuntimeDirectory(const std::string& directory, bool fence, bool state) {
	(void)unlink((directory + "/fogcast-runtime.sock").c_str());
	(void)unlink((directory + "/fogcast-runtime.lock").c_str());
	if (state) assert(unlink((directory + "/state.json").c_str()) == 0);
	if (fence) assert(unlink((directory + "/backend-fence.json").c_str()) == 0);
	assert(rmdir(directory.c_str()) == 0);
}

void CreateFence(const std::string& directory, uint64_t epoch = 1) {
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", epoch)) == fogcast::ErrorClass::ok);
}

}  // namespace

int main(int argc, char** argv) {
	assert(argc == 3);
	const std::string fake = argv[1];
	const std::string unavailable = argv[2];

	// Exact argc and canonical positive uint63 parsing.
	AssertExitedNonzero(Spawn(fake, std::vector<std::string>()));
	const char* invalid_epochs[] = {"0", "+1", "-1", " 1", "1 ", "01",
		"9223372036854775808", "1x"};
	for (size_t i = 0; i != sizeof(invalid_epochs) / sizeof(invalid_epochs[0]); ++i)
		AssertExitedNonzero(Spawn(fake, {"/private/not-echoed", invalid_epochs[i]}));
	AssertExitedNonzero(Spawn(fake, {"/private/not-echoed", "1", "extra"}));
	CapturedProcess captured = SpawnCaptured(fake,
		{"/private/fogcast-secret-path-marker", "01"});
	AssertExitedNonzero(captured.pid);
	std::string diagnostic;
	char diagnostic_bytes[128];
	ssize_t diagnostic_count;
	while ((diagnostic_count = read(captured.output, diagnostic_bytes,
		sizeof(diagnostic_bytes))) > 0)
		diagnostic.append(diagnostic_bytes, static_cast<size_t>(diagnostic_count));
	assert(diagnostic_count == 0);
	close(captured.output);
	assert(diagnostic.find("fogcast-secret-path-marker") == std::string::npos);

	const std::string missing_parent = std::string("/tmp/fogcast-main-missing-") +
		std::to_string(static_cast<unsigned long long>(getpid()));
	assert(!Exists(missing_parent));
	AssertExitedNonzero(Spawn(fake, {missing_parent, "1"}));

	// A null target provider exits before fence/state/socket/lock mutation.
	const std::string null_directory = TemporaryDirectory();
	AssertExitedNonzero(Spawn(unavailable, {null_directory, "1"}));
	assert(!Exists(null_directory + "/fogcast-runtime.lock"));
	assert(!Exists(null_directory + "/fogcast-runtime.sock"));
	assert(!Exists(null_directory + "/state.json"));
	assert(!Exists(null_directory + "/backend-fence.json"));
	RemoveRuntimeDirectory(null_directory, false, false);
	for (const char* platform_failure : {"binding", "abi", "callback"}) {
		const std::string directory = TemporaryDirectory();
		AssertExitedNonzero(Spawn(fake, {directory, "1"}, 0, platform_failure));
		assert(!Exists(directory + "/fogcast-runtime.lock"));
		assert(!Exists(directory + "/fogcast-runtime.sock"));
		assert(!Exists(directory + "/state.json"));
		assert(!Exists(directory + "/backend-fence.json"));
		RemoveRuntimeDirectory(directory, false, false);
	}

	// Parent validation is fail closed.
	const std::string insecure = TemporaryDirectory();
	assert(chmod(insecure.c_str(), 0755) == 0);
	AssertExitedNonzero(Spawn(fake, {insecure, "1"}));
	assert(chmod(insecure.c_str(), 0700) == 0);
	RemoveRuntimeDirectory(insecure, false, false);

	// A valid binding with non-ok Initialize still exposes canonical unavailable
	// inspection, and a signal cannot falsely claim graceful drain.
	const std::string unavailable_directory = TemporaryDirectory();
	const std::string unavailable_socket = unavailable_directory + "/fogcast-runtime.sock";
	pid_t child = Spawn(fake, {unavailable_directory, "1"});
	assert(WaitForPath(unavailable_socket));
	int client = Connect(unavailable_socket);
	const std::string unavailable_hello = Query(client);
	assert(unavailable_hello.find("\"ready\":false") != std::string::npos);
	assert(Query(client, "health", 2).find("\"ready\":false") != std::string::npos);
	assert(Query(client, "status", 3).find("\"code\":\"NOT_READY\"") != std::string::npos);
	close(client);
	assert(kill(child, SIGTERM) == 0);
	usleep(30000);
	assert(kill(child, 0) == 0);
	assert(kill(child, SIGKILL) == 0);
	const int killed = Wait(child);
	assert(WIFSIGNALED(killed) && WTERMSIG(killed) == SIGKILL);

	// SIGKILL leaves no graceful claim; the next cooperating start safely
	// recovers its stale socket and then drains from initialized idle.
	CreateFence(unavailable_directory);
	child = Spawn(fake, {unavailable_directory, "1"});
	assert(WaitForPath(unavailable_socket));
	client = Connect(unavailable_socket);
	assert(Query(client).find("\"ready\":true") != std::string::npos);
	close(client);
	assert(kill(child, SIGTERM) == 0);
	int status = Wait(child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(!Exists(unavailable_socket));
	RemoveRuntimeDirectory(unavailable_directory, true, true);

	// A real process drain during an admitted ABI callback cannot cancel the
	// mutation or invent cleanup authority. The client disconnects before the
	// callback is released; explicit CAS-conditioned stop is still required.
	const std::string active_directory = TemporaryDirectory();
	CreateFence(active_directory);
	const std::string launch_marker = active_directory + "/launch-entered";
	const std::string launch_release = active_directory + "/launch-release";
	child = Spawn(fake, {active_directory, "1"}, 0, 0,
		launch_marker.c_str(), launch_release.c_str());
	const std::string active_socket = active_directory + "/fogcast-runtime.sock";
	assert(WaitForPath(active_socket));
	client = Connect(active_socket);
	WriteAll(client, Frame(LaunchRequest(1)));
	assert(WaitForPath(launch_marker));
	close(client);
	assert(kill(child, SIGTERM) == 0);
	usleep(30000);
	assert(kill(child, 0) == 0);
	assert(Exists(active_socket));
	assert(LockHeld(active_directory + "/fogcast-runtime.lock"));
	CreateRegular(launch_release);
	usleep(30000);
	client = Connect(active_socket);
	const std::string active_status = Query(client, "status", 11);
	assert(active_status.find("\"phase\":\"active\"") != std::string::npos);
	assert(active_status.find("\"mode\":\"fpga_native\"") != std::string::npos);
	assert(active_status.find("\"session\":\"55555555555555555555555555555555\"") != std::string::npos);
	const uint64_t active_sequence = SnapshotSequence(active_status);
	const std::string stop_response = Exchange(client, StopRequest(active_sequence));
	assert(stop_response.find("\"ok\":true") != std::string::npos);
	assert(stop_response.find("\"phase\":\"idle\"") != std::string::npos);
	close(client);
	status = Wait(child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(!Exists(active_socket));
	assert(unlink(launch_marker.c_str()) == 0);
	assert(unlink(launch_release.c_str()) == 0);
	RemoveRuntimeDirectory(active_directory, true, true);

	// Socket publication is observed before signaling, covering the narrow
	// startup window in which main is publishing handlers. Either signal wakes
	// blocking Poll; a near-teardown signal burst cannot hit a reused descriptor.
	for (int signal_number : {SIGINT, SIGTERM}) {
		const std::string directory = TemporaryDirectory();
		CreateFence(directory);
		child = Spawn(fake, {directory, "1"});
		assert(WaitForPath(directory + "/fogcast-runtime.sock"));
		assert(kill(child, signal_number) == 0);
		for (unsigned int repeat = 0; repeat != 100; ++repeat)
			if (kill(child, signal_number) != 0) { assert(errno == ESRCH); break; }
		status = Wait(child);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		assert(!Exists(directory + "/fogcast-runtime.sock"));
		RemoveRuntimeDirectory(directory, true, true);
	}

	// Repeated process starts exercise the real publication window with exactly
	// one SIGTERM each; no second signal may be needed to escape Poll.
	for (unsigned int iteration = 0; iteration != 50; ++iteration) {
		const std::string directory = TemporaryDirectory();
		CreateFence(directory);
		child = Spawn(fake, {directory, "1"});
		assert(WaitForPath(directory + "/fogcast-runtime.sock"));
		assert(kill(child, SIGTERM) == 0);
		status = Wait(child);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		RemoveRuntimeDirectory(directory, true, true);
	}

	// Inherited blocked drain signals must not disable the runtime's installed
	// handlers. One SIGTERM after publication must still terminate cleanly.
	const std::string inherited_mask_directory = TemporaryDirectory();
	CreateFence(inherited_mask_directory);
	child = Spawn(fake, {inherited_mask_directory, "1"}, 0, 0, 0, 0, true);
	assert(WaitForPath(inherited_mask_directory + "/fogcast-runtime.sock"));
	assert(kill(child, SIGTERM) == 0);
	bool inherited_mask_exited = false;
	for (unsigned int attempt = 0; attempt != 500; ++attempt) {
		const pid_t waited = waitpid(child, &status, WNOHANG);
		assert(waited == 0 || waited == child);
		if (waited == child) { inherited_mask_exited = true; break; }
		usleep(1000);
	}
	if (!inherited_mask_exited) {
		assert(kill(child, SIGKILL) == 0);
		(void)Wait(child);
	}
	assert(inherited_mask_exited);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(!Exists(inherited_mask_directory + "/fogcast-runtime.sock"));
	RemoveRuntimeDirectory(inherited_mask_directory, true, true);

	// The upper positive uint63 bound is accepted canonically.
	const std::string maximum_epoch_directory = TemporaryDirectory();
	CreateFence(maximum_epoch_directory, UINT64_C(9223372036854775807));
	child = Spawn(fake, {maximum_epoch_directory, "9223372036854775807"});
	assert(WaitForPath(maximum_epoch_directory + "/fogcast-runtime.sock"));
	assert(kill(child, SIGTERM) == 0);
	status = Wait(child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	RemoveRuntimeDirectory(maximum_epoch_directory, true, true);

	// Partial handler installation and running-mask failure clean the published
	// server rather than serving with incomplete signal control.
	const char* startup_failures[] = {"install_sigint", "install_sigterm", "unblock_running"};
	for (size_t i = 0; i != sizeof(startup_failures) / sizeof(startup_failures[0]); ++i) {
		const std::string directory = TemporaryDirectory();
		CreateFence(directory);
		AssertExitedNonzero(Spawn(fake, {directory, "1"}, startup_failures[i]));
		assert(!Exists(directory + "/fogcast-runtime.sock"));
		RemoveRuntimeDirectory(directory, true, true);
	}

	// If restoration of the running mask and the mandatory re-block both fail,
	// startup takes the no-close abrupt path and stale recovery remains valid.
	const std::string startup_abrupt_directory = TemporaryDirectory();
	CreateFence(startup_abrupt_directory);
	AssertExitedNonzero(Spawn(fake, {startup_abrupt_directory, "1"},
		"unblock_running,reblock_startup"));
	assert(Exists(startup_abrupt_directory + "/fogcast-runtime.sock"));
	child = Spawn(fake, {startup_abrupt_directory, "1"});
	client = Connect(startup_abrupt_directory + "/fogcast-runtime.sock");
	assert(Query(client).find("\"ready\":true") != std::string::npos);
	close(client);
	assert(kill(child, SIGTERM) == 0);
	status = Wait(child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	RemoveRuntimeDirectory(startup_abrupt_directory, true, true);

	// Initial blocking failure happens before any persistent/server mutation.
	const std::string initial_block_directory = TemporaryDirectory();
	AssertExitedNonzero(Spawn(fake, {initial_block_directory, "1"}, "block_initial"));
	assert(!Exists(initial_block_directory + "/fogcast-runtime.lock"));
	assert(!Exists(initial_block_directory + "/fogcast-runtime.sock"));
	assert(!Exists(initial_block_directory + "/state.json"));
	RemoveRuntimeDirectory(initial_block_directory, false, false);

	// Teardown re-block failure is deliberately abrupt and does not close the
	// signal-visible descriptors. The next start proves stale recovery.
	const std::string abrupt_directory = TemporaryDirectory();
	CreateFence(abrupt_directory);
	child = Spawn(fake, {abrupt_directory, "1"}, "block_teardown");
	assert(WaitForPath(abrupt_directory + "/fogcast-runtime.sock"));
	assert(kill(child, SIGTERM) == 0);
	status = Wait(child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) != 0);
	assert(Exists(abrupt_directory + "/fogcast-runtime.sock"));
	child = Spawn(fake, {abrupt_directory, "1"});
	client = Connect(abrupt_directory + "/fogcast-runtime.sock");
	assert(Query(client).find("\"ready\":true") != std::string::npos);
	close(client);
	assert(kill(child, SIGINT) == 0);
	status = Wait(child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	RemoveRuntimeDirectory(abrupt_directory, true, true);

	// A disposition-restore failure occurs only after signals are blocked and
	// the handler pointer is clear; cleanup still happens exactly once.
	for (const char* restore_failure : {"restore_sigterm", "restore_sigint"}) {
		const std::string restore_directory = TemporaryDirectory();
		CreateFence(restore_directory);
		child = Spawn(fake, {restore_directory, "1"}, restore_failure);
		assert(WaitForPath(restore_directory + "/fogcast-runtime.sock"));
		assert(kill(child, SIGTERM) == 0);
		AssertExitedNonzero(child);
		assert(!Exists(restore_directory + "/fogcast-runtime.sock"));
		RemoveRuntimeDirectory(restore_directory, true, true);
	}
	return 0;
}
