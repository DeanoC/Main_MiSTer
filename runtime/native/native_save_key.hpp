// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_SAVE_KEY_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_SAVE_KEY_HPP

#include "runtime/native/native_core_profile.hpp"

#include <stddef.h>
#include <stdint.h>

namespace mister {
namespace native {

struct NativeSaveKey {
	NativeSystem system_id;
	uint8_t content_sha256[32];
};

// Accepts only the normalized, lower-case SHA-256 identifier already bound to
// launch content. It never accepts a path, game name, artifact digest, or core
// name.
bool MakeNativeSaveKey(const NativeCoreProfile &profile,
	const char *content_sha256, NativeSaveKey *key);
bool NativeSaveKeyFileName(const NativeSaveKey &key, char *output,
	size_t output_capacity);
const char *NativeSaveSystemToken(NativeSystem system_id);

} // namespace native
} // namespace mister

#endif
