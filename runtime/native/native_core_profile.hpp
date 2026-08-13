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

} // namespace native
} // namespace mister

#endif
