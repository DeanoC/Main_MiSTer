/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fogcast/runtime_coordinator.hpp"
#include "fogcast/backend_fence.hpp"
#include "runtime/mister_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

namespace {

void AssertLiveCleanup(const std::vector<std::string>& values);

struct FakePlatform : public fogcast::LifecyclePlatform {
	std::vector<std::string> actions;
	std::vector<uint32_t> deadlines;
	fogcast::LifecycleOwner live_owner;
	fogcast::LiveHandleState live_state = fogcast::LiveHandleState::none;
	fogcast::LaunchMetadata last_launch;
	fogcast::LifecycleOwner granted_owner;
	uint32_t active_mask = 0;
	uint32_t neutral_mask = 0;
	bool fail_create = false;
	bool fail_start = false;
	bool fail_load = false;
	bool fail_observe = false;
	bool fail_stop = false;
	bool fail_destroy = false;
	bool fail_reset = false;
	bool neutral = true;
	bool main_absent = true;
	unsigned int stateless_attempt_while_live = 0;
	uint64_t* mutable_now = 0;
	uint64_t advance_start = 0;
	uint64_t advance_stop = 0;
	uint64_t advance_neutral = 0;
	uint64_t advance_reset = 0;
	unsigned int incomplete_stops = 0;
	unsigned int incomplete_neutral = 0;
	std::vector<uint32_t> reset_neutral_masks;
	std::vector<MisterResult> reset_results;
	std::vector<uint32_t> neutral_masks;
	std::vector<uint64_t> call_times;
	uint32_t reset_observed_or = 0;

	void Called() { if (mutable_now) call_times.push_back(*mutable_now); }

