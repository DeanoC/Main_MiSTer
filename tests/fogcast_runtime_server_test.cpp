/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fogcast/runtime_server.hpp"
#include "fogcast/runtime_coordinator.hpp"
#include "fogcast/backend_fence.hpp"

#include <assert.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <thread>

namespace {

struct SameUserCredentials : public fogcast::ServerCredentials {
	bool PeerUid(int, uint32_t* uid) override { *uid = static_cast<uint32_t>(geteuid()); return true; }
};

struct OtherUserCredentials : public fogcast::ServerCredentials {
	bool PeerUid(int, uint32_t* uid) override { *uid = static_cast<uint32_t>(geteuid()) + 1u; return true; }
};

struct FakeClock : public fogcast::ServerClock {
	uint64_t now;
	FakeClock() : now(0) {}
	uint64_t NowMs() override { return now; }
};

struct FailFirstPostBindStat : public fogcast::ServerTestHooks {
	bool fail_first;
	bool fail_validation;
	bool interrupt_poll;
	unsigned int interrupt_read_count;
	unsigned int interrupt_send_count;
	unsigned int interrupt_read_calls;
	unsigned int interrupt_send_calls;
	FakeClock* interrupt_clock;
	uint64_t interrupt_advance_ms;
	bool stall_send;
	uint32_t send_budget;
	bool request_drain_after_entry;
	FailFirstPostBindStat() : fail_first(true), fail_validation(false), interrupt_poll(false),
		interrupt_read_count(0), interrupt_send_count(0), interrupt_read_calls(0), interrupt_send_calls(0),
		interrupt_clock(0), interrupt_advance_ms(0), stall_send(false), send_budget(0),
		request_drain_after_entry(false) {}
	bool FailFirstPostBindEntryStat() override {
		if (!fail_first) return false;
		fail_first = false;
		return true;
	}
	bool FailPostBindValidation() override {
		if (!fail_validation) return false;
		fail_validation = false;
		return true;
	}
	bool InterruptPoll() override {
		if (!interrupt_poll) return false;
		interrupt_poll = false;
		return true;
	}
	bool InterruptRead() override {
		if (interrupt_read_count == 0) return false;
		--interrupt_read_count; ++interrupt_read_calls;
		if (interrupt_clock) interrupt_clock->now += interrupt_advance_ms;
		return true;
	}
	bool InterruptSend() override {
		if (interrupt_send_count == 0) return false;
		--interrupt_send_count; ++interrupt_send_calls;
		if (interrupt_clock) interrupt_clock->now += interrupt_advance_ms;
		return true;
	}
	bool StallSend() override { return stall_send && send_budget == 0; }
	uint32_t SendAllowance(uint32_t requested) override {
		if (!stall_send) return requested;
		const uint32_t allowed = requested < send_budget ? requested : send_budget;
		send_budget -= allowed;
		return allowed;
	}
	bool RequestDrainAfterEntryLatch() override {
		if (!request_drain_after_entry) return false;
		request_drain_after_entry = false;
		return true;
	}
};

struct NeutralPlatform : public fogcast::LifecyclePlatform {
	fogcast::LifecycleResult ProveNeutral(uint32_t, uint32_t) override { return fogcast::LifecycleResult::ok; }
	fogcast::LifecycleResult GrantTransfer(const fogcast::LifecycleOwner&) override { return fogcast::LifecycleResult::ok; }
	fogcast::LifecycleResult Create(const fogcast::LifecycleOwner&) override { return fogcast::LifecycleResult::ok; }
	fogcast::LifecycleResult Start(uint32_t) override { return fogcast::LifecycleResult::ok; }
	fogcast::LifecycleResult Load(const fogcast::LaunchMetadata&, uint32_t) override { return fogcast::LifecycleResult::ok; }
	fogcast::LifecycleResult Observe(const std::string&, uint32_t) override { return fogcast::LifecycleResult::ok; }
	fogcast::LifecycleResult Stop(uint32_t) override { return fogcast::LifecycleResult::ok; }
	fogcast::LifecycleResult ObserveNeutral(uint32_t, uint32_t) override { return fogcast::LifecycleResult::ok; }
	fogcast::LifecycleResult Destroy() override { return fogcast::LifecycleResult::ok; }
	fogcast::LifecycleResult RecoverStateless(uint32_t, uint32_t) override { return fogcast::LifecycleResult::ok; }
	fogcast::LiveHandleState live_handle_state() const override { return fogcast::LiveHandleState::none; }
	bool MainAbsent() override { return true; }
};

// This is a small real-coordinator fixture rather than a mocked response
// source.  It tracks the owner needed for the coordinator's live cleanup
// path, and can hold Stop so a drain request races an admitted recovery.
struct DrainPlatform : public NeutralPlatform {
	fogcast::LifecycleOwner live_owner;
	bool live;
	bool fail_stop;
	std::atomic<bool> block_stop;
	std::atomic<bool> stop_entered;
	std::atomic<bool> release_stop;
	DrainPlatform() : live(false), fail_stop(false), block_stop(false), stop_entered(false), release_stop(false) {}
	fogcast::LifecycleResult Create(const fogcast::LifecycleOwner& owner) override {
		live_owner = owner; live = true; return fogcast::LifecycleResult::ok;
	}
	fogcast::LifecycleResult Stop(uint32_t) override {
		stop_entered.store(true);
		while (block_stop.load() && !release_stop.load()) std::this_thread::yield();
		return fail_stop ? fogcast::LifecycleResult::cleanup_incomplete :
			fogcast::LifecycleResult(MISTER_RESULT_OK, MISTER_RESULT_OK, MISTER_RESULT_OK, MISTER_STATE_STOPPED);
	}
	fogcast::LifecycleResult ObserveNeutral(uint32_t mask, uint32_t) override {
		fogcast::LifecycleResult value(MISTER_RESULT_OK, MISTER_RESULT_OK, MISTER_RESULT_OK, MISTER_STATE_STOPPED);
		value.neutral_mask = mask;
		value.observed_mask = 0;
		return value;
	}
	fogcast::LifecycleResult Destroy() override { live = false; live_owner = fogcast::LifecycleOwner(); return fogcast::LifecycleResult::ok; }
	fogcast::LiveHandleState live_handle_state() const override {
		return live ? fogcast::LiveHandleState(fogcast::LiveHandleState::live, live_owner) : fogcast::LiveHandleState::none;
	}
};

struct BlockingPlatform : public DrainPlatform {
	std::atomic<bool> entered;
	std::atomic<bool> release;
	std::atomic<unsigned int> starts;
	BlockingPlatform() : entered(false), release(false), starts(0) {}
	fogcast::LifecycleResult Start(uint32_t) override {
		++starts;
		entered.store(true);
		while (!release.load()) std::this_thread::yield();
		return fogcast::LifecycleResult::ok;
	}
};

std::string TemporaryDirectory() {
	char path[] = "/tmp/fogcast-runtime-server.XXXXXX";
	char* value = mkdtemp(path);
	assert(value != 0);
	assert(chmod(value, 0700) == 0);
	return value;
}

void RemoveDirectory(const std::string& path) {
	assert(unlink((path + "/fogcast-runtime.lock").c_str()) == 0);
	assert(rmdir(path.c_str()) == 0);
}

void WriteAll(int fd, const char* bytes, size_t length) {
	while (length != 0) {
		const ssize_t written = send(fd, bytes, length, MSG_NOSIGNAL);
		assert(written > 0);
		bytes += written;
		length -= static_cast<size_t>(written);
	}
}

std::string Frame(const std::string& payload) {
	assert(payload.size() <= 65536);
	std::string frame(4, '\0');
	frame[0] = static_cast<char>(payload.size() >> 24);
	frame[1] = static_cast<char>(payload.size() >> 16);
	frame[2] = static_cast<char>(payload.size() >> 8);
	frame[3] = static_cast<char>(payload.size());
	return frame + payload;
}

void Pump(fogcast::RuntimeServer* server, unsigned int attempts = 40) {
	for (unsigned int attempt = 0; attempt != attempts; ++attempt)
		assert(server->Poll(1) == fogcast::ServerResult::ok);
}

void PumpUntilReadable(fogcast::RuntimeServer* server, int client) {
	for (unsigned int attempt = 0; attempt != 1000; ++attempt) {
		assert(server->Poll(1) == fogcast::ServerResult::ok);
		struct pollfd readable;
		readable.fd = client; readable.events = POLLIN; readable.revents = 0;
		assert(poll(&readable, 1, 0) >= 0);
		if ((readable.revents & POLLIN) != 0) return;
	}
	assert(false);
}

void PumpUntilClosed(fogcast::RuntimeServer* server) {
	bool drained = false;
	for (unsigned int attempt = 0; attempt != 1000 && !drained; ++attempt) {
		const fogcast::ServerResult result = server->Poll(1);
		assert(result == fogcast::ServerResult::ok || result == fogcast::ServerResult::drained);
		drained = result == fogcast::ServerResult::drained;
	}
	assert(drained);
	assert(server->running());
	server->Close();
	assert(!server->running());
}

std::string LaunchRequest(uint64_t sequence, uint64_t request_id, const char* operation_id,
	const char* session, uint64_t generation) {
	return std::string("{\"protocol\":1,\"request_id\":") + std::to_string(request_id) +
		",\"operation_id\":\"" + operation_id + "\",\"operation\":\"launch\",\"candidate\":{\"session\":\"" + session +
		"\",\"generation\":" + std::to_string(generation) + "},\"precondition\":{\"sequence\":" +
		std::to_string(sequence) + ",\"owner\":null},\"body\":{\"game_id\":\"sonic\",\"system\":\"megadrive\",\"expected_core\":\"MegaDrive\",\"cache_lease_id\":\"22222222222222222222222222222222\",\"content\":{\"sha256\":\"3333333333333333333333333333333333333333333333333333333333333333\",\"size\":1,\"extension\":\"md\"}}}";
}

std::string OwnerRequest(uint64_t sequence, uint64_t request_id, const char* operation_id,
	const char* operation, const char* session, uint64_t generation) {
	const std::string owner = std::string("{\"session\":\"") + session + "\",\"generation\":" +
		std::to_string(generation) + ",\"mode\":\"fpga_native\"}";
	const std::string body = std::string(operation) == "stop" ? "{}" : "{\"reason\":\"ambiguous\"}";
	return std::string("{\"protocol\":1,\"request_id\":") + std::to_string(request_id) +
		",\"operation_id\":\"" + operation_id + "\",\"operation\":\"" + operation + "\",\"owner\":" + owner +
		",\"precondition\":{\"sequence\":" + std::to_string(sequence) + ",\"owner\":" + owner + "},\"body\":" + body + "}";
}

std::string ReadFrame(int fd) {
	unsigned char prefix[4];
	assert(read(fd, prefix, sizeof(prefix)) == static_cast<ssize_t>(sizeof(prefix)));
	const uint32_t length = (static_cast<uint32_t>(prefix[0]) << 24) |
		(static_cast<uint32_t>(prefix[1]) << 16) | (static_cast<uint32_t>(prefix[2]) << 8) | prefix[3];
	std::string result(length, '\0');
	size_t offset = 0;
	while (offset != result.size()) {
		const ssize_t count = read(fd, &result[offset], result.size() - offset);
		assert(count > 0);
		offset += static_cast<size_t>(count);
	}
	std::string canonical;
	std::string digest;
	assert(fogcast::ParseWire(result, &canonical, &digest) == fogcast::ErrorClass::ok);
	assert(canonical == result);
	return result;
}

void ExpectClosed(int fd) {
	char byte = 0;
	const ssize_t result = read(fd, &byte, 1);
	assert(result == 0 || (result < 0 && errno == ECONNRESET));
}

uint64_t MonotonicMs() {
	struct timespec value;
	assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
	return static_cast<uint64_t>(value.tv_sec) * 1000u +
		static_cast<uint64_t>(value.tv_nsec / 1000000u);
}

volatile sig_atomic_t g_sigpipe_count = 0;
void CountSigpipe(int) { ++g_sigpipe_count; }

}  // namespace

