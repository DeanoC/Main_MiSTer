// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_AV_IO_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_AV_IO_ADAPTER_HPP

#include "runtime/native/native_clock.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_peripheral_session.hpp"
#include "runtime/native/hardware_broker.hpp"

#include <stdint.h>

namespace mister {
namespace native {
namespace linux_native {

#if defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
// Test-only virtual user-I/O backend. The production adapter has no generic
// raw command/mask surface and rejects fixture authority before opening I/O.
class NativeAvIoTestOperations {
public:
	virtual ~NativeAvIoTestOperations() {}
	// A non-OK Begin is a known non-admission: it opens no transaction and
	// emits no word, so the caller must neither Finish nor advance its ledger.
	virtual Result Begin(uint64_t absolute_deadline_ms) = 0;
	virtual Result SendWord(uint16_t word, uint16_t *ack_low,
		uint64_t absolute_deadline_ms) = 0;
	virtual Result Finish(uint64_t absolute_deadline_ms) = 0;
};
#endif

class NativeAvIoAdapter final {
public:
	explicit NativeAvIoAdapter(NativeClock &clock);
#if defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
	NativeAvIoAdapter(NativeClock &clock, NativeAvIoTestOperations &operations);
#endif
	~NativeAvIoAdapter();
	NativeAvIoAdapter(const NativeAvIoAdapter &) = delete;
	NativeAvIoAdapter &operator=(const NativeAvIoAdapter &) = delete;

	Result StartAudio(HardwareBroker &broker, ActiveAudioSession &session,
		const NativeAudioProfile &profile, PeripheralCompletionReceipt *receipt);
	Result StopAudio(HardwareBroker &broker, CleanupAudioSession &session,
		const NativeAudioProfile &profile, PeripheralCompletionReceipt *receipt);
	Result RecoverAudio(HardwareBroker &broker, RecoveryAudioSession &session,
		const NativeAudioProfile &profile, PeripheralCompletionReceipt *receipt);
	Result StartVideo(HardwareBroker &broker, ActiveVideoSession &session,
		const NativeVideoProfile &profile, PeripheralCompletionReceipt *receipt);
	Result StopVideo(HardwareBroker &broker, CleanupVideoSession &session,
		const NativeVideoProfile &profile, PeripheralCompletionReceipt *receipt);
	Result RecoverVideo(HardwareBroker &broker, RecoveryVideoSession &session,
		const NativeVideoProfile &profile, PeripheralCompletionReceipt *receipt);
	Result StartAudioVideo(HardwareBroker &broker,
		ActiveAudioVideoSession &session, const NativeVideoProfile &profile,
		CoupledAcquisitionReceipt *receipt);
	Result StopAudioVideo(HardwareBroker &broker,
		CleanupAudioVideoSession &session, const NativeVideoProfile &profile,
		CoupledCompletionReceipt *receipt);
	Result RecoverAudioVideo(HardwareBroker &broker,
		RecoveryAudioVideoSession &session, const NativeVideoProfile &profile,
		CoupledRecoveryReceipt *receipt);

private:
	Result SendAudio(HardwareBroker &broker,
		const std::shared_ptr<PeripheralSessionState> &state,
		const NativeAudioProfile &profile, uint8_t attenuation,
		PeripheralCompletionReceipt *receipt);
	Result SendVideoActivation(HardwareBroker &broker,
		const std::shared_ptr<PeripheralSessionState> &state,
		const NativeVideoProfile &profile, PeripheralCompletionReceipt *receipt);
	Result SendVideoTeardown(HardwareBroker &broker,
		const std::shared_ptr<PeripheralSessionState> &state,
		const NativeVideoProfile &profile, PeripheralCompletionReceipt *receipt);
	Result SendCoupled(HardwareBroker &broker,
		const std::shared_ptr<PeripheralSessionState> &state,
		const NativeVideoProfile &profile, bool power_down,
		CoupledCompletionReceipt *receipt);
#if defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
	Result SendFixtureActionWords(HardwareBroker &broker,
		const std::shared_ptr<PeripheralSessionState> &state,
		PeripheralSessionAction action, const uint16_t *words,
		uint8_t word_count, uint8_t next_word_index, uint16_t *residue,
		uint64_t deadline);
	Result CloseFixtureAction(HardwareBroker &broker,
		const std::shared_ptr<PeripheralSessionState> &state,
		PeripheralSessionAction action, bool closed);
#endif
	NativeClock &clock_;
#if defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
	NativeAvIoTestOperations *operations_;
#endif
};

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
