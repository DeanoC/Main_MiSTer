// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_linux_v2_context.hpp"

#include <limits.h>
#include <string.h>

namespace mister {
namespace native {
namespace linux_native {

#if defined(MISTER_NATIVE_LINUX_V2_CONTEXT_TESTING)
namespace {

bool AllZero(const uint32_t *values, size_t count)
{
	if (values == nullptr) return false;
	for (size_t index = 0; index < count; ++index) {
		if (values[index] != 0) return false;
	}
	return true;
}

bool EqualView(MisterStringView view, const char *text)
{
	if (text == nullptr || (view.length != 0 && view.data == nullptr)) return false;
	const size_t length = strlen(text);
	return length == view.length &&
		(length == 0 || memcmp(view.data, text, length) == 0);
}

bool LowerAlphaNumeric(char value)
{
	return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

bool ValidGameId(MisterStringView view)
{
	if (view.data == nullptr || view.length == 0 || view.length > 128 ||
		!LowerAlphaNumeric(view.data[0]) ||
		!LowerAlphaNumeric(view.data[view.length - 1])) return false;
	for (uint32_t index = 1; index + 1 < view.length; ++index) {
		if (!LowerAlphaNumeric(view.data[index]) && view.data[index] != '-') return false;
	}
	return true;
}

bool ValidDigest(MisterStringView view)
{
	if (view.data == nullptr || view.length != 64) return false;
	for (uint32_t index = 0; index < view.length; ++index) {
		const char value = view.data[index];
		if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
			return false;
		}
	}
	return true;
}

bool ProfileAccepts(const NativeCoreProfile &profile, const MisterLaunchV2 &launch)
{
	if (!EqualView(launch.system, profile.system) ||
		!EqualView(launch.expected_core, profile.core) ||
		launch.content.size < profile.protocol.minimum_source_bytes ||
		launch.content.size > profile.protocol.maximum_source_bytes) return false;
	for (size_t index = 0; index < kNativeExtensionCount; ++index) {
		if (EqualView(launch.content.extension, profile.extensions[index])) return true;
	}
	return false;
}

bool ValidLaunchPrefix(const MisterLaunchV2 *launch)
{
	return launch != nullptr &&
		launch->abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		launch->struct_size >= sizeof(*launch) && AllZero(launch->reserved, 4) &&
		launch->content.abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		launch->content.struct_size >= sizeof(launch->content) &&
		AllZero(launch->content.reserved, 4) && ValidGameId(launch->game_id) &&
		ValidDigest(launch->content.sha256) && launch->content.size != 0;
}

bool InitializedObservation(const MisterObservationV2 *observation)
{
	return observation != nullptr &&
		observation->abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		observation->struct_size == sizeof(*observation) &&
		observation->ready == 0 && observation->observed_core.data == nullptr &&
		observation->observed_core.length == 0 && observation->resource_flags == 0 &&
		AllZero(observation->reserved, 4);
}

bool InitializedRecovery(const MisterRecoveryObservationV2 *observation)
{
	return observation != nullptr &&
		observation->abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		observation->struct_size == sizeof(*observation) &&
		observation->observed_resource_flags == 0 &&
		observation->neutral_resource_flags == 0 &&
		AllZero(observation->reserved, 4);
}

bool ValidResult(MisterResult result)
{
	return result >= MISTER_RESULT_OK && result <= MISTER_RESULT_EXIT_REQUIRED;
}

MisterResult Normalize(MisterResult result)
{
	return ValidResult(result) ? result : MISTER_RESULT_PLATFORM;
}

uint64_t SaturatingAdd(uint64_t left, uint32_t right)
{
	return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

MisterStringView ProfileCoreView(const NativeCoreProfile &profile)
{
	MisterStringView view = {profile.core,
		static_cast<uint32_t>(strlen(profile.core))};
	return view;
}

} // namespace

class NativeLinuxV2Context::Impl final {
public:
#if defined(MISTER_NATIVE_LINUX_V2_CONTEXT_TESTING)
	Impl(NativeClock &clock, const NativeLinuxV2FixtureProfiles &profiles,
		NativeLinuxV2GenerationFactory &generations,
		NativeLinuxV2RecoveryFactory &recoveries)
		: clock_(&clock), snes_(profiles.snes), megadrive_(profiles.megadrive),
		  generations_(&generations), recoveries_(&recoveries), started_(false),
		  recovery_admitted_(false), generation_active_(false), recovery_mask_(0),
		  selected_profile_(nullptr)
	{
		InitializePlatform();
	}
#endif

	const MisterPlatformV2 &platform() const { return platform_; }

private:
	void InitializePlatform()
	{
		memset(&platform_, 0, sizeof(platform_));
		platform_.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
		platform_.struct_size = sizeof(platform_);
		platform_.capability_flags = MISTER_CAP_V2_KNOWN;
		platform_.context = this;
		platform_.start = &Start;
		platform_.load = &Load;
		platform_.tick = &Tick;
		platform_.observe = &Observe;
		platform_.stop = &Stop;
		platform_.recover = &Recover;
	}

