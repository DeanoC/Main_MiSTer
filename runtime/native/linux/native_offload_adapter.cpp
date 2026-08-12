// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_offload_adapter.hpp"

#include <new>

namespace mister {
namespace native {
namespace linux_native {

namespace {

constexpr size_t kOffloadCapacity = 8;

class OffloadOperations {
public:
	virtual ~OffloadOperations() {}
	virtual Result Initialize(uint64_t deadline) = 0;
	virtual Result Release() = 0;
	virtual uint64_t NowMs() const = 0;
};

class LocalOffloadOperations final : public OffloadOperations {
public:
	explicit LocalOffloadOperations(NativeClock &clock) : clock_(clock) {}
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
class InjectedOffloadOperations final : public OffloadOperations {
public:
	InjectedOffloadOperations() : ops_(nullptr) {}
	explicit InjectedOffloadOperations(NativeLinuxExecutionOperations &ops)
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

class NativeOffloadAdapter::Impl {
public:
	explicit Impl(NativeClock &clock)
		: local(clock),
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
		  injected(),
#endif
		  operations(&local), mutex(), drained(), admission(false),
		  initialized(false), initialization_in_progress(false),
		  cleanup_started(false), release_in_progress(false),
		  process_exit(false), slots{} {}
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
	Impl(NativeClock &clock, NativeLinuxExecutionOperations &ops)
		: local(clock), injected(ops), operations(&injected), mutex(), drained(),
		  admission(false), initialized(false), initialization_in_progress(false),
		  cleanup_started(false), release_in_progress(false),
		  process_exit(false), slots{} {}
#endif
	LocalOffloadOperations local;
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
	InjectedOffloadOperations injected;
#endif
	OffloadOperations *operations;
	std::mutex mutex;
	std::condition_variable drained;
	bool admission;
	bool initialized;
	bool initialization_in_progress;
	bool cleanup_started;
	bool release_in_progress;
	bool process_exit;
	bool slots[kOffloadCapacity];
};

NativeOffloadAdapter::NativeOffloadAdapter(HardwareBroker &broker,
	NativeClock &clock)
	: broker_(broker), clock_(clock), impl_(new (std::nothrow) Impl(clock))
{
}

#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
NativeOffloadAdapter::NativeOffloadAdapter(HardwareBroker &broker,
	NativeClock &clock, NativeLinuxExecutionOperations &operations)
	: broker_(broker), clock_(clock),
	  impl_(new (std::nothrow) Impl(clock, operations))
{
}
#endif

NativeOffloadAdapter::~NativeOffloadAdapter()
{
	CloseOffloadForProcessExit();
	if (impl_ != nullptr) {
		std::unique_lock<std::mutex> lock(impl_->mutex);
		impl_->drained.wait(lock, [this] {
			if (impl_->initialization_in_progress || impl_->release_in_progress)
				return false;
			for (size_t i = 0; i < kOffloadCapacity; ++i)
				if (impl_->slots[i]) return false;
			return true;
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

NativeAcquisitionOutcome NativeOffloadAdapter::StartOffload(
	const OperationLease &lease)
{
	if (impl_ == nullptr) return {MISTER_RESULT_PLATFORM, false};
	std::unique_ptr<ProcessOperationGuard> guard;
	const Result admitted = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::offload, nullptr, &guard);
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
		if (impl_->process_exit && impl_->initialized &&
			!impl_->release_in_progress) {
			bool live = false;
			for (size_t i = 0; i < kOffloadCapacity; ++i)
				live |= impl_->slots[i];
			if (!live) {
				impl_->release_in_progress = true;
				release_for_process_exit = true;
			}
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

Result NativeOffloadAdapter::Submit(const OperationLease &lease,
	NativeExecutionCallback callback, void *context)
{
	if (impl_ == nullptr || callback == nullptr)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::unique_ptr<ProcessOperationGuard> guard;
	Result result = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::offload, nullptr, &guard);
	if (result != MISTER_RESULT_OK) return result;
	if (guard->authority() != LeaseAuthority::active_generation)
		return MISTER_RESULT_INVALID_STATE;
	size_t slot = kOffloadCapacity;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (!impl_->admission || impl_->cleanup_started)
			return MISTER_RESULT_INVALID_STATE;
		if (clock_.NowMs() >= guard->absolute_deadline_ms())
			return MISTER_RESULT_DEADLINE;
		for (size_t i = 0; i < kOffloadCapacity; ++i) {
			if (!impl_->slots[i]) { slot = i; break; }
		}
		if (slot == kOffloadCapacity) return MISTER_RESULT_INVALID_STATE;
		impl_->slots[slot] = true;
	}
	result = callback(context);
	const bool expired = result == MISTER_RESULT_OK &&
		clock_.NowMs() >= guard->absolute_deadline_ms();
	bool release_for_process_exit = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->slots[slot] = false;
		bool live = false;
		for (size_t i = 0; i < kOffloadCapacity; ++i) live |= impl_->slots[i];
		if (!live && impl_->process_exit && impl_->initialized &&
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

Result NativeOffloadAdapter::RejectAndJoinOffload(
	const OperationLease &lease)
{
	if (impl_ == nullptr) return MISTER_RESULT_PLATFORM;
	std::unique_ptr<ProcessOperationGuard> guard;
	Result result = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::offload, nullptr, &guard);
	if (result != MISTER_RESULT_OK) return result;
	if (guard->authority() != LeaseAuthority::cleanup_epoch)
		return MISTER_RESULT_INVALID_STATE;
	std::unique_lock<std::mutex> lock(impl_->mutex);
	impl_->admission = false;
	impl_->cleanup_started = true;
	if (impl_->release_in_progress) return MISTER_RESULT_INVALID_STATE;
	for (;;) {
		bool live = false;
		for (size_t i = 0; i < kOffloadCapacity; ++i) live |= impl_->slots[i];
		if (!live) break;
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

void NativeOffloadAdapter::CloseOffloadForProcessExit()
{
	if (impl_ == nullptr) return;
	bool release = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->admission = false;
		impl_->cleanup_started = true;
		impl_->process_exit = true;
		bool live = false;
		for (size_t i = 0; i < kOffloadCapacity; ++i) live |= impl_->slots[i];
		if (!live && !impl_->initialization_in_progress && impl_->initialized &&
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
