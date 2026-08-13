// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_VIDEO_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_VIDEO_ADAPTER_HPP

#include "runtime/native/linux/native_av_io_adapter.hpp"
#include "runtime/native/native_resources.hpp"

namespace mister {
namespace native {
namespace linux_native {

class NativeVideoAdapter final : public NativeVideoResource,
	public NativeAudioVideoResource {
public:
	NativeVideoAdapter(NativeClock &clock, HardwareBroker &broker,
		NativeAvIoAdapter &io);
	~NativeVideoAdapter();
	NativeVideoAdapter(const NativeVideoAdapter &) = delete;
	NativeVideoAdapter &operator=(const NativeVideoAdapter &) = delete;

	PeripheralBackendIdentity BackendIdentity() const override;
	NativePeripheralAcquisitionOutcome StartVideo(
		std::unique_ptr<ActiveVideoSessionBundle> &&bundle) override;
	NativePeripheralReleaseOutcome StopVideo(
		std::unique_ptr<CleanupVideoSessionBundle> &&bundle) override;
	NativePeripheralReleaseOutcome RecoverVideo(
		std::unique_ptr<RecoveryVideoSessionBundle> &&bundle) override;
	void CloseVideoForProcessExit() override;
	NativeCoupledAcquisitionOutcome StartAudioVideo(
		std::unique_ptr<ActiveAudioVideoSessionBundle> &&bundle) override;
	NativeCoupledReleaseOutcome StopAudioVideo(
		std::unique_ptr<CleanupAudioVideoSessionBundle> &&bundle) override;
	NativeCoupledReleaseOutcome RecoverAudioVideo(
		std::unique_ptr<RecoveryAudioVideoSessionBundle> &&bundle) override;
	void CloseAudioVideoForProcessExit() override;

private:
	HardwareBroker &broker_;
	NativeAvIoAdapter &io_;
	PeripheralBackendIdentity backend_;
};

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
