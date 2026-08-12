// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_OFFLOAD_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_OFFLOAD_ADAPTER_HPP

#include "runtime/native/linux/native_scheduler_adapter.hpp"

namespace mister {
namespace native {
namespace linux_native {

class NativeOffloadAdapter final : public NativeOffloadResource {
public:
	NativeOffloadAdapter(HardwareBroker &broker, NativeClock &clock);
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
	NativeOffloadAdapter(HardwareBroker &broker, NativeClock &clock,
		NativeLinuxExecutionOperations &operations);
#endif
	~NativeOffloadAdapter() override;
	NativeOffloadAdapter(const NativeOffloadAdapter &) = delete;
	NativeOffloadAdapter &operator=(const NativeOffloadAdapter &) = delete;

	NativeAcquisitionOutcome StartOffload(
		const OperationLease &lease) override;
	Result Submit(const OperationLease &lease,
		NativeExecutionCallback callback, void *context);
	Result RejectAndJoinOffload(const OperationLease &lease) override;
	void CloseOffloadForProcessExit() override;

private:
	class Impl;
	HardwareBroker &broker_;
	NativeClock &clock_;
	Impl *impl_;
};

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
