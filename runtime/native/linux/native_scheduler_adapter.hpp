// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_SCHEDULER_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_SCHEDULER_ADAPTER_HPP

#include "runtime/native/native_clock.hpp"
#include "runtime/native/native_resources.hpp"

namespace mister {
namespace native {
namespace linux_native {

using NativeExecutionCallback = Result (*)(void *context);

#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
class NativeLinuxExecutionOperations {
public:
	virtual ~NativeLinuxExecutionOperations() {}
	virtual Result Initialize(uint64_t absolute_deadline_ms) = 0;
	virtual Result Release() = 0;
	virtual uint64_t NowMs() const = 0;
};
#endif

class NativeSchedulerAdapter final : public NativeSchedulerResource {
public:
	NativeSchedulerAdapter(HardwareBroker &broker, NativeClock &clock);
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
	NativeSchedulerAdapter(HardwareBroker &broker, NativeClock &clock,
		NativeLinuxExecutionOperations &operations);
#endif
	~NativeSchedulerAdapter() override;
	NativeSchedulerAdapter(const NativeSchedulerAdapter &) = delete;
	NativeSchedulerAdapter &operator=(const NativeSchedulerAdapter &) = delete;

	NativeAcquisitionOutcome StartScheduler(
		const OperationLease &lease) override;
	Result Dispatch(const OperationLease &lease,
		NativeExecutionCallback callback, void *context);
	Result StopScheduler(const OperationLease &lease) override;
	void CloseSchedulerForProcessExit() override;

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
