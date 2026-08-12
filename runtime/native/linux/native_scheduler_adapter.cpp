// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_scheduler_adapter.hpp"

#include <new>

namespace mister {
namespace native {
namespace linux_native {

namespace {

class ExecutionOperations {
public:
	virtual ~ExecutionOperations() {}
	virtual Result Initialize(uint64_t deadline) = 0;
	virtual Result Release() = 0;
	virtual uint64_t NowMs() const = 0;
};

class LocalExecutionOperations final : public ExecutionOperations {
public:
	explicit LocalExecutionOperations(NativeClock &clock) : clock_(clock) {}
	Result Initialize(uint64_t deadline) override
	{
		return clock_.NowMs() >= deadline ? MISTER_RESULT_DEADLINE :
			MISTER_RESULT_OK;
	}
	Result Release() override { return MISTER_RESULT_OK; }
	uint64_t NowMs() const override { return clock_.NowMs(); }
private:
	NativeClock &clock_;
};

#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
class InjectedExecutionOperations final : public ExecutionOperations {
public:
	InjectedExecutionOperations() : ops_(nullptr) {}
	explicit InjectedExecutionOperations(NativeLinuxExecutionOperations &ops)
		: ops_(&ops) {}
	Result Initialize(uint64_t deadline) override
	{
		return ops_->Initialize(deadline);
	}
	Result Release() override { return ops_->Release(); }
	uint64_t NowMs() const override { return ops_->NowMs(); }
private:
	NativeLinuxExecutionOperations *ops_;
};
#endif

} // namespace

class NativeSchedulerAdapter::Impl {
public:
	explicit Impl(NativeClock &clock)
		: local(clock),
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
		  injected(),
#endif
		  operations(&local), mutex(), drained(), admission(false),
		  initialized(false), initialization_in_progress(false),
		  cleanup_started(false), release_in_progress(false),
		  process_exit(false), active(0) {}
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
	Impl(NativeClock &clock, NativeLinuxExecutionOperations &ops)
		: local(clock), injected(ops), operations(&injected), mutex(), drained(),
		  admission(false), initialized(false), initialization_in_progress(false),
		  cleanup_started(false), release_in_progress(false),
		  process_exit(false), active(0) {}
#endif
	LocalExecutionOperations local;
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
	InjectedExecutionOperations injected;
#endif
	ExecutionOperations *operations;
	std::mutex mutex;
	std::condition_variable drained;
	bool admission;
	bool initialized;
	bool initialization_in_progress;
	bool cleanup_started;
	bool release_in_progress;
	bool process_exit;
	size_t active;
};

NativeSchedulerAdapter::NativeSchedulerAdapter(HardwareBroker &broker,
	NativeClock &clock)
	: broker_(broker), clock_(clock), impl_(new (std::nothrow) Impl(clock))
{
}

#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
NativeSchedulerAdapter::NativeSchedulerAdapter(HardwareBroker &broker,
	NativeClock &clock, NativeLinuxExecutionOperations &operations)
	: broker_(broker), clock_(clock),
	  impl_(new (std::nothrow) Impl(clock, operations))
{
}
#endif

NativeSchedulerAdapter::~NativeSchedulerAdapter()
{
	CloseSchedulerForProcessExit();
	if (impl_ != nullptr) {
		std::unique_lock<std::mutex> lock(impl_->mutex);
		impl_->drained.wait(lock, [this] {
			return impl_->active == 0 && !impl_->initialization_in_progress &&
				!impl_->release_in_progress;
		});
		if (impl_->initialized) {
			impl_->release_in_progress = true;
			lock.unlock();
			(void)impl_->operations->Release();
			lock.lock();
			impl_->initialized = false;
			impl_->release_in_progress = false;
		}
	}
	delete impl_;
}

NativeAcquisitionOutcome NativeSchedulerAdapter::StartScheduler(
	const OperationLease &lease)
{
	if (impl_ == nullptr) return {MISTER_RESULT_PLATFORM, false};
	std::unique_ptr<ProcessOperationGuard> guard;
	const Result admitted = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::scheduler, nullptr, &guard);
	if (admitted != MISTER_RESULT_OK) return {admitted, false};
	if (guard->authority() != LeaseAuthority::active_generation)
		return {MISTER_RESULT_INVALID_STATE, false};
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->initialized || impl_->cleanup_started)
			return {MISTER_RESULT_INVALID_STATE, impl_->initialized};
		impl_->initialized = true;
		impl_->initialization_in_progress = true;
	}
	Result result = impl_->operations->Initialize(
		guard->absolute_deadline_ms());
	const bool expired =
		impl_->operations->NowMs() >= guard->absolute_deadline_ms();
	bool release_for_process_exit = false;
	bool closed = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->initialization_in_progress = false;
		closed = impl_->cleanup_started;
		if (result == MISTER_RESULT_OK && !expired && !closed)
			impl_->admission = true;
		if (impl_->process_exit && impl_->initialized && impl_->active == 0 &&
			!impl_->release_in_progress) {
			impl_->release_in_progress = true;
			release_for_process_exit = true;
		}
		impl_->drained.notify_all();
	}
	if (release_for_process_exit) {
		(void)impl_->operations->Release();
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->initialized = false;
		impl_->release_in_progress = false;
		impl_->drained.notify_all();
	}
	if (result != MISTER_RESULT_OK) return {result, true};
	if (expired) return {MISTER_RESULT_DEADLINE, true};
	if (closed) return {MISTER_RESULT_INVALID_STATE, true};
	return {MISTER_RESULT_OK, true};
}

