// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_LINUX_V2_CONTEXT_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_LINUX_V2_CONTEXT_HPP

#include "runtime/mister_runtime.h"
#include "runtime/native/native_clock.hpp"
#include "runtime/native/native_core_profile.hpp"

#include <memory>

namespace mister {
namespace native {
namespace linux_native {

class NativeLinuxV2Context final {
public:
	~NativeLinuxV2Context();
	NativeLinuxV2Context(const NativeLinuxV2Context &) = delete;
	NativeLinuxV2Context &operator=(const NativeLinuxV2Context &) = delete;
	const MisterPlatformV2 &platform() const;

private:
	class Impl;
	explicit NativeLinuxV2Context(Impl *impl);
	Impl *impl_;

#if defined(MISTER_NATIVE_LINUX_V2_CONTEXT_TESTING)
	friend MisterResult CreateFixtureNativeLinuxV2ContextForTest(
		NativeClock &, const struct NativeLinuxV2FixtureProfiles &,
		class NativeLinuxV2GenerationFactory &,
		class NativeLinuxV2RecoveryFactory &,
		std::unique_ptr<NativeLinuxV2Context> *);
#endif
};

MisterResult CreateProductionNativeLinuxV2Context(
	std::unique_ptr<NativeLinuxV2Context> *context);

#if defined(MISTER_NATIVE_LINUX_V2_CONTEXT_TESTING)
struct NativeLinuxV2FixtureProfiles {
	const NativeCoreProfile *snes;
	const NativeCoreProfile *megadrive;
};

class NativeLinuxV2Generation {
public:
	virtual ~NativeLinuxV2Generation() {}
	virtual MisterResult Activate(uint64_t absolute_deadline_ms) = 0;
	virtual MisterResult Tick(uint64_t absolute_deadline_ms) = 0;
	virtual MisterResult Observe(MisterObservationV2 *observation,
		uint64_t absolute_deadline_ms) const = 0;
	virtual MisterResult Stop(uint64_t callback_absolute_deadline_ms) = 0;
	virtual bool idle() const = 0;
};

class NativeLinuxV2GenerationFactory {
public:
	virtual ~NativeLinuxV2GenerationFactory() {}
	virtual MisterResult Create(const NativeCoreProfile &profile,
		const MisterLaunchV2 &borrowed_launch,
		std::unique_ptr<NativeLinuxV2Generation> *generation) = 0;
};

class NativeLinuxV2RecoveryAttempt {
public:
	virtual ~NativeLinuxV2RecoveryAttempt() {}
	virtual uint32_t requested_resource_flags() const = 0;
	virtual MisterResult Continue(uint64_t callback_absolute_deadline_ms,
		MisterRecoveryObservationV2 *observation) = 0;
	virtual bool complete() const = 0;
};

class NativeLinuxV2RecoveryFactory {
public:
	virtual ~NativeLinuxV2RecoveryFactory() {}
	virtual MisterResult Create(uint32_t requested_resource_flags,
		uint64_t cleanup_start_ms,
		std::unique_ptr<NativeLinuxV2RecoveryAttempt> *attempt) = 0;
};

MisterResult CreateFixtureNativeLinuxV2ContextForTest(
	NativeClock &clock,
	const NativeLinuxV2FixtureProfiles &profiles,
	NativeLinuxV2GenerationFactory &generations,
	NativeLinuxV2RecoveryFactory &recoveries,
	std::unique_ptr<NativeLinuxV2Context> *context);
#endif

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
