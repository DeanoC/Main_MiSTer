// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_core_profile.hpp"

#include <assert.h>
#include <string.h>
#include <type_traits>

namespace mister {
namespace native {
namespace {

static_assert(sizeof(((NativeInputProfile *)0)->player_command[0]) == sizeof(uint16_t),
	"player command width is part of the immutable schema");

void TestExactFixtureProfiles()
{
	const NativeCoreProfile *snes = FixtureNativeCoreProfile("snes");
	assert(snes != nullptr);
	assert(strcmp(snes->system, "snes") == 0);
	assert(strcmp(snes->core, "SNES") == 0);
	assert(strcmp(snes->extensions[0], "sfc") == 0);
	assert(strcmp(snes->extensions[1], "smc") == 0);
	assert(strcmp(snes->extensions[2], "bin") == 0);
	assert(strcmp(snes->artifact.sha256,
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0);
	assert(snes->artifact.size == 1048576);
	assert(snes->input.player_command[0] == 0x02);
	assert(snes->input.player_command[1] == 0x03);
	assert(!snes->input.joystick_swap);
	assert(snes->authority == NativeProfileAuthority::fixture);
	assert(snes->system_id == NativeSystem::snes);
	assert(snes->input.system_id == NativeSystem::snes);
	assert(snes->input.fpga_io_version == 2);
	assert(snes->input.digital_word_count == 2);
	assert(snes->input.player_count == 2);
	assert(snes->input.player_command[0] == 0x02);
	assert(snes->input.player_command[1] == 0x03);
	assert(IsExactFixtureNativeCoreProfile(*snes));

	const NativeCoreProfile *mega = FixtureNativeCoreProfile("megadrive");
	assert(mega != nullptr);
	assert(strcmp(mega->system, "megadrive") == 0);
	assert(strcmp(mega->core, "MegaDrive") == 0);
	assert(strcmp(mega->extensions[0], "md") == 0);
	assert(strcmp(mega->extensions[1], "gen") == 0);
	assert(strcmp(mega->extensions[2], "bin") == 0);
	assert(strcmp(mega->artifact.sha256,
		"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210") == 0);
	assert(mega->artifact.size == 2097152);
	assert(mega->input.player_command[0] == 0x02);
	assert(mega->input.player_command[1] == 0x03);
	assert(!mega->input.joystick_swap);
	assert(mega->authority == NativeProfileAuthority::fixture);
	assert(mega->system_id == NativeSystem::megadrive);
	assert(mega->input.system_id == NativeSystem::megadrive);
	assert(mega->input.fpga_io_version == 2);
	assert(mega->input.digital_word_count == 2);
	assert(mega->input.player_count == 2);
	assert(IsExactFixtureNativeCoreProfile(*mega));

	assert(FixtureNativeCoreProfile("SNES") == nullptr);
	assert(FixtureNativeCoreProfile("MegaDrive") == nullptr);
	assert(ProductionNativeCoreProfile("snes") == nullptr);
	assert(ProductionNativeCoreProfile("megadrive") == nullptr);
}

void TestExtensionsAndRecordAuthorityAreExact()
{
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	assert(NativeCoreProfileAcceptsExtension(*profile, "sfc"));
	assert(NativeCoreProfileAcceptsExtension(*profile, "smc"));
	assert(NativeCoreProfileAcceptsExtension(*profile, "bin"));
	assert(!NativeCoreProfileAcceptsExtension(*profile, "SFC"));
	assert(!NativeCoreProfileAcceptsExtension(*profile, ".sfc"));
	assert(!NativeCoreProfileAcceptsExtension(*profile, "sfc/"));
	assert(!NativeCoreProfileAcceptsExtension(*profile, ""));

	NativeCoreProfile copy = *profile;
	assert(ValidateNativeCoreProfileRecord(*profile));
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy.authority = NativeProfileAuthority::untrusted;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.system = "MEGADRIVE";
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.core = nullptr;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.extensions[1] = "bad";
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.extensions[2] = nullptr;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.artifact.sha256 = nullptr;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.system_id = NativeSystem::megadrive;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.extensions[0] = "bad";
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.input.digital_word_count = 1;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.input.system_id = NativeSystem::megadrive;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.input.fpga_io_version = 3;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.input.player_count = 1;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.input.player_command[0] = 0;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.core = "MegaDrive";
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.artifact.sha256 =
		"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.artifact.size++;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.input.fpga_io_version++;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.input.player_command[1] = 0x04;
	assert(!ValidateNativeCoreProfileRecord(copy));
	copy = *profile;
	copy.input.joystick_swap = true;
	assert(!ValidateNativeCoreProfileRecord(copy));
	const NativeCoreProfile *mega = FixtureNativeCoreProfile("megadrive");
	assert(mega != nullptr);
	NativeCoreProfile mega_copy = *mega;
	mega_copy.system = "MEGADRIVE";
	assert(!ValidateNativeCoreProfileRecord(mega_copy));
	mega_copy = *mega;
	mega_copy.core = nullptr;
	assert(!ValidateNativeCoreProfileRecord(mega_copy));
	mega_copy = *mega;
	mega_copy.extensions[0] = nullptr;
	assert(!ValidateNativeCoreProfileRecord(mega_copy));
	mega_copy = *mega;
	mega_copy.artifact.sha256 = "BAD";
	assert(!ValidateNativeCoreProfileRecord(mega_copy));
	mega_copy = *mega;
	mega_copy.input.fpga_io_version = 1;
	assert(!ValidateNativeCoreProfileRecord(mega_copy));
	mega_copy = *mega;
	mega_copy.input.player_command[1] = 0;
	assert(!ValidateNativeCoreProfileRecord(mega_copy));
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestExactFixtureProfiles();
	mister::native::TestExtensionsAndRecordAuthorityAreExact();
	return 0;
}
