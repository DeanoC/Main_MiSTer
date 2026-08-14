// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_core_protocol_io_adapter.hpp"
#include "runtime/native/native_containment.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_recovery.hpp"
#include "runtime/native/native_resources.hpp"

#include <assert.h>

#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mister {
namespace native {
namespace {

class FakeClock final : public NativeClock {
public:
	explicit FakeClock(uint64_t now_ms) : now_ms_(now_ms) {}
	uint64_t NowMs() const override { return now_ms_; }
	void SetNow(uint64_t now_ms) { now_ms_ = now_ms; }
	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t) override { return false; }

private:
	uint64_t now_ms_;
};

class MemoryContent final : public NativeContentResource {
public:
	MemoryContent() : describe_calls(0), read_calls(0), bytes_{0x11, 0x22, 0x33} {}
	NativeAcquisitionOutcome RetainContent(uint64_t) override
		{ return {MISTER_RESULT_OK, true}; }
	Result DescribeRetained(NativeRetainedContentDescription *description,
		uint64_t) override
	{
		++describe_calls;
		if (description == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		description->size = bytes_.size();
		description->extension_length = 2;
		description->extension[0] = 'm';
		description->extension[1] = 'd';
		description->extension[2] = '\0';
		description->extension[3] = '\0';
		return MISTER_RESULT_OK;
	}
	Result ReadRetainedAt(uint64_t offset, void *bytes, size_t count,
		uint64_t) override
	{
		++read_calls;
		if (bytes == nullptr || offset > bytes_.size() || count >
			bytes_.size() - static_cast<size_t>(offset))
			return MISTER_RESULT_INVALID_ARGUMENT;
		uint8_t *out = static_cast<uint8_t *>(bytes);
		for (size_t index = 0; index < count; ++index)
			out[index] = bytes_[static_cast<size_t>(offset) + index];
		return MISTER_RESULT_OK;
	}
	Result CloseContent(uint64_t) override { return MISTER_RESULT_OK; }
	void CloseContentForProcessExit() override {}

	int describe_calls;
	int read_calls;

private:
	std::vector<uint8_t> bytes_;
};

std::vector<uint8_t> ValidSnesBytes()
{
	std::vector<uint8_t> bytes(0x8000, 0);
	bytes[0] = 0x78;
	bytes[0x7fc0 + 0x15] = 0x20;
	bytes[0x7fc0 + 0x1a] = 0x33;
	bytes[0x7fc0 + 0x1c] = 0xff;
	bytes[0x7fc0 + 0x1d] = 0xff;
	bytes[0x7fc0 + 0x3c] = 0x00;
	bytes[0x7fc0 + 0x3d] = 0x80;
	return bytes;
}

uint64_t Fnv1a64(const std::vector<uint8_t> &bytes)
{
	uint64_t digest = UINT64_C(14695981039346656037);
	for (uint8_t byte : bytes) {
		digest ^= byte;
		digest *= UINT64_C(1099511628211);
	}
	return digest;
}

uint32_t Sha256RotateRight(uint32_t value, uint32_t count)
{
	return (value >> count) | (value << (32u - count));
}

void Sha256Transform(const uint8_t block[64], uint32_t state[8])
{
	static const uint32_t constants[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
		0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
		0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
		0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
		0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
		0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
		0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
		0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
		0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
	uint32_t schedule[64] = {};
	for (size_t index = 0; index < 16; ++index) {
		schedule[index] = (static_cast<uint32_t>(block[index * 4]) << 24) |
			(static_cast<uint32_t>(block[index * 4 + 1]) << 16) |
			(static_cast<uint32_t>(block[index * 4 + 2]) << 8) |
			static_cast<uint32_t>(block[index * 4 + 3]);
	}
	for (size_t index = 16; index < 64; ++index) {
		const uint32_t lower = Sha256RotateRight(schedule[index - 15], 7) ^
			Sha256RotateRight(schedule[index - 15], 18) ^
			(schedule[index - 15] >> 3);
		const uint32_t upper = Sha256RotateRight(schedule[index - 2], 17) ^
			Sha256RotateRight(schedule[index - 2], 19) ^
			(schedule[index - 2] >> 10);
		schedule[index] = schedule[index - 16] + lower +
			schedule[index - 7] + upper;
	}
	uint32_t a = state[0];
	uint32_t b = state[1];
	uint32_t c = state[2];
	uint32_t d = state[3];
	uint32_t e = state[4];
	uint32_t f = state[5];
	uint32_t g = state[6];
	uint32_t h = state[7];
	for (size_t index = 0; index < 64; ++index) {
		const uint32_t sigma1 = Sha256RotateRight(e, 6) ^
			Sha256RotateRight(e, 11) ^ Sha256RotateRight(e, 25);
		const uint32_t choice = (e & f) ^ ((~e) & g);
		const uint32_t temporary1 = h + sigma1 + choice + constants[index] +
			schedule[index];
		const uint32_t sigma0 = Sha256RotateRight(a, 2) ^
			Sha256RotateRight(a, 13) ^ Sha256RotateRight(a, 22);
		const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		const uint32_t temporary2 = sigma0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

void Sha256(const std::vector<uint8_t> &bytes, uint8_t digest[32])
{
	uint32_t state[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
	size_t offset = 0;
	while (bytes.size() - offset >= 64) {
		Sha256Transform(bytes.data() + offset, state);
		offset += 64;
	}
	uint8_t tail[128] = {};
	const size_t remainder = bytes.size() - offset;
	if (remainder != 0) memcpy(tail, bytes.data() + offset, remainder);
	tail[remainder] = 0x80;
	const size_t length_offset = remainder < 56 ? 56 : 120;
	const uint64_t bit_length = static_cast<uint64_t>(bytes.size()) * 8u;
	for (size_t index = 0; index < 8; ++index)
		tail[length_offset + index] = static_cast<uint8_t>(bit_length >>
			((7 - index) * 8));
	Sha256Transform(tail, state);
	if (length_offset == 120) Sha256Transform(tail + 64, state);
	for (size_t index = 0; index < 8; ++index) {
		digest[index * 4] = static_cast<uint8_t>(state[index] >> 24);
		digest[index * 4 + 1] = static_cast<uint8_t>(state[index] >> 16);
		digest[index * 4 + 2] = static_cast<uint8_t>(state[index] >> 8);
		digest[index * 4 + 3] = static_cast<uint8_t>(state[index]);
	}
}

void AssertValidSnesSha256(const std::vector<uint8_t> &bytes)
{
	static const uint8_t expected[32] = {
		0xd8, 0x50, 0x93, 0x73, 0x92, 0x74, 0xb4, 0x3a,
		0x1a, 0xdc, 0x29, 0x43, 0x31, 0x5e, 0x15, 0x15,
		0x2f, 0x52, 0x04, 0x41, 0x4c, 0x74, 0x9d, 0x64,
		0xbe, 0x5e, 0x44, 0x31, 0x05, 0xf4, 0x3c, 0xa6};
	uint8_t digest[32] = {};
	Sha256(bytes, digest);
	assert(memcmp(digest, expected, sizeof(expected)) == 0);
}

class SnesMemoryContent final : public NativeContentResource {
public:
	explicit SnesMemoryContent(const char *extension = "sfc",
		const std::vector<uint8_t> &bytes = ValidSnesBytes())
		: describe_calls(0), read_calls(0), pre_mapping_reads(0),
		  post_mapping_reads(0), describe_result(MISTER_RESULT_OK),
		  fail_read_call(0), fail_read_result(MISTER_RESULT_PLATFORM),
		  expire_on_read_call(0), expire_clock(nullptr), expire_at_ms(0),
		  described_size(bytes.size()), read_windows(),
		  extension_(extension), bytes_(bytes), map_calls_(nullptr) {}
	NativeAcquisitionOutcome RetainContent(uint64_t) override
		{ return {MISTER_RESULT_OK, true}; }
	Result DescribeRetained(NativeRetainedContentDescription *description,
		uint64_t) override
	{
		++describe_calls;
		if (description == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		if (describe_result != MISTER_RESULT_OK) return describe_result;
		description->size = described_size;
		description->extension_length = extension_.size();
		memset(description->extension, 0, sizeof(description->extension));
		memcpy(description->extension, extension_.data(), extension_.size());
		return MISTER_RESULT_OK;
	}
	Result ReadRetainedAt(uint64_t offset, void *bytes, size_t count,
		uint64_t) override
	{
		++read_calls;
		if (expire_clock != nullptr && read_calls == expire_on_read_call)
			expire_clock->SetNow(expire_at_ms);
		if (fail_read_call != 0 && read_calls == fail_read_call)
			return fail_read_result;
		if (bytes == nullptr || offset > bytes_.size() || count >
			bytes_.size() - static_cast<size_t>(offset))
			return MISTER_RESULT_PLATFORM;
		if (map_calls_ != nullptr && *map_calls_ != 0) ++post_mapping_reads;
		else ++pre_mapping_reads;
		read_windows.push_back({offset, count});
		memcpy(bytes, bytes_.data() + static_cast<size_t>(offset), count);
		return MISTER_RESULT_OK;
	}
	Result CloseContent(uint64_t) override { return MISTER_RESULT_OK; }
	void CloseContentForProcessExit() override {}
	void ObserveMapping(const int *map_calls) { map_calls_ = map_calls; }

	struct ReadWindow {
		uint64_t offset;
		size_t count;
	};

	int describe_calls;
	int read_calls;
	int pre_mapping_reads;
	int post_mapping_reads;
	Result describe_result;
	int fail_read_call;
	Result fail_read_result;
	int expire_on_read_call;
	FakeClock *expire_clock;
	uint64_t expire_at_ms;
	uint64_t described_size;
	std::vector<ReadWindow> read_windows;

private:
	std::string extension_;
	std::vector<uint8_t> bytes_;
	const int *map_calls_;
};

class FixtureSnesContent final : public NativeCoreProtocolContent {
public:
	explicit FixtureSnesContent(const std::vector<uint8_t> &bytes)
		: describe_calls(0), read_calls(0), bytes_(bytes) {}
	MisterResult Describe(NativeCoreProtocolContentDescription *description,
		uint64_t) override
	{
		++describe_calls;
		if (description == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		description->size = bytes_.size();
		description->extension = "sfc";
		return MISTER_RESULT_OK;
	}
	MisterResult ReadAt(uint64_t offset, void *bytes, size_t count,
		uint64_t) override
	{
		++read_calls;
		if (bytes == nullptr || offset > bytes_.size() || count >
			bytes_.size() - static_cast<size_t>(offset))
			return MISTER_RESULT_PLATFORM;
		memcpy(bytes, bytes_.data() + static_cast<size_t>(offset), count);
		return MISTER_RESULT_OK;
	}

	int describe_calls;
	int read_calls;

private:
	const std::vector<uint8_t> &bytes_;
};

struct FixtureSpiWord {
	NativeSpiTarget target;
	uint16_t word;
};

class FixtureSnesIo final : public NativeActiveCoreProtocolIo {
public:
	FixtureSnesIo() : words(), response_index_(0), responses_() {
		responses_.insert(responses_.end(), 11, 0);
		responses_.push_back(0);
		const char *name = "SNES";
		for (size_t index = 0; name[index] != '\0'; ++index)
			responses_.push_back(static_cast<uint8_t>(name[index]));
		responses_.push_back(';');
		responses_.insert(responses_.end(), 2 + 3 + 2 + 513 + 8 * 4097 + 1 +
			2 + 9, 0);
	}
	MisterResult Probe(NativeLiveCoreObservation *observation,
		uint64_t) override
	{
		if (observation == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		*observation = {0x005ca623, 0xa4, NativeFileIoWidth::byte_per_word, 2};
		return MISTER_RESULT_OK;
	}
	MisterResult Exchange(NativeSpiTarget target, const uint16_t *transmit_words,
		size_t word_count, uint16_t *received_words, size_t received_capacity,
		bool, bool, uint64_t) override
	{
		if (transmit_words == nullptr || word_count == 0 ||
			(received_words == nullptr && received_capacity != 0) ||
			(received_words != nullptr && received_capacity < word_count) ||
			response_index_ + word_count > responses_.size())
			return MISTER_RESULT_INVALID_ARGUMENT;
		for (size_t index = 0; index < word_count; ++index) {
			words.push_back({target, transmit_words[index]});
			if (received_words != nullptr)
				received_words[index] = responses_[response_index_ + index];
		}
		response_index_ += word_count;
		return MISTER_RESULT_OK;
	}
	MisterResult CloseSelected(NativeSpiTarget, uint64_t) override
		{ return MISTER_RESULT_OK; }
	bool consumed() const { return response_index_ == responses_.size(); }

	std::vector<FixtureSpiWord> words;

private:
	size_t response_index_;
	std::vector<uint16_t> responses_;
};

class ProtocolRecoveryIo final : public NativeRecoveryIo {
public:
	explicit ProtocolRecoveryIo(NativeCoreProtocol &protocol) : protocol_(protocol) {}
	Result CloseInputDescriptors(const OperationLease &, RecoveryResourceState *)
		override { return MISTER_RESULT_INVALID_STATE; }
	Result MuteAudio(const OperationLease &, RecoveryResourceState *)
		override { return MISTER_RESULT_INVALID_STATE; }
	Result PowerDownVideo(const OperationLease &, RecoveryResourceState *)
		override { return MISTER_RESULT_INVALID_STATE; }
	Result CloseContent(const OperationLease &, RecoveryResourceState *)
		override { return MISTER_RESULT_INVALID_STATE; }
	Result DisableCoreProtocol(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return protocol_.DisableStateless(lease, state);
	}

private:
	NativeCoreProtocol &protocol_;
};

class TerminalContainmentIo final : public NativeContainmentIo {
public:
	TerminalContainmentIo()
		: calls(0), mapping_held(true), descriptor_held(true),
		  core_gpo(0x40000000u), interface_module(0), sdr_port(0),
		  bridge_reset(7), remap(1) {}
	Result WriteCoreReset(const Access &, uint32_t mask, uint32_t value) override
	{
		++calls;
		if (mask != 0xc0000000u) return MISTER_RESULT_PLATFORM;
		core_gpo = value;
		return MISTER_RESULT_OK;
	}
	Result WriteInterfaceModule(const Access &, uint32_t value) override
		{ ++calls; interface_module = value; return MISTER_RESULT_OK; }
	Result WriteSdrPortControl(const Access &, uint32_t offset,
		uint32_t value) override
	{
		++calls;
		if (offset != 0x5080u) return MISTER_RESULT_PLATFORM;
		sdr_port = value;
		return MISTER_RESULT_OK;
	}
	Result WriteBridgeReset(const Access &, uint32_t value) override
		{ ++calls; bridge_reset = value; return MISTER_RESULT_OK; }
	Result WriteRemap(const Access &, uint32_t value) override
		{ ++calls; remap = value; return MISTER_RESULT_OK; }
	NativeManagerNeutralReceipt ReconcileManager(const Access &) override
	{
		const NativeManagerNeutralReceipt receipt = {
			MISTER_RESULT_OK, 0x2u, 0x2u, true, false, false};
		return receipt;
	}
	Result ReadCoreGpo(const Access &, uint32_t *value) override
		{ ++calls; *value = core_gpo; return MISTER_RESULT_OK; }
	Result ReadInterfaceModule(const Access &, uint32_t *value) override
		{ ++calls; *value = interface_module; return MISTER_RESULT_OK; }
	Result ReadSdrPortControl(const Access &, uint32_t offset,
		uint32_t *value) override
	{
		++calls;
		if (offset != 0x5080u) return MISTER_RESULT_PLATFORM;
		*value = sdr_port;
		return MISTER_RESULT_OK;
	}
	Result ReadBridgeReset(const Access &, uint32_t *value) override
		{ ++calls; *value = bridge_reset; return MISTER_RESULT_OK; }
	Result ReadRemap(const Access &, uint32_t *value) override
		{ ++calls; *value = remap; return MISTER_RESULT_OK; }
	Result ReleaseMappings(const Access &) override
	{
		++calls;
		mapping_held = false;
		descriptor_held = false;
		return MISTER_RESULT_OK;
	}

	int calls;
	bool mapping_held;
	bool descriptor_held;
	uint32_t core_gpo;
	uint32_t interface_module;
	uint32_t sdr_port;
	uint32_t bridge_reset;
	uint32_t remap;
};

class FakeOperations final : public linux_native::NativeCoreProtocolIoTestOperations {
public:
	enum class Operation {
		open,
		close,
		map,
		unmap,
		read_gpo,
		read_gpi,
		write_gpo,
		barrier,
	};
	struct OperationCall {
		int number;
		Operation operation;
		size_t gpi_index;
	};
	struct SpiWord {
		NativeSpiTarget target;
		uint16_t word;
	};

	FakeOperations() : page_size(4096), page_size_calls(0), gpo(0),
		gpi_samples(), gpi_index(0), map_calls(0),
		unmap_calls(0), close_calls(0), fail_unmap_calls(0),
		fail_close_calls(0), fail_write_at(0), write_calls(0), fail_at(0),
		action_clock(nullptr), action_at_call(0), action_time_ms(0),
		calls(0), operation_calls(), gpo_writes(), spi_words() {}
	bool Step(Operation operation)
	{
		++calls;
		operation_calls.push_back({calls, operation, gpi_index});
		if (action_clock != nullptr && calls == action_at_call) {
			action_clock->SetNow(action_time_ms);
			action_at_call = 0;
		}
		return fail_at != 0 && calls == fail_at;
	}
	size_t PageSize() const override { ++page_size_calls; return page_size; }
	int Open(const char *, int) override { return Step(Operation::open) ? -1 : 9; }
	int Close(int descriptor) override
	{
		if (Step(Operation::close)) return -1;
		if (descriptor != 9) return -1;
		++close_calls;
		if (fail_close_calls != 0) {
			--fail_close_calls;
			return -1;
		}
		return 0;
	}
	int Map(int descriptor, uint64_t page_offset, size_t length,
		linux_native::NativeCoreProtocolIoTestMapping *mapping) override
	{
		if (Step(Operation::map)) return -1;
		if (descriptor != 9 || page_offset != 0xff706000u || length != 4096 ||
			mapping == nullptr) return -1;
		mapping->identity = 0x1000;
		++map_calls;
		return 0;
	}
	int Unmap(const linux_native::NativeCoreProtocolIoTestMapping &mapping,
		size_t length) override
	{
		if (Step(Operation::unmap)) return -1;
		if (mapping.identity != 0x1000 || length != 4096) return -1;
		++unmap_calls;
		if (fail_unmap_calls != 0) {
			--fail_unmap_calls;
			return -1;
		}
		return 0;
	}
	int Read32(const linux_native::NativeCoreProtocolIoTestMapping &mapping,
		size_t offset, uint32_t *value) override
	{
		if (Step(offset == 0x10 ? Operation::read_gpo : Operation::read_gpi))
			return -1;
		if (mapping.identity != 0x1000 || value == nullptr) return -1;
		if (offset == 0x10) {
			*value = gpo;
			return 0;
		}
		if (offset != 0x14 || gpi_index >= gpi_samples.size()) return -1;
		*value = gpi_samples[gpi_index++];
		return 0;
	}
	int Write32(const linux_native::NativeCoreProtocolIoTestMapping &mapping,
		size_t offset, uint32_t value) override
	{
		if (Step(Operation::write_gpo)) return -1;
		if (mapping.identity != 0x1000 || offset != 0x10) return -1;
		++write_calls;
		if (fail_write_at != 0 && write_calls == fail_write_at) return -1;
		const uint32_t select_mask = 0x00140000u;
		if ((gpo & select_mask) != 0 && (value & select_mask) != 0 &&
			(gpo & 0x00020000u) == 0 && (value & 0x00020000u) == 0) {
			spi_words.push_back({(value & 0x00100000u) != 0 ?
				NativeSpiTarget::user_io : NativeSpiTarget::file_io,
				static_cast<uint16_t>(value)});
		}
		gpo = value;
		gpo_writes.push_back(value);
		return 0;
	}
	int OrderingBarrier() override { return Step(Operation::barrier) ? -1 : 0; }

	void ScriptMegaDriveActivation()
	{
		gpi_samples.push_back(0x5ca623a8u);
		gpi_samples.push_back(0x00010000u);
		gpi_samples.push_back(0x00080000u);
		std::vector<uint16_t> responses;
		responses.insert(responses.end(), 11, 0);
		responses.push_back(0);
		const char *name = "MegaDrive";
		for (size_t index = 0; name[index] != '\0'; ++index)
			responses.push_back(static_cast<uint8_t>(name[index]));
		responses.push_back(';');
		responses.insert(responses.end(), 10, 0);
		responses.push_back(0);
		responses.insert(responses.end(), 11, 0);
		for (uint16_t response : responses) {
			gpi_samples.push_back(0x00020000u);
			gpi_samples.push_back(response);
		}
	}

	void ScriptSnesActivation()
	{
		gpi_samples.push_back(0x5ca623a4u);
		gpi_samples.push_back(0x00000000u);
		gpi_samples.push_back(0x00080000u);
		std::vector<uint16_t> responses;
		responses.insert(responses.end(), 11, 0);
		responses.push_back(0);
		const char *name = "SNES";
		for (size_t index = 0; name[index] != '\0'; ++index)
			responses.push_back(static_cast<uint8_t>(name[index]));
		responses.push_back(';');
		responses.insert(responses.end(), 2 + 3 + 2 + 513 + 8 * 4097 + 1 +
			2 + 9, 0);
		for (uint16_t response : responses) {
			gpi_samples.push_back(0x00020000u);
			gpi_samples.push_back(response);
		}
	}

	void ScriptMegaDriveFailureAfterDownloadStart()
	{
		ScriptMegaDriveActivation();
		// 3 live-probe reads, then high/low ACK samples for 29 successful
		// words through the file-I/O start command. The next ACK belongs to
		// the first payload word, after download residue is positive.
		const size_t first_payload_ack = 3 + 2 * 29;
		gpi_samples.resize(first_payload_ack + 1);
		gpi_samples[first_payload_ack] = 0x80000000u;
		// Cleanup carries the exact profile but has no active probe capability;
		// it sends only the profile-authorized file-I/O stop command.
		for (size_t index = 0; index != 2; ++index) {
			gpi_samples.push_back(0x00020000u);
			gpi_samples.push_back(0);
		}
	}

	void ScriptCleanupStop()
	{
		for (size_t index = 0; index != 2; ++index) {
			gpi_samples.push_back(0x00020000u);
			gpi_samples.push_back(0);
		}
	}

	void FailAckHighForRecipeWord(size_t word_index)
	{
		const size_t sample_index = 3 + word_index * 2;
		assert(sample_index < gpi_samples.size());
		gpi_samples.resize(sample_index + 1);
		gpi_samples[sample_index] = 0x80000000u;
	}

	void FailAckLowForRecipeWord(size_t word_index)
	{
		const size_t sample_index = 3 + word_index * 2 + 1;
		assert(sample_index < gpi_samples.size());
		gpi_samples.resize(sample_index + 1);
		gpi_samples[sample_index] = 0x80000000u;
	}

	size_t page_size;
	mutable int page_size_calls;
	uint32_t gpo;
	std::vector<uint32_t> gpi_samples;
	size_t gpi_index;
	int map_calls;
	int unmap_calls;
	int close_calls;
	int fail_unmap_calls;
	int fail_close_calls;
	int fail_write_at;
	int write_calls;
	int fail_at;
	FakeClock *action_clock;
	int action_at_call;
	uint64_t action_time_ms;
	int calls;
	std::vector<OperationCall> operation_calls;
	std::vector<uint32_t> gpo_writes;
	std::vector<SpiWord> spi_words;
};

void PrepareCleanup(HardwareBroker &broker, const NativeCoreProfile &profile,
	PlatformGenerationId *generation,
	std::unique_ptr<CleanupEpoch> *epoch)
{
	assert(broker.EnterFixtureForTest(profile, generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active;
	assert(broker.Begin(*generation, OperationKind::input, 2000, &active) ==
		MISTER_RESULT_OK);
	active.reset();
	assert(broker.Quiesce(*generation, 2000) == MISTER_RESULT_OK);
	assert(broker.BeginCleanup(*generation, 3000, 6000, epoch) == MISTER_RESULT_OK);
}

void AssertSnesPreflightFailureIsMutationFree(SnesMemoryContent &content,
	MisterResult expected, int deadline_offset_ms = 0);

void FinishSnesCleanupToBrokerIdle(HardwareBroker &broker,
	PlatformGenerationId generation, std::unique_ptr<CleanupEpoch> *epoch,
	std::unique_ptr<OperationLease> *protocol_cleanup)
{
	assert(epoch != nullptr && epoch->get() != nullptr &&
		protocol_cleanup != nullptr && protocol_cleanup->get() != nullptr);
	assert(!broker.core_protocol_session_current_for_test());
	assert(!broker.core_protocol_session_abandoned_for_test(
		**protocol_cleanup));
	protocol_cleanup->reset();

	TerminalContainmentIo containment_io;
	NativeContainment containment(broker, containment_io);
	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginCleanupOperation(**epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	assert(containment.ResetAndContain(**epoch, *terminal) == MISTER_RESULT_OK);
	assert(containment_io.calls == 11 && !containment_io.mapping_held &&
		!containment_io.descriptor_held);
	assert(broker.containment_receipt_sequence_for_test() != 0 &&
		broker.containment_receipt_sequence_for_test() ==
		broker.mutation_sequence_for_test());
	assert(!broker.core_protocol_session_current_for_test());
	assert(!broker.core_protocol_session_abandoned_for_test(*terminal));
	std::unique_ptr<OperationLease> rejected;
	assert(broker.BeginCleanupOperation(**epoch, OperationKind::save,
		&rejected) == MISTER_RESULT_INVALID_STATE);
	terminal.reset();
	assert(broker.Leave(generation, std::move(*epoch)) == MISTER_RESULT_OK);
	assert(epoch->get() == nullptr && !broker.has_live_generation_for_test());
	assert(!broker.core_protocol_session_current_for_test());
	assert(broker.Begin(generation, OperationKind::core_protocol, UINT64_MAX,
		&rejected) == MISTER_RESULT_INVALID_STATE);
}

void TestMegaDriveActivationUsesOneTypedMappingAndExactRelease()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.ScriptMegaDriveActivation();
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	MemoryContent content;
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &lease) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
		content);
	assert(outcome.result == MISTER_RESULT_OK && outcome.acquired);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1);
	assert((operations.gpo & 0x00160000u) == 0);
	assert(operations.gpi_index == operations.gpi_samples.size());
}

void TestSnesActivationUsesOneTypedMappingAndExactRelease()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.ScriptSnesActivation();
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	const std::vector<uint8_t> bytes = ValidSnesBytes();
	// Frozen deterministic fixture: SHA-256
	// d85093739274b43a1adc2943315e15152f5204414c749d64be5e443105f43ca6.
	AssertValidSnesSha256(bytes);
	assert(Fnv1a64(bytes) == UINT64_C(0x2b4211a25f8b098c));
	SnesMemoryContent content("sfc", bytes);
	content.ObserveMapping(&operations.map_calls);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &lease) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
		content);
	assert(outcome.result == MISTER_RESULT_OK && outcome.acquired &&
		!outcome.broker_failure_completed);
	assert(content.describe_calls == 1 && content.read_calls > 0);
	assert(content.pre_mapping_reads > 0 && content.post_mapping_reads == 0x8000);
	assert(content.pre_mapping_reads == 26);
	const SnesMemoryContent::ReadWindow expected_preflight[] = {
		{32764, 2}, {32734, 2}, {32732, 2}, {0, 1}, {32725, 1},
		{32730, 1}, {32726, 1}, {32727, 1}, {32728, 1}, {32729, 1},
		{32704, 21}, {0, 14}, {32704, 32}, {32704, 32}, {32704, 64},
		{32728, 1}, {32727, 1}, {32729, 1}, {32725, 1}, {32726, 1},
		{32730, 1}, {32690, 1}, {32691, 1}, {32693, 1}, {32694, 1},
		{32700, 1},
	};
	assert(content.read_windows.size() == 26 + 0x8000);
	for (size_t index = 0; index < 26; ++index) {
		assert(content.read_windows[index].offset ==
			expected_preflight[index].offset);
		assert(content.read_windows[index].count == expected_preflight[index].count);
	}
	for (size_t index = 0; index < 0x8000; ++index) {
		assert(content.read_windows[26 + index].offset == index);
		assert(content.read_windows[26 + index].count == 1);
	}
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 1);
	assert((operations.gpo & 0x00160000u) == 0);
	assert(operations.gpi_index == operations.gpi_samples.size());
	const size_t completed_word_count = operations.spi_words.size();
	lease.reset();
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&cleanup) == MISTER_RESULT_OK);
	assert(protocol.ShutdownLive(*cleanup) == MISTER_RESULT_OK);
	assert(operations.map_calls == 2 && operations.unmap_calls == 2 &&
		operations.close_calls == 2);
	assert(operations.spi_words.size() == completed_word_count);
	FinishSnesCleanupToBrokerIdle(broker, generation, &epoch, &cleanup);
	assert(operations.map_calls == 2 && operations.unmap_calls == 2 &&
		operations.close_calls == 2 &&
		operations.spi_words.size() == completed_word_count);
}

void TestSnesDeadlineBeforeAndDuringPlanningIsPreSession()
{
	{
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		SnesMemoryContent content;
		const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
			&lease) == MISTER_RESULT_OK);
		clock.SetNow(lease->absolute_deadline_ms());
		const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
			content);
		assert(outcome.result == MISTER_RESULT_DEADLINE && !outcome.acquired);
		assert(content.describe_calls == 0 && content.read_calls == 0);
		assert(operations.calls == 0 && broker.mutation_sequence_for_test() == 0);
	}
	for (int offset = -1; offset <= 1; ++offset) {
		SnesMemoryContent planner_boundary;
		planner_boundary.expire_on_read_call = 1;
		planner_boundary.fail_read_call = 2;
		AssertSnesPreflightFailureIsMutationFree(planner_boundary,
			offset < 0 ? MISTER_RESULT_PLATFORM : MISTER_RESULT_DEADLINE,
			offset);
		assert(planner_boundary.describe_calls == 1);
		assert(planner_boundary.read_calls == (offset < 0 ? 2 : 1));
		assert(planner_boundary.pre_mapping_reads == 1 &&
			planner_boundary.post_mapping_reads == 0);
		assert(planner_boundary.read_windows.size() == 1);
	}
}