	fogcast::LifecycleResult ProveNeutral(uint32_t mask, uint32_t deadline) override {
		Called();
		actions.push_back("neutral"); deadlines.push_back(deadline);
		fogcast::LifecycleResult result(neutral && (active_mask & mask) == 0 ?
			MISTER_RESULT_OK : MISTER_RESULT_CLEANUP_INCOMPLETE);
		result.neutral_mask = result == MISTER_RESULT_OK ? mask : 0;
		result.observed_mask = active_mask & mask;
		return result;
	}
	fogcast::LifecycleResult GrantTransfer(const fogcast::LifecycleOwner& owner) override {
		Called();
		actions.push_back("grant");
		if (active_mask != 0 || !owner.present) return fogcast::LifecycleResult::invalid_state;
		granted_owner = owner;
		return fogcast::LifecycleResult::ok;
	}
	fogcast::LifecycleResult Create(const fogcast::LifecycleOwner& owner) override {
		Called();
		actions.push_back("create");
		if (active_mask != 0 || !granted_owner.present || granted_owner.session != owner.session || granted_owner.generation != owner.generation)
			return fogcast::LifecycleResult::invalid_state;
		if (fail_create) return fogcast::LifecycleResult::platform;
		live_owner = owner; live_state = fogcast::LiveHandleState::live;
		return fogcast::LifecycleResult::ok;
	}
	fogcast::LifecycleResult Start(uint32_t deadline) override {
		Called();
		actions.push_back("start"); deadlines.push_back(deadline);
		if (mutable_now) *mutable_now += advance_start;
		return fail_start ? fogcast::LifecycleResult::platform : fogcast::LifecycleResult::ok;
	}
	fogcast::LifecycleResult Load(const fogcast::LaunchMetadata& launch, uint32_t deadline) override {
		Called();
		actions.push_back("load"); deadlines.push_back(deadline); last_launch = launch;
		return fail_load ? fogcast::LifecycleResult::platform : fogcast::LifecycleResult::ok;
	}
	fogcast::LifecycleResult Observe(const std::string&, uint32_t deadline) override {
		Called();
		actions.push_back("observe"); deadlines.push_back(deadline);
		if (fail_observe) return fogcast::LifecycleResult::platform;
		active_mask = MISTER_RESOURCE_V2_KNOWN; neutral_mask = 0;
		fogcast::LifecycleResult result(MISTER_RESULT_OK, MISTER_RESULT_OK, MISTER_RESULT_OK, MISTER_STATE_READY);
		result.ready = true; result.active_mask = active_mask; result.observed_mask = active_mask;
		result.observed_core = "MegaDrive";
		return result;
	}
	fogcast::LifecycleResult Stop(uint32_t deadline) override {
		Called();
		actions.push_back("stop"); deadlines.push_back(deadline);
		if (mutable_now) *mutable_now += advance_stop;
		if (incomplete_stops != 0) {
			--incomplete_stops;
			return fogcast::LifecycleResult(MISTER_RESULT_CLEANUP_INCOMPLETE,
				MISTER_RESULT_OK, MISTER_RESULT_CLEANUP_INCOMPLETE, MISTER_STATE_CLEANUP_INCOMPLETE);
		}
		return fail_stop ? fogcast::LifecycleResult(MISTER_RESULT_CLEANUP_INCOMPLETE,
			MISTER_RESULT_OK, MISTER_RESULT_CLEANUP_INCOMPLETE, MISTER_STATE_CLEANUP_INCOMPLETE) :
			fogcast::LifecycleResult(MISTER_RESULT_OK, MISTER_RESULT_OK, MISTER_RESULT_OK, MISTER_STATE_STOPPED);
	}
	fogcast::LifecycleResult ObserveNeutral(uint32_t mask, uint32_t deadline) override {
		Called();
		actions.push_back("neutral"); deadlines.push_back(deadline);
		neutral_masks.push_back(mask);
		if (mutable_now) *mutable_now += advance_neutral;
		if (live_state != fogcast::LiveHandleState::live || !neutral) return fogcast::LifecycleResult::cleanup_incomplete;
		uint32_t cleared = mask;
		if (!reset_neutral_masks.empty()) {
			cleared = reset_neutral_masks.front() & mask;
			reset_neutral_masks.erase(reset_neutral_masks.begin());
		}
		active_mask &= ~cleared; neutral_mask |= cleared;
		fogcast::LifecycleResult result(MISTER_RESULT_OK, MISTER_RESULT_OK, MISTER_RESULT_OK, MISTER_STATE_STOPPED);
		result.neutral_mask = cleared;
		// A live ABI observation is a whole-runtime fact.  Keeping it whole here
		// catches coordinators that hide a later reassertion behind a narrowed bit.
		result.observed_mask = active_mask;
		if (incomplete_neutral != 0) {
			--incomplete_neutral;
			result.call_result = MISTER_RESULT_CLEANUP_INCOMPLETE;
			result.cleanup_result = MISTER_RESULT_CLEANUP_INCOMPLETE;
			result.runtime_state = MISTER_STATE_CLEANUP_INCOMPLETE;
		}
		return result;
	}
	fogcast::LifecycleResult Destroy() override {
		Called();
		actions.push_back("destroy");
		if (fail_destroy || active_mask != 0 || live_state != fogcast::LiveHandleState::live) return fogcast::LifecycleResult::invalid_state;
		live_owner = fogcast::LifecycleOwner(); live_state = fogcast::LiveHandleState::none;
		return fogcast::LifecycleResult::ok;
	}
	fogcast::LifecycleResult RecoverStateless(uint32_t mask, uint32_t deadline) override {
		Called();
		actions.push_back("reset"); deadlines.push_back(deadline);
		if (live_state != fogcast::LiveHandleState::none) {
			++stateless_attempt_while_live;
			return fogcast::LifecycleResult::invalid_state;
		}
		if (!neutral || fail_reset) return fogcast::LifecycleResult::cleanup_incomplete;
		uint32_t cleared = mask;
		if (!reset_neutral_masks.empty()) {
			cleared = reset_neutral_masks.front() & mask;
			reset_neutral_masks.erase(reset_neutral_masks.begin());
		}
		active_mask &= ~cleared; neutral_mask |= cleared;
		if (mutable_now) *mutable_now += advance_reset;
		fogcast::LifecycleResult result;
		result.neutral_mask = cleared; result.observed_mask = (active_mask & mask) | reset_observed_or;
		if (!reset_results.empty()) {
			result.call_result = reset_results.front();
			result.cleanup_result = reset_results.front();
			if (reset_results.front() != MISTER_RESULT_OK) result.runtime_state = MISTER_STATE_CLEANUP_INCOMPLETE;
			reset_results.erase(reset_results.begin());
		}
		return result;
	}
	void LoseHandle() { live_owner = fogcast::LifecycleOwner(); live_state = fogcast::LiveHandleState::none; }
	fogcast::LiveHandleState live_handle_state() const override {
		return fogcast::LiveHandleState(live_state.state, live_owner);
	}
	bool MainAbsent() override { return main_absent; }
};

struct FakeClock : public fogcast::MonotonicClock {
	uint64_t now = 0;
	uint64_t NowMs() override { return now; }
};

struct FakeCrash : public fogcast::CommitCrashInjector {
	std::string phase;
	std::vector<std::string> commits;
	uint64_t* mutable_now = 0;
	uint64_t advance_recorded = 0;
	bool AfterCommit(const std::string& committed_phase) override {
		commits.push_back(committed_phase);
		if (committed_phase == "recorded" && mutable_now) *mutable_now += advance_recorded;
		return phase == committed_phase;
	}
};

struct AbiV2Fake {
	std::vector<std::string> actions;
	std::vector<uint32_t> deadlines;
	MisterResult observe_result = MISTER_RESULT_OK;
	MisterResult recover_result = MISTER_RESULT_OK;
	MisterResult start_result = MISTER_RESULT_OK;
	MisterResult load_result = MISTER_RESULT_OK;
	MisterResult stop_result = MISTER_RESULT_OK;
	uint32_t observation_flags = MISTER_RESOURCE_V2_KNOWN;
	uint32_t neutral_flags = MISTER_RESOURCE_V2_KNOWN;
	std::string game_id;
	std::string system;
	std::string expected_core;
	std::string sha256;
	uint64_t size = 0;
	std::string extension;
	static MisterResult Start(void* opaque, uint32_t deadline) {
		AbiV2Fake* self = static_cast<AbiV2Fake*>(opaque);
		self->actions.push_back("start"); self->deadlines.push_back(deadline);
		return self->start_result;
	}
	static MisterResult Load(void* opaque, const MisterLaunchV2* launch, uint32_t deadline) {
		AbiV2Fake* self = static_cast<AbiV2Fake*>(opaque);
		self->actions.push_back("load"); self->deadlines.push_back(deadline);
		self->game_id.assign(launch->game_id.data, launch->game_id.length);
		self->system.assign(launch->system.data, launch->system.length);
		self->expected_core.assign(launch->expected_core.data, launch->expected_core.length);
		self->sha256.assign(launch->content.sha256.data, launch->content.sha256.length);
		self->size = launch->content.size;
		self->extension.assign(launch->content.extension.data, launch->content.extension.length);
		return self->load_result;
	}
	static MisterResult Tick(void*, uint32_t) { return MISTER_RESULT_OK; }
	static MisterResult Observe(void* opaque, MisterObservationV2* result, uint32_t deadline) {
		AbiV2Fake* self = static_cast<AbiV2Fake*>(opaque);
		self->actions.push_back("observe"); self->deadlines.push_back(deadline);
		result->ready = 1; result->observed_core = {"MegaDrive", 9};
		result->resource_flags = self->observation_flags;
		return self->observe_result;
	}
	static MisterResult Stop(void* opaque, uint32_t deadline) {
		AbiV2Fake* self = static_cast<AbiV2Fake*>(opaque);
		self->actions.push_back("stop"); self->deadlines.push_back(deadline);
		self->observation_flags = 0;
		return self->stop_result;
	}
	static MisterResult Recover(void* opaque, uint32_t required,
		MisterRecoveryObservationV2* result, uint32_t deadline) {
		AbiV2Fake* self = static_cast<AbiV2Fake*>(opaque);
		self->actions.push_back("recover"); self->deadlines.push_back(deadline);
		result->observed_resource_flags = 0;
		result->neutral_resource_flags = self->neutral_flags & required;
		return self->recover_result;
	}
	MisterPlatformV2 Platform() {
		MisterPlatformV2 platform = {};
		platform.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
		platform.struct_size = sizeof(platform);
		platform.capability_flags = MISTER_CAP_V2_KNOWN;
		platform.context = this; platform.start = Start; platform.load = Load;
		platform.tick = Tick; platform.observe = Observe; platform.stop = Stop;
		platform.recover = Recover;
		return platform;
	}
};

const char* const kLaunchOne =
	"{\"protocol\":1,\"request_id\":5,\"operation_id\":\"fedcba9876543210fedcba9876543210\",\"operation\":\"launch\",\"candidate\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42},\"precondition\":{\"sequence\":1,\"owner\":null},\"body\":{\"game_id\":\"synthetic-game\",\"system\":\"megadrive\",\"expected_core\":\"MegaDrive\",\"cache_lease_id\":\"00112233445566778899aabbccddeeff\",\"content\":{\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":1048576,\"extension\":\"md\"}}}";

std::string IdleLaunch(uint64_t sequence, const char* operation_id) {
	std::string value(kLaunchOne);
	const size_t sequence_at = value.find("\"sequence\":1");
	assert(sequence_at != std::string::npos);
	value.replace(sequence_at, sizeof("\"sequence\":1") - 1,
		std::string("\"sequence\":") + std::to_string(sequence));
	const size_t id = value.find("fedcba9876543210fedcba9876543210");
	assert(id != std::string::npos);
	value.replace(id, 32, operation_id);
	return value;
}

std::string IdleLaunchSecond(uint64_t sequence) {
	std::string value = IdleLaunch(sequence, "11111111111111111111111111111111");
	const size_t session = value.find("0123456789abcdef0123456789abcdef");
	assert(session != std::string::npos);
	value.replace(session, 32, "22222222222222222222222222222222");
	const size_t generation = value.find("\"generation\":42");
	assert(generation != std::string::npos);
	value.replace(generation, sizeof("\"generation\":42") - 1, "\"generation\":43");
	return value;
}

std::string ActiveLaunchSecond(uint64_t sequence) {
	std::string value = IdleLaunchSecond(sequence);
	const size_t precondition = value.find("\"precondition\":");
	assert(precondition != std::string::npos);
	const size_t owner = value.find("\"owner\":null", precondition);
	assert(owner != std::string::npos);
	value.replace(owner, sizeof("\"owner\":null") - 1,
		"\"owner\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42,\"mode\":\"fpga_native\"}");
	return value;
}

std::string ReleaseRequest(const char* operation, const char* operation_id,
	uint64_t sequence, const char* reason = 0) {
	const std::string owner = "{\"session\":\"22222222222222222222222222222222\",\"generation\":43,\"mode\":\"fpga_native\"}";
	std::string body = reason ? std::string("{\"reason\":\"") + reason + "\"}" : "{}";
	return std::string("{\"protocol\":1,\"request_id\":9,\"operation_id\":\"") + operation_id +
		"\",\"operation\":\"" + operation + "\",\"owner\":" + owner +
		",\"precondition\":{\"sequence\":" + std::to_string(sequence) + ",\"owner\":" + owner +
		"},\"body\":" + body + '}';
}

std::string IdleRequest(const char* operation, const char* operation_id, uint64_t sequence,
	const char* reason = 0) {
	const std::string body = reason ? std::string("{\"reason\":\"") + reason + "\"}" : "{}";
	const std::string owner = operation == std::string("recover") ? ",\"owner\":null" : "";
	return std::string("{\"protocol\":1,\"request_id\":10,\"operation_id\":\"") + operation_id +
		"\",\"operation\":\"" + operation + "\"" + owner + ",\"precondition\":{\"sequence\":" +
		std::to_string(sequence) + ",\"owner\":null},\"body\":" + body + '}';
}

std::string ActiveLaunchSameOwner(uint64_t sequence, const char* operation_id) {
	std::string value = ActiveLaunchSecond(sequence);
	const size_t id = value.find("11111111111111111111111111111111");
	assert(id != std::string::npos);
	value.replace(id, 32, operation_id);
	const size_t session = value.find("22222222222222222222222222222222");
	assert(session != std::string::npos);
	value.replace(session, 32, "0123456789abcdef0123456789abcdef");
	const size_t generation = value.find("\"generation\":43");
	assert(generation != std::string::npos);
	value.replace(generation, sizeof("\"generation\":43") - 1, "\"generation\":42");
	return value;
}

std::string LedgerBytes(const std::string& canonical) {
	const size_t start = canonical.find("\"ledger\":");
	const size_t end = canonical.rfind('}');
	assert(start != std::string::npos && end != std::string::npos);
	return canonical.substr(start + sizeof("\"ledger\":") - 1, end - start - (sizeof("\"ledger\":") - 1));
}

std::string WireDigest(const std::string& request) {
	std::string canonical;
	std::string digest;
	assert(fogcast::ParseWire(request, &canonical, &digest) == fogcast::ErrorClass::ok);
	return digest;
}

std::string StateField(const std::string& state, const char* name) {
	fogcast::detail::Token root;
	assert(fogcast::detail::ScanV1Json(state, 256 * 1024, &root) == fogcast::ErrorClass::ok);
	const fogcast::detail::Token* field = fogcast::detail::Member(root, name);
	assert(field != 0);
	return fogcast::detail::Encode(*field);
}

std::string OwnerRequest(const char* operation, const char* operation_id, uint64_t sequence,
	const char* session, uint64_t generation, const char* reason = "ambiguous") {
	const std::string owner = std::string("{\"session\":\"") + session +
		"\",\"generation\":" + std::to_string(generation) + ",\"mode\":\"fpga_native\"}";
	const std::string body = operation == std::string("stop") ? "{}" :
		std::string("{\"reason\":\"") + reason + "\"}";
	return std::string("{\"protocol\":1,\"request_id\":11,\"operation_id\":\"") + operation_id +
		"\",\"operation\":\"" + operation + "\",\"owner\":" + owner +
		",\"precondition\":{\"sequence\":" + std::to_string(sequence) + ",\"owner\":" + owner +
		"},\"body\":" + body + '}';
}

std::string NumericOperationId(unsigned int value) {
	char id[33];
	const int written = snprintf(id, sizeof(id), "%032x", value);
	assert(written == 32);
	return std::string(id);
}

std::string ReadBytes(const std::string& path) {
	std::ifstream input(path.c_str(), std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::string& path, const std::string& bytes) {
	std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
	output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	output.close();
	assert(output.good());
}

std::string Checksummed(const std::string& raw) {
	const size_t at = raw.rfind(",\"sha256\":");
	assert(at != std::string::npos);
	const std::string canonical = raw.substr(0, at) + "}";
	return raw.substr(0, at) + ",\"sha256\":\"" + fogcast::Sha256Hex(canonical) + "\"}";
}

std::string CorruptChecksum(const std::string& raw) {
	assert(raw.size() > 3);
	std::string result(raw);
	result[result.size() - 3] = result[result.size() - 3] == '0' ? '1' : '0';
	return result;
}

void TestInitializeStoreTriage() {
	// Break caught: a new coordinator may create sequence one only after the
	// complete resource mask is observed neutral; failed proof leaves no file.
	char fresh[] = "/tmp/fogcast-coordinator-fresh-XXXXXX";
	assert(mkdtemp(fresh) != 0);
	fogcast::BackendFence fresh_fence(fresh);
	assert(fresh_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform fresh_platform;
	fogcast::Coordinator fresh_coordinator(fresh, &fresh_fence, &fresh_platform);
	assert(fresh_coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	assert(fresh_coordinator.ready() && fresh_coordinator.phase() == "idle");
	assert((fresh_platform.actions == std::vector<std::string>{"neutral"}));
	const std::string state_path = std::string(fresh) + "/state.json";
	struct stat state_status;
	assert(lstat(state_path.c_str(), &state_status) == 0 && S_ISREG(state_status.st_mode));

	char no_neutral[] = "/tmp/fogcast-coordinator-no-neutral-XXXXXX";
	assert(mkdtemp(no_neutral) != 0);
	fogcast::BackendFence no_neutral_fence(no_neutral);
	assert(no_neutral_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform no_neutral_platform;
	no_neutral_platform.neutral = false;
	fogcast::Coordinator no_neutral_coordinator(no_neutral, &no_neutral_fence, &no_neutral_platform);
	assert(no_neutral_coordinator.Initialize(7) == fogcast::ErrorClass::transition);
	assert(!no_neutral_coordinator.ready());
	assert((no_neutral_platform.actions == std::vector<std::string>{"neutral"}));
	assert(lstat((std::string(no_neutral) + "/state.json").c_str(), &state_status) != 0 && errno == ENOENT);

	// Break caught: corrupt, unsafe, or semantically invalid existing state is
	// never treated as initial absence.  It cannot cause a platform call, write,
	// replacement, or temporary file.
	const std::string valid = ReadBytes(state_path);
	const std::string cases[] = {
		"{",
		CorruptChecksum(valid),
		Checksummed(valid.substr(0, valid.find("\"sequence\":1") + std::string("\"sequence\":").size()) +
			"0" + valid.substr(valid.find(',', valid.find("\"sequence\":1"))))
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		WriteBytes(state_path, cases[i]);
		assert(chmod(state_path.c_str(), 0600) == 0);
		const std::string before = ReadBytes(state_path);
		const std::string before_hash = fogcast::Sha256Hex(before);
		assert(stat(state_path.c_str(), &state_status) == 0);
		const mode_t before_mode = state_status.st_mode & 0777;
		FakePlatform invalid_platform;
		fogcast::Coordinator invalid(fresh, &fresh_fence, &invalid_platform);
		assert(invalid.Initialize(7) == fogcast::ErrorClass::schema);
		assert(!invalid.ready() && invalid_platform.actions.empty());
		assert(ReadBytes(state_path) == before && fogcast::Sha256Hex(ReadBytes(state_path)) == before_hash);
		assert(stat(state_path.c_str(), &state_status) == 0 && (state_status.st_mode & 0777) == before_mode);
		assert(lstat((std::string(fresh) + "/state.json.tmp").c_str(), &state_status) != 0 && errno == ENOENT);
	}
	WriteBytes(state_path, valid);
	assert(chmod(state_path.c_str(), 0400) == 0);
	FakePlatform unsafe_platform;
	fogcast::Coordinator unsafe(fresh, &fresh_fence, &unsafe_platform);
	assert(unsafe.Initialize(7) == fogcast::ErrorClass::schema);
	assert(!unsafe.ready() && unsafe_platform.actions.empty());
	assert((stat(state_path.c_str(), &state_status) == 0) && (state_status.st_mode & 0777) == 0400);
	assert(lstat((std::string(fresh) + "/state.json.tmp").c_str(), &state_status) != 0 && errno == ENOENT);
}

void TestAbiRecoveryRejectsIncompleteAndInvalidObservation() {
	AbiV2Fake fake;
	MisterPlatformV2 platform = fake.Platform();
	fogcast::AbiV2LifecyclePlatform lifecycle(&platform);
	fogcast::LifecycleOwner owner;
	owner.present = true; owner.session = "0123456789abcdef0123456789abcdef"; owner.generation = 42;
	assert(lifecycle.Create(owner) == fogcast::LifecycleResult::invalid_state);
	assert(lifecycle.GrantTransfer(owner) == fogcast::LifecycleResult::ok);
	assert(lifecycle.Create(owner) == fogcast::LifecycleResult::ok);
	fake.observation_flags = MISTER_RESOURCE_V2_KNOWN | (1u << 31);
	assert(lifecycle.Observe("MegaDrive", 5000) == fogcast::LifecycleResult::platform);
	fake.observation_flags = MISTER_RESOURCE_V2_KNOWN;
	fake.neutral_flags = MISTER_RESOURCE_V2_KNOWN & ~MISTER_RESOURCE_FPGA;
	assert(lifecycle.ProveNeutral(MISTER_RESOURCE_V2_KNOWN, 5000) == fogcast::LifecycleResult::invalid_state);
	fake.neutral_flags = MISTER_RESOURCE_V2_KNOWN;
	fake.recover_result = MISTER_RESULT_DEADLINE;
	assert(lifecycle.Stop(5000) == fogcast::LifecycleResult::ok);
	assert(lifecycle.Destroy() == fogcast::LifecycleResult::ok);
	assert(lifecycle.RecoverStateless(MISTER_RESOURCE_V2_KNOWN, 5000) == fogcast::LifecycleResult::deadline);
}

void TestExactMetadataAndDeadlineBudgets() {
	// Break caught: the ABI adapter must forward the admitted bytes rather than
	// inventing convenient game/system/hash/extension placeholders.
	AbiV2Fake abi_fake;
	MisterPlatformV2 abi_platform = abi_fake.Platform();
	fogcast::AbiV2LifecyclePlatform abi(&abi_platform);
	fogcast::LifecycleOwner owner;
	owner.present = true; owner.session = "0123456789abcdef0123456789abcdef"; owner.generation = 42;
	fogcast::LaunchMetadata metadata;
	metadata.game_id = "game-with-exact-bytes"; metadata.system = "system-with-exact-bytes";
	metadata.expected_core = "ExactCore";
	metadata.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	metadata.size = 0x123456ULL; metadata.extension = "exactext";
	assert(abi.GrantTransfer(owner) == fogcast::LifecycleResult::ok);
	assert(abi.Create(owner) == fogcast::LifecycleResult::ok);
	assert(abi.Start(77) == fogcast::LifecycleResult::ok);
	assert(abi.Load(metadata, 76) == fogcast::LifecycleResult::ok);
	assert(abi_fake.game_id == metadata.game_id && abi_fake.system == metadata.system &&
		abi_fake.expected_core == metadata.expected_core && abi_fake.sha256 == metadata.sha256 &&
		abi_fake.size == metadata.size && abi_fake.extension == metadata.extension);
	assert(abi.Stop(75) == fogcast::LifecycleResult::ok);
	assert(abi.Destroy() == fogcast::LifecycleResult::ok);

	// EXIT_REQUIRED retains the ABI handle and is neither stateless recovery nor
	// a destroyable stop result.
	AbiV2Fake exiting_fake;
	MisterPlatformV2 exiting_platform = exiting_fake.Platform();
	fogcast::AbiV2LifecyclePlatform exiting(&exiting_platform);
	assert(exiting.GrantTransfer(owner) == fogcast::LifecycleResult::ok);
	assert(exiting.Create(owner) == fogcast::LifecycleResult::ok);
	assert(exiting.Start(75) == fogcast::LifecycleResult::ok);
	exiting_fake.stop_result = MISTER_RESULT_EXIT_REQUIRED;
	assert(exiting.Stop(74) == fogcast::LifecycleResult::exit_required);
	assert(exiting.live_handle_state() == fogcast::LiveHandleState::exit_required);
	assert(exiting.Destroy() == fogcast::LifecycleResult::invalid_state);
	assert(exiting.RecoverStateless(MISTER_RESOURCE_V2_KNOWN, 73) == fogcast::LifecycleResult::invalid_state);

	char directory[] = "/tmp/fogcast-coordinator-budgets-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakeClock clock;
	FakePlatform platform;
	platform.mutable_now = &clock.now;
	fogcast::Coordinator coordinator(directory, &fence, &platform, 0, &clock);
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	platform.actions.clear(); platform.deadlines.clear();
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab")) == fogcast::ErrorClass::ok);
	assert(platform.last_launch.game_id == "synthetic-game" && platform.last_launch.system == "megadrive" &&
		platform.last_launch.expected_core == "MegaDrive" && platform.last_launch.size == 1048576 &&
		platform.last_launch.extension == "md");
	assert((platform.deadlines == std::vector<uint32_t>{15000, 15000, 15000, 15000}));
	platform.actions.clear(); platform.deadlines.clear();
	assert(coordinator.Execute(OwnerRequest("stop", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac", coordinator.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	assert((platform.deadlines == std::vector<uint32_t>{2000, 2000, 2000}));
	AssertLiveCleanup(platform.actions);

	// Once start consumes launch's entire budget the coordinator must neither
	// call load/observe nor start a fresh cleanup budget.
	char exhausted_directory[] = "/tmp/fogcast-coordinator-exhausted-XXXXXX";
	assert(mkdtemp(exhausted_directory) != 0);
	fogcast::BackendFence exhausted_fence(exhausted_directory);
	assert(exhausted_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakeClock exhausted_clock;
	FakePlatform exhausted_platform;
	exhausted_platform.mutable_now = &exhausted_clock.now;
	exhausted_platform.advance_start = 15000;
	fogcast::Coordinator exhausted(exhausted_directory, &exhausted_fence, &exhausted_platform, 0, &exhausted_clock);
	assert(exhausted.Initialize(7) == fogcast::ErrorClass::ok);
	exhausted_platform.actions.clear(); exhausted_platform.deadlines.clear();
	assert(exhausted.Execute(IdleLaunch(exhausted.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad")) == fogcast::ErrorClass::ok);
	assert(exhausted.phase() == "failed");
	assert((exhausted_platform.actions == std::vector<std::string>{"neutral", "grant", "create", "start"}));
	assert((exhausted_platform.deadlines == std::vector<uint32_t>{15000, 15000}));
}

void TestFenceQuiescingAndMetadataAdoption() {
	char directory[] = "/tmp/fogcast-coordinator-adoption-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform platform;
	fogcast::Coordinator native(directory, &fence, &platform);
	assert(native.Initialize(7) == fogcast::ErrorClass::ok);
	assert(native.Execute(IdleRequest("recover", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1",
		native.sequence(), "ambiguous")) == fogcast::ErrorClass::ok);
	const std::string ledger_before_adoption = LedgerBytes(native.canonical_state());
	const uint64_t native_sequence = native.sequence();
	assert(fence.Commit(fogcast::BackendFence::Record(2, "native_quiescing", 7)) ==
		fogcast::ErrorClass::ok);
	platform.actions.clear();
	fogcast::Coordinator drain(directory, &fence, &platform);
	// Break caught: restricted drain must continue an exact-epoch idle
	// reconciliation, but must not reopen candidate admission.
	assert(drain.Initialize(7) == fogcast::ErrorClass::ok);
	assert((platform.actions == std::vector<std::string>{"neutral"}));
	const std::string quiescing_bytes = drain.canonical_state();
	platform.actions.clear();
	assert(drain.Execute(IdleLaunch(drain.sequence(), "abababababababababababababababab")) ==
		fogcast::ErrorClass::transition);
	assert(drain.canonical_state() == quiescing_bytes && platform.actions.empty());
	assert(fence.Commit(fogcast::BackendFence::Record(3, "transitioning", 7)) ==
		fogcast::ErrorClass::ok);
	const std::string transitioning_bytes = ReadBytes(std::string(directory) + "/state.json");
	platform.actions.clear();
	fogcast::Coordinator transitioning(directory, &fence, &platform);
	assert(transitioning.Initialize(7) == fogcast::ErrorClass::transition);
	assert(ReadBytes(std::string(directory) + "/state.json") == transitioning_bytes &&
		platform.actions.empty());
	assert(fence.Commit(fogcast::BackendFence::Record(4, "native", 8)) == fogcast::ErrorClass::ok);
	platform.actions.clear();
	fogcast::Coordinator adopted(directory, &fence, &platform);
	// Break caught: a newly granted native authority can adopt only a true
	// no-owner checkpoint, with exactly one metadata commit before admission.
	assert(adopted.Initialize(8) == fogcast::ErrorClass::ok);
	assert(adopted.sequence() == native_sequence + 1);
	assert(adopted.canonical_state().find("\"backend_epoch\":8") != std::string::npos);
	assert(LedgerBytes(adopted.canonical_state()) == ledger_before_adoption);
	assert((platform.actions == std::vector<std::string>{"neutral"}));
	platform.actions.clear();
	assert(adopted.Execute(IdleLaunch(adopted.sequence(), "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd")) ==
		fogcast::ErrorClass::ok);
	assert((platform.actions == std::vector<std::string>{"neutral", "grant", "create", "start", "load", "observe"}));
	assert(fence.Commit(fogcast::BackendFence::Record(5, "native_quiescing", 8)) ==
		fogcast::ErrorClass::ok);
	const std::string active_before_shutdown = adopted.canonical_state();
	platform.actions.clear();
	// Break caught: quiescing is release authority, not a general mutation
	// grant; an active-owner shutdown must fail before its recorded commit.
	assert(adopted.Execute(IdleRequest("shutdown", "dededededededededededededededede",
		adopted.sequence())) == fogcast::ErrorClass::transition);
	assert(adopted.canonical_state() == active_before_shutdown && platform.actions.empty());
	assert(adopted.Execute(OwnerRequest("stop", "edededededededededededededededed",
		adopted.sequence(), "0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	assert(adopted.phase() == "idle");

	char unsafe_directory[] = "/tmp/fogcast-coordinator-adoption-unsafe-XXXXXX";
	assert(mkdtemp(unsafe_directory) != 0);
	fogcast::BackendFence unsafe_fence(unsafe_directory);
	assert(unsafe_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform unsafe_platform;
	fogcast::Coordinator unsafe_native(unsafe_directory, &unsafe_fence, &unsafe_platform);
	assert(unsafe_native.Initialize(7) == fogcast::ErrorClass::ok);
	const std::string before_bytes = ReadBytes(std::string(unsafe_directory) + "/state.json");
	assert(unsafe_fence.Commit(fogcast::BackendFence::Record(2, "native_quiescing", 7)) == fogcast::ErrorClass::ok);
	assert(unsafe_fence.Commit(fogcast::BackendFence::Record(3, "transitioning", 7)) == fogcast::ErrorClass::ok);
	assert(unsafe_fence.Commit(fogcast::BackendFence::Record(4, "native", 8)) == fogcast::ErrorClass::ok);
	unsafe_platform.main_absent = false;
	unsafe_platform.actions.clear();
	fogcast::Coordinator main_present(unsafe_directory, &unsafe_fence, &unsafe_platform);
	assert(main_present.Initialize(8) == fogcast::ErrorClass::transition);
	assert(ReadBytes(std::string(unsafe_directory) + "/state.json") == before_bytes);
	assert(unsafe_platform.actions.empty());
	unsafe_platform.main_absent = true;
	unsafe_platform.neutral = false;
	fogcast::Coordinator nonneutral(unsafe_directory, &unsafe_fence, &unsafe_platform);
	assert(nonneutral.Initialize(8) == fogcast::ErrorClass::transition);
	assert(ReadBytes(std::string(unsafe_directory) + "/state.json") == before_bytes);
	assert((unsafe_platform.actions == std::vector<std::string>{"neutral"}));
	unsafe_platform.neutral = true;
	assert(unsafe_fence.Commit(fogcast::BackendFence::Record(5, "native_quiescing", 8)) == fogcast::ErrorClass::ok);
	assert(unsafe_fence.Commit(fogcast::BackendFence::Record(6, "transitioning", 8)) == fogcast::ErrorClass::ok);
	assert(unsafe_fence.Commit(fogcast::BackendFence::Record(7, "native", 9)) == fogcast::ErrorClass::ok);
	unsafe_platform.actions.clear();
	fogcast::Coordinator jumped(unsafe_directory, &unsafe_fence, &unsafe_platform);
	assert(jumped.Initialize(9) == fogcast::ErrorClass::transition);
	assert(unsafe_platform.actions.empty());

	char active_directory[] = "/tmp/fogcast-coordinator-adoption-active-XXXXXX";
	assert(mkdtemp(active_directory) != 0);
	fogcast::BackendFence active_fence(active_directory);
	assert(active_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform active_platform;
	fogcast::Coordinator active(active_directory, &active_fence, &active_platform);
	assert(active.Initialize(7) == fogcast::ErrorClass::ok);
	assert(active.Execute(IdleLaunch(active.sequence(), "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")) == fogcast::ErrorClass::ok);
	const std::string active_bytes = ReadBytes(std::string(active_directory) + "/state.json");
	assert(active_fence.Commit(fogcast::BackendFence::Record(2, "native_quiescing", 7)) == fogcast::ErrorClass::ok);
	assert(active_fence.Commit(fogcast::BackendFence::Record(3, "transitioning", 7)) == fogcast::ErrorClass::ok);
	assert(active_fence.Commit(fogcast::BackendFence::Record(4, "native", 8)) == fogcast::ErrorClass::ok);
	active_platform.actions.clear();
	fogcast::Coordinator unsafe_active(active_directory, &active_fence, &active_platform);
	assert(unsafe_active.Initialize(8) == fogcast::ErrorClass::transition);
	assert(ReadBytes(std::string(active_directory) + "/state.json") == active_bytes &&
		active_platform.actions.empty());

	char legacy_directory[] = "/tmp/fogcast-coordinator-legacy-XXXXXX";
	assert(mkdtemp(legacy_directory) != 0);
	fogcast::BackendFence legacy_fence(legacy_directory);
	assert(legacy_fence.Commit(fogcast::BackendFence::Record(1, "legacy", 7)) == fogcast::ErrorClass::ok);
	FakePlatform legacy_platform;
	fogcast::Coordinator legacy(legacy_directory, &legacy_fence, &legacy_platform);
	assert(legacy.Initialize(7) == fogcast::ErrorClass::transition);
	assert(legacy_platform.actions.empty());
	struct stat legacy_state;
	assert(lstat((std::string(legacy_directory) + "/state.json").c_str(), &legacy_state) != 0 && errno == ENOENT);
}

void AssertContains(const std::vector<std::string>& values, const char* value) {
	for (size_t i = 0; i < values.size(); ++i) if (values[i] == value) return;
	assert(false);
}

void AssertLiveCleanup(const std::vector<std::string>& values) {
	assert(values.size() >= 4);
	const size_t first = values.size() - 4;
	assert(values[first] == "stop" && values.back() == "destroy");
	for (size_t i = first + 1; i != values.size() - 1; ++i) assert(values[i] == "neutral");
}

// Every crash boundary must leave one durable owner shape: either the old
// release owner, no owner, or the transferred candidate.  In particular, the
// no-owner checkpoint cannot retain any exclusive lease while a candidate is
// merely an intent.
void AssertExclusiveOwnerShape(const fogcast::Coordinator& coordinator) {
	const std::string& state = coordinator.canonical_state();
	const std::string phase = coordinator.phase();
	if (phase == "idle" || phase == "no_owner") {
		assert(state.find("\"owner\":null") != std::string::npos);
		assert(state.find("\"leases\":[]") != std::string::npos);
	}
	if (phase == "intent" || phase == "releasing") {
		assert(state.find("\"candidate\":{\"session\"") != std::string::npos);
		assert(state.find("\"candidate\":{\"session\"") >
			state.find("\"release_owner\":"));
	}
	if (phase == "transferred" || phase == "active" || phase == "unwinding") {
		assert(state.find("\"owner\":{\"session\"") != std::string::npos);
		assert(state.find("\"leases\":[{\"resource\":\"fpga\"") != std::string::npos);
	}
}

void TestLaunchCrashMatrix() {
	const char* const phases[] = {
		"recorded", "intent", "releasing", "no_owner", "transferred", "active", "terminal"
	};
	for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i) {
		char directory[] = "/tmp/fogcast-coordinator-phase-XXXXXX";
		assert(mkdtemp(directory) != 0);
		fogcast::BackendFence fence(directory);
		assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
		FakePlatform platform;
		FakeCrash crash;
		crash.phase = phases[i];
		fogcast::Coordinator coordinator(directory, &fence, &platform, &crash);
		assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
		platform.actions.clear();
		const fogcast::ErrorClass crashed = coordinator.Execute(
			IdleLaunch(coordinator.sequence(), NumericOperationId(100 + i).c_str()));
		if (crashed != fogcast::ErrorClass::transition)
			fprintf(stderr, "crash phase %s returned %d\n", phases[i], static_cast<int>(crashed));
		assert(crashed == fogcast::ErrorClass::transition);
		AssertContains(crash.commits, phases[i]);
		AssertExclusiveOwnerShape(coordinator);
		fogcast::StateRecord transitional;
		assert(fogcast::StateStore(directory).Load(&transitional) == fogcast::StateLoadStatus::loaded);

		platform.actions.clear();
		platform.LoseHandle();
		fogcast::Coordinator restarted(directory, &fence, &platform);
		const fogcast::ErrorClass restored = restarted.Initialize(7);
		if (restored != fogcast::ErrorClass::ok)
			fprintf(stderr, "restart phase %s returned %d\n", phases[i], static_cast<int>(restored));
		assert(restored == fogcast::ErrorClass::ok);
		AssertExclusiveOwnerShape(restarted);
		assert(restarted.phase() == "idle");
		assert(restarted.canonical_state().find("\"last_error\":{\"code\":\"INTERRUPTED\"") !=
			std::string::npos);
		if (std::string(phases[i]) == "recorded" || std::string(phases[i]) == "intent" ||
			std::string(phases[i]) == "releasing" || std::string(phases[i]) == "no_owner")
			assert((platform.actions == std::vector<std::string>{"neutral"}));
		else
			assert((platform.actions == std::vector<std::string>{"reset", "neutral"}));
		fogcast::Coordinator::OperationStatus status = restarted.operation_status(NumericOperationId(100 + i));
		assert(status.kind == fogcast::Coordinator::OperationStatus::completed);
		if (std::string(phases[i]) == "active" || std::string(phases[i]) == "terminal") assert(status.ok);
		else {
			if (status.ok || status.error != "INTERRUPTED")
				fprintf(stderr, "restart phase %s terminal ok=%d error=%s\n", phases[i],
					status.ok ? 1 : 0, status.error.c_str());
			assert(!status.ok && status.error == "INTERRUPTED");
		}
	}
}

void TestActivationFailureMatrix() {
	struct FailureCase { const char* name; };
	const FailureCase cases[] = {{"create"}, {"start"}, {"load"}, {"observe"}};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		char directory[] = "/tmp/fogcast-coordinator-activate-XXXXXX";
		assert(mkdtemp(directory) != 0);
		fogcast::BackendFence fence(directory);
		assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
		FakePlatform platform;
		if (std::string(cases[i].name) == "create") platform.fail_create = true;
		if (std::string(cases[i].name) == "start") platform.fail_start = true;
		if (std::string(cases[i].name) == "load") platform.fail_load = true;
		if (std::string(cases[i].name) == "observe") platform.fail_observe = true;
		fogcast::Coordinator coordinator(directory, &fence, &platform);
		assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
		const std::string id = NumericOperationId(200 + i);
		assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), id.c_str())) == fogcast::ErrorClass::ok);
		assert(coordinator.phase() == "idle");
		AssertExclusiveOwnerShape(coordinator);
		fogcast::Coordinator::OperationStatus status = coordinator.operation_status(id);
		assert(status.kind == fogcast::Coordinator::OperationStatus::completed && !status.ok &&
			status.error == "INTERNAL");
		if (std::string(cases[i].name) == "create") AssertContains(platform.actions, "reset");
		else AssertLiveCleanup(platform.actions);
	}

	// If candidate cleanup cannot prove neutrality, the terminal record remains
	// failed and retains the candidate lease rather than inventing no_owner.
	char directory[] = "/tmp/fogcast-coordinator-cleanup-fail-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform platform;
	platform.fail_create = true;
	platform.fail_reset = true;
	fogcast::Coordinator coordinator(directory, &fence, &platform);
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), NumericOperationId(299).c_str())) ==
		fogcast::ErrorClass::ok);
		assert(coordinator.phase() == "failed");
		assert(coordinator.canonical_state().find("\"owner\":{\"session\"") != std::string::npos);
		assert(coordinator.canonical_state().find("\"leases\":[{\"resource\":\"fpga\"") != std::string::npos);
		fogcast::StateRecord failed_record;
		assert(fogcast::StateStore(directory).Load(&failed_record) == fogcast::StateLoadStatus::loaded);
		platform.actions.clear();
		fogcast::Coordinator restored(directory, &fence, &platform);
		assert(restored.Initialize(7) == fogcast::ErrorClass::ok && restored.ready());
		assert(platform.actions.empty());
}

void TestRecoverAndReleaseFailureMatrix() {
	char directory[] = "/tmp/fogcast-coordinator-recover-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform platform;
	fogcast::Coordinator coordinator(directory, &fence, &platform);
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), NumericOperationId(301).c_str())) ==
		fogcast::ErrorClass::ok);

	// Recovering an active owner uses the normal bounded stop/neutral/destroy
	// path, never a stateless reset that might attach to a live handle.
	platform.actions.clear();
	assert(coordinator.Execute(OwnerRequest("recover", NumericOperationId(302).c_str(), coordinator.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	AssertLiveCleanup(platform.actions);
	assert(coordinator.phase() == "idle");

	// Each release failure leaves the currently recorded owner and all eight
	// leases durable.  Retrying recover after the fault clears must use a fresh
	// CAS and complete only after another neutral observation.
	const char* const failures[] = {"stop", "neutral", "destroy"};
	for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
		char failed_directory[] = "/tmp/fogcast-coordinator-release-fail-XXXXXX";
		assert(mkdtemp(failed_directory) != 0);
		fogcast::BackendFence failed_fence(failed_directory);
		assert(failed_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
		FakePlatform failed_platform;
		fogcast::Coordinator failed(failed_directory, &failed_fence, &failed_platform);
		assert(failed.Initialize(7) == fogcast::ErrorClass::ok);
		assert(failed.Execute(IdleLaunch(failed.sequence(), NumericOperationId(310 + i).c_str())) == fogcast::ErrorClass::ok);
		if (std::string(failures[i]) == "stop") failed_platform.fail_stop = true;
		if (std::string(failures[i]) == "neutral") failed_platform.neutral = false;
		if (std::string(failures[i]) == "destroy") failed_platform.fail_destroy = true;
		assert(failed.Execute(OwnerRequest("stop", NumericOperationId(320 + i).c_str(), failed.sequence(),
			"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
		assert(failed.phase() == "failed");
		assert(failed.canonical_state().find("\"owner\":{\"session\"") != std::string::npos);
		assert(failed.canonical_state().find("\"leases\":[{\"resource\":\"fpga\"") != std::string::npos);
		failed_platform.fail_stop = false;
		failed_platform.fail_destroy = false;
		failed_platform.neutral = true;
		assert(failed.Execute(OwnerRequest("recover", NumericOperationId(330 + i).c_str(), failed.sequence(),
			"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
		assert(failed.phase() == "idle");
	}

	// Owner-null recovery does not pretend a partial/incomplete stateless reset
	// is neutral.  The failed shape has no owner, and only a later complete
	// observation permits its explicit recovery.
	char idle_directory[] = "/tmp/fogcast-coordinator-idle-fail-XXXXXX";
	assert(mkdtemp(idle_directory) != 0);
	fogcast::BackendFence idle_fence(idle_directory);
	assert(idle_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform idle_platform;
	fogcast::Coordinator idle(idle_directory, &idle_fence, &idle_platform);
	assert(idle.Initialize(7) == fogcast::ErrorClass::ok);
	idle_platform.neutral = false;
	assert(idle.Execute(IdleRequest("recover", NumericOperationId(340).c_str(), idle.sequence(), "ambiguous")) ==
		fogcast::ErrorClass::ok);
	assert(idle.phase() == "failed");
	assert(idle.canonical_state().find("\"owner\":null") != std::string::npos);
	idle_platform.neutral = true;
	assert(idle.Execute(IdleRequest("recover", NumericOperationId(341).c_str(), idle.sequence(), "ambiguous")) ==
		fogcast::ErrorClass::ok);
	assert(idle.phase() == "idle");
}

void TestReplacementAndFailureCrashMatrix() {
	const char* const handoff_phases[] = {"intent", "releasing", "no_owner", "transferred"};
	for (size_t i = 0; i < sizeof(handoff_phases) / sizeof(handoff_phases[0]); ++i) {
		char directory[] = "/tmp/fogcast-coordinator-replace-crash-XXXXXX";
		assert(mkdtemp(directory) != 0);
		fogcast::BackendFence fence(directory);
		assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
		FakePlatform platform;
		FakeCrash crash;
		fogcast::Coordinator handoff(directory, &fence, &platform, &crash);
		assert(handoff.Initialize(7) == fogcast::ErrorClass::ok);
		assert(handoff.Execute(IdleLaunch(handoff.sequence(), NumericOperationId(350 + i).c_str())) ==
			fogcast::ErrorClass::ok);
		crash.phase = handoff_phases[i];
		platform.actions.clear();
		const std::string replacement = ActiveLaunchSecond(handoff.sequence());
		const fogcast::ErrorClass handoff_result = handoff.Execute(replacement);
		if (handoff_result != fogcast::ErrorClass::transition)
			fprintf(stderr, "replacement crash %s returned %d request %s\n", handoff_phases[i], static_cast<int>(handoff_result), replacement.c_str());
		assert(handoff_result == fogcast::ErrorClass::transition);
		AssertContains(crash.commits, handoff_phases[i]);
		if (std::string(handoff_phases[i]) == "intent" || std::string(handoff_phases[i]) == "releasing")
			assert(platform.actions.empty());
		else {
			if (platform.actions.size() != 4)
				fprintf(stderr, "replacement crash %s action count %zu\n", handoff_phases[i], platform.actions.size());
			AssertLiveCleanup(platform.actions);
		}
		platform.actions.clear();
		platform.LoseHandle();
		fogcast::Coordinator restarted(directory, &fence, &platform);
		assert(restarted.Initialize(7) == fogcast::ErrorClass::ok);
		assert(restarted.phase() == "idle");
		if (std::string(handoff_phases[i]) == "no_owner")
			assert((platform.actions == std::vector<std::string>{"neutral"}));
		else
				assert((platform.actions == std::vector<std::string>{"reset", "neutral"}));
		AssertExclusiveOwnerShape(restarted);
	}

	const char* const failure_phases[] = {"unwinding", "failed"};
	for (size_t i = 0; i < sizeof(failure_phases) / sizeof(failure_phases[0]); ++i) {
		char directory[] = "/tmp/fogcast-coordinator-failure-crash-XXXXXX";
		assert(mkdtemp(directory) != 0);
		fogcast::BackendFence fence(directory);
		assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
		FakePlatform platform;
		platform.fail_create = true;
		platform.fail_reset = std::string(failure_phases[i]) == "failed";
		FakeCrash crash;
		crash.phase = failure_phases[i];
		fogcast::Coordinator coordinator(directory, &fence, &platform, &crash);
		assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
		assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), NumericOperationId(370 + i).c_str())) ==
			fogcast::ErrorClass::transition);
		AssertContains(crash.commits, failure_phases[i]);
		platform.fail_reset = false;
		platform.actions.clear();
		platform.LoseHandle();
		fogcast::Coordinator restarted(directory, &fence, &platform);
		assert(restarted.Initialize(7) == fogcast::ErrorClass::ok);
		if (std::string(failure_phases[i]) == "unwinding") {
			assert(restarted.phase() == "idle");
				assert((platform.actions == std::vector<std::string>{"reset", "neutral"}));
		} else {
			assert(restarted.phase() == "failed");
			assert(platform.actions.empty());
		}
	}
}

void TestStableAdmissionTableAndShutdownLatch() {
	// A failed record that retains its candidate owner accepts recovery only.
	// The otherwise exact stop request must be rejected before a recorded commit.
	char failed_directory[] = "/tmp/fogcast-coordinator-admission-failed-XXXXXX";
	assert(mkdtemp(failed_directory) != 0);
	fogcast::BackendFence failed_fence(failed_directory);
	assert(failed_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform failed_platform;
	failed_platform.fail_create = true;
	failed_platform.fail_reset = true;
	fogcast::Coordinator failed(failed_directory, &failed_fence, &failed_platform);
	assert(failed.Initialize(7) == fogcast::ErrorClass::ok);
	assert(failed.Execute(IdleLaunch(failed.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa6")) == fogcast::ErrorClass::ok);
	assert(failed.phase() == "failed");
	const std::string failed_before = failed.canonical_state();
	failed_platform.actions.clear();
	assert(failed.Execute(OwnerRequest("stop", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7", failed.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::transition);
	assert(failed.canonical_state() == failed_before && failed_platform.actions.empty());

	// A completed shutdown latches the coordinator and refuses every subsequent
	// mutation, including otherwise legal idle recovery.
	char shutdown_directory[] = "/tmp/fogcast-coordinator-shutdown-latch-XXXXXX";
	assert(mkdtemp(shutdown_directory) != 0);
	fogcast::BackendFence shutdown_fence(shutdown_directory);
	assert(shutdown_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform shutdown_platform;
	fogcast::Coordinator shutdown(shutdown_directory, &shutdown_fence, &shutdown_platform);
	assert(shutdown.Initialize(7) == fogcast::ErrorClass::ok);
	assert(shutdown.Execute(IdleRequest("shutdown", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa8",
		shutdown.sequence())) == fogcast::ErrorClass::ok);
	const std::string shutdown_before = shutdown.canonical_state();
	shutdown_platform.actions.clear();
	assert(shutdown.Execute(IdleRequest("recover", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa9",
		shutdown.sequence(), "ambiguous")) == fogcast::ErrorClass::transition);
	assert(shutdown.canonical_state() == shutdown_before && shutdown_platform.actions.empty());
}

// Break caught: lifecycle outcomes, live-handle ownership, and the monotonic
// source of deadline budgets are protocol-visible safety state.  A bool seam
// cannot distinguish a retryable cleanup from EXIT_REQUIRED or a deadline.
void TestTypedLifecycleContractExists() {
	fogcast::LifecycleOwner owner;
	owner.session = "0123456789abcdef0123456789abcdef";
	owner.generation = 42;
	owner.present = true;
	assert(owner.present);
	fogcast::LaunchMetadata launch;
	launch.game_id = "exact-game";
	launch.system = "exact-system";
	launch.expected_core = "ExactCore";
	launch.sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	launch.size = 99;
	launch.extension = "exact";
	assert(fogcast::LifecycleResult::exit_required != fogcast::LifecycleResult::cleanup_incomplete);
	assert(fogcast::LiveHandleState::live != fogcast::LiveHandleState::none);
}

void TestCleanupDeadlineBudgetSelection() {
	const uint32_t short_resources = MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_CONTENT;
	const uint32_t long_resources = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_CORE_PROTOCOL;
	assert(fogcast::CleanupDeadlineBudget(0, 30000, 1000, 1500) == 0);
	assert(fogcast::CleanupDeadlineBudget(short_resources, 30000, 1000, 1500) == 1500);
	assert(fogcast::CleanupDeadlineBudget(long_resources, 30000, 1000, 1500) == 4500);
	assert(fogcast::CleanupDeadlineBudget(short_resources | long_resources,
		30000, 1000, 1500) == 1500);
	assert(fogcast::CleanupDeadlineBudget(long_resources, 4000, 1000, 1500) == 2500);
	assert(fogcast::CleanupDeadlineBudget(short_resources, 30000, 1000, 3000) == 0);
}

// Break caught: observing a deadline is a runtime fact, not a generic
// platform failure.  The coordinator needs the raw result, runtime state, and
// observation payload to choose cleanup without rewriting the primary cause.
void TestLifecycleResultRetainsObserveDeadline() {
	AbiV2Fake fake;
	MisterPlatformV2 platform = fake.Platform();
	fogcast::AbiV2LifecyclePlatform lifecycle(&platform);
	fogcast::LifecycleOwner owner;
	owner.present = true;
	owner.session = "0123456789abcdef0123456789abcdef";
	owner.generation = 42;
	assert(lifecycle.GrantTransfer(owner) == fogcast::LifecycleResult::ok);
	assert(lifecycle.Create(owner) == fogcast::LifecycleResult::ok);
	fake.observe_result = MISTER_RESULT_DEADLINE;
	const fogcast::LifecycleResult observed = lifecycle.Observe("MegaDrive", 123);
	assert(observed.call_result == MISTER_RESULT_DEADLINE);
	assert(observed.primary_result == MISTER_RESULT_DEADLINE);
	assert(observed.cleanup_result == MISTER_RESULT_OK);
	assert(observed.runtime_state == MISTER_STATE_CREATED);
	assert(!observed.ready && observed.active_mask == 0 && observed.observed_core.empty());
	assert(lifecycle.Stop(123) == fogcast::LifecycleResult::ok);
	assert(lifecycle.Destroy() == fogcast::LifecycleResult::ok);
}

// Cleanup failures are evidence about teardown only.  They must never rewrite
// the launch failure that caused teardown, and Destroy is legal only after the
// ABI has reported STOPPED.
void TestAbiCleanupResultsAndRuntimeStateStaySeparated() {
	AbiV2Fake fake;
	MisterPlatformV2 platform = fake.Platform();
	fogcast::AbiV2LifecyclePlatform lifecycle(&platform);
	fogcast::LifecycleOwner owner;
	owner.present = true; owner.session = "0123456789abcdef0123456789abcdef"; owner.generation = 42;
	fogcast::LaunchMetadata launch;
	launch.game_id = "synthetic-game"; launch.system = "megadrive"; launch.expected_core = "MegaDrive";
	launch.sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	launch.size = 1; launch.extension = "md";
	assert(lifecycle.GrantTransfer(owner) == fogcast::LifecycleResult::ok);
	assert(lifecycle.Create(owner) == fogcast::LifecycleResult::ok);
	assert(lifecycle.Start(99).runtime_state == MISTER_STATE_READY);
	assert(lifecycle.Load(launch, 98).runtime_state == MISTER_STATE_RUNNING);
	assert(lifecycle.Observe("MegaDrive", 97).runtime_state == MISTER_STATE_RUNNING);
	fake.observe_result = MISTER_RESULT_DEADLINE;
	const fogcast::LifecycleResult neutral = lifecycle.ObserveNeutral(MISTER_RESOURCE_FPGA, 96);
	assert(neutral.call_result == MISTER_RESULT_DEADLINE);
	assert(neutral.primary_result == MISTER_RESULT_OK);
	assert(neutral.cleanup_result == MISTER_RESULT_DEADLINE);
	assert(lifecycle.Destroy().call_result == MISTER_RESULT_INVALID_STATE);
	assert(lifecycle.Destroy().primary_result == MISTER_RESULT_OK);
	assert(lifecycle.Stop(95).runtime_state == MISTER_STATE_STOPPED);
	assert(lifecycle.Destroy() == fogcast::LifecycleResult::ok);
}

// Both live and stateless retryable calls can carry useful partial proof.  It
// must be validated before branching on the non-OK result, then the next call
// gets only the long FPGA tail and its remaining absolute budget.
void TestRetryablePartialProofNarrowsOutstandingResources() {
	const uint32_t short_resources = MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_CORE_INPUT | MISTER_RESOURCE_SAVES | MISTER_RESOURCE_CONTENT;
	const uint32_t fpga_resources = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	char directory[] = "/tmp/fogcast-coordinator-partial-live-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakeClock clock;
	FakePlatform platform;
	platform.mutable_now = &clock.now;
	fogcast::Coordinator coordinator(directory, &fence, &platform, 0, &clock);
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaac01")) == fogcast::ErrorClass::ok);
	platform.actions.clear(); platform.deadlines.clear(); platform.neutral_masks.clear();
	platform.incomplete_stops = 1;
	platform.incomplete_neutral = 1;
	platform.reset_neutral_masks.push_back(short_resources);
	platform.advance_stop = 1500;
	assert(coordinator.Execute(OwnerRequest("stop", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaac02", coordinator.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	assert(coordinator.phase() == "idle");
	assert((platform.actions == std::vector<std::string>{"stop", "neutral", "stop", "neutral", "neutral", "destroy"}));
	assert(platform.neutral_masks[0] == MISTER_RESOURCE_V2_KNOWN);
	assert(platform.deadlines[0] == 2000 && platform.deadlines[2] == 3500);
	assert(platform.neutral_masks[1] == fpga_resources && platform.neutral_masks[2] == MISTER_RESOURCE_V2_KNOWN);

	char stateless_directory[] = "/tmp/fogcast-coordinator-partial-stateless-XXXXXX";
	assert(mkdtemp(stateless_directory) != 0);
	fogcast::BackendFence stateless_fence(stateless_directory);
	assert(stateless_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakeClock stateless_clock;
	FakePlatform stateless_platform;
	stateless_platform.mutable_now = &stateless_clock.now;
	fogcast::Coordinator stateless(stateless_directory, &stateless_fence, &stateless_platform, 0, &stateless_clock);
	assert(stateless.Initialize(7) == fogcast::ErrorClass::ok);
	assert(stateless.Execute(IdleLaunch(stateless.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaac03")) == fogcast::ErrorClass::ok);
	stateless_platform.LoseHandle(); stateless_platform.actions.clear(); stateless_platform.deadlines.clear();
	stateless_platform.call_times.clear();
	stateless_platform.reset_neutral_masks.push_back(short_resources);
	stateless_platform.reset_results.push_back(MISTER_RESULT_CLEANUP_INCOMPLETE);
	stateless_platform.advance_reset = 1500;
	assert(stateless.Execute(OwnerRequest("recover", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaac04", stateless.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	assert((stateless_platform.actions == std::vector<std::string>{"reset", "reset", "neutral"}));
	// The final whole-mask reassertion retains the last nonzero long-tail
	// deadline.  It receives 2s at t=3s, never the 27s global remainder that
	// ForMask(0) would manufacture.
	assert((stateless_platform.deadlines == std::vector<uint32_t>{2000, 3500, 2000}));
	assert((stateless_platform.call_times == std::vector<uint64_t>{0, 1500, 3000}));
}

void TestRetryableFullLiveProofCannotUseZeroMaskDeadline() {
	char directory[] = "/tmp/fogcast-coordinator-full-live-retry-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakeClock clock;
	FakePlatform platform;
	platform.mutable_now = &clock.now;
	fogcast::Coordinator coordinator(directory, &fence, &platform, 0, &clock);
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaac05")) == fogcast::ErrorClass::ok);
	platform.actions.clear(); platform.deadlines.clear(); platform.neutral_masks.clear();
	platform.call_times.clear();
	platform.incomplete_neutral = 1;
	platform.advance_stop = 500;
	platform.advance_neutral = 1000;
	assert(coordinator.Execute(OwnerRequest("stop", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaac06", coordinator.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	// The retryable observation has already made outstanding zero at t=1.5s.
	// Its retry keeps the original nonzero all-resource deadline, so the second
	// Stop gets only 500ms and expiry prevents any mask-zero Observe or destroy.
	assert((platform.actions == std::vector<std::string>{"stop", "neutral", "stop"}));
	assert((platform.deadlines == std::vector<uint32_t>{2000, 1500, 500}));
	assert((platform.call_times == std::vector<uint64_t>{0, 500, 1500}));
	assert((platform.neutral_masks == std::vector<uint32_t>{MISTER_RESOURCE_V2_KNOWN}));
	assert(coordinator.phase() == "failed");
}

// Break caught: a durable owner does not authorize a live-handle operation.
// A mismatched handle must retain the failed owner without Stop, Observe,
// Destroy, or a forbidden stateless reset against the live handle.
void TestMismatchedLiveOwnerHasNoCleanupSideEffects() {
	char directory[] = "/tmp/fogcast-coordinator-mismatch-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform platform;
	fogcast::Coordinator coordinator(directory, &fence, &platform);
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), "abababababababababababababababaf")) == fogcast::ErrorClass::ok);
	platform.live_owner.session = "ffffffffffffffffffffffffffffffff";
	platform.actions.clear();
	assert(coordinator.Execute(OwnerRequest("stop", "abababababababababababababababb0", coordinator.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	assert(coordinator.phase() == "failed");
	assert(platform.actions.empty());
	assert(platform.stateless_attempt_while_live == 0);
}

size_t CountAction(const std::vector<std::string>& actions, const char* expected) {
	size_t count = 0;
	for (size_t i = 0; i < actions.size(); ++i) if (actions[i] == expected) ++count;
	return count;
}

// These use a single fake monotonic source.  They deliberately distinguish a
// cleanup deadline captured once at cleanup entry from a cap freshly applied
// to every resource bit or retry.
void TestAbsoluteCleanupDeadlinesAndRetries() {
	char directory[] = "/tmp/fogcast-coordinator-absolute-deadlines-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakeClock clock;
	FakePlatform platform;
	platform.mutable_now = &clock.now;
	fogcast::Coordinator coordinator(directory, &fence, &platform, 0, &clock);
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab01")) == fogcast::ErrorClass::ok);

	platform.actions.clear(); platform.deadlines.clear(); platform.neutral_masks.clear(); platform.call_times.clear();
	platform.advance_stop = 1500;
	assert(coordinator.Execute(OwnerRequest("stop", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab02", coordinator.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	// The earliest outstanding group is input/video/audio/saves/content.  Stop
	// and its first proof therefore share that fixed 2s group deadline; no
	// individual call may regain a new 2s window at 4.5/6/7.5/9 seconds.
	assert(!platform.deadlines.empty() && platform.deadlines[0] == 2000);
	const uint32_t short_resources = MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_CORE_INPUT | MISTER_RESOURCE_SAVES | MISTER_RESOURCE_CONTENT;
	for (size_t i = 0; i < platform.neutral_masks.size(); ++i) {
		if ((platform.neutral_masks[i] & short_resources) == 0) continue;
		assert(platform.deadlines[i + 1] <= 500);
		assert(platform.call_times[i + 1] < 2000);
	}

	char retry_directory[] = "/tmp/fogcast-coordinator-retry-XXXXXX";
	assert(mkdtemp(retry_directory) != 0);
	fogcast::BackendFence retry_fence(retry_directory);
	assert(retry_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform retry_platform;
	fogcast::Coordinator retry(retry_directory, &retry_fence, &retry_platform);
	assert(retry.Initialize(7) == fogcast::ErrorClass::ok);
	assert(retry.Execute(IdleLaunch(retry.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab03")) == fogcast::ErrorClass::ok);
	retry_platform.actions.clear(); retry_platform.incomplete_stops = 1;
	assert(retry.Execute(OwnerRequest("stop", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab04", retry.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	assert(CountAction(retry_platform.actions, "stop") == 2);
	assert(retry.phase() == "idle");

	char stateless_directory[] = "/tmp/fogcast-coordinator-stateless-retry-XXXXXX";
	assert(mkdtemp(stateless_directory) != 0);
	fogcast::BackendFence stateless_fence(stateless_directory);
	assert(stateless_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform stateless_platform;
	fogcast::Coordinator stateless(stateless_directory, &stateless_fence, &stateless_platform);
	assert(stateless.Initialize(7) == fogcast::ErrorClass::ok);
	assert(stateless.Execute(IdleLaunch(stateless.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab05")) == fogcast::ErrorClass::ok);
	stateless_platform.LoseHandle();
	stateless_platform.actions.clear();
	stateless_platform.reset_neutral_masks.push_back(MISTER_RESOURCE_FPGA);
	assert(stateless.Execute(OwnerRequest("recover", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab06", stateless.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	assert((stateless_platform.actions == std::vector<std::string>{"reset", "reset", "neutral"}));
	assert(stateless.phase() == "idle");

	char nonmonotonic_directory[] = "/tmp/fogcast-coordinator-nonmonotonic-XXXXXX";
	assert(mkdtemp(nonmonotonic_directory) != 0);
	fogcast::BackendFence nonmonotonic_fence(nonmonotonic_directory);
	assert(nonmonotonic_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform nonmonotonic_platform;
	fogcast::Coordinator nonmonotonic(nonmonotonic_directory, &nonmonotonic_fence, &nonmonotonic_platform);
	assert(nonmonotonic.Initialize(7) == fogcast::ErrorClass::ok);
	assert(nonmonotonic.Execute(IdleLaunch(nonmonotonic.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab07")) == fogcast::ErrorClass::ok);
	nonmonotonic_platform.LoseHandle();
	nonmonotonic_platform.reset_neutral_masks.push_back(MISTER_RESOURCE_FPGA);
	nonmonotonic_platform.reset_observed_or = MISTER_RESOURCE_FPGA;
	assert(nonmonotonic.Execute(OwnerRequest("recover", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab08", nonmonotonic.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	assert(nonmonotonic.phase() == "failed");
}

void TestRecordedDeadlineIsTheLaunchDeadline() {
	char directory[] = "/tmp/fogcast-coordinator-recorded-deadline-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakeClock clock;
	FakePlatform platform;
	platform.mutable_now = &clock.now;
	FakeCrash recorded_delay;
	recorded_delay.mutable_now = &clock.now;
	recorded_delay.advance_recorded = 16000;
	fogcast::Coordinator coordinator(directory, &fence, &platform, &recorded_delay, &clock);
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	platform.actions.clear(); platform.deadlines.clear(); platform.call_times.clear();
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab09")) == fogcast::ErrorClass::ok);
	assert(platform.actions.empty());
	assert(coordinator.phase() == "failed");
	const fogcast::Coordinator::OperationStatus status = coordinator.operation_status("aaaaaaaaaaaaaaaaaaaaaaaaaaaaab09");
	assert(status.kind == fogcast::Coordinator::OperationStatus::completed && !status.ok && status.error == "DEADLINE");
}

// A recorded overlay in the failed phase is a crash boundary.  It preserves
// the failed snapshot's forensic fields instead of zeroing last_error while
// the operation is merely in flight.
void TestFailedRecordedAdmissionPreservesSnapshot() {
	char directory[] = "/tmp/fogcast-coordinator-failed-recorded-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakePlatform platform;
	FakeCrash crash;
	fogcast::Coordinator coordinator(directory, &fence, &platform, &crash);
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaad01")) == fogcast::ErrorClass::ok);
	platform.fail_stop = true;
	assert(coordinator.Execute(OwnerRequest("stop", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaad02", coordinator.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	assert(coordinator.phase() == "failed");
	const std::string failed = coordinator.canonical_state();
	const char* const fields[] = {"owner", "release_owner", "candidate", "leases", "content_lease", "observed_core", "last_error"};
	std::vector<std::string> before;
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) before.push_back(StateField(failed, fields[i]));
	crash.phase = "recorded";
	assert(coordinator.Admit(OwnerRequest("recover", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaad03", coordinator.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::transition);
	const std::string recorded = coordinator.canonical_state();
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
		assert(StateField(recorded, fields[i]) == before[i]);
	assert(StateField(recorded, "in_flight").find("aaaaaaaaaaaaaaaaaaaaaaaaaaaaad03") != std::string::npos);
}

void TestLateStopCannotReopenShortGroupDeadline() {
	char directory[] = "/tmp/fogcast-coordinator-late-stop-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) == fogcast::ErrorClass::ok);
	FakeClock clock;
	FakePlatform platform;
	platform.mutable_now = &clock.now;
	fogcast::Coordinator coordinator(directory, &fence, &platform, 0, &clock);
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab10")) == fogcast::ErrorClass::ok);
	platform.actions.clear(); platform.call_times.clear(); platform.advance_stop = 4500;
	assert(coordinator.Execute(OwnerRequest("stop", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab11", coordinator.sequence(),
		"0123456789abcdef0123456789abcdef", 42)) == fogcast::ErrorClass::ok);
	assert((platform.actions == std::vector<std::string>{"stop"}));
	assert((platform.call_times == std::vector<uint64_t>{0}));
	assert(coordinator.phase() == "failed");
}

// These source-level tripwires complement the behavioral fakes above.  Each
// names the exact safety decision that a one-line removal would otherwise make
// easy to miss while leaving broad lifecycle coverage apparently green.
void TestMandatoryCoordinatorSourceGuards() {
	const std::string source = ReadBytes("fogcast/runtime_coordinator.cpp");
	assert(source.find("operation_deadline_ = clock_->NowMs() + budget") != std::string::npos);
	assert(source.find("retained_deadline_mask") != std::string::npos);
	assert(source.find("RetryableCleanup(result)") != std::string::npos);
	assert(source.find("SameOwner(candidate, owner_)") != std::string::npos);
	assert(source.find("handle == LiveHandleState::live") != std::string::npos);
	assert(source.find("handle == LiveHandleState::none") != std::string::npos);
	assert(source.find("result.observed_mask & proven") != std::string::npos);
	assert(source.find("observed.observed_mask & proven") != std::string::npos);
	assert(source.find("platform_->ObserveNeutral(observation_mask") != std::string::npos);
	assert(source.find("FinishRuntimeCall(MisterRuntime_ObserveV2(runtime_, &observation, deadline_ms), false, true)") != std::string::npos);
	assert(source.find("runtime_state_ != MISTER_STATE_STOPPED") != std::string::npos);
	assert(source.find("retained_error") != std::string::npos);
	assert(source.find("PrimaryError(activation)") != std::string::npos);
	assert(source.find("Commit(\"failed\", candidate, candidate, candidate") != std::string::npos);
}

}  // namespace

int main()
{
	TestTypedLifecycleContractExists();
	TestCleanupDeadlineBudgetSelection();
	TestLifecycleResultRetainsObserveDeadline();
	TestAbiCleanupResultsAndRuntimeStateStaySeparated();
	TestRetryablePartialProofNarrowsOutstandingResources();
	TestRetryableFullLiveProofCannotUseZeroMaskDeadline();
	TestMismatchedLiveOwnerHasNoCleanupSideEffects();
	TestAbsoluteCleanupDeadlinesAndRetries();
	TestRecordedDeadlineIsTheLaunchDeadline();
	TestFailedRecordedAdmissionPreservesSnapshot();
	TestLateStopCannotReopenShortGroupDeadline();
	TestMandatoryCoordinatorSourceGuards();
	TestInitializeStoreTriage();
	TestFenceQuiescingAndMetadataAdoption();
	TestAbiRecoveryRejectsIncompleteAndInvalidObservation();
	TestExactMetadataAndDeadlineBudgets();
	// Break caught: the production seam must drive ABI-v2 create/start/load/
	// observe/stop/destroy/recover in order, with the caller's exact deadline.
	AbiV2Fake abi_fake;
	MisterPlatformV2 abi_platform = abi_fake.Platform();
	fogcast::AbiV2LifecyclePlatform abi(&abi_platform);
	fogcast::LifecycleOwner abi_owner;
	abi_owner.present = true; abi_owner.session = "0123456789abcdef0123456789abcdef"; abi_owner.generation = 42;
	fogcast::LaunchMetadata abi_launch;
	abi_launch.game_id = "synthetic-game"; abi_launch.system = "megadrive";
	abi_launch.expected_core = "MegaDrive";
	abi_launch.sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	abi_launch.size = 1048576; abi_launch.extension = "md";
	assert(abi.GrantTransfer(abi_owner) == fogcast::LifecycleResult::ok);
	assert(abi.Create(abi_owner) == fogcast::LifecycleResult::ok);
	assert(abi.Start(321) == fogcast::LifecycleResult::ok);
	assert(abi.Load(abi_launch, 321) == fogcast::LifecycleResult::ok);
	assert(abi.Observe("MegaDrive", 321) == fogcast::LifecycleResult::ok);
	assert(abi.Stop(321) == fogcast::LifecycleResult::ok);
	assert(abi.Destroy() == fogcast::LifecycleResult::ok);
	assert(abi.ProveNeutral(MISTER_RESOURCE_V2_KNOWN, 321) == fogcast::LifecycleResult::ok);
	assert((abi_fake.actions == std::vector<std::string>{"start", "load", "observe", "observe", "stop", "recover"}));
	assert((abi_fake.deadlines == std::vector<uint32_t>{321, 321, 321, 321, 321, 321}));
	char directory[] = "/tmp/fogcast-coordinator-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	assert(fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) ==
		fogcast::ErrorClass::ok);
	FakePlatform platform;
	fogcast::Coordinator coordinator(directory, &fence, &platform);
	assert(!coordinator.ready());
	assert(coordinator.Initialize(7) == fogcast::ErrorClass::ok);
	assert(coordinator.ready());
	assert(coordinator.sequence() == 1);
	platform.actions.clear();
	// Break caught: without a public retained-operation query an ambiguous
	// client result cannot distinguish an in-flight request from an unknown ID.
	fogcast::Coordinator::OperationStatus initial_status =
		coordinator.operation_status("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	assert(initial_status.kind == fogcast::Coordinator::OperationStatus::unknown);
	assert(initial_status.request_digest.empty());
	// Break caught: omitting the recorded-before-side-effect lifecycle would
	// leave an idle launch without a durable owner handoff or observation.
	assert(coordinator.Execute(kLaunchOne) == fogcast::ErrorClass::ok);
	assert((platform.actions == std::vector<std::string>{"neutral", "grant", "create", "start", "load", "observe"}));
	assert(coordinator.phase() == "active");
	// Break caught: a restarted daemon must not re-attach or resume an active
	// ABI handle.  It must statelessly reset the recorded owner, preserve its
	// historical successful terminal, and expose a new interrupted idle state.
	platform.actions.clear();
	platform.LoseHandle();
	const std::string ledger_before_terminal_restart = LedgerBytes(coordinator.canonical_state());
	fogcast::Coordinator restarted(directory, &fence, &platform);
	assert(restarted.Initialize(7) == fogcast::ErrorClass::ok);
	assert((platform.actions == std::vector<std::string>{"reset", "neutral"}));
	assert(restarted.phase() == "idle");
	assert(restarted.canonical_state().find("00000000000000000000000000000000") == std::string::npos);
	assert(LedgerBytes(restarted.canonical_state()) == ledger_before_terminal_restart);
	fogcast::Coordinator::OperationStatus retained = restarted.operation_status(
		"fedcba9876543210fedcba9876543210");
	assert(retained.kind == fogcast::Coordinator::OperationStatus::completed && retained.ok &&
		retained.request_digest == WireDigest(kLaunchOne));
	// Continue the normal lifecycle with the replay-safe current coordinator.
	coordinator = restarted;
	const uint64_t recovered_sequence = coordinator.sequence();
	platform.actions.clear();
	assert(coordinator.Execute(IdleLaunchSecond(recovered_sequence)) == fogcast::ErrorClass::ok);
	assert((platform.actions == std::vector<std::string>{"neutral", "grant", "create", "start", "load", "observe"}));
	assert(coordinator.phase() == "active");
	// Sequence and owner CAS checks must independently reject before recording
	// an operation or invoking the lifecycle platform.
	const std::string cas_before = coordinator.canonical_state();
	platform.actions.clear();
	assert(coordinator.Execute(ReleaseRequest("stop", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa3",
		coordinator.sequence() - 1)) == fogcast::ErrorClass::transition);
	assert(coordinator.canonical_state() == cas_before && platform.actions.empty());
	std::string wrong_owner = ReleaseRequest("stop", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa4",
		coordinator.sequence());
	for (size_t owner = wrong_owner.find("22222222222222222222222222222222");
		owner != std::string::npos;
		owner = wrong_owner.find("22222222222222222222222222222222", owner + 32))
		wrong_owner.replace(owner, 32, "33333333333333333333333333333333");
	assert(coordinator.Execute(wrong_owner) == fogcast::ErrorClass::transition);
	assert(coordinator.canonical_state() == cas_before && platform.actions.empty());
	// A candidate may never reuse the current generation, even for a different
	// operation ID and otherwise exact precondition.
	assert(coordinator.Execute(ActiveLaunchSameOwner(coordinator.sequence(),
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa5")) == fogcast::ErrorClass::transition);
	assert(coordinator.canonical_state() == cas_before && platform.actions.empty());
	// An identical completed operation is retained without replaying hardware.
	const std::string before = coordinator.canonical_state();
	platform.actions.clear();
	assert(coordinator.Execute(kLaunchOne) == fogcast::ErrorClass::ok);
	assert(coordinator.canonical_state() == before);
	assert(platform.actions.empty());
	// A new operation with a stale CAS is rejected before any durable mutation
	// or platform action.
	std::string stale(kLaunchOne);
	stale.replace(stale.find("fedcba9876543210fedcba9876543210"), 32,
		"33333333333333333333333333333333");
	assert(coordinator.Execute(stale) == fogcast::ErrorClass::transition);
	assert(coordinator.canonical_state() == before);
	assert(platform.actions.empty());
	// A normal stop retains the release owner through neutral observation, then
	// publishes idle only after destruction.
	platform.actions.clear();
	assert(coordinator.Execute(ReleaseRequest("stop", "44444444444444444444444444444444",
		coordinator.sequence())) == fogcast::ErrorClass::ok);
	AssertLiveCleanup(platform.actions);
	assert(coordinator.phase() == "idle");
	// Break caught: retained terminal results must be queryable by operation ID,
	// while the 65th completed operation atomically evicts only the FIFO oldest.
	char replay_directory[] = "/tmp/fogcast-coordinator-replay-XXXXXX";
	assert(mkdtemp(replay_directory) != 0);
	fogcast::BackendFence replay_fence(replay_directory);
	assert(replay_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) ==
		fogcast::ErrorClass::ok);
	FakePlatform replay_platform;
	fogcast::Coordinator replay(replay_directory, &replay_fence, &replay_platform);
	assert(replay.Initialize(7) == fogcast::ErrorClass::ok);
	std::string first_id;
	std::string second_id;
	for (unsigned int i = 1; i <= 65; ++i) {
		const std::string id = NumericOperationId(i);
		if (i == 1) first_id = id;
		if (i == 2) second_id = id;
		assert(replay.Execute(IdleRequest("recover", id.c_str(), replay.sequence(), "ambiguous")) ==
			fogcast::ErrorClass::ok);
	}
	assert(replay.operation_status(first_id).kind == fogcast::Coordinator::OperationStatus::unknown);
	assert(replay.operation_status(second_id).kind == fogcast::Coordinator::OperationStatus::completed);
	const std::string replay_state = replay.canonical_state();
	replay_platform.actions.clear();
	assert(replay.Execute(IdleRequest("recover", second_id.c_str(), replay.sequence(), "failed")) ==
		fogcast::ErrorClass::transition);
	assert(replay.canonical_state() == replay_state && replay_platform.actions.empty());
	// Recorded is a durable in-flight operation, not a lifecycle action.  A
	// restart terminalizes it in idle without executing the requested launch.
	char recorded_directory[] = "/tmp/fogcast-coordinator-recorded-XXXXXX";
	assert(mkdtemp(recorded_directory) != 0);
	fogcast::BackendFence recorded_fence(recorded_directory);
	assert(recorded_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) ==
		fogcast::ErrorClass::ok);
	FakePlatform recorded_platform;
	fogcast::Coordinator recorded(recorded_directory, &recorded_fence, &recorded_platform);
	assert(recorded.Initialize(7) == fogcast::ErrorClass::ok);
	assert(recorded.Admit(kLaunchOne) == fogcast::ErrorClass::ok);
	assert(recorded.operation_status("fedcba9876543210fedcba9876543210").kind ==
		fogcast::Coordinator::OperationStatus::in_flight);
	assert(recorded.operation_status("fedcba9876543210fedcba9876543210").request_digest ==
		WireDigest(kLaunchOne));
	recorded_platform.actions.clear();
	recorded_platform.LoseHandle();
	fogcast::Coordinator recorded_restart(recorded_directory, &recorded_fence, &recorded_platform);
	assert(recorded_restart.Initialize(7) == fogcast::ErrorClass::ok);
	assert((recorded_platform.actions == std::vector<std::string>{"neutral"}));
	fogcast::Coordinator::OperationStatus interrupted = recorded_restart.operation_status(
		"fedcba9876543210fedcba9876543210");
	assert(interrupted.kind == fogcast::Coordinator::OperationStatus::completed &&
		!interrupted.ok && interrupted.error == "INTERRUPTED");
	// Break caught: every durable phase is an immediate crash boundary; no
	// lifecycle callback may run after the recorded commit if the daemon dies.
	char crash_directory[] = "/tmp/fogcast-coordinator-crash-XXXXXX";
	assert(mkdtemp(crash_directory) != 0);
	fogcast::BackendFence crash_fence(crash_directory);
	assert(crash_fence.Commit(fogcast::BackendFence::Record(1, "native", 7)) ==
		fogcast::ErrorClass::ok);
	FakePlatform crash_platform;
	FakeCrash crash;
	crash.phase = "recorded";
	fogcast::Coordinator crashing(crash_directory, &crash_fence, &crash_platform, &crash);
	assert(crashing.Initialize(7) == fogcast::ErrorClass::ok);
	crash_platform.actions.clear();
	assert(crashing.Execute(kLaunchOne) == fogcast::ErrorClass::transition);
	assert(crash_platform.actions.empty());
	crash_platform.LoseHandle();
	fogcast::Coordinator crash_restart(crash_directory, &crash_fence, &crash_platform);
	assert(crash_restart.Initialize(7) == fogcast::ErrorClass::ok);
	assert(crash_restart.phase() == "idle");
	// Owner-null recovery and idle shutdown independently prove the full mask is
	// neutral; neither can activate a candidate.
	platform.actions.clear();
	assert(coordinator.Execute(IdleRequest("recover", "55555555555555555555555555555555",
		coordinator.sequence(), "ambiguous")) == fogcast::ErrorClass::ok);
	assert((platform.actions == std::vector<std::string>{"reset", "neutral"}));
	assert(coordinator.phase() == "idle");
	// Every activation failure is unwound only after transfer and reset back to
	// the durable no-owner checkpoint.
	platform.fail_create = true;
	platform.actions.clear();
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(),
		"77777777777777777777777777777777")) == fogcast::ErrorClass::ok);
	assert((platform.actions == std::vector<std::string>{"neutral", "grant", "create", "reset", "neutral"}));
	assert(coordinator.phase() == "idle");
	platform.fail_create = false;
	// Quiescing permits only drain/reconciliation operations; it never grants a
	// replacement candidate even when the state CAS is otherwise exact.
	assert(fence.Commit(fogcast::BackendFence::Record(2, "native_quiescing", 7)) ==
		fogcast::ErrorClass::ok);
	platform.actions.clear();
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(),
		"99999999999999999999999999999999")) == fogcast::ErrorClass::transition);
	assert(platform.actions.empty());
	platform.actions.clear();
	assert(coordinator.Execute(IdleRequest("shutdown", "66666666666666666666666666666666",
		coordinator.sequence())) == fogcast::ErrorClass::ok);
	assert((platform.actions == std::vector<std::string>{"neutral"}));
	assert(coordinator.phase() == "idle");
	// A quiesced coordinator refuses subsequent candidate grants without a
	// second platform action.
	platform.actions.clear();
	assert(coordinator.Execute(IdleLaunch(coordinator.sequence(),
		"88888888888888888888888888888888")) == fogcast::ErrorClass::transition);
	assert(platform.actions.empty());
	TestLaunchCrashMatrix();
	TestActivationFailureMatrix();
	TestRecoverAndReleaseFailureMatrix();
	TestReplacementAndFailureCrashMatrix();
	TestStableAdmissionTableAndShutdownLatch();
	return 0;
}
