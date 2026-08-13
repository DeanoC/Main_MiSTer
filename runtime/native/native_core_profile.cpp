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
	{0xa4, NativeFileIoWidth::byte_per_word, 0x1234,
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		NativeContentTransform::snes_header_and_mirror,
		1, 8 * 1024 * 1024, 16 * 1024 * 1024},
	NativeProfileAuthority::fixture
};

const NativeCoreProfile kMegaDriveProfile = {
	NativeSystem::megadrive, "megadrive", "MegaDrive", {"md", "gen", "bin"},
	{"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",
		2097152},
	{NativeSystem::megadrive, 2, 2, 2, {0x02, 0x03}, false},
	{0xa8, NativeFileIoWidth::little_endian_byte_pairs, 0x4321,
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		NativeContentTransform::megadrive_raw,
		1, 8 * 1024 * 1024, 8 * 1024 * 1024},
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
		left.protocol.exact_core_type != right.protocol.exact_core_type ||
		left.protocol.file_io_width != right.protocol.file_io_width ||
		left.protocol.sdram_size_word != right.protocol.sdram_size_word ||
		left.protocol.transform != right.protocol.transform ||
		left.protocol.minimum_source_bytes != right.protocol.minimum_source_bytes ||
		left.protocol.maximum_source_bytes != right.protocol.maximum_source_bytes ||
		left.protocol.maximum_wire_bytes != right.protocol.maximum_wire_bytes ||
		left.authority != right.authority)
		return false;
	for (size_t index = 0; index < 3; ++index)
		if (!SameString(left.extensions[index], right.extensions[index]))
			return false;
	for (size_t index = 0; index < kNativeInitialStatusBytes; ++index)
		if (left.protocol.initial_status[index] !=
			right.protocol.initial_status[index]) return false;
	return true;
}

bool ValidProtocolRecord(const NativeCoreProfile &profile)
{
	const NativeCoreProtocolProfile &protocol = profile.protocol;
	if ((protocol.exact_core_type != 0xa4 && protocol.exact_core_type != 0xa8) ||
		(protocol.file_io_width != NativeFileIoWidth::byte_per_word &&
		 protocol.file_io_width != NativeFileIoWidth::little_endian_byte_pairs) ||
		protocol.sdram_size_word == 0 || (protocol.initial_status[0] & 1) != 0 ||
		protocol.minimum_source_bytes == 0 ||
		protocol.minimum_source_bytes > protocol.maximum_source_bytes ||
		protocol.maximum_source_bytes > protocol.maximum_wire_bytes)
		return false;
	if (profile.system == nullptr || profile.core == nullptr) return false;
	const size_t core_length = strlen(profile.core);
	if (core_length == 0 || core_length > kNativeMaximumCoreNameBytes ||
		strchr(profile.core, ';') != nullptr) return false;
	return (profile.system_id == NativeSystem::snes &&
		protocol.transform == NativeContentTransform::snes_header_and_mirror) ||
		(profile.system_id == NativeSystem::megadrive &&
		 protocol.transform == NativeContentTransform::megadrive_raw);
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
		profile.input.joystick_swap || !ValidProtocolRecord(profile))
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