void AssertSnesPostSessionDeadlineReceipt(HardwareBroker &broker,
	const NativeCoreProtocolOutcome &outcome, MisterResult primary,
	bool retained_mapping, bool expected_download)
{
	assert(outcome.result == primary && outcome.acquired &&
		outcome.broker_failure_completed);
	assert(!broker.core_protocol_session_current_for_test());
	ActiveProtocolFailureReceipt receipt = {};
	assert(broker.core_protocol_failure_receipt_for_test(&receipt));
	assert(receipt.primary_result == primary &&
		receipt.final_mutation_sequence != 0 &&
		receipt.final_mutation_sequence == broker.mutation_sequence_for_test());
	assert(receipt.mapping_release.mutation_sequence ==
		receipt.final_mutation_sequence);
	assert(receipt.mapping_release.mapping_absent == !retained_mapping &&
		receipt.mapping_release.descriptor_absent == !retained_mapping);
	assert(receipt.residue.mapping_retained == retained_mapping &&
		receipt.residue.download_may_be_active == expected_download &&
		receipt.residue.last_mutation_sequence ==
			receipt.final_mutation_sequence);
	if (retained_mapping) {
		assert(receipt.mapping_release.result == MISTER_RESULT_DEADLINE &&
			!receipt.mapping_release.selected_transaction_closed &&
			!receipt.mapping_release.unmap_attempted &&
			!receipt.mapping_release.descriptor_close_attempted);
	} else {
		assert(receipt.mapping_release.result == MISTER_RESULT_OK &&
			receipt.mapping_release.selected_transaction_closed &&
			receipt.mapping_release.unmap_attempted &&
			receipt.mapping_release.descriptor_close_attempted);
	}
}

