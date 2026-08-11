// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fogcast/runtime_platform_factory.hpp"
#include "fogcast/runtime_server.hpp"
#include "fogcast/runtime_coordinator.hpp"
#include "fogcast/backend_fence.hpp"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <limits>
#include <string>

namespace {

fogcast::RuntimeServer* volatile g_signal_server = 0;

void DrainHandler(int) {
	const int saved_errno = errno;
	fogcast::RuntimeServer* server = g_signal_server;
	if (server) server->RequestDrain();
	errno = saved_errno;
}

#ifdef FOGCAST_RUNTIME_HOST_TEST
bool Injected(const char* point) {
	const char* configured = getenv("FOGCAST_TEST_SIGNAL_FAIL");
	if (!configured) return false;
	const std::string list(configured);
	size_t begin = 0;
	while (begin <= list.size()) {
		const size_t end = list.find(',', begin);
		if (list.substr(begin, end == std::string::npos ? end : end - begin) == point) return true;
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return false;
}
#else
bool Injected(const char*) { return false; }
#endif

int ChangeMask(int how, const sigset_t* set, sigset_t* old, const char* point) {
	return Injected(point) ? EINVAL : pthread_sigmask(how, set, old);
}

int ChangeAction(int signal_number, const struct sigaction* action,
	struct sigaction* old, const char* point) {
	if (Injected(point)) { errno = EINVAL; return -1; }
	return sigaction(signal_number, action, old);
}

bool ParseEpoch(const char* text, uint64_t* value) {
	if (!text || !value || text[0] < '1' || text[0] > '9') return false;
	uint64_t parsed = 0;
	for (const unsigned char* byte = reinterpret_cast<const unsigned char*>(text); *byte; ++byte) {
		if (*byte < '0' || *byte > '9') return false;
		const uint64_t digit = static_cast<uint64_t>(*byte - '0');
		if (parsed > (UINT64_C(9223372036854775807) - digit) / 10) return false;
		parsed = parsed * 10 + digit;
	}
	*value = parsed;
	return true;
}

class BoundLifecyclePlatform : public fogcast::AbiV2LifecyclePlatform {
public:
	explicit BoundLifecyclePlatform(const fogcast::RuntimePlatformBinding& binding)
		: AbiV2LifecyclePlatform(binding.abi_v2), context_(binding.observation_context),
		  main_absent_(binding.main_absent) {}
	bool MainAbsent() override { return main_absent_(context_); }
private:
	void* context_;
	bool (*main_absent_)(void*);
};

int StartupFailure(fogcast::RuntimeServer* server, bool started,
	bool int_installed, bool term_installed,
	const struct sigaction& old_int, const struct sigaction& old_term) {
	g_signal_server = 0;
	bool restoration_ok = true;
	if (term_installed && ChangeAction(SIGTERM, &old_term, 0, "restore_sigterm") != 0)
		restoration_ok = false;
	if (int_installed && ChangeAction(SIGINT, &old_int, 0, "restore_sigint") != 0)
		restoration_ok = false;
	if (started) server->Close();
	return restoration_ok ? 2 : 3;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 3) return 2;
	uint64_t epoch = 0;
	if (!ParseEpoch(argv[2], &epoch)) return 2;
	const fogcast::RuntimePlatformBinding* binding = fogcast::RuntimePlatform();
	if (!binding || !binding->abi_v2 || !binding->main_absent) return 2;
	const uid_t effective_uid = geteuid();
	if (static_cast<uintmax_t>(effective_uid) >
		static_cast<uintmax_t>(std::numeric_limits<uint32_t>::max())) return 2;

	sigset_t drain_set;
	if (sigemptyset(&drain_set) != 0 || sigaddset(&drain_set, SIGINT) != 0 ||
		sigaddset(&drain_set, SIGTERM) != 0) return 2;
	sigset_t old_mask;
	if (ChangeMask(SIG_BLOCK, &drain_set, &old_mask, "block_initial") != 0) return 2;
	sigset_t running_mask = old_mask;
	if (sigdelset(&running_mask, SIGINT) != 0 || sigdelset(&running_mask, SIGTERM) != 0)
		return 2;

	BoundLifecyclePlatform platform(*binding);
	fogcast::BackendFence fence(argv[1]);
	fogcast::Coordinator coordinator(argv[1], &fence, &platform);
	(void)coordinator.Initialize(epoch);  // Exactly once; unavailable remains inspectable.
	fogcast::RuntimeServerConfig config;
	config.parent_directory = argv[1];
	config.service_uid = static_cast<uint32_t>(effective_uid);
	fogcast::RuntimeServer server(config, &coordinator);
	if (server.Start() != fogcast::ServerResult::ok) return 2;

	struct sigaction action;
	action.sa_handler = DrainHandler;
	action.sa_flags = 0;
	if (sigemptyset(&action.sa_mask) != 0 || sigaddset(&action.sa_mask, SIGINT) != 0 ||
		sigaddset(&action.sa_mask, SIGTERM) != 0) {
		server.Close();
		return 2;
	}
	struct sigaction old_int;
	struct sigaction old_term;
	g_signal_server = &server;
	if (ChangeAction(SIGINT, &action, &old_int, "install_sigint") != 0)
		return StartupFailure(&server, true, false, false, old_int, old_term);
	if (ChangeAction(SIGTERM, &action, &old_term, "install_sigterm") != 0)
		return StartupFailure(&server, true, true, false, old_int, old_term);
	if (ChangeMask(SIG_SETMASK, &running_mask, 0, "unblock_running") != 0) {
		if (ChangeMask(SIG_BLOCK, &drain_set, 0, "reblock_startup") != 0) {
			g_signal_server = 0;
			_Exit(4);
		}
		return StartupFailure(&server, true, true, true, old_int, old_term);
	}

	fogcast::ServerResult result = fogcast::ServerResult::ok;
	while (result == fogcast::ServerResult::ok) result = server.Poll(60000);

	if (ChangeMask(SIG_BLOCK, &drain_set, 0, "block_teardown") != 0) {
		g_signal_server = 0;
		_Exit(4);
	}
	g_signal_server = 0;
	server.Close();
	const bool mask_ok = ChangeMask(SIG_SETMASK, &old_mask, 0, "restore_mask") == 0;
	bool dispositions_ok = true;
	if (ChangeAction(SIGTERM, &old_term, 0, "restore_sigterm") != 0) dispositions_ok = false;
	if (ChangeAction(SIGINT, &old_int, 0, "restore_sigint") != 0) dispositions_ok = false;
	return result == fogcast::ServerResult::drained && dispositions_ok && mask_ok ? 0 : 3;
}
