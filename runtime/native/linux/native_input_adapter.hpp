// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_INPUT_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_INPUT_ADAPTER_HPP

#include "runtime/native/native_input.hpp"
#include "runtime/native/native_resources.hpp"

#include <stddef.h>
#include <stdint.h>

namespace mister {
namespace native {
namespace linux_native {

enum class NativeLinuxInputEventKind : uint8_t {
	digital_button,
	digital_hat_x,
	digital_hat_y,
	syn_report,
	syn_dropped,
	device_removed,
	unsupported
};

namespace NativeLinuxInputCode {
constexpr uint16_t south = 0;
constexpr uint16_t east = 1;
constexpr uint16_t select = 2;
constexpr uint16_t start = 3;
constexpr uint16_t up = 4;
constexpr uint16_t down = 5;
constexpr uint16_t left = 6;
constexpr uint16_t right = 7;
constexpr uint16_t left_shoulder = 8;
constexpr uint16_t right_shoulder = 9;
constexpr uint16_t north = 10;
constexpr uint16_t west = 11;
constexpr uint16_t left_trigger = 12;
constexpr uint16_t right_trigger = 13;
constexpr uint16_t left_thumb = 14;
constexpr uint16_t right_thumb = 15;
}

struct NativeLinuxInputEvent {
	NativeLinuxInputEventKind kind;
	uint16_t code;
	int32_t value;
	uint64_t device_identity;
};

struct NativeLinuxInputCapabilities {
	bool digital_gamepad_buttons;
	bool digital_hat;
	bool keyboard;
	bool mouse;
	bool analog_absolute;
	bool touch;
	bool wheel;
	bool rumble_or_output;
	bool bluetooth_admin;
	bool uinput;
	bool fifo_or_signal;
	bool led;
	bool unknown;
};

#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
struct NativeLinuxInputCandidate {
	uint64_t identity;
	char path[128];
};

struct NativeLinuxInputSourceProbe {
	bool character_device;
	bool identity_available;
	uint16_t bus_type;
	bool name_identifies_uinput;
	bool topology_available;
	bool virtual_topology;
	size_t recognized_gamepad_buttons;
};

NativeLinuxInputCapabilities ClassifyNativeLinuxInputSourceForTest(
	NativeLinuxInputCapabilities capabilities,
	const NativeLinuxInputSourceProbe &probe);

class NativeLinuxInputOperations {
public:
	virtual ~NativeLinuxInputOperations() {}
	virtual Result OpenDirectory(uint64_t absolute_deadline_ms,
		int *descriptor) = 0;
	virtual Result OpenWatch(uint64_t absolute_deadline_ms,
		int *descriptor) = 0;
	virtual Result AddWatch(int watch_descriptor,
		uint64_t absolute_deadline_ms, int *watch) = 0;
	virtual Result DiscoverDevice(size_t index,
		NativeLinuxInputCandidate *candidate) = 0;
	virtual Result OpenDevice(const NativeLinuxInputCandidate &candidate,
		uint64_t absolute_deadline_ms, int *descriptor) = 0;
	virtual Result QueryCapabilities(int descriptor,
		NativeLinuxInputCapabilities *capabilities) = 0;
	virtual Result SetGrab(int descriptor, bool grab) = 0;
	virtual Result ReadEvents(int descriptor, NativeLinuxInputEvent *events,
		size_t capacity, size_t *count, bool *partial) = 0;
	virtual Result RemoveWatch(int watch_descriptor, int watch) = 0;
	virtual Result Close(int descriptor) = 0;
	virtual uint64_t NowMs() const = 0;
};
#endif

class NativeInputAdapter final : public NativeInputDescriptorResource {
public:
	NativeInputAdapter(HardwareBroker &broker, NativeClock &clock,
		const NativeCoreProfile &profile, NativeInput &input);
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
	NativeInputAdapter(HardwareBroker &broker, NativeClock &clock,
		const NativeCoreProfile &profile, NativeInput &input,
		NativeLinuxInputOperations &operations);
#endif
	~NativeInputAdapter() override;
	NativeInputAdapter(const NativeInputAdapter &) = delete;
	NativeInputAdapter &operator=(const NativeInputAdapter &) = delete;

	NativeAcquisitionOutcome OpenInputDescriptors(
		const OperationLease &lease) override;
	Result PollInput(const OperationLease &lease);
	Result CloseInputDescriptors(const OperationLease &lease) override;
	void CloseInputDescriptorsForProcessExit() override;
	Result CaptureDigitalNeutral(const OperationLease &lease,
		NativeDigitalNeutral *values, bool *valid, size_t count) override;

#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
	size_t admitted_devices_for_test() const;
	size_t grabbed_devices_for_test() const;
	size_t owned_descriptors_for_test() const;
	bool player_uncertain_for_test(size_t player) const;
	void set_player_sequence_for_test(size_t player, uint64_t sequence);
#endif

private:
	class Impl;
	HardwareBroker &broker_;
	const NativeCoreProfile &profile_;
	NativeInput &input_;
	Impl *impl_;
};

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