void TestSnesAdapterDeadlineMatrixStopsBeforeProbeSuffix()
{
	for (int offset = -1; offset <= 1; ++offset) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		operations.ScriptSnesActivation();
		operations.action_clock = &clock;
		operations.action_at_call = 3;
		operations.action_time_ms = static_cast<uint64_t>(1100 + offset);
		operations.fail_at = 4;
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		SnesMemoryContent content;
		content.ObserveMapping(&operations.map_calls);
		const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::core_protocol, 1100,
			&lease) == MISTER_RESULT_OK);
		const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
			content);
		AssertSnesPostSessionDeadlineReceipt(broker, outcome,
			offset < 0 ? MISTER_RESULT_PLATFORM : MISTER_RESULT_DEADLINE,
			offset >= 0, false);
		assert(!broker.core_protocol_session_abandoned_for_test(*lease));
		assert(content.describe_calls == 1 && content.pre_mapping_reads == 26 &&
			content.post_mapping_reads == 0);
		assert(operations.operation_calls.size() >= 3);
		assert(operations.operation_calls[0].operation ==
			FakeOperations::Operation::open);
		assert(operations.operation_calls[1].operation ==
			FakeOperations::Operation::map);
		assert(operations.operation_calls[2].operation ==
			FakeOperations::Operation::read_gpo);
		if (offset < 0) {
			assert(operations.operation_calls.size() > 3);
			assert(operations.operation_calls[3].operation ==
				FakeOperations::Operation::write_gpo);
		} else {
			assert(operations.operation_calls.size() == 3);
		}
		assert(operations.gpi_index == 0 && operations.spi_words.empty());
		if (offset >= 0) assert(operations.gpo_writes.empty());
	}
}