Result NativeSchedulerAdapter::Dispatch(const OperationLease &lease,
	NativeExecutionCallback callback, void *context)
{
	if (impl_ == nullptr || callback == nullptr)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::unique_ptr<ProcessOperationGuard> guard;
	Result result = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::scheduler, nullptr, &guard);
	if (result != MISTER_RESULT_OK) return result;
	if (guard->authority() != LeaseAuthority::active_generation)
		return MISTER_RESULT_INVALID_STATE;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (!impl_->admission || impl_->cleanup_started)
			return MISTER_RESULT_INVALID_STATE;
		if (clock_.NowMs() >= guard->absolute_deadline_ms())
			return MISTER_RESULT_DEADLINE;
		++impl_->active;
	}
	result = callback(context);
	const bool expired = result == MISTER_RESULT_OK &&
		clock_.NowMs() >= guard->absolute_deadline_ms();
	bool release_for_process_exit = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->active != 0) --impl_->active;
		if (impl_->active == 0 && impl_->process_exit && impl_->initialized &&
			!impl_->release_in_progress) {
			impl_->release_in_progress = true;
			release_for_process_exit = true;
		}
		impl_->drained.notify_all();
	}
	if (release_for_process_exit) {
		(void)impl_->operations->Release();
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->initialized = false;
		impl_->release_in_progress = false;
		impl_->drained.notify_all();
	}
	if (expired) return MISTER_RESULT_DEADLINE;
	return result;
}

Result NativeSchedulerAdapter::StopScheduler(const OperationLease &lease)
{
	if (impl_ == nullptr) return MISTER_RESULT_PLATFORM;
	std::unique_ptr<ProcessOperationGuard> guard;
	Result result = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::scheduler, nullptr, &guard);
	if (result != MISTER_RESULT_OK) return result;
	if (guard->authority() != LeaseAuthority::cleanup_epoch)
		return MISTER_RESULT_INVALID_STATE;
	std::unique_lock<std::mutex> lock(impl_->mutex);
	impl_->admission = false;
	impl_->cleanup_started = true;
	if (impl_->release_in_progress) return MISTER_RESULT_INVALID_STATE;
	while (impl_->active != 0) {
		if (clock_.NowMs() >= guard->absolute_deadline_ms() ||
			!clock_.WaitUntil(impl_->drained, lock,
				guard->absolute_deadline_ms()))
			return MISTER_RESULT_DEADLINE;
	}
	if (impl_->release_in_progress) return MISTER_RESULT_INVALID_STATE;
	if (!impl_->initialized) return MISTER_RESULT_OK;
	impl_->release_in_progress = true;
	lock.unlock();
	result = impl_->operations->Release();
	const bool expired = clock_.NowMs() >= guard->absolute_deadline_ms();
	lock.lock();
	if (result == MISTER_RESULT_OK) impl_->initialized = false;
	impl_->release_in_progress = false;
	impl_->drained.notify_all();
	if (result != MISTER_RESULT_OK) return result == MISTER_RESULT_DEADLINE ?
		MISTER_RESULT_DEADLINE : MISTER_RESULT_CLEANUP_INCOMPLETE;
	return expired ? MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
}

void NativeSchedulerAdapter::CloseSchedulerForProcessExit()
{
	if (impl_ == nullptr) return;
	bool release = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->admission = false;
		impl_->cleanup_started = true;
		impl_->process_exit = true;
		if (impl_->active == 0 && !impl_->initialization_in_progress &&
			impl_->initialized &&
			!impl_->release_in_progress) {
			impl_->release_in_progress = true;
			release = true;
		}
	}
	if (release) {
		(void)impl_->operations->Release();
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->initialized = false;
		impl_->release_in_progress = false;
		impl_->drained.notify_all();
	}
}

} // namespace linux_native
} // namespace native
} // namespace mister