	static Impl *Self(void *context)
	{
		return static_cast<Impl *>(context);
	}

	static MisterResult Start(void *context, uint32_t deadline_ms)
	{
		if (context == nullptr || deadline_ms == 0) return MISTER_RESULT_INVALID_ARGUMENT;
		Impl *self = Self(context);
		(void)self->AbsoluteDeadline(deadline_ms);
		if (self->started_ || self->generation_ || self->recovery_admitted_) {
			return MISTER_RESULT_INVALID_STATE;
		}
		self->started_ = true;
		return MISTER_RESULT_OK;
	}

	static MisterResult Load(void *context, const MisterLaunchV2 *launch,
		uint32_t deadline_ms)
	{
		if (context == nullptr || deadline_ms == 0 || !ValidLaunchPrefix(launch)) {
			return MISTER_RESULT_INVALID_ARGUMENT;
		}
		Impl *self = Self(context);
		const uint64_t deadline = self->AbsoluteDeadline(deadline_ms);
		if (!self->started_ || self->generation_ || self->recovery_admitted_) {
			return MISTER_RESULT_INVALID_STATE;
		}
		const NativeCoreProfile *profile = self->SelectProfile(*launch);
		if (profile == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;

		std::unique_ptr<NativeLinuxV2Generation> candidate;
		MisterResult result = Normalize(self->generations_->Create(
			*profile, *launch, &candidate));
		if (candidate) {
			self->selected_profile_ = profile;
			self->generation_ = std::move(candidate);
		}
		if (result != MISTER_RESULT_OK) {
			self->DropIdleGeneration();
			return result;
		}
		if (!self->generation_) return MISTER_RESULT_PLATFORM;
		result = Normalize(self->generation_->Activate(deadline));
		if (result == MISTER_RESULT_OK && self->generation_->idle()) {
			self->DropGeneration();
			return MISTER_RESULT_PLATFORM;
		}
		self->generation_active_ = result == MISTER_RESULT_OK;
		if (result != MISTER_RESULT_OK) self->DropIdleGeneration();
		return result;
	}

	static MisterResult Tick(void *context, uint32_t deadline_ms)
	{
		if (context == nullptr || deadline_ms == 0) return MISTER_RESULT_INVALID_ARGUMENT;
		Impl *self = Self(context);
		const uint64_t deadline = self->AbsoluteDeadline(deadline_ms);
		if (!self->started_ || !self->generation_ || !self->generation_active_) {
			return MISTER_RESULT_INVALID_STATE;
		}
		const MisterResult result = Normalize(self->generation_->Tick(deadline));
		if (result != MISTER_RESULT_OK) self->generation_active_ = false;
		return result;
	}

	static MisterResult Observe(void *context, MisterObservationV2 *observation,
		uint32_t deadline_ms)
	{
		if (context == nullptr || deadline_ms == 0 ||
			!InitializedObservation(observation)) return MISTER_RESULT_INVALID_ARGUMENT;
		Impl *self = Self(context);
		const uint64_t deadline = self->AbsoluteDeadline(deadline_ms);
		if (!self->generation_) return MISTER_RESULT_OK;
		MisterResult result = Normalize(self->generation_->Observe(observation, deadline));
		observation->observed_core = ProfileCoreView(*self->selected_profile_);
		if (observation->ready > 1 ||
			(observation->ready == 1 && (!self->generation_active_ ||
			 observation->resource_flags != MISTER_RESOURCE_V2_KNOWN)) ||
			(observation->resource_flags & ~MISTER_RESOURCE_V2_KNOWN) != 0 ||
			!AllZero(observation->reserved, 4)) return MISTER_RESULT_PLATFORM;
		return result;
	}

	static MisterResult Stop(void *context, uint32_t deadline_ms)
	{
		if (context == nullptr || deadline_ms == 0) return MISTER_RESULT_INVALID_ARGUMENT;
		Impl *self = Self(context);
		const uint64_t deadline = self->AbsoluteDeadline(deadline_ms);
		if (!self->generation_) return MISTER_RESULT_OK;
		MisterResult result = Normalize(self->generation_->Stop(deadline));
		if (self->generation_->idle()) {
			self->DropGeneration();
			return result;
		}
		return result == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : result;
	}

	static MisterResult Recover(void *context, uint32_t required_resource_flags,
		MisterRecoveryObservationV2 *observation, uint32_t deadline_ms)
	{
		if (context == nullptr || deadline_ms == 0 || required_resource_flags == 0 ||
			(required_resource_flags & ~MISTER_RESOURCE_V2_KNOWN) != 0 ||
			!InitializedRecovery(observation)) return MISTER_RESULT_INVALID_ARGUMENT;
		Impl *self = Self(context);
		const uint64_t now = self->clock_->NowMs();
		const uint64_t deadline = SaturatingAdd(now, deadline_ms);
		if (self->started_ || self->generation_) return MISTER_RESULT_INVALID_STATE;
		if (!self->recovery_admitted_) {
			self->recovery_admitted_ = true;
			self->recovery_mask_ = required_resource_flags;
			std::unique_ptr<NativeLinuxV2RecoveryAttempt> attempt;
			MisterResult create = Normalize(self->recoveries_->Create(
				required_resource_flags, now, &attempt));
			if (attempt) self->recovery_ = std::move(attempt);
			if (create != MISTER_RESULT_OK) return create;
			if (!self->recovery_ ||
				self->recovery_->requested_resource_flags() != required_resource_flags) {
				self->recovery_.reset();
				return MISTER_RESULT_PLATFORM;
			}
		} else if (self->recovery_mask_ != required_resource_flags || !self->recovery_) {
			return MISTER_RESULT_INVALID_STATE;
		}

		MisterRecoveryObservationV2 candidate = {};
		candidate.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
		candidate.struct_size = sizeof(candidate);
		MisterResult result = Normalize(self->recovery_->Continue(deadline, &candidate));
		const bool prefix_valid =
			candidate.abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
			candidate.struct_size == sizeof(candidate);
		const uint32_t observed = candidate.observed_resource_flags;
		const uint32_t neutral = candidate.neutral_resource_flags;
		if (!prefix_valid || ((observed | neutral) & ~required_resource_flags) != 0 ||
			(observed & neutral) != 0 || !AllZero(candidate.reserved, 4)) {
			result = MISTER_RESULT_PLATFORM;
		} else {
			*observation = candidate;
		}
		const bool complete = self->recovery_->complete();
		if (result == MISTER_RESULT_OK &&
			(!complete || observed != 0 || neutral != required_resource_flags)) {
			result = MISTER_RESULT_PLATFORM;
		}
		if (complete) self->recovery_.reset();
		return result;
	}

	uint64_t AbsoluteDeadline(uint32_t relative_ms) const
	{
		return SaturatingAdd(clock_->NowMs(), relative_ms);
	}

	const NativeCoreProfile *SelectProfile(const MisterLaunchV2 &launch) const
	{
		if (ProfileAccepts(*snes_, launch)) return snes_;
		if (ProfileAccepts(*megadrive_, launch)) return megadrive_;
		return nullptr;
	}

	void DropIdleGeneration()
	{
		if (generation_ && generation_->idle()) DropGeneration();
	}

	void DropGeneration()
	{
		generation_.reset();
		generation_active_ = false;
		selected_profile_ = nullptr;
	}

	MisterPlatformV2 platform_;
	NativeClock *clock_;
	const NativeCoreProfile *snes_;
	const NativeCoreProfile *megadrive_;
	NativeLinuxV2GenerationFactory *generations_;
	NativeLinuxV2RecoveryFactory *recoveries_;
	bool started_;
	bool recovery_admitted_;
	bool generation_active_;
	uint32_t recovery_mask_;
	const NativeCoreProfile *selected_profile_;
	std::unique_ptr<NativeLinuxV2Generation> generation_;
	std::unique_ptr<NativeLinuxV2RecoveryAttempt> recovery_;
};

#else

class NativeLinuxV2Context::Impl final {
public:
	const MisterPlatformV2 &platform() const { return platform_; }

private:
	MisterPlatformV2 platform_;
};

#endif

NativeLinuxV2Context::NativeLinuxV2Context(Impl *impl) : impl_(impl) {}

NativeLinuxV2Context::~NativeLinuxV2Context()
{
	delete impl_;
}

const MisterPlatformV2 &NativeLinuxV2Context::platform() const
{
	return impl_->platform();
}

MisterResult CreateProductionNativeLinuxV2Context(
	std::unique_ptr<NativeLinuxV2Context> *context)
{
	if (context == nullptr || *context) return MISTER_RESULT_INVALID_ARGUMENT;
	if (ProductionNativeCoreProfile("snes") == nullptr ||
		ProductionNativeCoreProfile("megadrive") == nullptr) {
		return MISTER_RESULT_UNSUPPORTED;
	}
	return MISTER_RESULT_UNSUPPORTED;
}

#if defined(MISTER_NATIVE_LINUX_V2_CONTEXT_TESTING)
MisterResult CreateFixtureNativeLinuxV2ContextForTest(
	NativeClock &clock, const NativeLinuxV2FixtureProfiles &profiles,
	NativeLinuxV2GenerationFactory &generations,
	NativeLinuxV2RecoveryFactory &recoveries,
	std::unique_ptr<NativeLinuxV2Context> *context)
{
	if (context == nullptr || *context || profiles.snes == nullptr ||
		profiles.megadrive == nullptr || profiles.snes == profiles.megadrive ||
		profiles.snes != FixtureNativeCoreProfile("snes") ||
		profiles.megadrive != FixtureNativeCoreProfile("megadrive")) {
		return MISTER_RESULT_INVALID_ARGUMENT;
	}
	NativeLinuxV2Context::Impl *impl =
		new NativeLinuxV2Context::Impl(clock, profiles, generations, recoveries);
	if (impl == nullptr) return MISTER_RESULT_PLATFORM;
	context->reset(new NativeLinuxV2Context(impl));
	if (!*context) {
		delete impl;
		return MISTER_RESULT_PLATFORM;
	}
	return MISTER_RESULT_OK;
}
#endif

} // namespace linux_native
} // namespace native
} // namespace mister