void TestSnesPayloadReadDeadlineMatrixStopsBeforePayloadFrame()
{
	for (int offset = -1; offset <= 1; ++offset) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		operations.ScriptSnesActivation();
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		SnesMemoryContent content;
		content.ObserveMapping(&operations.map_calls);
		content.expire_on_read_call = 27;
		content.expire_clock = &clock;
		content.expire_at_ms = static_cast<uint64_t>(1100 + offset);
		content.fail_read_call = 28;
		const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::core_protocol, 1100,
			&lease) == MISTER_RESULT_OK);
		const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
			content);
		AssertSnesPostSessionDeadlineReceipt(broker, outcome,
			offset < 0 ? MISTER_RESULT_PLATFORM : MISTER_RESULT_DEADLINE,
			offset >= 0, true);
		assert(!broker.core_protocol_session_abandoned_for_test(*lease));
		assert(content.describe_calls == 1 && content.pre_mapping_reads == 26);
		assert(content.read_calls == (offset < 0 ? 28 : 27));
		assert(content.post_mapping_reads == 1 &&
			content.read_windows.size() == 27);
		assert(content.read_windows[26].offset == 0 &&
			content.read_windows[26].count == 1);
		assert(operations.spi_words.size() == 537);
		assert(operations.spi_words.back().word != 0x0054);
	}
}

