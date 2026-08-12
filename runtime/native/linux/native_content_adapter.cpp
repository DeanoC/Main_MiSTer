// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_content_adapter.hpp"

#include <string.h>

namespace mister {
namespace native {
namespace linux_native {
namespace {

Result ToResult(NativeArtifactResult result)
{
	switch (result) {
	case NativeArtifactResult::ok: return MISTER_RESULT_OK;
	case NativeArtifactResult::invalid_argument:
	case NativeArtifactResult::invalid_identity:
		return MISTER_RESULT_INVALID_ARGUMENT;
	case NativeArtifactResult::deadline: return MISTER_RESULT_DEADLINE;
	case NativeArtifactResult::cleanup_incomplete:
		return MISTER_RESULT_CLEANUP_INCOMPLETE;
	case NativeArtifactResult::insecure:
	case NativeArtifactResult::not_found:
	case NativeArtifactResult::changed:
	case NativeArtifactResult::digest_mismatch:
	case NativeArtifactResult::io:
	default: return MISTER_RESULT_PLATFORM;
	}
}

bool CopyView(char *output, size_t capacity, const char *input, size_t length)
{
	if (input == nullptr || length == 0 || length >= capacity) return false;
	memcpy(output, input, length);
	output[length] = '\0';
	return true;
}

} // namespace

NativeContentAdapter::NativeContentAdapter(const char *root,
	NativeFileSystem &filesystem)
	: root_(), root_valid_(false), filesystem_(filesystem), content_(),
	  authority_(), system_(), digest_(), extension_(), configured_(false)
{
	if (root == nullptr) return;
	const size_t length = strlen(root);
	if (length == 0 || length >= sizeof(root_)) return;
	memcpy(root_, root, length + 1);
	root_valid_ = true;
}

NativeContentAdapter::~NativeContentAdapter()
{
	CloseContentForProcessExit();
}

Result NativeContentAdapter::Configure(const NativeArtifactAuthority &authority)
{
	char system[sizeof(system_)] = {};
	char digest[sizeof(digest_)] = {};
	char extension[sizeof(extension_)] = {};
	if (content_.owns_descriptors() ||
		!CopyView(system, sizeof(system), authority.system,
			authority.system_length) ||
		!CopyView(digest, sizeof(digest), authority.sha256,
			authority.sha256_length) ||
		!CopyView(extension, sizeof(extension), authority.extension,
			authority.extension_length))
		return MISTER_RESULT_INVALID_ARGUMENT;
	memcpy(system_, system, sizeof(system_));
	memcpy(digest_, digest, sizeof(digest_));
	memcpy(extension_, extension, sizeof(extension_));
	authority_ = {system_, authority.system_length, digest_,
		authority.sha256_length, authority.size, extension_,
		authority.extension_length};
	configured_ = true;
	return MISTER_RESULT_OK;
}

NativeAcquisitionOutcome NativeContentAdapter::RetainContent(
	uint64_t absolute_deadline_ms)
{
	if (!root_valid_ || !configured_ || content_.owns_descriptors())
		return {MISTER_RESULT_INVALID_STATE, content_.owns_descriptors()};
	NativeCoreArtifactAdapter resolver(root_, filesystem_);
	const NativeArtifactResult result = resolver.ResolveHeld(authority_,
		absolute_deadline_ms, &content_);
	return {ToResult(result), content_.owns_descriptors()};
}

Result NativeContentAdapter::ReadAt(uint64_t offset, void *bytes, size_t count,
	uint64_t absolute_deadline_ms)
{
	if (!content_.valid()) return MISTER_RESULT_INVALID_STATE;
	return ToResult(content_.ReadAtForUse(offset, bytes, count,
		absolute_deadline_ms));
}

Result NativeContentAdapter::CloseContent(uint64_t absolute_deadline_ms)
{
	return ToResult(content_.CloseBefore(absolute_deadline_ms));
}

void NativeContentAdapter::CloseContentForProcessExit()
{
	content_.Close();
}

bool NativeContentAdapter::active() const { return content_.valid(); }
bool NativeContentAdapter::owns_descriptors() const
{
	return content_.owns_descriptors();
}

} // namespace linux_native
} // namespace native
} // namespace mister
