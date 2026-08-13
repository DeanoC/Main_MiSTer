// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_RESOURCES_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_RESOURCES_HPP

#include "runtime/native/hardware_broker.hpp"

#include <stdint.h>

namespace mister {
namespace native {

struct NativeDigitalNeutral {
	uint8_t player;
	uint16_t words[2];
};

struct NativeAcquisitionOutcome {
	Result result;
	// True means ownership exists, including partial ownership on failure.
	bool acquired;
};

struct NativeCoreProtocolOutcome {
	Result result;
	// True means protocol ownership/mutation exists, including partial state.
	bool acquired;
	// A future broker-atomic failure path consumes the active protocol session
	// before this outcome is returned. The ordinary fixture path remains false.
	bool broker_failure_completed;
};

struct NativeRetainedContentDescription {
	uint64_t size;
	uint8_t extension_length;
	char extension[4];
};

class NativeContentResource;

class NativePreflight {
public:
	virtual ~NativePreflight() {}
	virtual Result Validate(const NativeCoreProfile &profile,
		uint64_t absolute_deadline_ms) = 0;
};

class NativeHardwareResources {
public:
	virtual ~NativeHardwareResources() {}
	virtual NativeAcquisitionOutcome AcquireFpga(
		const OperationLease &lease) = 0;
	virtual NativeAcquisitionOutcome EnableBridges(
		const OperationLease &lease) = 0;
	virtual NativeCoreProtocolOutcome StartCoreProtocol(
		const OperationLease &lease, const NativeCoreProfile &profile,
		NativeContentResource &content) = 0;
	virtual NativeAcquisitionOutcome StartVideo(
		const OperationLease &lease) = 0;
	virtual NativeAcquisitionOutcome StartAudio(
		const OperationLease &lease) = 0;
	virtual Result ReplayDigitalNeutral(const OperationLease &lease,
		const NativeDigitalNeutral &neutral) = 0;
	virtual Result StopVideo(const OperationLease &lease) = 0;
	virtual Result StopAudio(const OperationLease &lease) = 0;
	virtual Result ShutdownCoreProtocol(const OperationLease &lease) = 0;
	virtual Result TerminalFpgaCleanup(const OperationLease &lease) = 0;
	virtual void CloseVideoForProcessExit() = 0;
	virtual void CloseAudioForProcessExit() = 0;
	virtual void CloseCoreProtocolForProcessExit() = 0;
	virtual void CloseFpgaMappingsForProcessExit() = 0;
};

// These process-resource interfaces intentionally have no hardware capability.
class NativeSchedulerResource {
public:
	virtual ~NativeSchedulerResource() {}
	virtual NativeAcquisitionOutcome StartScheduler(
		const OperationLease &lease) = 0;
	virtual Result StopScheduler(const OperationLease &lease) = 0;
	virtual void CloseSchedulerForProcessExit() = 0;
};

class NativeOffloadResource {
public:
	virtual ~NativeOffloadResource() {}
	virtual NativeAcquisitionOutcome StartOffload(
		const OperationLease &lease) = 0;
	virtual Result RejectAndJoinOffload(const OperationLease &lease) = 0;
	virtual void CloseOffloadForProcessExit() = 0;
};

class NativeSaveResource {
public:
	virtual ~NativeSaveResource() {}
	virtual NativeAcquisitionOutcome OpenSave(const OperationLease &lease) = 0;
	virtual Result FlushAndCloseSave(const OperationLease &lease) = 0;
	virtual void CloseSaveForProcessExit() = 0;
};

class NativeContentResource {
public:
	virtual ~NativeContentResource() {}
	virtual NativeAcquisitionOutcome RetainContent(
		uint64_t absolute_deadline_ms) = 0;
	virtual Result DescribeRetained(NativeRetainedContentDescription *description,
		uint64_t absolute_deadline_ms) = 0;
	virtual Result ReadRetainedAt(uint64_t offset, void *bytes, size_t count,
		uint64_t absolute_deadline_ms) = 0;
	virtual Result CloseContent(uint64_t absolute_deadline_ms) = 0;
	virtual void CloseContentForProcessExit() = 0;
};

class NativeInputDescriptorResource {
public:
	virtual ~NativeInputDescriptorResource() {}
	virtual NativeAcquisitionOutcome OpenInputDescriptors(
		const OperationLease &lease) = 0;
	virtual Result CloseInputDescriptors(const OperationLease &lease) = 0;
	virtual void CloseInputDescriptorsForProcessExit() = 0;
	virtual Result CaptureDigitalNeutral(const OperationLease &lease,
		NativeDigitalNeutral *values, bool *valid, size_t count) = 0;
};

struct NativeResourceSet {
	NativePreflight &preflight;
	NativeHardwareResources &hardware;
	NativeSchedulerResource &scheduler;
	NativeOffloadResource &offload;
	NativeSaveResource &save;
	NativeContentResource &content;
	NativeInputDescriptorResource &input_descriptors;
};

} // namespace native
} // namespace mister

#endif