void TestSnesPostStartFailureRetainsOnlyRetryableCleanupAuthority()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.ScriptSnesActivation();
	operations.FailAckHighForRecipeWord(24);
	operations.ScriptCleanupStop();
	operations.fail_unmap_calls = 1;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	SnesMemoryContent content;
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &active) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*active, profile,
		content);
	assert(outcome.result == MISTER_RESULT_PLATFORM && outcome.acquired &&
		outcome.broker_failure_completed);
	ActiveProtocolFailureReceipt receipt = {};
	assert(broker.core_protocol_failure_receipt_for_test(&receipt));
	assert(receipt.residue.download_may_be_active &&
		receipt.final_mutation_sequence != 0 &&
		broker.mutation_sequence_for_test() != 0);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 1);
	const size_t words_before_cleanup = operations.spi_words.size();
	active.reset();
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&cleanup) == MISTER_RESULT_OK);
	assert(protocol.ShutdownLive(*cleanup) == MISTER_RESULT_OK);
	assert(operations.map_calls == 1 && operations.unmap_calls == 2 &&
		operations.close_calls == 1);
	assert(operations.spi_words.size() == words_before_cleanup + 2);
	assert(operations.spi_words[words_before_cleanup].word == 0x0053 &&
		operations.spi_words[words_before_cleanup + 1].word == 0x0000);
	FinishSnesCleanupToBrokerIdle(broker, generation, &epoch, &cleanup);
	assert(operations.map_calls == 1 && operations.unmap_calls == 2 &&
		operations.close_calls == 1 &&
		operations.spi_words.size() == words_before_cleanup + 2);
}

