// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_CORE_PROTOCOL_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_CORE_PROTOCOL_HPP

#include "runtime/mister_runtime.h"
#include "runtime/native/native_clock.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_hardware_io.hpp"
#include "runtime/native/native_snes_content.hpp"
#include "runtime/native/hardware_broker.hpp"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <atomic>

namespace mister {
namespace native {

namespace linux_native {
class NativeCoreProtocolIoAdapter;
class NativeCoreProtocolIoAdapterCapabilitySet;
}

struct ProtocolSessionState;
struct NativeCoreProtocolOutcome;
class NativeContentResource;
class NativeCoreProtocolTeardown;
enum class RecoveryResourceState : uint8_t;

class ActiveCoreProtocolSession final {
public:
	~ActiveCoreProtocolSession();
	ActiveCoreProtocolSession(const ActiveCoreProtocolSession &) = delete;
	ActiveCoreProtocolSession &operator=(const ActiveCoreProtocolSession &) = delete;
private:
	friend class NativeCoreProtocol;
	friend class HardwareBroker;
	friend class linux_native::NativeCoreProtocolActiveAdapterView;
	ActiveCoreProtocolSession();
	std::shared_ptr<ProtocolSessionState> state_;
};

class ActiveSelectedTransaction final {
public:
	~ActiveSelectedTransaction();
	ActiveSelectedTransaction(const ActiveSelectedTransaction &) = delete;
	ActiveSelectedTransaction &operator=(const ActiveSelectedTransaction &) = delete;
private:
	friend class NativeCoreProtocol;
	ActiveSelectedTransaction();
	std::shared_ptr<ProtocolSessionState> state_;
};

class CleanupCoreProtocolSession final {
public:
	~CleanupCoreProtocolSession();
	CleanupCoreProtocolSession(const CleanupCoreProtocolSession &) = delete;
	CleanupCoreProtocolSession &operator=(const CleanupCoreProtocolSession &) = delete;
private:
	friend class NativeCoreProtocol;
	friend class HardwareBroker;
	friend class linux_native::NativeCoreProtocolCleanupAdapterView;
	CleanupCoreProtocolSession();
	std::shared_ptr<ProtocolSessionState> state_;
};

class RecoveryCoreProtocolSession final {
public:
	~RecoveryCoreProtocolSession();
	RecoveryCoreProtocolSession(const RecoveryCoreProtocolSession &) = delete;
	RecoveryCoreProtocolSession &operator=(const RecoveryCoreProtocolSession &) = delete;
private:
	friend class NativeCoreProtocol;
	friend class HardwareBroker;
	friend class linux_native::NativeCoreProtocolRecoveryAdapterView;
	RecoveryCoreProtocolSession();
	std::shared_ptr<ProtocolSessionState> state_;
};

struct NativeLiveCoreObservation {
	uint32_t identity_magic;
	uint8_t core_type;
	NativeFileIoWidth file_io_width;
	uint8_t fpga_io_version;
};

struct NativeCoreProtocolContentDescription {
	uint64_t size;
	const char *extension;
};

class NativeCoreProtocolContent {
public:
	virtual ~NativeCoreProtocolContent() {}
	virtual MisterResult Describe(NativeCoreProtocolContentDescription *description,
		uint64_t absolute_deadline_ms) = 0;
	virtual MisterResult ReadAt(uint64_t offset, void *bytes, size_t count,
		uint64_t absolute_deadline_ms) = 0;
};

class NativeActiveCoreProtocolIo {
public:
	virtual ~NativeActiveCoreProtocolIo() {}
	virtual bool Available() const;
	virtual MisterResult BeginActive(ActiveCoreProtocolSession &session,
		uint64_t absolute_deadline_ms);
	virtual MisterResult FinishActive(ActiveCoreProtocolSession &session,
		ProtocolMappingReleaseReceipt *receipt);
	virtual MisterResult AbortActive(ActiveCoreProtocolSession &session,
		MisterResult primary_result, CoreProtocolResidue *residue,
		ProtocolMappingReleaseReceipt *receipt);
	virtual MisterResult Probe(NativeLiveCoreObservation *observation,
		uint64_t absolute_deadline_ms) = 0;
	// A selected exchange can explicitly keep the target selected across a
	// response-dependent follow-up. The adapter owns all select/strobe/ACK
	// mechanics and rejects invalid begin/end sequences.
	virtual MisterResult Exchange(NativeSpiTarget target,
		const uint16_t *transmit_words, size_t word_count,
		uint16_t *received_words, size_t received_capacity, bool begins_selected,
		bool ends_selected,
		uint64_t absolute_deadline_ms) = 0;
	virtual MisterResult CloseSelected(NativeSpiTarget target,
		uint64_t absolute_deadline_ms) = 0;
};

class NativeCleanupCoreProtocolIo {
public:
	virtual ~NativeCleanupCoreProtocolIo() {}
	virtual bool Available() const;
	virtual MisterResult BeginCleanup(CleanupCoreProtocolSession &session,
		uint64_t absolute_deadline_ms);
	virtual MisterResult ShutdownCleanup(CleanupCoreProtocolSession &session,
		bool download_may_be_active, ProtocolMappingReleaseReceipt *receipt);
};

class NativeRecoveryCoreProtocolIo {
public:
	virtual ~NativeRecoveryCoreProtocolIo() {}
	virtual bool Available() const;
	virtual MisterResult BeginRecovery(RecoveryCoreProtocolSession &session,
		uint64_t absolute_deadline_ms);
	virtual MisterResult DisableRecovery(RecoveryCoreProtocolSession &session,
		ProtocolMappingReleaseReceipt *receipt);
};

class NativeCoreProtocolExitIo {
public:
	virtual ~NativeCoreProtocolExitIo() {}
	virtual bool Available() const;
	virtual void CloseForProcessExit();
};

// An opaque single-consumer token for the one state bundle which owns all four
// typed protocol views. It is intentionally not convertible to a view and
// moves exactly once into one core-protocol owner.
class NativeCoreProtocolCapabilities final {
public:
	NativeCoreProtocolCapabilities();
	NativeCoreProtocolCapabilities(const NativeCoreProtocolCapabilities &) = delete;
	NativeCoreProtocolCapabilities &operator=(
		const NativeCoreProtocolCapabilities &) = delete;
	NativeCoreProtocolCapabilities(NativeCoreProtocolCapabilities &&other) noexcept;
	NativeCoreProtocolCapabilities &operator=(
		NativeCoreProtocolCapabilities &&other) noexcept;
	~NativeCoreProtocolCapabilities();
	bool Available() const;

private:
	class State;
	friend class NativeCoreProtocol;
	friend class linux_native::NativeCoreProtocolIoAdapter;
	friend class linux_native::NativeCoreProtocolIoAdapterCapabilitySet;
	explicit NativeCoreProtocolCapabilities(State *state);
	NativeActiveCoreProtocolIo &active_io() const;
	NativeCleanupCoreProtocolIo &cleanup_io() const;
	NativeRecoveryCoreProtocolIo &recovery_io() const;
	NativeCoreProtocolExitIo &exit_io() const;
	State *state_;
};

class NativeCoreProtocolCapabilities::State {
public:
	State() : references_(0) {}
	virtual ~State() {}

private:
	friend class NativeCoreProtocolCapabilities;
	void Retain() { references_.fetch_add(1, std::memory_order_relaxed); }
	void Release()
	{
		if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1)
			delete this;
	}
	virtual bool Available() const = 0;
	virtual NativeActiveCoreProtocolIo &ActiveIo() = 0;
	virtual NativeCleanupCoreProtocolIo &CleanupIo() = 0;
	virtual NativeRecoveryCoreProtocolIo &RecoveryIo() = 0;
	virtual NativeCoreProtocolExitIo &ExitIo() = 0;
	std::atomic<size_t> references_;
};

class NativeCoreProtocol final {
public:
	// The fixture-only entry point has no broker or teardown authority. It is
	// intentionally absent from production so every production protocol binds
	// active, cleanup, recovery, and exit to one inseparable bundle.
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	NativeCoreProtocol(NativeClock &clock, NativeActiveCoreProtocolIo &active_io);
#endif
	NativeCoreProtocol(NativeClock &clock, HardwareBroker &broker,
		NativeCoreProtocolCapabilities &&capabilities);
	~NativeCoreProtocol();
	MisterResult ActivateFixtureForTest(const NativeCoreProfile &profile,
		NativeCoreProtocolContent &content, uint64_t absolute_deadline_ms);
	NativeCoreProtocolOutcome Activate(const OperationLease &active_lease,
		const NativeCoreProfile &profile, NativeContentResource &content);
	bool valid() const;
	Result ShutdownLive(const OperationLease &cleanup_lease);
	Result DisableStateless(const OperationLease &recovery_lease,
		RecoveryResourceState *state);
	void CloseForProcessExit();

private:
	MisterResult Exchange(NativeSpiTarget target,
		const uint16_t *words, size_t count, uint16_t *responses,
		size_t response_capacity, bool begins_selected, bool ends_selected,
		uint64_t absolute_deadline_ms);
	MisterResult SendStatus(const uint8_t status[16],
		uint64_t absolute_deadline_ms);
	MisterResult CloseSelected(NativeSpiTarget target,
		uint64_t absolute_deadline_ms);
	MisterResult ValidateLive(const NativeCoreProfile &profile,
		uint64_t absolute_deadline_ms);
	MisterResult TransferNativeSnesContent(NativeCoreProtocolContent &content,
		const NativeSnesContentPlan &plan, uint64_t absolute_deadline_ms);
	MisterResult TransferRawContent(NativeCoreProtocolContent &content,
		uint64_t size, const NativeCoreProfile &profile,
		uint64_t absolute_deadline_ms);
	NativeClock &clock_;
	HardwareBroker *broker_;
	NativeActiveCoreProtocolIo *active_io_;
	NativeCoreProtocolCapabilities capabilities_;
	std::unique_ptr<NativeCoreProtocolTeardown> teardown_;
	bool capabilities_available_;
	uint8_t last_status_sequence_;
	const NativeCoreProfile *active_profile_;
	CoreProtocolResidue residue_;
};

#if defined(MISTER_NATIVE_PROFILE_TESTING)
// Host-only deterministic construction-failure seam. Production builds do not
// expose or link this test allocator control.
void SetNativeCoreProtocolAllocationFailureForTest(size_t allocation_index);
void ClearNativeCoreProtocolAllocationFailureForTest();
bool NativeCoreProtocolAllocationAllowedForTest();
#endif

} // namespace native
} // namespace mister

#endif
