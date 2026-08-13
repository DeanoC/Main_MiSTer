// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_CORE_PROFILE_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_CORE_PROFILE_HPP

#include <stddef.h>
#include <stdint.h>

namespace mister {
namespace native {

enum class NativeProfileAuthority : uint8_t {
	untrusted = 0,
	fixture = 1
};

enum class NativeSystem : uint8_t {
	snes,
	megadrive
};

static const uint8_t kNativePlayerCount = 2;
static const uint8_t kNativeDigitalWordCount = 2;
static const uint8_t kNativeExtensionCount = 3;
static const size_t kNativeInitialStatusBytes = 16;
static const size_t kNativeMaximumCoreNameBytes = 255;
static const size_t kNativeVideoWireWordCount = 2;

struct NativeArtifactRecord {
	const char *sha256;
	uint64_t size;
};

struct NativeInputProfile {
	NativeSystem system_id;
	uint8_t fpga_io_version;
	uint8_t player_count;
	uint8_t digital_word_count;
	uint16_t player_command[kNativePlayerCount];
	bool joystick_swap;
};

enum class NativeFileIoWidth : uint8_t {
	byte_per_word = 0,
	little_endian_byte_pairs = 1
};

enum class NativeContentTransform : uint8_t {
	snes_header_and_mirror = 0,
	megadrive_raw = 1
};

// Audio/video values are private, immutable fixture authority. Production
// records deliberately remain unavailable until Task 10 supplies image-bound
// provenance and recovery records.
enum class NativeAudioRecipeId : uint8_t {
	fixture_synthetic_v1,
	production_unavailable
};

enum class NativeAudioNeutralProof : uint8_t {
	terminal_containment,
	stable_state_readback
};

struct NativeAudioProfile {
	NativeAudioRecipeId recipe;
	uint8_t active_attenuation;
	uint8_t mute_attenuation;
	NativeAudioNeutralProof neutral_proof;
	uint8_t stable_readback_recipe_sha256[32];
};

enum class NativeVideoRecipeId : uint8_t {
	fixture_synthetic_v1,
	production_unavailable
};

struct NativeVideoProfile {
	NativeVideoRecipeId recipe;
	uint8_t recipe_sha256[32];
	uint8_t effect_manifest_sha256[32];
	uint32_t affected_resource_flags;
	uint16_t set_video_words[kNativeVideoWireWordCount];
	uint16_t framebuffer_disable_word;
	uint32_t i2c_device_major;
	uint32_t i2c_device_minor;
	uint16_t i2c_bus_number;
	uint8_t adv7513_main_address;
	uint8_t power_on_value;
	uint8_t power_down_value;
	uint8_t power_read_mask;
	uint8_t power_on_expected;
	uint8_t power_down_expected;
	bool edid_disabled;
	bool hotplug_disabled;
	bool cec_disabled;
	bool spd_disabled;
	bool hps_framebuffer_disabled;
	// The sole currently classified transmitter operation is ADV7513 0x41.
	// It is always owned by the separate coupled A/V registration.
	bool coupled_transmitter;
};

enum class NativeSaveDirectoryRole : uint8_t {
	root_anchor,
	immutable_parent,
	save_root,
	system_directory
};

enum class NativeSaveMountRelation : uint8_t {
	root_anchor,
	same_mount_as_parent,
	distinct_mount_from_parent
};

enum class NativeSaveDeviceRelation : uint8_t {
	root_anchor,
	same_device_as_parent,
	distinct_device_from_parent
};

struct NativeSaveDirectoryAuthority {
	const char *component;
	uint32_t uid;
	uint32_t gid;
	uint16_t exact_mode;
	NativeSaveDirectoryRole role;
	NativeSaveMountRelation mount_relation;
	NativeSaveDeviceRelation device_relation;
};

struct NativeSaveRootAuthority {
	const char *absolute_root;
	const NativeSaveDirectoryAuthority *chain;
	size_t chain_count;
	uint32_t file_uid;
	uint32_t file_gid;
	uint16_t file_mode;
};

enum class NativeSaveMode : uint8_t {
	single_slot_growable_block_file,
	unsupported
};

struct NativeSaveProfile {
	NativeSaveMode mode;
	uint32_t sector_bytes;
	uint64_t maximum_bytes;
	uint64_t advertised_empty_bytes;
	uint8_t empty_fill_byte;
	uint8_t slot;
	const NativeSaveRootAuthority *root_authority;
};

struct SafeSaveRecoveryRecord {
	NativeProfileAuthority authority;
	NativeSystem system_id;
	const NativeSaveRootAuthority *root_authority;
	uint8_t content_sha256[32];
};

struct SafePeripheralRecoveryRecord {
	NativeProfileAuthority authority;
	NativeSystem system_id;
};

struct SafeAudioRecoveryRecord {
	SafePeripheralRecoveryRecord base;
	NativeAudioProfile audio;
};

struct SafeVideoRecoveryRecord {
	SafePeripheralRecoveryRecord base;
	NativeVideoProfile video;
};

struct SafeAudioVideoRecoveryRecord {
	SafePeripheralRecoveryRecord base;
	NativeVideoProfile video;
};

// This is private fixture/profile authority. Production records remain
// unavailable until the image-locked Task 10 inputs exist.
struct NativeCoreProtocolProfile {
	uint8_t exact_core_type;
	NativeFileIoWidth file_io_width;
	uint16_t sdram_size_word;
	uint8_t initial_status[kNativeInitialStatusBytes];
	NativeContentTransform transform;
	uint64_t minimum_source_bytes;
	uint64_t maximum_source_bytes;
	uint64_t maximum_wire_bytes;
};

// A profile is an immutable record when obtained from one of the authority
// accessors below. Callers must not manufacture a record for admission.
struct NativeCoreProfile {
	NativeSystem system_id;
	const char *system;
	const char *core;
	const char *extensions[kNativeExtensionCount];
	NativeArtifactRecord artifact;
	NativeInputProfile input;
	NativeCoreProtocolProfile protocol;
	NativeAudioProfile audio;
	NativeVideoProfile video;
	NativeSaveProfile save;
	NativeProfileAuthority authority;
};

const NativeCoreProfile *FixtureNativeCoreProfile(const char *system);

// Production image-locked authority is intentionally unavailable until the
// image/provenance task supplies records.
const NativeCoreProfile *ProductionNativeCoreProfile(const char *system);

bool IsExactFixtureNativeCoreProfile(const NativeCoreProfile &profile);
bool ValidateNativeCoreProfileRecord(const NativeCoreProfile &profile);
bool NativeCoreProfileAcceptsExtension(const NativeCoreProfile &profile,
	const char *extension);

#if defined(MISTER_NATIVE_PROFILE_TESTING)
const SafeAudioRecoveryRecord *FixtureSafeAudioRecoveryRecordForTest(
	NativeSystem system_id);
const SafeVideoRecoveryRecord *FixtureSafeVideoRecoveryRecordForTest(
	NativeSystem system_id);
const SafeAudioVideoRecoveryRecord *FixtureSafeAudioVideoRecoveryRecordForTest(
	NativeSystem system_id);
const SafeSaveRecoveryRecord *FixtureSafeSaveRecoveryRecordForTest(
	NativeSystem system_id);
const NativeCoreProfile *FixtureNativeCoreProfileForSafeSaveRecoveryRecordForTest(
	const SafeSaveRecoveryRecord *record);
bool IsExactFixtureSafePeripheralRecoveryRecordForTest(
	const SafePeripheralRecoveryRecord *record);
bool IsExactFixtureNativeSaveRootAuthorityForTest(
	const NativeSaveRootAuthority *authority);
bool IsExactFixtureSafeSaveRecoveryRecordForTest(
	const SafeSaveRecoveryRecord *record);
#endif

} // namespace native
} // namespace mister

#endif