void TestBrokeredAndFixtureSnesUseTheExactSamePreparedTranscript()
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	const std::vector<uint8_t> bytes = ValidSnesBytes();
	FakeClock fixture_clock(100);
	FixtureSnesIo fixture_io;
	FixtureSnesContent fixture_content(bytes);
	NativeCoreProtocol fixture_protocol(fixture_clock, fixture_io);
	assert(fixture_protocol.ActivateFixtureForTest(profile, fixture_content,
		1000) == MISTER_RESULT_OK);
	assert(fixture_content.describe_calls == 1 && fixture_io.consumed());

	FakeClock broker_clock(100);
	HardwareBroker broker(broker_clock);
	FakeOperations operations;
	operations.ScriptSnesActivation();
	linux_native::NativeCoreProtocolIoAdapter io(broker_clock, operations);
	NativeCoreProtocol broker_protocol(broker_clock, broker, io.capabilities());
	SnesMemoryContent broker_content("sfc", bytes);
	broker_content.ObserveMapping(&operations.map_calls);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &lease) ==
		MISTER_RESULT_OK);
	assert(broker_protocol.Activate(*lease, profile, broker_content).result ==
		MISTER_RESULT_OK);
	assert(broker_content.describe_calls == 1);
	assert(operations.spi_words.size() == fixture_io.words.size());
	for (size_t index = 0; index < fixture_io.words.size(); ++index) {
		assert(operations.spi_words[index].target == fixture_io.words[index].target);
		assert(operations.spi_words[index].word == fixture_io.words[index].word);
	}

	assert(operations.spi_words.size() == 33325);
	assert(operations.spi_words[0].word == 0x0031 &&
		operations.spi_words[1].word == 0x1234);
	assert(operations.spi_words[17].word == 0x0055 &&
		operations.spi_words[18].word == 0x0000);
	assert(operations.spi_words[19].word == 0x0056 &&
		operations.spi_words[20].word == 0x2e53 &&
		operations.spi_words[21].word == 0x4643);
	assert(operations.spi_words[22].word == 0x0053 &&
		operations.spi_words[23].word == 0x00ff);
	assert(operations.spi_words[24].word == 0x0054);
	assert(operations.spi_words[29].word == 0x00c0 &&
		operations.spi_words[30].word == 0x007f);
	assert(operations.spi_words[33].word == 0x0000 &&
		operations.spi_words[34].word == 0x0080);
	for (size_t frame = 0; frame < 8; ++frame) {
		const size_t command = 537 + frame * 4097;
		assert(operations.spi_words[command].target == NativeSpiTarget::file_io &&
			operations.spi_words[command].word == 0x0054);
		for (size_t index = 0; index < 4096; ++index)
			assert(operations.spi_words[command + 1 + index].word ==
				bytes[frame * 4096 + index]);
	}
	assert(operations.spi_words[33313].word == 0x0029);
	assert(operations.spi_words[33314].word == 0x0053 &&
		operations.spi_words[33315].word == 0x0000);
	assert(operations.spi_words[33316].word == 0x001e &&
		operations.spi_words[33317].word == 0x0000);
}

void AssertSnesPreflightFailureIsMutationFree(SnesMemoryContent &content,
	MisterResult expected, int deadline_offset_ms)
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	content.ObserveMapping(&operations.map_calls);
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &lease) ==
		MISTER_RESULT_OK);
	if (content.expire_on_read_call != 0) {
		content.expire_clock = &clock;
		content.expire_at_ms = static_cast<uint64_t>(
			static_cast<int64_t>(lease->absolute_deadline_ms()) +
			deadline_offset_ms);
	}
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
		content);
	assert(outcome.result == expected && !outcome.acquired &&
		!outcome.broker_failure_completed);
	assert(!broker.core_protocol_session_current_for_test());
	assert(!broker.core_protocol_session_abandoned_for_test(*lease));
	assert(broker.mutation_sequence_for_test() == 0);
	ActiveProtocolFailureReceipt receipt = {};
	assert(!broker.core_protocol_failure_receipt_for_test(&receipt));
	assert(operations.calls == 0 && operations.map_calls == 0 &&
		operations.gpo_writes.empty() && operations.spi_words.empty());
	lease.reset();
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&cleanup) == MISTER_RESULT_OK);
	assert(protocol.ShutdownLive(*cleanup) == MISTER_RESULT_OK);
	assert(operations.spi_words.empty());
}

void TestSnesValidationFailuresRemainBeforeTypedSession()
{
	SnesMemoryContent wrong_extension("zip");
	AssertSnesPreflightFailureIsMutationFree(wrong_extension,
		MISTER_RESULT_UNSUPPORTED);
	assert(wrong_extension.describe_calls == 1 && wrong_extension.read_calls == 0);

	SnesMemoryContent too_large;
	too_large.described_size = UINT64_C(8) * 1024 * 1024 + 1;
	AssertSnesPreflightFailureIsMutationFree(too_large,
		MISTER_RESULT_UNSUPPORTED);
	assert(too_large.describe_calls == 1 && too_large.read_calls == 0);

	SnesMemoryContent describe_failure;
	describe_failure.describe_result = MISTER_RESULT_PLATFORM;
	AssertSnesPreflightFailureIsMutationFree(describe_failure,
		MISTER_RESULT_PLATFORM);
	assert(describe_failure.describe_calls == 1 &&
		describe_failure.read_calls == 0);

	SnesMemoryContent too_short("sfc", std::vector<uint8_t>(0x7fff, 0));
	AssertSnesPreflightFailureIsMutationFree(too_short,
		MISTER_RESULT_UNSUPPORTED);
	assert(too_short.describe_calls == 1 && too_short.read_calls == 0);

	SnesMemoryContent planner_unsupported;
	planner_unsupported.fail_read_call = 1;
	planner_unsupported.fail_read_result = MISTER_RESULT_UNSUPPORTED;
	AssertSnesPreflightFailureIsMutationFree(planner_unsupported,
		MISTER_RESULT_UNSUPPORTED);
	assert(planner_unsupported.describe_calls == 1 &&
		planner_unsupported.read_calls == 1);
}

void TestEverySnesPlannerReadFailureRemainsBeforeTypedSession()
{
	// The frozen fixture performs this many candidate/header/scoring reads
	// before its first mapping. Each one is independently fallible.
	const int preflight_read_count = 26;
	for (int failed_read = 1; failed_read <= preflight_read_count; ++failed_read) {
		SnesMemoryContent content;
		content.fail_read_call = failed_read;
		AssertSnesPreflightFailureIsMutationFree(content, MISTER_RESULT_PLATFORM);
		assert(content.describe_calls == 1 && content.read_calls == failed_read);
	}
}

void TestConstructionAllocationFailureIsAtomicAndPreventsActivation()
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	// The construction graph deliberately has one allocation for the adapter's
	// inseparable four capability views and one for core-protocol teardown.
	// Neither failure may expose a null view, retain partially usable authority,
	// describe content, acquire a broker session, or touch the adapter backend.
	for (size_t allocation = 1; allocation != 3; ++allocation) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		SetNativeCoreProtocolAllocationFailureForTest(allocation);
		{
			linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
			NativeCoreProtocol protocol(clock, broker, io.capabilities());
			ClearNativeCoreProtocolAllocationFailureForTest();
			assert(!protocol.valid());
			if (allocation == 1) {
				assert(!io.valid());
				assert(!io.capabilities().Available());
			} else {
				assert(!io.valid());
			}
			MemoryContent content;
			PlatformGenerationId generation = 0;
			assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
			std::unique_ptr<OperationLease> lease;
			assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
				&lease) == MISTER_RESULT_OK);
			const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
				content);
			assert(outcome.result == MISTER_RESULT_PLATFORM && !outcome.acquired &&
				!outcome.broker_failure_completed);
			assert(content.describe_calls == 0 && content.read_calls == 0);
		}
		assert(operations.calls == 0 && operations.map_calls == 0 &&
			operations.gpo_writes.empty());
	}
}

