// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_CORE_PROTOCOL_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_CORE_PROTOCOL_HPP

#include "runtime/mister_runtime.h"
#include "runtime/native/native_clock.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_hardware_io.hpp"

#include <stddef.h>
#include <stdint.h>

#include <memory>

namespace mister {
namespace native {

struct ProtocolSessionState;

class ActiveCoreProtocolSession final {
public:
	~ActiveCoreProtocolSession();
	ActiveCoreProtocolSession(const ActiveCoreProtocolSession &) = delete;
	ActiveCoreProtocolSession &operator=(const ActiveCoreProtocolSession &) = delete;
private:
	friend class NativeCoreProtocol;
	friend class HardwareBroker;
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

class NativeCoreProtocolIo {
public:
	virtual ~NativeCoreProtocolIo() {}
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

class NativeCoreProtocol final {
public:
	NativeCoreProtocol(NativeClock &clock, NativeCoreProtocolIo &io);
	MisterResult ActivateFixtureForTest(const NativeCoreProfile &profile,
		NativeCoreProtocolContent &content, uint64_t absolute_deadline_ms);

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
	NativeClock &clock_;
	NativeCoreProtocolIo &io_;
	uint8_t last_status_sequence_;
};

} // namespace native
} // namespace mister

#endif
