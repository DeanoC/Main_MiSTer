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

// A profile is an immutable record when obtained from one of the authority
// accessors below. Callers must not manufacture a record for admission.
struct NativeCoreProfile {
	NativeSystem system_id;
	const char *system;
	const char *core;
	const char *extensions[kNativeExtensionCount];
	NativeArtifactRecord artifact;
	NativeInputProfile input;
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