int main() {
	// Break caught: accepting a zero-byte IPC frame would bypass the fixed
	// framing contract and could reach coordinator dispatch.
	assert(fogcast::RuntimeServer::ValidFrameLength(0) == false);
	assert(fogcast::RuntimeServer::ValidFrameLength(1) == true);
	assert(fogcast::RuntimeServer::ValidFrameLength(65536) == true);
	assert(fogcast::RuntimeServer::ValidFrameLength(65537) == false);

	// A drain arriving after Poll's entry latch must not lose its only wake
	// edge and then wait for another external event.
	const std::string latch_directory = TemporaryDirectory();
	fogcast::BackendFence latch_fence(latch_directory);
	assert(latch_fence.Commit(fogcast::BackendFence::Record(1, "native", 1)) == fogcast::ErrorClass::ok);
	NeutralPlatform latch_platform;
	fogcast::Coordinator latch_coordinator(latch_directory, &latch_fence, &latch_platform);
	assert(latch_coordinator.Initialize(1) == fogcast::ErrorClass::ok);
	fogcast::RuntimeServerConfig latch_config;
	latch_config.parent_directory = latch_directory;
	latch_config.service_uid = static_cast<uint32_t>(geteuid());
	FailFirstPostBindStat latch_hook;
	latch_hook.fail_first = false;
	latch_hook.request_drain_after_entry = true;
	fogcast::RuntimeServer latch_server(latch_config, &latch_coordinator, 0, 0, &latch_hook);
	assert(latch_server.Start() == fogcast::ServerResult::ok);
	assert(latch_server.Poll(100) == fogcast::ServerResult::ok);
	assert(latch_server.Poll(100) == fogcast::ServerResult::drained);
	latch_server.Close();
	assert(unlink((latch_directory + "/fogcast-runtime.lock").c_str()) == 0);
	assert(unlink((latch_directory + "/state.json").c_str()) == 0);
	assert(unlink((latch_directory + "/backend-fence.json").c_str()) == 0);
	assert(rmdir(latch_directory.c_str()) == 0);

	// Break caught: accepting a parent, singleton lock, or socket alias leaves
	// later pathname operations vulnerable to replacement before admission.
	const std::string unsafe_parent = TemporaryDirectory();
	assert(chmod(unsafe_parent.c_str(), 0755) == 0);
	fogcast::RuntimeServerConfig unsafe_parent_config;
	unsafe_parent_config.parent_directory = unsafe_parent;
	unsafe_parent_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator unsafe_parent_coordinator;
	fogcast::RuntimeServer unsafe_parent_server(unsafe_parent_config, &unsafe_parent_coordinator);
	assert(unsafe_parent_server.Start() == fogcast::ServerResult::unsafe_parent);
	assert(rmdir(unsafe_parent.c_str()) == 0);

	const std::string unsafe_lock_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig unsafe_lock_config;
	unsafe_lock_config.parent_directory = unsafe_lock_directory;
	unsafe_lock_config.service_uid = static_cast<uint32_t>(geteuid());
	assert(symlink("/dev/null", (unsafe_lock_directory + "/" + unsafe_lock_config.lock_name).c_str()) == 0);
	fogcast::Coordinator unsafe_lock_coordinator;
	fogcast::RuntimeServer unsafe_lock_server(unsafe_lock_config, &unsafe_lock_coordinator);
	assert(unsafe_lock_server.Start() == fogcast::ServerResult::unsafe_lock);
	assert(unlink((unsafe_lock_directory + "/" + unsafe_lock_config.lock_name).c_str()) == 0);
	assert(rmdir(unsafe_lock_directory.c_str()) == 0);

	const std::string unsafe_socket_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig unsafe_socket_config;
	unsafe_socket_config.parent_directory = unsafe_socket_directory;
	unsafe_socket_config.service_uid = static_cast<uint32_t>(geteuid());
	assert(symlink("/dev/null", (unsafe_socket_directory + "/" + unsafe_socket_config.socket_name).c_str()) == 0);
	fogcast::Coordinator unsafe_socket_coordinator;
	fogcast::RuntimeServer unsafe_socket_server(unsafe_socket_config, &unsafe_socket_coordinator);
	assert(unsafe_socket_server.Start() == fogcast::ServerResult::unsafe_socket);
	assert(unlink((unsafe_socket_directory + "/" + unsafe_socket_config.socket_name).c_str()) == 0);
	assert(unlink((unsafe_socket_directory + "/" + unsafe_socket_config.lock_name).c_str()) == 0);
	assert(rmdir(unsafe_socket_directory.c_str()) == 0);

	// Break caught: an exact dead socket that returns ECONNREFUSED is stale
	// server residue, not a live listener; startup must revalidate then remove
	// only that entry before binding the replacement listener.
	const std::string stale_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig stale_config;
	stale_config.parent_directory = stale_directory;
	stale_config.service_uid = static_cast<uint32_t>(geteuid());
	const int stale_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(stale_fd >= 0);
	struct sockaddr_un stale_address;
	memset(&stale_address, 0, sizeof(stale_address));
	stale_address.sun_family = AF_UNIX;
	const std::string stale_path = stale_directory + "/" + stale_config.socket_name;
	strncpy(stale_address.sun_path, stale_path.c_str(), sizeof(stale_address.sun_path) - 1);
	const mode_t stale_umask = umask(0177);
	assert(bind(stale_fd, reinterpret_cast<const struct sockaddr*>(&stale_address), sizeof(stale_address)) == 0);
	assert(umask(stale_umask) == 0177);
	close(stale_fd);
	fogcast::Coordinator stale_coordinator;
	fogcast::RuntimeServer stale_server(stale_config, &stale_coordinator);
	assert(stale_server.Start() == fogcast::ServerResult::ok);
	assert(stale_server.running());
	stale_server.Close();
	RemoveDirectory(stale_directory);

	// Break caught: a live listener with no valid singleton lock is a
	// disagreement, never stale residue to unlink during startup.
	const std::string live_listener_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig live_listener_config;
	live_listener_config.parent_directory = live_listener_directory;
	live_listener_config.service_uid = static_cast<uint32_t>(geteuid());
	const int live_listener_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(live_listener_fd >= 0);
	struct sockaddr_un live_listener_address;
	memset(&live_listener_address, 0, sizeof(live_listener_address));
	live_listener_address.sun_family = AF_UNIX;
	const std::string live_listener_path = live_listener_directory + "/" + live_listener_config.socket_name;
	strncpy(live_listener_address.sun_path, live_listener_path.c_str(), sizeof(live_listener_address.sun_path) - 1);
	const mode_t live_listener_umask = umask(0177);
	assert(bind(live_listener_fd, reinterpret_cast<const struct sockaddr*>(&live_listener_address), sizeof(live_listener_address)) == 0);
	assert(umask(live_listener_umask) == 0177);
	assert(listen(live_listener_fd, 1) == 0);
	fogcast::Coordinator live_listener_coordinator;
	fogcast::RuntimeServer live_listener_server(live_listener_config, &live_listener_coordinator);
	assert(live_listener_server.Start() == fogcast::ServerResult::unsafe_socket);
	struct stat live_listener_stat;
	assert(lstat(live_listener_path.c_str(), &live_listener_stat) == 0 && S_ISSOCK(live_listener_stat.st_mode));
	close(live_listener_fd);
	assert(unlink(live_listener_path.c_str()) == 0);
	RemoveDirectory(live_listener_directory);

	// Break caught: aliases and embedded NULs can cause an apparently validated
	// name to identify a different filesystem entry when passed to POSIX APIs.
	const char* unsafe_names[] = { "", ".", "..", "socket/name", "socket\0alias" };
	for (size_t name = 0; name != sizeof(unsafe_names) / sizeof(unsafe_names[0]); ++name) {
		const std::string invalid_name_directory = TemporaryDirectory();
		fogcast::RuntimeServerConfig invalid_name_config;
		invalid_name_config.parent_directory = invalid_name_directory;
		invalid_name_config.socket_name.assign(unsafe_names[name], name == 4 ? 12 : strlen(unsafe_names[name]));
		invalid_name_config.service_uid = static_cast<uint32_t>(geteuid());
		fogcast::Coordinator invalid_name_coordinator;
		fogcast::RuntimeServer invalid_name_server(invalid_name_config, &invalid_name_coordinator);
		assert(invalid_name_server.Start() == fogcast::ServerResult::invalid_argument);
		assert(rmdir(invalid_name_directory.c_str()) == 0);
	}

	const std::string invalid_lock_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig invalid_lock_config;
	invalid_lock_config.parent_directory = invalid_lock_directory;
	invalid_lock_config.lock_name.assign("lock\0alias", 10);
	invalid_lock_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator invalid_lock_coordinator;
	fogcast::RuntimeServer invalid_lock_server(invalid_lock_config, &invalid_lock_coordinator);
	assert(invalid_lock_server.Start() == fogcast::ServerResult::invalid_argument);
	assert(rmdir(invalid_lock_directory.c_str()) == 0);

	const std::string invalid_parent_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig invalid_parent_config;
	invalid_parent_config.parent_directory = invalid_parent_directory;
	invalid_parent_config.parent_directory.append("\0alias", 6);
	invalid_parent_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator invalid_parent_coordinator;
	fogcast::RuntimeServer invalid_parent_server(invalid_parent_config, &invalid_parent_coordinator);
	assert(invalid_parent_server.Start() == fogcast::ServerResult::invalid_argument);
	assert(rmdir(invalid_parent_directory.c_str()) == 0);

	// Break caught: a pathname created by bind(2) must remain cleanup-owned
	// through every later post-bind validation, otherwise a failed start leaks
	// a rendezvous endpoint indefinitely.
	const std::string post_bind_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig post_bind_config;
	post_bind_config.parent_directory = post_bind_directory;
	post_bind_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator post_bind_coordinator;
	FailFirstPostBindStat post_bind_fault;
	fogcast::RuntimeServer post_bind_server(post_bind_config, &post_bind_coordinator,
		0, 0, &post_bind_fault);
	assert(post_bind_server.Start() == fogcast::ServerResult::bind_failed);
	struct stat post_bind_stat;
	assert(lstat((post_bind_directory + "/" + post_bind_config.socket_name).c_str(), &post_bind_stat) != 0);
	RemoveDirectory(post_bind_directory);

	// Break caught: Close must not unlink a same-UID socket that replaced the
	// recorded entry after startup.  It may detect the disagreement, but cannot
	// atomically exclude an untrusted same-UID path mutator.
	const std::string replaced_socket_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig replaced_socket_config;
	replaced_socket_config.parent_directory = replaced_socket_directory;
	replaced_socket_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator replaced_socket_coordinator;
	fogcast::RuntimeServer replaced_socket_server(replaced_socket_config, &replaced_socket_coordinator);
	assert(replaced_socket_server.Start() == fogcast::ServerResult::ok);
	const std::string replacement_path = replaced_socket_directory + "/" + replaced_socket_config.socket_name;
	assert(unlink(replacement_path.c_str()) == 0);
	const int replacement_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(replacement_fd >= 0);
	struct sockaddr_un replacement_address;
	memset(&replacement_address, 0, sizeof(replacement_address));
	replacement_address.sun_family = AF_UNIX;
	strncpy(replacement_address.sun_path, replacement_path.c_str(), sizeof(replacement_address.sun_path) - 1);
	const mode_t replacement_umask = umask(0177);
	assert(bind(replacement_fd, reinterpret_cast<const struct sockaddr*>(&replacement_address), sizeof(replacement_address)) == 0);
	assert(umask(replacement_umask) == 0177);
	assert(replaced_socket_server.Poll(0) == fogcast::ServerResult::unsafe_socket);
	assert(replaced_socket_server.running());
	replaced_socket_server.Close();
	assert(lstat(replacement_path.c_str(), &post_bind_stat) == 0);
	assert(S_ISSOCK(post_bind_stat.st_mode));
	assert((post_bind_stat.st_mode & 0777) == 0600);
	close(replacement_fd);
	assert(unlink(replacement_path.c_str()) == 0);
	RemoveDirectory(replaced_socket_directory);

	// Break caught: a live parent or singleton-lock replacement must be noticed
	// before the server can accept or dispatch another request.
	const std::string lock_replacement_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig lock_replacement_config;
	lock_replacement_config.parent_directory = lock_replacement_directory;
	lock_replacement_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator lock_replacement_coordinator;
	fogcast::RuntimeServer lock_replacement_server(lock_replacement_config, &lock_replacement_coordinator);
	assert(lock_replacement_server.Start() == fogcast::ServerResult::ok);
	const std::string lock_replacement_path = lock_replacement_directory + "/" + lock_replacement_config.lock_name;
	assert(unlink(lock_replacement_path.c_str()) == 0);
	const int replacement_lock_fd = open(lock_replacement_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(replacement_lock_fd >= 0);
	assert(lock_replacement_server.Poll(0) == fogcast::ServerResult::unsafe_lock);
	assert(lock_replacement_server.running());
	lock_replacement_server.Close();
	assert(lstat(lock_replacement_path.c_str(), &post_bind_stat) == 0);
	close(replacement_lock_fd);
	assert(unlink(lock_replacement_path.c_str()) == 0);
	assert(lstat((lock_replacement_directory + "/" + lock_replacement_config.socket_name).c_str(), &post_bind_stat) != 0);
	assert(rmdir(lock_replacement_directory.c_str()) == 0);

	const std::string parent_replacement_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig parent_replacement_config;
	parent_replacement_config.parent_directory = parent_replacement_directory;
	parent_replacement_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator parent_replacement_coordinator;
	fogcast::RuntimeServer parent_replacement_server(parent_replacement_config, &parent_replacement_coordinator);
	assert(parent_replacement_server.Start() == fogcast::ServerResult::ok);
	const std::string moved_parent_directory = parent_replacement_directory + ".moved";
	assert(rename(parent_replacement_directory.c_str(), moved_parent_directory.c_str()) == 0);
	assert(mkdir(parent_replacement_directory.c_str(), 0700) == 0);
	assert(parent_replacement_server.Poll(0) == fogcast::ServerResult::unsafe_parent);
	assert(parent_replacement_server.running());
	parent_replacement_server.Close();
	assert(lstat((moved_parent_directory + "/" + parent_replacement_config.socket_name).c_str(), &post_bind_stat) != 0);
	assert(unlink((moved_parent_directory + "/" + parent_replacement_config.lock_name).c_str()) == 0);
	assert(rmdir(moved_parent_directory.c_str()) == 0);
	assert(rmdir(parent_replacement_directory.c_str()) == 0);

	// Break caught: reading even a well-formed frame before credential rejection
	// could dispatch an unauthorized mutation.
	const std::string denied_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig denied_config;
	denied_config.parent_directory = denied_directory;
	denied_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator denied_coordinator;
	OtherUserCredentials denied_credentials;
	fogcast::RuntimeServer denied_server(denied_config, &denied_coordinator, &denied_credentials);
	assert(denied_server.Start() == fogcast::ServerResult::ok);
	struct sockaddr_un denied_address;
	memset(&denied_address, 0, sizeof(denied_address));
	denied_address.sun_family = AF_UNIX;
	const std::string denied_socket = denied_directory + "/" + denied_config.socket_name;
	strncpy(denied_address.sun_path, denied_socket.c_str(), sizeof(denied_address.sun_path) - 1);
	int denied_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(denied_client >= 0);
	assert(connect(denied_client, reinterpret_cast<const struct sockaddr*>(&denied_address), sizeof(denied_address)) == 0);
	const std::string denied_frame = Frame("{\"protocol\":1,\"request_id\":1,\"operation\":\"hello\",\"body\":{}}");
	WriteAll(denied_client, denied_frame.data(), denied_frame.size());
	Pump(&denied_server, 2);
	ExpectClosed(denied_client);
	assert(denied_coordinator.sequence() == 0);
	close(denied_client);
	denied_server.Close();
	RemoveDirectory(denied_directory);

#ifdef __linux__
	// Break caught: the Linux production credential path must authenticate the
	// actual AF_UNIX peer with SO_PEERCRED rather than only the test seam.
	const std::string peercred_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig peercred_config;
	peercred_config.parent_directory = peercred_directory;
	peercred_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator peercred_coordinator;
	fogcast::RuntimeServer peercred_server(peercred_config, &peercred_coordinator);
	assert(peercred_server.Start() == fogcast::ServerResult::ok);
	struct sockaddr_un peercred_address;
	memset(&peercred_address, 0, sizeof(peercred_address));
	peercred_address.sun_family = AF_UNIX;
	const std::string peercred_socket = peercred_directory + "/" + peercred_config.socket_name;
	strncpy(peercred_address.sun_path, peercred_socket.c_str(), sizeof(peercred_address.sun_path) - 1);
	const int peercred_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(peercred_client >= 0);
	assert(connect(peercred_client, reinterpret_cast<const struct sockaddr*>(&peercred_address), sizeof(peercred_address)) == 0);
	const std::string peercred_frame = Frame("{\"protocol\":1,\"request_id\":1,\"operation\":\"hello\",\"body\":{}}");
	WriteAll(peercred_client, peercred_frame.data(), peercred_frame.size());
	Pump(&peercred_server);
	assert(ReadFrame(peercred_client).find("\"request_id\":1") != std::string::npos);
	close(peercred_client);
	peercred_server.Close();
	RemoveDirectory(peercred_directory);
#endif

	// Break caught: a full nonblocking drain wake pipe must not overwrite errno
	// in the signal-safe request path or turn an interrupted caller into an I/O
	// failure on its next Poll.
	const std::string wake_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig wake_config;
	wake_config.parent_directory = wake_directory;
	wake_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator wake_coordinator;
	fogcast::RuntimeServer wake_server(wake_config, &wake_coordinator);
	assert(wake_server.Start() == fogcast::ServerResult::ok);
	for (unsigned int attempt = 0; attempt != 131072; ++attempt) wake_server.RequestDrain();
	errno = EINTR;
	wake_server.RequestDrain();
	assert(errno == EINTR);
	assert(wake_server.Poll(0) == fogcast::ServerResult::ok);
	wake_server.Close();
	RemoveDirectory(wake_directory);

	// Break caught: two independently constructed servers racing for the same
	// lock must leave exactly one listener owner, rather than both proceeding
	// through stale-socket cleanup or both reporting success.
	const std::string race_directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig race_config;
	race_config.parent_directory = race_directory;
	race_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator race_first_coordinator;
	fogcast::Coordinator race_second_coordinator;
	fogcast::RuntimeServer race_first(race_config, &race_first_coordinator);
	fogcast::RuntimeServer race_second(race_config, &race_second_coordinator);
	fogcast::ServerResult race_first_result = fogcast::ServerResult::io_error;
	fogcast::ServerResult race_second_result = fogcast::ServerResult::io_error;
	std::thread first_starter([&] { race_first_result = race_first.Start(); });
	std::thread second_starter([&] { race_second_result = race_second.Start(); });
	first_starter.join();
	second_starter.join();
	assert((race_first_result == fogcast::ServerResult::ok) != (race_second_result == fogcast::ServerResult::ok));
	assert(race_first_result == fogcast::ServerResult::ok || race_first_result == fogcast::ServerResult::locked);
	assert(race_second_result == fogcast::ServerResult::ok || race_second_result == fogcast::ServerResult::locked);
	race_first.Close();
	race_second.Close();
	RemoveDirectory(race_directory);

	// Break caught: a startup path that accepts a shared lock could delete or
	// replace the live daemon's socket.
	const std::string directory = TemporaryDirectory();
	fogcast::RuntimeServerConfig config;
	config.parent_directory = directory;
	config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::Coordinator coordinator;
	SameUserCredentials credentials;
	fogcast::RuntimeServer first(config, &coordinator, &credentials);
	assert(first.Start() == fogcast::ServerResult::ok);
	struct stat socket_stat;
	assert(lstat((directory + "/" + config.socket_name).c_str(), &socket_stat) == 0);
	assert(S_ISSOCK(socket_stat.st_mode));
	assert((socket_stat.st_mode & 0777) == 0600);
	struct stat lock_stat;
	assert(lstat((directory + "/" + config.lock_name).c_str(), &lock_stat) == 0);
	assert(S_ISREG(lock_stat.st_mode));
	assert((lock_stat.st_mode & 0777) == 0600);
	assert(lock_stat.st_nlink == 1);
	const ino_t first_socket_inode = socket_stat.st_ino;
	const ino_t first_lock_inode = lock_stat.st_ino;
	// Break caught: Start re-entry can replace the held anchors or leak
	// descriptors while the original listener continues to serve requests.
	assert(first.Start() == fogcast::ServerResult::invalid_argument);
	assert(first.running());
	assert(lstat((directory + "/" + config.socket_name).c_str(), &socket_stat) == 0);
	assert(lstat((directory + "/" + config.lock_name).c_str(), &lock_stat) == 0);
	assert(socket_stat.st_ino == first_socket_inode);
	assert(lock_stat.st_ino == first_lock_inode);

	// Break caught: a valid peer frame being rejected before a read-only
	// response would make the configured peer-credential seam unusable.
	int client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(client >= 0);
	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	const std::string socket_path = directory + "/" + config.socket_name;
	assert(socket_path.size() < sizeof(address.sun_path));
	strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
	assert(connect(client, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) == 0);
	const char frame[] = "\x00\x00\x00\x3b{\"protocol\":1,\"request_id\":1,\"operation\":\"hello\",\"body\":{}}";
	WriteAll(client, frame, sizeof(frame) - 1);
	for (unsigned int attempt = 0; attempt != 20; ++attempt)
		assert(first.Poll(10) == fogcast::ServerResult::ok);
	const std::string response = ReadFrame(client);
	assert(response == "{\"protocol\":1,\"request_id\":1,\"ok\":true,\"result\":{\"protocol\":1,\"abi_version\":2,\"ready\":false,\"max_frame_bytes\":65536,\"max_connections\":1,\"max_replay_entries\":64,\"capabilities\":0},\"snapshot\":null}");
	const std::string unavailable_health = Frame("{\"protocol\":1,\"request_id\":4,\"operation\":\"health\",\"body\":{}}");
	WriteAll(client, unavailable_health.data(), unavailable_health.size());
	Pump(&first);
	assert(ReadFrame(client).find("\"ready\":false,\"phase\":null,\"mode\":null,\"capabilities\":0") != std::string::npos);
	const std::string unavailable_status = Frame("{\"protocol\":1,\"request_id\":5,\"operation\":\"status\",\"body\":{}}");
	WriteAll(client, unavailable_status.data(), unavailable_status.size());
	Pump(&first);
	assert(ReadFrame(client).find("\"code\":\"NOT_READY\"") != std::string::npos);

	// Break caught: closing every connection after its first response loses the
	// protocol's required sequential multi-frame session semantics.
	const char second_frame[] = "\x00\x00\x00\x3b{\"protocol\":1,\"request_id\":2,\"operation\":\"hello\",\"body\":{}}";
	WriteAll(client, second_frame, sizeof(second_frame) - 1);
	for (unsigned int attempt = 0; attempt != 20; ++attempt)
		assert(first.Poll(10) == fogcast::ServerResult::ok);
	assert(ReadFrame(client) == "{\"protocol\":1,\"request_id\":2,\"ok\":true,\"result\":{\"protocol\":1,\"abi_version\":2,\"ready\":false,\"max_frame_bytes\":65536,\"max_connections\":1,\"max_replay_entries\":64,\"capabilities\":0},\"snapshot\":null}");

	// Break caught: an extra client accepted while the active client has work
	// outstanding could issue a second concurrent coordinator request.
	const char third_frame[] = "\x00\x00\x00\x3b{\"protocol\":1,\"request_id\":3,\"operation\":\"hello\",\"body\":{}}";
	struct sigaction previous_sigpipe, sigpipe_action;
	memset(&sigpipe_action, 0, sizeof(sigpipe_action));
	sigpipe_action.sa_handler = CountSigpipe;
	assert(sigemptyset(&sigpipe_action.sa_mask) == 0);
	assert(sigaction(SIGPIPE, &sigpipe_action, &previous_sigpipe) == 0);
	g_sigpipe_count = 0;
	assert(shutdown(client, SHUT_RD) == 0);
	WriteAll(client, third_frame, sizeof(third_frame) - 1);
	assert(first.Poll(0) == fogcast::ServerResult::ok);
	int extra = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(extra >= 0);
	assert(connect(extra, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) == 0);
	assert(first.Poll(0) == fogcast::ServerResult::ok);
	char closed = 0;
	assert(read(extra, &closed, 1) == 0);
	close(extra);
	for (unsigned int attempt = 0; attempt != 20; ++attempt)
		assert(first.Poll(10) == fogcast::ServerResult::ok);
	assert(g_sigpipe_count == 0);
	assert(sigaction(SIGPIPE, &previous_sigpipe, 0) == 0);
	close(client);
	first.RequestDrain();
	for (unsigned int attempt = 0; attempt != 5; ++attempt)
		assert(first.Poll(0) == fogcast::ServerResult::ok);
	// An unavailable/failed-style coordinator is never turned into an implicit
	// shutdown mutation merely because a drain was requested.
	assert(first.running());
	fogcast::RuntimeServer second(config, &coordinator, &credentials);
	assert(second.Start() == fogcast::ServerResult::locked);
	second.RequestDrain();
	for (unsigned int attempt = 0; attempt != 5; ++attempt)
		assert(second.Poll(0) == fogcast::ServerResult::invalid_argument);
	first.Close();
	assert(lstat((directory + "/" + config.socket_name).c_str(), &socket_stat) != 0);
	assert(first.Start() == fogcast::ServerResult::invalid_argument);
	RemoveDirectory(directory);

	// Break caught: an already verified idle coordinator that remains bound
	// after a drain request leaks its singleton socket and blocks replacement.
	const std::string idle_directory = TemporaryDirectory();
	fogcast::BackendFence fence(idle_directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 1)) == fogcast::ErrorClass::ok);
	NeutralPlatform platform;
	fogcast::Coordinator idle_coordinator(idle_directory, &fence, &platform);
	assert(idle_coordinator.Initialize(1) == fogcast::ErrorClass::ok);
	fogcast::RuntimeServerConfig idle_config;
	idle_config.parent_directory = idle_directory;
	idle_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::RuntimeServer idle_server(idle_config, &idle_coordinator, &credentials);
	assert(idle_server.Start() == fogcast::ServerResult::ok);
	int drain_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(drain_client >= 0);
	struct sockaddr_un idle_address;
	memset(&idle_address, 0, sizeof(idle_address));
	idle_address.sun_family = AF_UNIX;
	const std::string idle_socket = idle_directory + "/" + idle_config.socket_name;
	assert(idle_socket.size() < sizeof(idle_address.sun_path));
	strncpy(idle_address.sun_path, idle_socket.c_str(), sizeof(idle_address.sun_path) - 1);
	assert(connect(drain_client, reinterpret_cast<const struct sockaddr*>(&idle_address), sizeof(idle_address)) == 0);
	const std::string drain_frame = Frame("{\"protocol\":1,\"request_id\":6,\"operation\":\"hello\",\"body\":{}}");
	assert(idle_server.Poll(0) == fogcast::ServerResult::ok);
	idle_server.RequestDrain();
	WriteAll(drain_client, drain_frame.data(), drain_frame.size());
	bool idle_drained = false;
	for (unsigned int attempt = 0; attempt != 20 && !idle_drained; ++attempt) {
		const fogcast::ServerResult result = idle_server.Poll(10);
		assert(result == fogcast::ServerResult::ok || result == fogcast::ServerResult::drained);
		idle_drained = result == fogcast::ServerResult::drained;
	}
	assert(idle_drained);
	assert(ReadFrame(drain_client).find("\"request_id\":6") != std::string::npos);
	close(drain_client);
	assert(idle_server.running());
	idle_server.Close();
	assert(!idle_server.running());
	assert(lstat((idle_directory + "/" + idle_config.socket_name).c_str(), &socket_stat) != 0);
	assert(unlink((idle_directory + "/" + idle_config.lock_name).c_str()) == 0);
	assert(unlink((idle_directory + "/state.json").c_str()) == 0);
	assert(unlink((idle_directory + "/backend-fence.json").c_str()) == 0);
	assert(rmdir(idle_directory.c_str()) == 0);

	// Break caught: reading a partial prefix synchronously consumes the entire
	// prefix deadline inside one Poll call, closes a still-valid peer, and keeps
	// the listener from accepting or draining other events.
	const std::string partial_directory = TemporaryDirectory();
	fogcast::Coordinator partial_coordinator;
	FakeClock partial_clock;
	FailFirstPostBindStat partial_hooks;
	partial_hooks.fail_first = false;
	fogcast::RuntimeServerConfig partial_config;
	partial_config.parent_directory = partial_directory;
	partial_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::RuntimeServer partial_server(partial_config, &partial_coordinator,
		&credentials, &partial_clock, &partial_hooks);
	assert(partial_server.Start() == fogcast::ServerResult::ok);
	int partial_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(partial_client >= 0);
	struct sockaddr_un partial_address;
	memset(&partial_address, 0, sizeof(partial_address));
	partial_address.sun_family = AF_UNIX;
	const std::string partial_socket = partial_directory + "/" + partial_config.socket_name;
	assert(partial_socket.size() < sizeof(partial_address.sun_path));
	strncpy(partial_address.sun_path, partial_socket.c_str(), sizeof(partial_address.sun_path) - 1);
	assert(connect(partial_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	const std::string partial_frame = Frame("{\"protocol\":1,\"request_id\":7,\"operation\":\"hello\",\"body\":{}}");
	WriteAll(partial_client, partial_frame.data(), 2);
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	WriteAll(partial_client, partial_frame.data() + 2, partial_frame.size() - 2);
	Pump(&partial_server);
	assert(ReadFrame(partial_client).find("\"request_id\":7") != std::string::npos);
	close(partial_client);
	Pump(&partial_server, 2);

	// Break caught: a payload has its own absolute deadline.  Advancing a test
	// clock after only one payload byte must close this client, without waiting
	// inside Poll or refreshing the budget after an interrupted/readable wake.
	int deadline_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(deadline_client >= 0);
	assert(connect(deadline_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	const std::string deadline_frame = Frame("{\"protocol\":1,\"request_id\":8,\"operation\":\"hello\",\"body\":{}}");
	WriteAll(deadline_client, deadline_frame.data(), 5);
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	partial_clock.now = 2000;
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	ExpectClosed(deadline_client);
	close(deadline_client);

	// Break caught: a prefix deadline must remain absolute through EINTR; an
	// interrupted poll cannot refresh a partially received prefix's budget.
	int prefix_deadline_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(prefix_deadline_client >= 0);
	assert(connect(prefix_deadline_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	partial_clock.now = 3000;
	WriteAll(prefix_deadline_client, deadline_frame.data(), 1);
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	partial_clock.now = 4999;
	partial_hooks.interrupt_poll = true;
	assert(partial_server.Poll(10) == fogcast::ServerResult::ok);
	partial_clock.now = 5000;
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	ExpectClosed(prefix_deadline_client);
	close(prefix_deadline_client);

	// Break caught: retrying an interrupted read inside ReadClient can keep
	// consuming syscall interruptions after its stored prefix deadline has
	// expired. One interrupted syscall returns to Poll; the next Poll enforces
	// the original deadline and closes this partial frame.
	int prefix_eintr_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(prefix_eintr_client >= 0);
	assert(connect(prefix_eintr_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	partial_clock.now = 6000;
	WriteAll(prefix_eintr_client, deadline_frame.data(), 1);
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	WriteAll(prefix_eintr_client, deadline_frame.data() + 1, 1);
	partial_hooks.interrupt_clock = &partial_clock;
	partial_hooks.interrupt_advance_ms = 2000;
	partial_hooks.interrupt_read_count = 3;
	partial_hooks.interrupt_read_calls = 0;
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	assert(partial_hooks.interrupt_read_calls == 1);
	assert(partial_clock.now == 8000);
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	ExpectClosed(prefix_eintr_client);
	close(prefix_eintr_client);
	partial_hooks.interrupt_read_count = 0;

	// Break caught: payload reads carry a separate deadline and must not spin
	// through repeated EINTR after a partial JSON body has started.
	int payload_eintr_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(payload_eintr_client >= 0);
	assert(connect(payload_eintr_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	partial_clock.now = 10000;
	WriteAll(payload_eintr_client, deadline_frame.data(), 5);
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	WriteAll(payload_eintr_client, deadline_frame.data() + 5, 1);
	partial_hooks.interrupt_read_count = 3;
	partial_hooks.interrupt_read_calls = 0;
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	assert(partial_hooks.interrupt_read_calls == 1);
	assert(partial_clock.now == 12000);
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	ExpectClosed(payload_eintr_client);
	close(payload_eintr_client);
	partial_hooks.interrupt_read_count = 0;

	// Break caught: a backpressured response shares the one write deadline;
	// partial/non-progress send must not keep the client alive indefinitely.
	int response_deadline_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(response_deadline_client >= 0);
	assert(connect(response_deadline_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	partial_clock.now = 7000;
	partial_hooks.stall_send = true;
	partial_hooks.send_budget = 2;
	const std::string response_deadline_frame = Frame("{\"protocol\":1,\"request_id\":18,\"operation\":\"hello\",\"body\":{}}");
	WriteAll(response_deadline_client, response_deadline_frame.data(), response_deadline_frame.size());
	Pump(&partial_server, 20);
	unsigned char partial_response_prefix[2];
	assert(read(response_deadline_client, partial_response_prefix, sizeof(partial_response_prefix)) ==
		static_cast<ssize_t>(sizeof(partial_response_prefix)));
	partial_clock.now = 9000;
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	ExpectClosed(response_deadline_client);
	close(response_deadline_client);
	partial_hooks.stall_send = false;

	// Break caught: WriteClient must preserve a partial real response but return
	// after one interrupted send, so its existing deadline is enforced by the
	// following Poll instead of being crossed by an in-function retry loop.
	int response_eintr_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(response_eintr_client >= 0);
	assert(connect(response_eintr_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	partial_clock.now = 14000;
	partial_hooks.stall_send = true;
	partial_hooks.send_budget = 2;
	const std::string response_eintr_frame = Frame("{\"protocol\":1,\"request_id\":19,\"operation\":\"hello\",\"body\":{}}");
	WriteAll(response_eintr_client, response_eintr_frame.data(), response_eintr_frame.size());
	Pump(&partial_server, 20);
	unsigned char response_eintr_prefix[2];
	assert(read(response_eintr_client, response_eintr_prefix, sizeof(response_eintr_prefix)) ==
		static_cast<ssize_t>(sizeof(response_eintr_prefix)));
	partial_hooks.stall_send = false;
	partial_hooks.interrupt_send_count = 3;
	partial_hooks.interrupt_send_calls = 0;
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	assert(partial_hooks.interrupt_send_calls == 1);
	assert(partial_clock.now == 16000);
	assert(partial_server.Poll(0) == fogcast::ServerResult::ok);
	ExpectClosed(response_eintr_client);
	close(response_eintr_client);
	partial_hooks.interrupt_send_count = 0;

	// Break caught: protocol v2 is a server-owned mismatch response, while a
	// v1 read-only request with an unknown key is a closed protocol frame.
	partial_clock.now = 2001;
	int mismatch_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(mismatch_client >= 0);
	assert(connect(mismatch_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	const std::string mismatch_frame = Frame("{\"protocol\":2,\"request_id\":9,\"operation\":\"hello\",\"body\":{}}");
	WriteAll(mismatch_client, mismatch_frame.data(), mismatch_frame.size());
	Pump(&partial_server);
	assert(ReadFrame(mismatch_client).find("\"code\":\"PROTOCOL_MISMATCH\"") != std::string::npos);
	close(mismatch_client);
	Pump(&partial_server, 2);
	int strict_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(strict_client >= 0);
	assert(connect(strict_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	const std::string strict_frame = Frame("{\"protocol\":1,\"request_id\":10,\"operation\":\"hello\",\"body\":{},\"extra\":true}");
	WriteAll(strict_client, strict_frame.data(), strict_frame.size());
	Pump(&partial_server, 2);
	ExpectClosed(strict_client);
	close(strict_client);
	int duplicate_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(duplicate_client >= 0);
	assert(connect(duplicate_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	const std::string duplicate_frame = Frame("{\"protocol\":1,\"protocol\":1,\"request_id\":10,\"operation\":\"hello\",\"body\":{}}");
	WriteAll(duplicate_client, duplicate_frame.data(), duplicate_frame.size());
	Pump(&partial_server, 2);
	ExpectClosed(duplicate_client);
	close(duplicate_client);
	int second_value_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(second_value_client >= 0);
	assert(connect(second_value_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	const std::string second_value_frame = Frame("{\"protocol\":1,\"request_id\":10,\"operation\":\"hello\",\"body\":{}}{}");
	WriteAll(second_value_client, second_value_frame.data(), second_value_frame.size());
	Pump(&partial_server, 2);
	ExpectClosed(second_value_client);
	close(second_value_client);
	int v2_mutation_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(v2_mutation_client >= 0);
	assert(connect(v2_mutation_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	const std::string v2_mutation = Frame("{\"protocol\":2,\"request_id\":11,\"operation_id\":\"0123456789abcdef0123456789abcdef\",\"operation\":\"launch\",\"body\":{}}");
	WriteAll(v2_mutation_client, v2_mutation.data(), v2_mutation.size());
	Pump(&partial_server);
	assert(ReadFrame(v2_mutation_client).find("\"code\":\"PROTOCOL_MISMATCH\"") != std::string::npos);
	close(v2_mutation_client);
	Pump(&partial_server, 2);

	// Break caught: schema-invalid mutations are not preclosed by transport
	// correlation parsing.  The coordinator sees exactly one mutation attempt
	// and the response retains only the safe request and operation IDs.
	int invalid_mutation_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(invalid_mutation_client >= 0);
	assert(connect(invalid_mutation_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	const std::string invalid_mutation = Frame("{\"protocol\":1,\"request_id\":11,\"operation_id\":\"0123456789abcdef0123456789abcdef\",\"operation\":\"launch\",\"body\":{}}");
	WriteAll(invalid_mutation_client, invalid_mutation.data(), invalid_mutation.size());
	Pump(&partial_server);
	const std::string invalid_response = ReadFrame(invalid_mutation_client);
	assert(invalid_response.find("\"request_id\":11") != std::string::npos);
	assert(invalid_response.find("\"operation_id\":\"0123456789abcdef0123456789abcdef\"") != std::string::npos);
	assert(invalid_response.find("\"code\":\"INVALID_REQUEST\"") != std::string::npos);
	close(invalid_mutation_client);
	Pump(&partial_server, 2);
	int unknown_mutation_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(unknown_mutation_client >= 0);
	assert(connect(unknown_mutation_client, reinterpret_cast<const struct sockaddr*>(&partial_address), sizeof(partial_address)) == 0);
	const std::string unknown_mutation = Frame("{\"protocol\":1,\"request_id\":12,\"operation_id\":\"abcdefabcdefabcdefabcdefabcdefab\",\"operation\":\"host_cast\",\"body\":{}}");
	WriteAll(unknown_mutation_client, unknown_mutation.data(), unknown_mutation.size());
	Pump(&partial_server);
	assert(ReadFrame(unknown_mutation_client).find("\"code\":\"INVALID_REQUEST\"") != std::string::npos);
	close(unknown_mutation_client);
	partial_server.Close();
	RemoveDirectory(partial_directory);

	// Break caught: while a lifecycle worker is blocked, every extra accepted
	// connection must be closed, disconnecting the original client must not
	// cancel the durable operation, and later status must expose its completion.
	const std::string blocked_directory = TemporaryDirectory();
	fogcast::BackendFence blocked_fence(blocked_directory);
	assert(blocked_fence.Commit(fogcast::BackendFence::Record(1, "native", 1)) == fogcast::ErrorClass::ok);
	BlockingPlatform blocked_platform;
	fogcast::Coordinator blocked_coordinator(blocked_directory, &blocked_fence, &blocked_platform);
	assert(blocked_coordinator.Initialize(1) == fogcast::ErrorClass::ok);
	fogcast::RuntimeServerConfig blocked_config;
	blocked_config.parent_directory = blocked_directory;
	blocked_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::RuntimeServer blocked_server(blocked_config, &blocked_coordinator, &credentials);
	assert(blocked_server.Start() == fogcast::ServerResult::ok);
	struct sockaddr_un blocked_address;
	memset(&blocked_address, 0, sizeof(blocked_address));
	blocked_address.sun_family = AF_UNIX;
	const std::string blocked_socket = blocked_directory + "/" + blocked_config.socket_name;
	strncpy(blocked_address.sun_path, blocked_socket.c_str(), sizeof(blocked_address.sun_path) - 1);
	int blocked_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(connect(blocked_client, reinterpret_cast<const struct sockaddr*>(&blocked_address), sizeof(blocked_address)) == 0);
	const std::string launch = Frame("{\"protocol\":1,\"request_id\":12,\"operation_id\":\"abcdefabcdefabcdefabcdefabcdefab\",\"operation\":\"launch\",\"candidate\":{\"session\":\"11111111111111111111111111111111\",\"generation\":1},\"precondition\":{\"sequence\":" + std::to_string(blocked_coordinator.sequence()) + ",\"owner\":null},\"body\":{\"game_id\":\"sonic\",\"system\":\"megadrive\",\"expected_core\":\"MegaDrive\",\"cache_lease_id\":\"22222222222222222222222222222222\",\"content\":{\"sha256\":\"3333333333333333333333333333333333333333333333333333333333333333\",\"size\":1,\"extension\":\"md\"}}}");
	WriteAll(blocked_client, launch.data(), launch.size());
	Pump(&blocked_server, 3);
	for (unsigned int wait = 0; wait != 1000 && !blocked_platform.entered.load(); ++wait)
		assert(blocked_server.Poll(1) == fogcast::ServerResult::ok);
	assert(blocked_platform.entered.load());
	// Break caught: polling POLLIN while a durable worker owns the request makes
	// pipelined bytes return immediately and spins one core.  The next frame is
	// intentionally left unread until the first response has completed.
	const std::string pipelined = Frame("{\"protocol\":1,\"request_id\":15,\"operation\":\"hello\",\"body\":{}}");
	WriteAll(blocked_client, pipelined.data(), pipelined.size());
	const uint64_t worker_wait_start = MonotonicMs();
	for (unsigned int attempt = 0; attempt != 3; ++attempt)
		assert(blocked_server.Poll(25) == fogcast::ServerResult::ok);
	assert(MonotonicMs() - worker_wait_start >= 45u);
	int extra_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(connect(extra_client, reinterpret_cast<const struct sockaddr*>(&blocked_address), sizeof(blocked_address)) == 0);
	assert(blocked_server.Poll(0) == fogcast::ServerResult::ok);
	ExpectClosed(extra_client);
	close(extra_client);
	close(blocked_client);
	Pump(&blocked_server, 10);
	blocked_platform.release.store(true);
	Pump(&blocked_server, 80);
	assert(blocked_platform.starts.load() == 1);
	int status_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(connect(status_client, reinterpret_cast<const struct sockaddr*>(&blocked_address), sizeof(blocked_address)) == 0);
	const std::string status_frame = Frame("{\"protocol\":1,\"request_id\":13,\"operation\":\"status\",\"body\":{}}");
	WriteAll(status_client, status_frame.data(), status_frame.size());
	Pump(&blocked_server);
	assert(ReadFrame(status_client).find("\"phase\":\"active\"") != std::string::npos);
	close(status_client);
	Pump(&blocked_server, 2);
	int health_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(health_client >= 0);
	assert(connect(health_client, reinterpret_cast<const struct sockaddr*>(&blocked_address), sizeof(blocked_address)) == 0);
	const std::string active_health = Frame("{\"protocol\":1,\"request_id\":16,\"operation\":\"health\",\"body\":{}}");
	WriteAll(health_client, active_health.data(), active_health.size());
	Pump(&blocked_server);
	assert(ReadFrame(health_client).find("\"ready\":true,\"phase\":\"active\",\"mode\":\"fpga_native\",\"capabilities\":31") != std::string::npos);
	close(health_client);
	Pump(&blocked_server, 2);
	// Break caught: a drain request before the coordinator is idle must still
	// admit an authenticated stop. Its canonical success response must be fully
	// observable before a *subsequent* Poll may close the singleton endpoint.
	blocked_server.RequestDrain();
	int draining_stop_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(draining_stop_client >= 0);
	assert(connect(draining_stop_client, reinterpret_cast<const struct sockaddr*>(&blocked_address), sizeof(blocked_address)) == 0);
	const std::string draining_stop = Frame(OwnerRequest(blocked_coordinator.sequence(), 14,
		"abcdefabcdefabcdefabcdefabcdefac", "stop", "11111111111111111111111111111111", 1));
	WriteAll(draining_stop_client, draining_stop.data(), draining_stop.size());
	PumpUntilReadable(&blocked_server, draining_stop_client);
	const std::string draining_stop_response = ReadFrame(draining_stop_client);
	assert(draining_stop_response.find("\"ok\":true") != std::string::npos);
	assert(draining_stop_response.find("\"phase\":\"idle\"") != std::string::npos);
	assert(blocked_server.running());
	close(draining_stop_client);
	PumpUntilClosed(&blocked_server);
	assert(lstat(blocked_socket.c_str(), &post_bind_stat) != 0);
	assert(unlink((blocked_directory + "/" + blocked_config.lock_name).c_str()) == 0);
	assert(unlink((blocked_directory + "/state.json").c_str()) == 0);
	assert(unlink((blocked_directory + "/backend-fence.json").c_str()) == 0);
	assert(rmdir(blocked_directory.c_str()) == 0);

	// Break caught: after an active stop fails, drain cannot discard the only
	// recover request. The recovery reply must be delivered before cleanup.
	const std::string failed_directory = TemporaryDirectory();
	fogcast::BackendFence failed_fence(failed_directory);
	assert(failed_fence.Commit(fogcast::BackendFence::Record(1, "native", 1)) == fogcast::ErrorClass::ok);
	DrainPlatform failed_platform;
	fogcast::Coordinator failed_coordinator(failed_directory, &failed_fence, &failed_platform);
	assert(failed_coordinator.Initialize(1) == fogcast::ErrorClass::ok);
	fogcast::RuntimeServerConfig failed_config;
	failed_config.parent_directory = failed_directory;
	failed_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::RuntimeServer failed_server(failed_config, &failed_coordinator, &credentials);
	assert(failed_server.Start() == fogcast::ServerResult::ok);
	struct sockaddr_un failed_address;
	memset(&failed_address, 0, sizeof(failed_address));
	failed_address.sun_family = AF_UNIX;
	const std::string failed_socket = failed_directory + "/" + failed_config.socket_name;
	strncpy(failed_address.sun_path, failed_socket.c_str(), sizeof(failed_address.sun_path) - 1);
	int failed_launch_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(failed_launch_client >= 0);
	assert(connect(failed_launch_client, reinterpret_cast<const struct sockaddr*>(&failed_address), sizeof(failed_address)) == 0);
	const std::string failed_launch = Frame(LaunchRequest(failed_coordinator.sequence(), 21,
		"abcdefabcdefabcdefabcdefabcdefad", "44444444444444444444444444444444", 2));
	WriteAll(failed_launch_client, failed_launch.data(), failed_launch.size());
	PumpUntilReadable(&failed_server, failed_launch_client);
	assert(ReadFrame(failed_launch_client).find("\"ok\":true") != std::string::npos);
	close(failed_launch_client);
	Pump(&failed_server, 2);
	failed_platform.fail_stop = true;
	int failed_stop_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(failed_stop_client >= 0);
	assert(connect(failed_stop_client, reinterpret_cast<const struct sockaddr*>(&failed_address), sizeof(failed_address)) == 0);
	const std::string failed_stop = Frame(OwnerRequest(failed_coordinator.sequence(), 22,
		"abcdefabcdefabcdefabcdefabcdefae", "stop", "44444444444444444444444444444444", 2));
	WriteAll(failed_stop_client, failed_stop.data(), failed_stop.size());
	PumpUntilReadable(&failed_server, failed_stop_client);
	assert(ReadFrame(failed_stop_client).find("\"code\":\"RECOVERY_REQUIRED\"") != std::string::npos);
	close(failed_stop_client);
	Pump(&failed_server, 2);
	assert(failed_coordinator.phase() == "failed");
	failed_platform.fail_stop = false;
	failed_server.RequestDrain();
	int failed_recover_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(failed_recover_client >= 0);
	assert(connect(failed_recover_client, reinterpret_cast<const struct sockaddr*>(&failed_address), sizeof(failed_address)) == 0);
	const std::string failed_recover = Frame(OwnerRequest(failed_coordinator.sequence(), 23,
		"abcdefabcdefabcdefabcdefabcdefaf", "recover", "44444444444444444444444444444444", 2));
	WriteAll(failed_recover_client, failed_recover.data(), failed_recover.size());
	PumpUntilReadable(&failed_server, failed_recover_client);
	const std::string failed_recover_response = ReadFrame(failed_recover_client);
	assert(failed_recover_response.find("\"ok\":true") != std::string::npos);
	assert(failed_recover_response.find("\"phase\":\"idle\"") != std::string::npos);
	assert(failed_server.running());
	close(failed_recover_client);
	PumpUntilClosed(&failed_server);
	assert(lstat(failed_socket.c_str(), &post_bind_stat) != 0);
	assert(unlink((failed_directory + "/" + failed_config.lock_name).c_str()) == 0);
	assert(unlink((failed_directory + "/state.json").c_str()) == 0);
	assert(unlink((failed_directory + "/backend-fence.json").c_str()) == 0);
	assert(rmdir(failed_directory.c_str()) == 0);

	// Break caught: a drain cannot close a server while an admitted recover is
	// blocked in the lifecycle platform. It must wait for the durable terminal
	// response, let the peer read it, and only then perform cleanup.
	const std::string recovering_directory = TemporaryDirectory();
	fogcast::BackendFence recovering_fence(recovering_directory);
	assert(recovering_fence.Commit(fogcast::BackendFence::Record(1, "native", 1)) == fogcast::ErrorClass::ok);
	DrainPlatform recovering_platform;
	fogcast::Coordinator recovering_coordinator(recovering_directory, &recovering_fence, &recovering_platform);
	assert(recovering_coordinator.Initialize(1) == fogcast::ErrorClass::ok);
	fogcast::RuntimeServerConfig recovering_config;
	recovering_config.parent_directory = recovering_directory;
	recovering_config.service_uid = static_cast<uint32_t>(geteuid());
	fogcast::RuntimeServer recovering_server(recovering_config, &recovering_coordinator, &credentials);
	assert(recovering_server.Start() == fogcast::ServerResult::ok);
	struct sockaddr_un recovering_address;
	memset(&recovering_address, 0, sizeof(recovering_address));
	recovering_address.sun_family = AF_UNIX;
	const std::string recovering_socket = recovering_directory + "/" + recovering_config.socket_name;
	strncpy(recovering_address.sun_path, recovering_socket.c_str(), sizeof(recovering_address.sun_path) - 1);
	int recovering_launch_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(recovering_launch_client >= 0);
	assert(connect(recovering_launch_client, reinterpret_cast<const struct sockaddr*>(&recovering_address), sizeof(recovering_address)) == 0);
	const std::string recovering_launch = Frame(LaunchRequest(recovering_coordinator.sequence(), 31,
		"abcdefabcdefabcdefabcdefabcdefb0", "55555555555555555555555555555555", 3));
	WriteAll(recovering_launch_client, recovering_launch.data(), recovering_launch.size());
	PumpUntilReadable(&recovering_server, recovering_launch_client);
	assert(ReadFrame(recovering_launch_client).find("\"ok\":true") != std::string::npos);
	close(recovering_launch_client);
	Pump(&recovering_server, 2);
	recovering_platform.block_stop.store(true);
	recovering_server.RequestDrain();
	int recovering_client = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(recovering_client >= 0);
	assert(connect(recovering_client, reinterpret_cast<const struct sockaddr*>(&recovering_address), sizeof(recovering_address)) == 0);
	const std::string recovering_request = Frame(OwnerRequest(recovering_coordinator.sequence(), 32,
		"abcdefabcdefabcdefabcdefabcdefb1", "recover", "55555555555555555555555555555555", 3));
	WriteAll(recovering_client, recovering_request.data(), recovering_request.size());
	for (unsigned int attempt = 0; attempt != 1000 && !recovering_platform.stop_entered.load(); ++attempt)
		assert(recovering_server.Poll(1) == fogcast::ServerResult::ok);
	assert(recovering_platform.stop_entered.load());
	assert(recovering_server.running());
	assert(lstat(recovering_socket.c_str(), &post_bind_stat) == 0);
	recovering_platform.release_stop.store(true);
	PumpUntilReadable(&recovering_server, recovering_client);
	const std::string recovering_response = ReadFrame(recovering_client);
	assert(recovering_response.find("\"ok\":true") != std::string::npos);
	assert(recovering_response.find("\"phase\":\"idle\"") != std::string::npos);
	assert(recovering_server.running());
	close(recovering_client);
	PumpUntilClosed(&recovering_server);
	assert(lstat(recovering_socket.c_str(), &post_bind_stat) != 0);
	assert(unlink((recovering_directory + "/" + recovering_config.lock_name).c_str()) == 0);
	assert(unlink((recovering_directory + "/state.json").c_str()) == 0);
	assert(unlink((recovering_directory + "/backend-fence.json").c_str()) == 0);
	assert(rmdir(recovering_directory.c_str()) == 0);
	return 0;
}
