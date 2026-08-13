// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_AUDIO_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_AUDIO_ADAPTER_HPP

#include "runtime/native/linux/native_av_io_adapter.hpp"
#include "runtime/native/native_resources.hpp"

namespace mister {
namespace native {
namespace linux_native {

class NativeAudioAdapter final : public NativeAudioResource {
public:
	NativeAudioAdapter(NativeClock &clock, HardwareBroker &broker,
		NativeAvIoAdapter &io);
	~NativeAudioAdapter();
	NativeAudioAdapter(const NativeAudioAdapter &) = delete;
	NativeAudioAdapter &operator=(const NativeAudioAdapter &) = delete;

	PeripheralBackendIdentity BackendIdentity() const override;
	NativePeripheralAcquisitionOutcome StartAudio(
		std::unique_ptr<ActiveAudioSessionBundle> &&bundle) override;
	NativePeripheralReleaseOutcome StopAudio(
		std::unique_ptr<CleanupAudioSessionBundle> &&bundle) override;
	NativePeripheralReleaseOutcome RecoverAudio(
		std::unique_ptr<RecoveryAudioSessionBundle> &&bundle) override;
	void CloseAudioForProcessExit() override;

private:
	HardwareBroker &broker_;
	NativeAvIoAdapter &io_;
	PeripheralBackendIdentity backend_;
};

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