void TestMixedBundlesCannotBeConstructedOrComplete()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations_a;
	FakeOperations operations_b;
	linux_native::NativeCoreProtocolIoAdapter adapter_a(clock, operations_a);
	linux_native::NativeCoreProtocolIoAdapter adapter_b(clock, operations_b);
	NativeCoreProtocolCapabilities capabilities_a = adapter_a.capabilities();
	NativeCoreProtocolCapabilities capabilities_b = adapter_b.capabilities();
	assert(capabilities_a.Available() && capabilities_b.Available());
	assert(!adapter_a.valid() && !adapter_b.valid());
	// The public constructor accepts exactly one cohesive value. It cannot be
	// expressed with active A and teardown B (or the reverse), so an unavailable
	// bundle fails closed without causing protocol broker or adapter activity.
	NativeCoreProtocolCapabilities absent;
	NativeCoreProtocol protocol(clock, broker, std::move(absent));
	assert(!protocol.valid());
	MemoryContent content;
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &active) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*active, profile,
		content);
	assert(outcome.result == MISTER_RESULT_PLATFORM && !outcome.acquired &&
		!outcome.broker_failure_completed);
	assert(content.describe_calls == 0 && operations_a.calls == 0 &&
		operations_b.calls == 0);
	active.reset();
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&cleanup) == MISTER_RESULT_OK);
	assert(protocol.ShutdownLive(*cleanup) == MISTER_RESULT_PLATFORM);
	assert(operations_a.calls == 0 && operations_b.calls == 0);
}

void TestSameBundleSurvivesDestroyedAdapterAndExternalHandle()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	NativeCoreProtocolCapabilities capabilities;
	std::unique_ptr<NativeCoreProtocol> protocol;
	{
		linux_native::NativeCoreProtocolIoAdapter adapter(clock, operations);
		capabilities = adapter.capabilities();
		assert(capabilities.Available());
		protocol.reset(new NativeCoreProtocol(clock, broker, std::move(capabilities)));
		assert(protocol->valid());
	}
	assert(!capabilities.Available());
	assert(protocol->valid());
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
		&epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	RecoveryResourceState state = RecoveryResourceState::unknown;
	assert(protocol->DisableStateless(*lease, &state) == MISTER_RESULT_OK);
	assert(state == RecoveryResourceState::neutral && operations.map_calls == 1 &&
		operations.unmap_calls == 1);
}

void TestCleanupRetryRetainsMappingAndUsesTheSameLease()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	PrepareCleanup(broker, profile, &generation, &epoch);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	operations.fail_unmap_calls = 1;
	assert(protocol.ShutdownLive(*lease) == MISTER_RESULT_PLATFORM);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1);
	assert(protocol.ShutdownLive(*lease) == MISTER_RESULT_OK);
	assert(operations.map_calls == 1 && operations.unmap_calls == 2);
}

void TestPostUnmapCloseRetryNeverRemaps()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	PrepareCleanup(broker, profile, &generation, &epoch);
	std::unique_ptr<OperationLease> owner;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&owner) == MISTER_RESULT_OK);
	operations.fail_close_calls = 1;
	assert(protocol.ShutdownLive(*owner) == MISTER_RESULT_PLATFORM);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 1);
	assert(protocol.ShutdownLive(*owner) == MISTER_RESULT_OK);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 2);
}

void TestPostUnmapCloseRetryRejectsForeignAndDoesNotExtendDeadline()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	PrepareCleanup(broker, profile, &generation, &epoch);
	std::unique_ptr<OperationLease> owner;
	std::unique_ptr<OperationLease> foreign;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&owner) == MISTER_RESULT_OK);
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&foreign) == MISTER_RESULT_OK);
	const uint64_t deadline = owner->absolute_deadline_ms();
	operations.fail_close_calls = 1;
	assert(protocol.ShutdownLive(*owner) == MISTER_RESULT_PLATFORM);
	assert(protocol.ShutdownLive(*foreign) == MISTER_RESULT_INVALID_STATE);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 1);
	clock.SetNow(deadline);
	assert(protocol.ShutdownLive(*owner) == MISTER_RESULT_DEADLINE);
	assert(owner->absolute_deadline_ms() == deadline);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 1);
}

std::vector<int> FinalDeselectBoundariesAfterAckLow(size_t gpi_index)
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.ScriptMegaDriveActivation();
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	MemoryContent content;
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
		&lease) == MISTER_RESULT_OK);
	assert(protocol.Activate(*lease, profile, content).result == MISTER_RESULT_OK);
	const FakeOperations::Operation expected[] = {
		FakeOperations::Operation::read_gpo,
		FakeOperations::Operation::write_gpo,
		FakeOperations::Operation::barrier,
		FakeOperations::Operation::read_gpo,
	};
	std::vector<int> boundaries;
	for (size_t index = 0; index + 4 <= operations.operation_calls.size(); ++index) {
		bool match = true;
		for (size_t offset = 0; offset != 4; ++offset) {
			const FakeOperations::OperationCall &call =
				operations.operation_calls[index + offset];
			if (call.gpi_index != gpi_index || call.operation != expected[offset]) {
				match = false;
				break;
			}
		}
		if (match) {
			for (size_t offset = 0; offset != 4; ++offset)
				boundaries.push_back(operations.operation_calls[index + offset].number);
			break;
		}
	}
	assert(boundaries.size() == 4);
	return boundaries;
}

void AssertFinalDeselectFailureReceipt(int boundary, bool expect_download,
	bool expect_reset)
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.ScriptMegaDriveActivation();
	operations.fail_at = boundary;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	MemoryContent content;
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
		&lease) == MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
		content);
	assert(outcome.result == MISTER_RESULT_PLATFORM && outcome.acquired &&
		outcome.broker_failure_completed);
	ActiveProtocolFailureReceipt receipt = {};
	assert(broker.core_protocol_failure_receipt_for_test(&receipt));
	assert(receipt.residue.download_may_be_active == expect_download);
	assert(receipt.residue.status_reset_asserted == expect_reset);
}

void TestPositiveFrameFinalDeselectBoundariesRecordResidue()
{
	// Each vector begins immediately after the final ACK-low of the exact frame.
	// The four fallible typed deselect primitives are GPO read, GPO write,
	// ordering barrier, and GPO readback. A reset/download effect is already
	// possible at this point, so every failure receipt must retain it.
	const std::vector<int> reset_assert = FinalDeselectBoundariesAfterAckLow(25);
	const std::vector<int> download_start = FinalDeselectBoundariesAfterAckLow(61);
	for (int boundary : reset_assert)
		AssertFinalDeselectFailureReceipt(boundary, false, true);
	for (int boundary : download_start)
		AssertFinalDeselectFailureReceipt(boundary, true, true);
}

void TestClearFrameFinalDeselectBoundariesDoNotClearResidue()
{
	const std::vector<int> download_stop = FinalDeselectBoundariesAfterAckLow(73);
	const std::vector<int> reset_clear = FinalDeselectBoundariesAfterAckLow(91);
	for (int boundary : download_stop)
		AssertFinalDeselectFailureReceipt(boundary, true, true);
	for (int boundary : reset_clear)
		AssertFinalDeselectFailureReceipt(boundary, false, true);
}

void TestActiveFailureTransfersOnlyRetainedMappingToCleanup()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	// This fails during payload transfer after the start command. The active
	// recipe therefore reports positive download residue after release leaves an
	// unmap retry pending. Cleanup must reuse that one mapping and issue its
	// profile-authorized stop—not map a fresh page or resurrect the completed
	// active registration.
	operations.ScriptMegaDriveFailureAfterDownloadStart();
	operations.fail_unmap_calls = 1;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	MemoryContent content;
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &active) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*active, profile,
		content);
	assert(outcome.result == MISTER_RESULT_PLATFORM && outcome.acquired &&
		outcome.broker_failure_completed);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 1);
	active.reset();
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&cleanup) == MISTER_RESULT_OK);
	assert(protocol.ShutdownLive(*cleanup) == MISTER_RESULT_OK);
	assert(operations.map_calls == 1 && operations.unmap_calls == 2 &&
		operations.close_calls == 1);
	assert(operations.gpi_index == operations.gpi_samples.size());
}

