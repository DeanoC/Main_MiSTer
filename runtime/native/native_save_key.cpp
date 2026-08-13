// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_save_key.hpp"

#include <string.h>

namespace mister {
namespace native {
namespace {

bool HexValue(char value, uint8_t *output)
{
	if (output == nullptr) return false;
	if (value >= '0' && value <= '9') {
		*output = static_cast<uint8_t>(value - '0');
		return true;
	}
	if (value >= 'a' && value <= 'f') {
		*output = static_cast<uint8_t>(value - 'a' + 10);
		return true;
	}
	return false;
}

} // namespace

const char *NativeSaveSystemToken(NativeSystem system_id)
{
	switch (system_id) {
	case NativeSystem::snes: return "snes";
	case NativeSystem::megadrive: return "megadrive";
	}
	return nullptr;
}

bool MakeNativeSaveKey(const NativeCoreProfile &profile,
	const char *content_sha256, NativeSaveKey *key)
{
	if (content_sha256 == nullptr || key == nullptr ||
		NativeSaveSystemToken(profile.system_id) == nullptr ||
		!ValidateNativeCoreProfileRecord(profile))
		return false;
	if (strlen(content_sha256) != 64) return false;
	NativeSaveKey normalized = {};
	normalized.system_id = profile.system_id;
	for (size_t index = 0; index < sizeof(normalized.content_sha256); ++index) {
		uint8_t high = 0;
		uint8_t low = 0;
		if (!HexValue(content_sha256[index * 2], &high) ||
			!HexValue(content_sha256[index * 2 + 1], &low)) return false;
		normalized.content_sha256[index] = static_cast<uint8_t>((high << 4) | low);
	}
	*key = normalized;
	return true;
}

bool NativeSaveKeyFileName(const NativeSaveKey &key, char *output,
	size_t output_capacity)
{
	if (output == nullptr || output_capacity < 69 ||
		NativeSaveSystemToken(key.system_id) == nullptr)
		return false;
	static const char hex[] = "0123456789abcdef";
	for (size_t index = 0; index < sizeof(key.content_sha256); ++index) {
		output[index * 2] = hex[key.content_sha256[index] >> 4];
		output[index * 2 + 1] = hex[key.content_sha256[index] & 15];
	}
	memcpy(output + 64, ".sav", 5);
	return true;
}

} // namespace native
} // namespace mister
