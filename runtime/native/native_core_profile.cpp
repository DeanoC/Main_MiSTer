// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_core_profile.hpp"

#include <string.h>

namespace mister {
namespace native {
namespace {

const NativeCoreProfile kSnesProfile = {
	NativeSystem::snes, "snes", "SNES", {"sfc", "smc", "bin"},
	{"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
		1048576},
	{NativeSystem::snes, 2, 2, 2, {0x02, 0x03}, false},
	NativeProfileAuthority::fixture
};

const NativeCoreProfile kMegaDriveProfile = {
	NativeSystem::megadrive, "megadrive", "MegaDrive", {"md", "gen", "bin"},
	{"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",
		2097152},
	{NativeSystem::megadrive, 2, 2, 2, {0x02, 0x03}, false},
	NativeProfileAuthority::fixture
};

bool SameString(const char *left, const char *right)
{
	return left != nullptr && right != nullptr && strcmp(left, right) == 0;
}

bool SameRecord(const NativeCoreProfile &left,
	const NativeCoreProfile &right)
{
	if (!SameString(left.system, right.system) ||
		!SameString(left.core, right.core) ||
		left.artifact.size != right.artifact.size ||
		!SameString(left.artifact.sha256, right.artifact.sha256) ||
		left.system_id != right.system_id ||
		left.input.system_id != right.input.system_id ||
		left.input.fpga_io_version != right.input.fpga_io_version ||
		left.input.player_count != right.input.player_count ||
		left.input.digital_word_count != right.input.digital_word_count ||
		left.input.player_command[0] != right.input.player_command[0] ||
		left.input.player_command[1] != right.input.player_command[1] ||
		left.input.joystick_swap != right.input.joystick_swap ||
		left.authority != right.authority)
		return false;
	for (size_t index = 0; index < 3; ++index)
		if (!SameString(left.extensions[index], right.extensions[index]))
			return false;
	return true;
}

} // namespace

const NativeCoreProfile *FixtureNativeCoreProfile(const char *system)
{
	if (SameString(system, kSnesProfile.system)) return &kSnesProfile;
	if (SameString(system, kMegaDriveProfile.system))
		return &kMegaDriveProfile;
	return nullptr;
}

const NativeCoreProfile *ProductionNativeCoreProfile(const char *)
{
	return nullptr;
}

bool IsExactFixtureNativeCoreProfile(const NativeCoreProfile &profile)
{
	return &profile == &kSnesProfile || &profile == &kMegaDriveProfile;
}

bool ValidateNativeCoreProfileRecord(const NativeCoreProfile &profile)
{
	if (!IsExactFixtureNativeCoreProfile(profile)) return false;
	if (profile.authority != NativeProfileAuthority::fixture ||
		profile.input.system_id != profile.system_id ||
		profile.input.fpga_io_version != 2 ||
		profile.input.player_count != kNativePlayerCount ||
		profile.input.digital_word_count != kNativeDigitalWordCount ||
		profile.input.player_command[0] != 0x02 ||
		profile.input.player_command[1] != 0x03 ||
		profile.input.joystick_swap)
		return false;
	return SameRecord(profile, profile.system != nullptr &&
		strcmp(profile.system, "snes") == 0 ? kSnesProfile : kMegaDriveProfile);
}

bool NativeCoreProfileAcceptsExtension(const NativeCoreProfile &profile,
	const char *extension)
{
	if (!ValidateNativeCoreProfileRecord(profile) || extension == nullptr)
		return false;
	for (size_t index = 0; index < kNativeExtensionCount; ++index)
		if (strcmp(profile.extensions[index], extension) == 0) return true;
	return false;
}

} // namespace native
} // namespace mister
