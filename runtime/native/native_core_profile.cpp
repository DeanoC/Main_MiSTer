// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_core_profile.hpp"

#include "runtime/mister_runtime.h"

#include <string.h>

namespace mister {
namespace native {
namespace {

#if defined(MISTER_NATIVE_PROFILE_TESTING)

const NativeSaveDirectoryAuthority kSnesSaveChain[] = {
	{"/", 0, 0, 0755, NativeSaveDirectoryRole::root_anchor,
		NativeSaveMountRelation::root_anchor,
		NativeSaveDeviceRelation::root_anchor},
	{"fogcast-fixture", 0, 0, 0755,
		NativeSaveDirectoryRole::immutable_parent,
		NativeSaveMountRelation::same_mount_as_parent,
		NativeSaveDeviceRelation::same_device_as_parent},
	{"saves", 1000, 1000, 0700, NativeSaveDirectoryRole::save_root,
		NativeSaveMountRelation::same_mount_as_parent,
		NativeSaveDeviceRelation::same_device_as_parent},
	{"snes", 1000, 1000, 0700,
		NativeSaveDirectoryRole::system_directory,
		NativeSaveMountRelation::same_mount_as_parent,
		NativeSaveDeviceRelation::same_device_as_parent}
};

const NativeSaveDirectoryAuthority kMegaDriveSaveChain[] = {
	{"/", 0, 0, 0755, NativeSaveDirectoryRole::root_anchor,
		NativeSaveMountRelation::root_anchor,
		NativeSaveDeviceRelation::root_anchor},
	{"fogcast-fixture", 0, 0, 0755,
		NativeSaveDirectoryRole::immutable_parent,
		NativeSaveMountRelation::same_mount_as_parent,
		NativeSaveDeviceRelation::same_device_as_parent},
	{"saves", 1000, 1000, 0700, NativeSaveDirectoryRole::save_root,
		NativeSaveMountRelation::same_mount_as_parent,
		NativeSaveDeviceRelation::same_device_as_parent},
	{"megadrive", 1000, 1000, 0700,
		NativeSaveDirectoryRole::system_directory,
		NativeSaveMountRelation::same_mount_as_parent,
		NativeSaveDeviceRelation::same_device_as_parent}
};

const NativeSaveRootAuthority kSnesSaveRoot = {
	"/fogcast-fixture/saves/snes", kSnesSaveChain,
	sizeof(kSnesSaveChain) / sizeof(kSnesSaveChain[0]), 1000, 1000, 0600};
const NativeSaveRootAuthority kMegaDriveSaveRoot = {
	"/fogcast-fixture/saves/megadrive", kMegaDriveSaveChain,
	sizeof(kMegaDriveSaveChain) / sizeof(kMegaDriveSaveChain[0]),
	1000, 1000, 0600};

const SafeSaveRecoveryRecord kSnesSaveRecovery = {
	NativeProfileAuthority::fixture, NativeSystem::snes, &kSnesSaveRoot,
	{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};
const SafeSaveRecoveryRecord kMegaDriveSaveRecovery = {
	NativeProfileAuthority::fixture, NativeSystem::megadrive,
	&kMegaDriveSaveRoot,
	{0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
	 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
	 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
	 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}};

const NativeCoreProfile kSnesProfile = {
	NativeSystem::snes, "snes", "SNES", {"sfc", "smc", "bin"},
	{"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
		1048576},
	{NativeSystem::snes, 2, 2, 2, {0x02, 0x03}, false},
	{0xa4, NativeFileIoWidth::byte_per_word, 0x1234,
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		NativeContentTransform::snes_header_and_mirror,
		0x8000, 8 * 1024 * 1024, 16 * 1024 * 1024},
	{NativeAudioRecipeId::fixture_synthetic_v1, 0x06, 0x10,
		NativeAudioNeutralProof::terminal_containment,
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
	{NativeVideoRecipeId::fixture_synthetic_v1,
		{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
		 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
		 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
		 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11},
		{0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
		 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
		 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
		 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22},
		MISTER_RESOURCE_NATIVE_VIDEO, {0x0400, 0x0000}, 0,
		0, 0, 0, 0x39, 0x10, 0x50, 0xff, 0x10, 0x50,
		true, true, true, true, true, true},
	{NativeSaveMode::single_slot_growable_block_file, 512, 512 * 1024,
		0, 0, 0, &kSnesSaveRoot},
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
	{NativeAudioRecipeId::fixture_synthetic_v1, 0x08, 0x10,
		NativeAudioNeutralProof::terminal_containment,
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
	{NativeVideoRecipeId::fixture_synthetic_v1,
		{0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
		 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
		 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
		 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33},
		{0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
		 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
		 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
		 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44},
		MISTER_RESOURCE_NATIVE_VIDEO, {0x0500, 0x0000}, 0,
		0, 0, 0, 0x39, 0x10, 0x50, 0xff, 0x10, 0x50,
		true, true, true, true, true, true},
	{NativeSaveMode::single_slot_growable_block_file, 512, 512 * 1024,
		0, 0, 0, &kMegaDriveSaveRoot},
	NativeProfileAuthority::fixture
};

const SafeAudioRecoveryRecord kSnesAudioRecovery = {
	{NativeProfileAuthority::fixture, NativeSystem::snes}, kSnesProfile.audio};
const SafeAudioRecoveryRecord kMegaDriveAudioRecovery = {
	{NativeProfileAuthority::fixture, NativeSystem::megadrive},
	kMegaDriveProfile.audio};
const SafeVideoRecoveryRecord kSnesVideoRecovery = {
	{NativeProfileAuthority::fixture, NativeSystem::snes}, kSnesProfile.video};
const SafeVideoRecoveryRecord kMegaDriveVideoRecovery = {
	{NativeProfileAuthority::fixture, NativeSystem::megadrive},
	kMegaDriveProfile.video};
const SafeAudioVideoRecoveryRecord kSnesAudioVideoRecovery = {
	{NativeProfileAuthority::fixture, NativeSystem::snes}, kSnesProfile.video};
const SafeAudioVideoRecoveryRecord kMegaDriveAudioVideoRecovery = {
	{NativeProfileAuthority::fixture, NativeSystem::megadrive},
	kMegaDriveProfile.video};

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
		memcmp(&left.audio, &right.audio, sizeof(left.audio)) != 0 ||
		memcmp(&left.video, &right.video, sizeof(left.video)) != 0 ||
		left.save.mode != right.save.mode ||
		left.save.sector_bytes != right.save.sector_bytes ||
		left.save.maximum_bytes != right.save.maximum_bytes ||
		left.save.advertised_empty_bytes != right.save.advertised_empty_bytes ||
		left.save.empty_fill_byte != right.save.empty_fill_byte ||
		left.save.slot != right.save.slot ||
		left.save.root_authority != right.save.root_authority ||
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

bool ValidAudioVideoRecord(const NativeCoreProfile &profile)
{
	const NativeAudioProfile &audio = profile.audio;
	const NativeVideoProfile &video = profile.video;
	if (audio.recipe != NativeAudioRecipeId::fixture_synthetic_v1 ||
		audio.neutral_proof != NativeAudioNeutralProof::terminal_containment ||
		video.recipe != NativeVideoRecipeId::fixture_synthetic_v1 ||
		video.affected_resource_flags != MISTER_RESOURCE_NATIVE_VIDEO ||
		video.framebuffer_disable_word != 0 || !video.edid_disabled ||
		!video.hotplug_disabled || !video.cec_disabled || !video.spd_disabled ||
		!video.hps_framebuffer_disabled || !video.coupled_transmitter ||
		video.adv7513_main_address != 0x39 ||
		video.power_on_value != 0x10 || video.power_down_value != 0x50 ||
		video.power_read_mask != 0xff || video.power_on_expected != 0x10 ||
		video.power_down_expected != 0x50)
		return false;
	return true;
}

bool ValidSaveRecord(const NativeCoreProfile &profile)
{
	const NativeSaveProfile &save = profile.save;
	const NativeSaveRootAuthority *const root = save.root_authority;
	if (save.mode != NativeSaveMode::single_slot_growable_block_file ||
		save.sector_bytes == 0 ||
		(save.sector_bytes & (save.sector_bytes - 1)) != 0 ||
		save.maximum_bytes == 0 ||
		save.maximum_bytes % save.sector_bytes != 0 ||
		save.advertised_empty_bytes > save.maximum_bytes ||
		(save.advertised_empty_bytes != 0 &&
		 save.advertised_empty_bytes % save.sector_bytes != 0) ||
		save.slot != 0 || root == nullptr || root->absolute_root == nullptr ||
		root->chain == nullptr || root->chain_count != 4 ||
		root->file_mode != 0600 || root->chain[0].component == nullptr ||
		strcmp(root->chain[0].component, "/") != 0 ||
		root->chain[0].role != NativeSaveDirectoryRole::root_anchor ||
		root->chain[3].role != NativeSaveDirectoryRole::system_directory)
		return false;
	return true;
}

#endif

} // namespace

const NativeCoreProfile *FixtureNativeCoreProfile(const char *system)
{
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	if (SameString(system, kSnesProfile.system)) return &kSnesProfile;
	if (SameString(system, kMegaDriveProfile.system))
		return &kMegaDriveProfile;
	return nullptr;
#else
	(void)system;
	return nullptr;
#endif
}

const NativeCoreProfile *ProductionNativeCoreProfile(const char *)
{
	return nullptr;
}

bool IsExactFixtureNativeCoreProfile(const NativeCoreProfile &profile)
{
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	return &profile == &kSnesProfile || &profile == &kMegaDriveProfile;
#else
	(void)profile;
	return false;
#endif
}

bool ValidateNativeCoreProfileRecord(const NativeCoreProfile &profile)
{
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	if (!IsExactFixtureNativeCoreProfile(profile)) return false;
	if (profile.authority != NativeProfileAuthority::fixture ||
		profile.input.system_id != profile.system_id ||
		profile.input.fpga_io_version != 2 ||
		profile.input.player_count != kNativePlayerCount ||
		profile.input.digital_word_count != kNativeDigitalWordCount ||
		profile.input.player_command[0] != 0x02 ||
		profile.input.player_command[1] != 0x03 ||
		profile.input.joystick_swap || !ValidProtocolRecord(profile) ||
		!ValidAudioVideoRecord(profile) || !ValidSaveRecord(profile))
		return false;
	return SameRecord(profile, profile.system != nullptr &&
		strcmp(profile.system, "snes") == 0 ? kSnesProfile : kMegaDriveProfile);
#else
	(void)profile;
	return false;
#endif
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
const SafeAudioRecoveryRecord *FixtureSafeAudioRecoveryRecordForTest(
	NativeSystem system_id)
{
	return system_id == NativeSystem::snes ? &kSnesAudioRecovery :
		system_id == NativeSystem::megadrive ? &kMegaDriveAudioRecovery : nullptr;
}

const SafeVideoRecoveryRecord *FixtureSafeVideoRecoveryRecordForTest(
	NativeSystem system_id)
{
	return system_id == NativeSystem::snes ? &kSnesVideoRecovery :
		system_id == NativeSystem::megadrive ? &kMegaDriveVideoRecovery : nullptr;
}

const SafeAudioVideoRecoveryRecord *FixtureSafeAudioVideoRecoveryRecordForTest(
	NativeSystem system_id)
{
	return system_id == NativeSystem::snes ? &kSnesAudioVideoRecovery :
		system_id == NativeSystem::megadrive ? &kMegaDriveAudioVideoRecovery : nullptr;
}

const SafeSaveRecoveryRecord *FixtureSafeSaveRecoveryRecordForTest(
	NativeSystem system_id)
{
	return system_id == NativeSystem::snes ? &kSnesSaveRecovery :
		system_id == NativeSystem::megadrive ? &kMegaDriveSaveRecovery : nullptr;
}

const NativeCoreProfile *FixtureNativeCoreProfileForSafeSaveRecoveryRecordForTest(
	const SafeSaveRecoveryRecord *record)
{
	return record == &kSnesSaveRecovery ? &kSnesProfile :
		record == &kMegaDriveSaveRecovery ? &kMegaDriveProfile : nullptr;
}

bool IsExactFixtureSafePeripheralRecoveryRecordForTest(
	const SafePeripheralRecoveryRecord *record)
{
	return record == &kSnesAudioRecovery.base ||
		record == &kMegaDriveAudioRecovery.base ||
		record == &kSnesVideoRecovery.base ||
		record == &kMegaDriveVideoRecovery.base ||
		record == &kSnesAudioVideoRecovery.base ||
		record == &kMegaDriveAudioVideoRecovery.base;
}

bool IsExactFixtureNativeSaveRootAuthorityForTest(
	const NativeSaveRootAuthority *authority)
{
	return authority == &kSnesSaveRoot || authority == &kMegaDriveSaveRoot;
}

bool IsExactFixtureSafeSaveRecoveryRecordForTest(
	const SafeSaveRecoveryRecord *record)
{
	return record == &kSnesSaveRecovery || record == &kMegaDriveSaveRecovery;
}
#endif

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
