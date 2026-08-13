// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_core_protocol.hpp"
#include "runtime/native/native_core_protocol_session_state.hpp"

#include <string.h>

#include <vector>

namespace mister {
namespace native {

ActiveCoreProtocolSession::ActiveCoreProtocolSession() : state_()
{
}

ActiveCoreProtocolSession::~ActiveCoreProtocolSession()
{
	MarkProtocolSessionAbandoned(state_);
}

ActiveSelectedTransaction::ActiveSelectedTransaction()
	: state_()
{
}

ActiveSelectedTransaction::~ActiveSelectedTransaction()
{
}

CleanupCoreProtocolSession::CleanupCoreProtocolSession() : state_()
{
}

CleanupCoreProtocolSession::~CleanupCoreProtocolSession()
{
	MarkProtocolSessionAbandoned(state_);
}

RecoveryCoreProtocolSession::RecoveryCoreProtocolSession() : state_()
{
}

RecoveryCoreProtocolSession::~RecoveryCoreProtocolSession()
{
	MarkProtocolSessionAbandoned(state_);
}

namespace {

const uint32_t kNativeCoreIdentityMagic = 0x005ca623u;
const size_t kMaximumPayloadBytes = 4096;

bool BeforeDeadline(const NativeClock &clock, uint64_t deadline)
{
	return clock.NowMs() < deadline;
}

bool ExtensionWords(const char *extension, uint16_t *first, uint16_t *second)
{
	if (extension == nullptr || first == nullptr || second == nullptr) return false;
	if (strcmp(extension, "sfc") == 0) { *first = 0x2e53; *second = 0x4643; return true; }
	if (strcmp(extension, "smc") == 0) { *first = 0x2e53; *second = 0x4d43; return true; }
	if (strcmp(extension, "bin") == 0) { *first = 0x2e42; *second = 0x494e; return true; }
	if (strcmp(extension, "md") == 0) { *first = 0x2e4d; *second = 0x4400; return true; }
	if (strcmp(extension, "gen") == 0) { *first = 0x2e47; *second = 0x454e; return true; }
	return false;
}

void BuildStatusWords(const uint8_t status[16], uint16_t words[9])
{
	words[0] = 0x001e;
	for (size_t index = 0; index < 8; ++index)
		words[index + 1] = static_cast<uint16_t>(status[index * 2]) |
			static_cast<uint16_t>(status[index * 2 + 1]) << 8;
}

} // namespace

NativeCoreProtocol::NativeCoreProtocol(NativeClock &clock,
	NativeCoreProtocolIo &io)
	: clock_(clock), io_(io), last_status_sequence_(0)
{
}

MisterResult NativeCoreProtocol::Exchange(NativeSpiTarget target,
	const uint16_t *words, size_t count, uint16_t *responses,
	size_t response_capacity, bool begins_selected, bool ends_selected,
	uint64_t absolute_deadline_ms)
{
	if (words == nullptr || count == 0 ||
		(responses == nullptr && response_capacity != 0) ||
		(responses != nullptr && response_capacity < count))
		return MISTER_RESULT_INVALID_ARGUMENT;
	if (!BeforeDeadline(clock_, absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	const MisterResult result = io_.Exchange(target, words, count, responses,
		response_capacity, begins_selected, ends_selected, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	return BeforeDeadline(clock_, absolute_deadline_ms) ? MISTER_RESULT_OK :
		MISTER_RESULT_DEADLINE;
}

MisterResult NativeCoreProtocol::SendStatus(const uint8_t status[16],
	uint64_t absolute_deadline_ms)
{
	uint16_t words[9] = {};
	BuildStatusWords(status, words);
	return Exchange(NativeSpiTarget::user_io, words,
		sizeof(words) / sizeof(words[0]), nullptr, 0, false, false,
		absolute_deadline_ms);
}

MisterResult NativeCoreProtocol::CloseSelected(NativeSpiTarget target,
	uint64_t absolute_deadline_ms)
{
	if (!BeforeDeadline(clock_, absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	const MisterResult result = io_.CloseSelected(target, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	return BeforeDeadline(clock_, absolute_deadline_ms) ? MISTER_RESULT_OK :
		MISTER_RESULT_DEADLINE;
}

MisterResult NativeCoreProtocol::ValidateLive(const NativeCoreProfile &profile,
	uint64_t absolute_deadline_ms)
{
	NativeLiveCoreObservation live = {};
	if (!BeforeDeadline(clock_, absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	MisterResult result = io_.Probe(&live, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	if (!BeforeDeadline(clock_, absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	if (live.identity_magic != kNativeCoreIdentityMagic ||
		live.core_type != profile.protocol.exact_core_type ||
		live.file_io_width != profile.protocol.file_io_width ||
		live.fpga_io_version != profile.input.fpga_io_version)
		return MISTER_RESULT_PLATFORM;
	return MISTER_RESULT_OK;
}

MisterResult NativeCoreProtocol::ActivateFixtureForTest(
	const NativeCoreProfile &profile, NativeCoreProtocolContent &content,
	uint64_t absolute_deadline_ms)
{
	if (!ValidateNativeCoreProfileRecord(profile)) return MISTER_RESULT_UNSUPPORTED;
	// The independent SNES normalizer is not yet connected to this intermediate
	// protocol checkpoint. Reject its transform before opening content or
	// touching hardware rather than sending an untransformed raw image.
	if (profile.protocol.transform ==
		NativeContentTransform::snes_header_and_mirror)
		return MISTER_RESULT_UNSUPPORTED;
	NativeCoreProtocolContentDescription description = {};
	if (!BeforeDeadline(clock_, absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	MisterResult result = content.Describe(&description, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	if (description.extension == nullptr || description.size == 0 ||
		description.size < profile.protocol.minimum_source_bytes ||
		description.size > profile.protocol.maximum_source_bytes ||
		!NativeCoreProfileAcceptsExtension(profile, description.extension))
		return MISTER_RESULT_UNSUPPORTED;
	uint16_t extension_first = 0;
	uint16_t extension_second = 0;
	if (!ExtensionWords(description.extension, &extension_first, &extension_second))
		return MISTER_RESULT_UNSUPPORTED;
	result = ValidateLive(profile, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	const uint16_t memory_size[] = {0x0031, profile.protocol.sdram_size_word};
	result = Exchange(NativeSpiTarget::user_io, memory_size,
		2, nullptr, 0, false, false, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	uint8_t current_status[16] = {};
	memcpy(current_status, profile.protocol.initial_status,
		sizeof(current_status));
	current_status[0] |= 1;
	result = SendStatus(current_status, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	const size_t name_length = strlen(profile.core);
	std::vector<uint16_t> name_words(name_length + 2, 0);
	std::vector<uint16_t> name_responses(name_length + 2, 0);
	name_words[0] = 0x0014;
	result = Exchange(NativeSpiTarget::user_io, name_words.data(),
		name_words.size(), name_responses.data(), name_responses.size(), true, true,
		absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	for (size_t index = 0; index < name_length; ++index) {
		if (static_cast<uint8_t>(name_responses[index + 1]) !=
			static_cast<uint8_t>(profile.core[index])) return MISTER_RESULT_PLATFORM;
	}
	const uint8_t delimiter = static_cast<uint8_t>(name_responses[name_length + 1]);
	if (delimiter != ';' && delimiter != '\0') return MISTER_RESULT_PLATFORM;
	const uint16_t index_words[] = {0x0055, 0x0000};
	const uint16_t info_words[] = {0x0056, extension_first, extension_second};
	const uint16_t start_words[] = {0x0053, 0x00ff};
	result = Exchange(NativeSpiTarget::file_io, index_words, 2,
		nullptr, 0, false, false, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	result = Exchange(NativeSpiTarget::file_io, info_words, 3,
		nullptr, 0, false, false, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	result = Exchange(NativeSpiTarget::file_io, start_words, 2,
		nullptr, 0, false, false, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	for (uint64_t offset = 0; offset < description.size;) {
		const size_t remaining = static_cast<size_t>(description.size - offset >
			kMaximumPayloadBytes ? kMaximumPayloadBytes : description.size - offset);
		std::vector<uint8_t> bytes(remaining);
		result = content.ReadAt(offset, bytes.data(), bytes.size(), absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		std::vector<uint16_t> words;
		words.push_back(0x0054);
		if (profile.protocol.file_io_width == NativeFileIoWidth::byte_per_word) {
			for (size_t index = 0; index < bytes.size(); ++index)
				words.push_back(bytes[index]);
		} else {
			for (size_t index = 0; index < bytes.size(); index += 2) {
				uint16_t word = bytes[index];
				if (index + 1 < bytes.size())
					word |= static_cast<uint16_t>(bytes[index + 1]) << 8;
				words.push_back(word);
			}
		}
		result = Exchange(NativeSpiTarget::file_io, words.data(),
			words.size(), nullptr, 0, false, false, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		offset += remaining;
	}
	uint16_t status_words[] = {0x0029};
	uint16_t status_response[] = {0};
	result = Exchange(NativeSpiTarget::user_io, status_words, 1,
		status_response, 1, true, false, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	const uint8_t sequence = static_cast<uint8_t>(status_response[0]);
	if ((sequence & 0xf0) == 0xa0 && (sequence & 0x0f) != last_status_sequence_) {
		uint16_t words[8] = {};
		uint16_t received[8] = {};
		result = Exchange(NativeSpiTarget::user_io, words, 8, received,
			8, false, true, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		for (size_t index = 0; index < 8; ++index) {
			current_status[index * 2] = static_cast<uint8_t>(received[index]);
			current_status[index * 2 + 1] = static_cast<uint8_t>(received[index] >> 8);
		}
		current_status[0] &= ~static_cast<uint8_t>(1);
		result = SendStatus(current_status, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		last_status_sequence_ = sequence & 0x0f;
	} else {
		result = CloseSelected(NativeSpiTarget::user_io,
			absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
	}
	const uint16_t stop_words[] = {0x0053, 0x0000};
	result = Exchange(NativeSpiTarget::file_io, stop_words, 2,
		nullptr, 0, false, false, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	current_status[0] &= ~static_cast<uint8_t>(1);
	return SendStatus(current_status, absolute_deadline_ms);
}

} // namespace native
} // namespace mister
