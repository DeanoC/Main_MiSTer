// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FOGCAST_RUNTIME_SERVER_HPP
#define FOGCAST_RUNTIME_SERVER_HPP

#include <stdint.h>
#include <string>

namespace fogcast {

class Coordinator;

// The header deliberately contains no Unix-domain or Linux socket types.  The
// descriptor is an opaque transport handle to this portable boundary.
class ServerCredentials {
public:
	virtual ~ServerCredentials() {}
	virtual bool PeerUid(int descriptor, uint32_t* uid) = 0;
};

class ServerClock {
public:
	virtual ~ServerClock() {}
	virtual uint64_t NowMs() = 0;
};

// This narrow seam is test-only. Production construction leaves it null; it
// exists so cleanup can be exercised at the post-bind validation boundary.
class ServerTestHooks {
public:
	virtual ~ServerTestHooks() {}
	virtual bool FailFirstPostBindEntryStat() { return false; }
	virtual bool FailPostBindValidation() { return false; }
	virtual bool InterruptPoll() { return false; }
	// Models a signal after Poll's entry latch and before its blocking wait.
	virtual bool RequestDrainAfterEntryLatch() { return false; }
	// Each call models one interrupted transport syscall. Test hooks may return
	// true once or repeatedly; production leaves both disabled.
	virtual bool InterruptRead() { return false; }
	virtual bool InterruptSend() { return false; }
	virtual bool StallSend() { return false; }
	virtual uint32_t SendAllowance(uint32_t requested) { return requested; }
};

struct RuntimeServerConfig {
	std::string parent_directory;
	std::string socket_name;
	std::string lock_name;
	uint32_t service_uid;
	RuntimeServerConfig() : socket_name("fogcast-runtime.sock"),
		lock_name("fogcast-runtime.lock"), service_uid(0) {}
};

enum class ServerResult {
	ok,
	drained,
	invalid_argument,
	unsafe_parent,
	unsafe_lock,
	locked,
	unsafe_socket,
	bind_failed,
	io_error,
};

class RuntimeServer {
public:
	struct State;
	RuntimeServer(const RuntimeServerConfig& config, Coordinator* coordinator,
		ServerCredentials* credentials = 0, ServerClock* clock = 0,
		ServerTestHooks* test_hooks = 0);
	~RuntimeServer();

	// Single-use: only a pristine object may be started. It opens and retains
	// the verified parent and singleton lock before inspecting a socket. A
	// non-ok result leaves no server-owned socket behind.
	ServerResult Start();
	// Serves one readiness interval. After Start publishes descriptors, this
	// never closes them: drained and fatal results transfer cleanup to main.
	ServerResult Poll(uint32_t timeout_ms);
	// Signal handlers call only this method's async-safe wake source in the
	// process wrapper; it does not synthesize a coordinator mutation.
	void RequestDrain();
	bool draining() const;
	bool running() const;
	void Close();

	static bool ValidFrameLength(uint32_t length);
	static const char* MessageForCode(const char* code);

private:
	RuntimeServer(const RuntimeServer&);
	RuntimeServer& operator=(const RuntimeServer&);
	State* state_;
};

}  // namespace fogcast

#endif