void TestSecondBundleMintCannotOmitRetainedDownloadStop()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.ScriptMegaDriveFailureAfterDownloadStart();
	operations.fail_unmap_calls = 1;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocolCapabilities first = io.capabilities();
	NativeCoreProtocolCapabilities second = io.capabilities();
	assert(first.Available() && !second.Available() && !io.valid());
	assert(operations.calls == 0 && operations.map_calls == 0);
	NativeCoreProtocol active_protocol(clock, broker, std::move(first));
	NativeCoreProtocol teardown_protocol(clock, broker, std::move(second));
	assert(active_protocol.valid() && !teardown_protocol.valid());
	MemoryContent content;
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &active) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = active_protocol.Activate(*active,
		profile, content);
	assert(outcome.result == MISTER_RESULT_PLATFORM && outcome.acquired &&
		outcome.broker_failure_completed);
	active.reset();
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&cleanup) == MISTER_RESULT_OK);
	const int calls_before_rejected_cleanup = operations.calls;
	assert(teardown_protocol.ShutdownLive(*cleanup) == MISTER_RESULT_PLATFORM);
	assert(operations.calls == calls_before_rejected_cleanup);
	assert(active_protocol.ShutdownLive(*cleanup) == MISTER_RESULT_OK);
	assert(operations.map_calls == 1 && operations.unmap_calls == 2 &&
		operations.gpi_index == operations.gpi_samples.size());
}

void TestProfilelessRecoveryOnlyForcesIdleAndReleases()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
		&epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	RecoveryResourceState state = RecoveryResourceState::unknown;
	assert(protocol.DisableStateless(*lease, &state) == MISTER_RESULT_OK);
	assert(state == RecoveryResourceState::neutral);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1);
	for (uint32_t write : operations.gpo_writes) {
		assert((write & 0x80000000u) == 0);
		assert((write & 0x0000ffffu) == 0);
	}
}

void TestInvalidPageSizeFailsBeforeMappingOrRegisterIo()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.page_size = 0;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
		&epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	RecoveryResourceState state = RecoveryResourceState::neutral;
	assert(protocol.DisableStateless(*lease, &state) == MISTER_RESULT_PLATFORM);
	assert(state == RecoveryResourceState::unknown);
	assert(operations.page_size_calls == 1 && operations.calls == 0 &&
		operations.map_calls == 0 && operations.gpo_writes.empty());
}

void TestProcessExitCloseMakesOnlyBestEffortRelease()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
		&epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	operations.fail_unmap_calls = 1;
	{
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		RecoveryResourceState state = RecoveryResourceState::neutral;
		assert(protocol.DisableStateless(*lease, &state) == MISTER_RESULT_PLATFORM);
		assert(state == RecoveryResourceState::unknown);
		assert(broker.core_protocol_session_abandoned_for_test(*lease));
	}
	// Process exit releases only the remaining physical residue; it never
	// converts the abandoned broker session to a successful completion.
	assert(operations.map_calls == 1 && operations.unmap_calls == 2 &&
		operations.close_calls == 1);
	assert(broker.core_protocol_session_abandoned_for_test(*lease));
}

void TestEveryAdapterBoundaryFailureRetainsATruthfulNonNeutralResult()
{
	// Profileless recovery exercises map, GPO read/write/barrier/readback,
	// unmap, and descriptor close without making any protocol exchange.
	for (int boundary = 1; boundary <= 12; ++boundary) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		operations.fail_at = boundary;
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
			&epoch) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::core_protocol, &lease) == MISTER_RESULT_OK);
		RecoveryResourceState state = RecoveryResourceState::neutral;
		assert(protocol.DisableStateless(*lease, &state) != MISTER_RESULT_OK);
		assert(state != RecoveryResourceState::neutral);
		assert(operations.calls >= boundary);
	}
}

void TestEveryActivationAdapterOperationBoundaryFailsClosed()
{
	// The complete fixture drives open, map, force-low/readback, identity
	// probe/restore, select, every data/strobe/ACK-high/ACK-low edge, selected
	// continuation, deselect/readback, unmap, and descriptor close. Injecting a
	// failure in each deterministic operation must never report activation OK.
	int total_operations = 0;
	{
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		operations.ScriptMegaDriveActivation();
		operations.fail_at = std::numeric_limits<int>::max();
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		MemoryContent content;
		const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
			&lease) == MISTER_RESULT_OK);
		assert(protocol.Activate(*lease, profile, content).result == MISTER_RESULT_OK);
		total_operations = operations.calls;
	}
	assert(total_operations > 0);
	for (int boundary = 1; boundary <= total_operations; ++boundary) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		operations.ScriptMegaDriveActivation();
		operations.fail_at = boundary;
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		MemoryContent content;
		const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
			&lease) == MISTER_RESULT_OK);
		const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease,
			profile, content);
		assert(outcome.result != MISTER_RESULT_OK);
		assert(operations.calls >= boundary);
	}
}

void TestTwoHundredProfilelessRecoveryCycles()
{
	for (size_t cycle = 0; cycle != 200; ++cycle) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		ProtocolRecoveryIo recovery_io(protocol);
		NativeRecovery recovery(broker, recovery_io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
			&epoch) == MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, OperationKind::core_protocol) ==
			MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = {};
		observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
		observation.struct_size = sizeof(observation);
		assert(recovery.Finish(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
	}
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestMegaDriveActivationUsesOneTypedMappingAndExactRelease();
	mister::native::TestSnesActivationUsesOneTypedMappingAndExactRelease();
	mister::native::TestBrokeredAndFixtureSnesUseTheExactSamePreparedTranscript();
	mister::native::TestSnesValidationFailuresRemainBeforeTypedSession();
	mister::native::TestEverySnesPlannerReadFailureRemainsBeforeTypedSession();
	mister::native::TestSnesDeadlineBeforeAndDuringPlanningIsPreSession();
	mister::native::TestSnesAdapterDeadlineMatrixStopsBeforeProbeSuffix();
	mister::native::TestSnesPayloadReadDeadlineMatrixStopsBeforePayloadFrame();
	mister::native::TestSnesPostStartFailureRetainsOnlyRetryableCleanupAuthority();
	mister::native::TestConstructionAllocationFailureIsAtomicAndPreventsActivation();
	mister::native::TestMixedBundlesCannotBeConstructedOrComplete();
	mister::native::TestSameBundleSurvivesDestroyedAdapterAndExternalHandle();
	mister::native::TestCleanupRetryRetainsMappingAndUsesTheSameLease();
	mister::native::TestPositiveFrameFinalDeselectBoundariesRecordResidue();
	mister::native::TestClearFrameFinalDeselectBoundariesDoNotClearResidue();
	mister::native::TestPostUnmapCloseRetryNeverRemaps();
	mister::native::TestPostUnmapCloseRetryRejectsForeignAndDoesNotExtendDeadline();
	mister::native::TestActiveFailureTransfersOnlyRetainedMappingToCleanup();
	mister::native::TestSecondBundleMintCannotOmitRetainedDownloadStop();
	mister::native::TestProfilelessRecoveryOnlyForcesIdleAndReleases();
	mister::native::TestInvalidPageSizeFailsBeforeMappingOrRegisterIo();
	mister::native::TestProcessExitCloseMakesOnlyBestEffortRelease();
	mister::native::TestEveryAdapterBoundaryFailureRetainsATruthfulNonNeutralResult();
	mister::native::TestEveryActivationAdapterOperationBoundaryFailsClosed();
	mister::native::TestTwoHundredProfilelessRecoveryCycles();
	return 0;
}
