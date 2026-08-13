// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Mechanical, bounded extraction of the SNES header/mirror behavior from
// Main_MiSTer support/snes/snes.cpp at a6fa625ca7c3a9bbafd5f917506aa9c274fb5d30
// (SHA-256 71223676dce2d8bc3f324158eb5dd318178c93969bede09743018d0b1f5bf35e).

#include "runtime/native/native_snes_content.hpp"

#include <limits.h>
#include <string.h>

namespace mister {
namespace native {
namespace {

enum HeaderField : uint64_t {
	kCartName = 0x00,
	kMapper = 0x15,
	kRomType = 0x16,
	kRomSize = 0x17,
	kRamSize = 0x18,
	kCartRegion = 0x19,
	kCompany = 0x1a,
	kComplement = 0x1c,
	kChecksum = 0x1e,
	kResetVector = 0x3c
};

const uint8_t kSnesCc92Header[] = {
	0x00, 0x08, 0x22, 0x02, 0x1c, 0x00, 0x10, 0x00,
	0x08, 0x65, 0x80, 0x84, 0x20, 0x00, 0x22, 0x25,
	0x00, 0x83, 0x0c, 0x80, 0x10, 0x00, 0x00, 0xa0,
	0x80, 0x01, 0x80, 0x80, 0x00, 0x01, 0x02, 0x2d
};

const uint8_t kSnesPf94_10kHeader[] = {
	0xc9, 0x80, 0x80, 0x44, 0x15, 0x00, 0x62, 0x09,
	0x29, 0xa0, 0x52, 0x70, 0x50, 0x12, 0x05, 0x35,
	0x31, 0x63, 0xc0, 0x22, 0x01, 0x80, 0xc2, 0x3a,
	0x6c, 0xb0, 0xe8, 0x4a, 0x11, 0x20, 0xc0, 0xf8
};

const uint8_t kSnesPf94_1mHeader[] = {
	0x50, 0x52, 0x45, 0x48, 0x49, 0x53, 0x54, 0x4f,
	0x52, 0x49, 0x4b, 0x20, 0x4d, 0x41, 0x4e, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x00, 0x0a,
	0x00, 0x01, 0x33, 0x00, 0xff, 0xff, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x2b, 0x80, 0x2b, 0x80,
	0x2b, 0x80, 0xfe, 0x91, 0x2b, 0x80, 0xa4, 0xf7,
	0xff, 0xff, 0xff, 0xff, 0x2b, 0x80, 0x2b, 0x80,
	0x2b, 0x80, 0x75, 0xf7, 0x00, 0x80, 0xa4, 0xf7
};

bool BeforeDeadline(const NativeClock &clock, uint64_t deadline)
{
	return clock.NowMs() < deadline;
}

bool Contains(uint64_t size, uint64_t offset, uint64_t count)
{
	return offset <= size && count <= size - offset;
}

MisterResult Read(const NativeClock &clock, NativeSnesContentSource &source,
	uint64_t source_offset, uint64_t source_size, uint64_t offset,
	void *bytes, size_t count, uint64_t deadline)
{
	if (bytes == nullptr || !Contains(source_size, offset, count))
		return MISTER_RESULT_PLATFORM;
	if (!BeforeDeadline(clock, deadline)) return MISTER_RESULT_DEADLINE;
	if (source_offset > UINT64_MAX - offset)
		return MISTER_RESULT_PLATFORM;
	const MisterResult result = source.ReadAt(source_offset + offset, bytes,
		count, deadline);
	if (result != MISTER_RESULT_OK) return result;
	return BeforeDeadline(clock, deadline) ? MISTER_RESULT_OK :
		MISTER_RESULT_DEADLINE;
}

MisterResult ReadByte(const NativeClock &clock, NativeSnesContentSource &source,
	uint64_t source_offset, uint64_t source_size, uint64_t offset,
	uint8_t *value, uint64_t deadline)
{
	return Read(clock, source, source_offset, source_size, offset, value, 1,
		deadline);
}

MisterResult ReadLe16(const NativeClock &clock, NativeSnesContentSource &source,
	uint64_t source_offset, uint64_t source_size, uint64_t offset,
	uint16_t *value, uint64_t deadline)
{
	uint8_t bytes[2] = {};
	const MisterResult result = Read(clock, source, source_offset, source_size,
		offset, bytes, sizeof(bytes), deadline);
	if (result != MISTER_RESULT_OK) return result;
	*value = static_cast<uint16_t>(bytes[0]) |
		static_cast<uint16_t>(bytes[1]) << 8;
	return MISTER_RESULT_OK;
}

MisterResult MatchesAt(const NativeClock &clock, NativeSnesContentSource &source,
	uint64_t source_offset, uint64_t source_size, uint64_t offset,
	const uint8_t *expected, size_t count, bool *matches, uint64_t deadline)
{
	if (expected == nullptr || matches == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	*matches = false;
	if (!Contains(source_size, offset, count)) return MISTER_RESULT_OK;
	uint8_t observed[64] = {};
	if (count > sizeof(observed)) return MISTER_RESULT_INVALID_ARGUMENT;
	const MisterResult result = Read(clock, source, source_offset, source_size,
		offset, observed, count, deadline);
	if (result != MISTER_RESULT_OK) return result;
	*matches = memcmp(observed, expected, count) == 0;
	return MISTER_RESULT_OK;
}

MisterResult MatchesTextAt(const NativeClock &clock,
	NativeSnesContentSource &source, uint64_t source_offset,
	uint64_t source_size, uint64_t offset, const char *expected, size_t count,
	bool *matches, uint64_t deadline)
{
	return MatchesAt(clock, source, source_offset, source_size, offset,
		reinterpret_cast<const uint8_t *>(expected), count, matches, deadline);
}

void PutLe32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = static_cast<uint8_t>(value);
	bytes[1] = static_cast<uint8_t>(value >> 8);
	bytes[2] = static_cast<uint8_t>(value >> 16);
	bytes[3] = static_cast<uint8_t>(value >> 24);
}

MisterResult ScoreHeader(const NativeClock &clock,
	NativeSnesContentSource &source, uint64_t source_offset,
	uint64_t source_size, uint64_t address, uint32_t *score,
	uint64_t deadline)
{
	if (score == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	*score = 0;
	if (!Contains(source_size, address, 64)) return MISTER_RESULT_OK;
	uint16_t reset_vector = 0;
	uint16_t checksum = 0;
	uint16_t complement = 0;
	MisterResult result = ReadLe16(clock, source, source_offset, source_size,
		address + kResetVector, &reset_vector, deadline);
	if (result != MISTER_RESULT_OK) return result;
	result = ReadLe16(clock, source, source_offset, source_size,
		address + kChecksum, &checksum, deadline);
	if (result != MISTER_RESULT_OK) return result;
	result = ReadLe16(clock, source, source_offset, source_size,
		address + kComplement, &complement, deadline);
	if (result != MISTER_RESULT_OK) return result;
	if (reset_vector < 0x8000) return MISTER_RESULT_OK;
	const uint64_t reset_opcode_offset =
		(address & ~static_cast<uint64_t>(0x7fff)) |
		(static_cast<uint64_t>(reset_vector) & 0x7fff);
	uint8_t reset_opcode = 0;
	uint8_t mapper = 0;
	result = ReadByte(clock, source, source_offset, source_size,
		reset_opcode_offset, &reset_opcode, deadline);
	if (result != MISTER_RESULT_OK) return result;
	result = ReadByte(clock, source, source_offset, source_size,
		address + kMapper, &mapper, deadline);
	if (result != MISTER_RESULT_OK) return result;
	mapper &= ~0x10u;
	int value = 0;
	if (reset_opcode == 0x78 || reset_opcode == 0x18 ||
		reset_opcode == 0x38 || reset_opcode == 0x9c ||
		reset_opcode == 0x4c || reset_opcode == 0x5c) value += 8;
	if (reset_opcode == 0xc2 || reset_opcode == 0xe2 ||
		reset_opcode == 0xad || reset_opcode == 0xae ||
		reset_opcode == 0xac || reset_opcode == 0xaf ||
		reset_opcode == 0xa9 || reset_opcode == 0xa2 ||
		reset_opcode == 0xa0 || reset_opcode == 0x20 || reset_opcode == 0x22)
		value += 4;
	if (reset_opcode == 0x40 || reset_opcode == 0x60 ||
		reset_opcode == 0x6b || reset_opcode == 0xcd ||
		reset_opcode == 0xec || reset_opcode == 0xcc) value -= 4;
	if (reset_opcode == 0x00 || reset_opcode == 0x02 ||
		reset_opcode == 0xdb || reset_opcode == 0x42 || reset_opcode == 0xff)
		value -= 8;
	if (static_cast<uint16_t>(checksum + complement) == 0xffff &&
		checksum != 0 && complement != 0) value += 4;
	if (address == 0x007fc0 && mapper == 0x20) value += 2;
	if (address == 0x00ffc0 && mapper == 0x21) value += 2;
	if (address == 0x007fc0 && mapper == 0x22) value += 2;
	if (address == 0x40ffc0 && mapper == 0x25) value += 2;
	const HeaderField plausible[] = {kCompany, kRomType, kRomSize, kRamSize,
		kCartRegion};
	const uint8_t limits[] = {0x33, 0x08, 0x10, 0x08, 14};
	for (size_t index = 0; index < sizeof(plausible) / sizeof(plausible[0]);
		++index) {
		uint8_t field = 0;
		result = ReadByte(clock, source, source_offset, source_size,
			address + plausible[index], &field, deadline);
		if (result != MISTER_RESULT_OK) return result;
		if (index == 0) {
			if (field == limits[index]) value += 2;
		} else if (field < limits[index]) {
			++value;
		}
	}
	*score = value > 0 ? static_cast<uint32_t>(value) : 0;
	return MISTER_RESULT_OK;
}

MisterResult FindHeader(const NativeClock &clock, NativeSnesContentSource &source,
	uint64_t source_offset, uint64_t source_size, uint32_t *address,
	uint64_t deadline)
{
	if (address == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	uint32_t lo = 0;
	uint32_t hi = 0;
	uint32_t ex = 0;
	MisterResult result = ScoreHeader(clock, source, source_offset, source_size,
		0x007fc0, &lo, deadline);
	if (result != MISTER_RESULT_OK) return result;
	result = ScoreHeader(clock, source, source_offset, source_size, 0x00ffc0,
		&hi, deadline);
	if (result != MISTER_RESULT_OK) return result;
	result = ScoreHeader(clock, source, source_offset, source_size, 0x40ffc0,
		&ex, deadline);
	if (result != MISTER_RESULT_OK) return result;
	if (ex != 0) ex += 4;
	if (lo >= hi && lo >= ex) *address = lo == 0 ? 0 : 0x007fc0;
	else if (hi >= ex) *address = hi == 0 ? 0 : 0x00ffc0;
	else *address = ex == 0 ? 0 : 0x40ffc0;
	return MISTER_RESULT_OK;
}

MisterResult NextPowerOfTwo(uint64_t value, uint64_t *result)
{
	if (value == 0 || result == nullptr || value > (UINT64_C(1) << 63))
		return MISTER_RESULT_UNSUPPORTED;
	uint64_t power = 1;
	while (power < value) power <<= 1;
	*result = power;
	return MISTER_RESULT_OK;
}

uint64_t MirrorOffset(uint64_t address, uint64_t size)
{
	uint64_t base = 0;
	uint64_t mask = 1;
	while (mask < size) mask <<= 1;
	while (address >= size) {
		while (mask != 0 && (address & mask) == 0) mask >>= 1;
		if (mask == 0) return address % size;
		address -= mask;
		if (size > mask) {
			size -= mask;
			base += mask;
		}
		mask >>= 1;
	}
	return base + address;
}

MisterResult ReadHeaderByte(const NativeClock &clock,
	NativeSnesContentSource &source, uint64_t source_offset,
	uint64_t source_size, uint32_t address, HeaderField field, uint8_t *value,
	uint64_t deadline)
{
	return ReadByte(clock, source, source_offset, source_size,
		static_cast<uint64_t>(address) + field, value, deadline);
}

} // namespace

MisterResult PrepareNativeSnesContent(NativeSnesContentSource &source,
	uint64_t retained_size, uint64_t minimum_source_bytes,
	uint64_t maximum_source_bytes, uint64_t maximum_wire_bytes,
	const NativeClock &clock, uint64_t absolute_deadline_ms,
	NativeSnesContentPlan *plan)
{
	if (plan == nullptr || minimum_source_bytes == 0 ||
		minimum_source_bytes > maximum_source_bytes ||
		maximum_source_bytes > maximum_wire_bytes)
		return MISTER_RESULT_INVALID_ARGUMENT;
	if (!BeforeDeadline(clock, absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	const uint64_t source_offset = (retained_size & 512u) != 0 ? 512u : 0u;
	if (retained_size < source_offset) return MISTER_RESULT_UNSUPPORTED;
	const uint64_t source_size = retained_size - source_offset;
	if (source_size < 0x8000 || source_size < minimum_source_bytes ||
		source_size > maximum_source_bytes || source_size > UINT32_MAX)
		return MISTER_RESULT_UNSUPPORTED;
	uint64_t wire_size = 0;
	MisterResult result = NextPowerOfTwo(source_size, &wire_size);
	if (result != MISTER_RESULT_OK || wire_size > maximum_wire_bytes)
		return MISTER_RESULT_UNSUPPORTED;

	NativeSnesContentPlan candidate = {};
	candidate.source_offset = source_offset;
	candidate.source_size = source_size;
	candidate.rom_wire_size = wire_size;
	PutLe32(candidate.metadata + 8, static_cast<uint32_t>(source_size));
	uint32_t address = 0;
	result = FindHeader(clock, source, source_offset, source_size, &address,
		absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	bool is_bsx_bios = false;
	result = MatchesTextAt(clock, source, source_offset, source_size, 0x7fc0,
		"Satellaview BS-X     ", 21, &is_bsx_bios, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	bool is_sufami_bios = false;
	bool is_sufami_base = false;
	bool is_sufami_turbo = false;
	bool is_sufami = false;
	result = MatchesTextAt(clock, source, source_offset, source_size, 0,
		"BANDAI SFC-ADX", 14, &is_sufami, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	if (is_sufami) {
		for (uint64_t offset = 0; offset < source_size;) {
			bool found = false;
			result = MatchesTextAt(clock, source, source_offset, source_size,
				offset, "BANDAI SFC-ADX", 14, &found, absolute_deadline_ms);
			if (result != MISTER_RESULT_OK) return result;
			if (found) {
				bool backup = false;
				result = MatchesTextAt(clock, source, source_offset, source_size,
					offset + 0x10, "SFC-ADX BACKUP", 14, &backup,
					absolute_deadline_ms);
				if (result != MISTER_RESULT_OK) return result;
				if (backup) is_sufami_bios = true;
				else {
					if (!is_sufami_base) address = static_cast<uint32_t>(offset);
					is_sufami_turbo = is_sufami_base;
					is_sufami_base = true;
				}
			}
			if (source_size - offset <= 1024 * 1024) break;
			offset += 1024 * 1024;
		}
	}
	bool is_cc92 = false;
	result = MatchesAt(clock, source, source_offset, source_size, 0x7fc0,
		kSnesCc92Header, sizeof(kSnesCc92Header), &is_cc92,
		absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	bool is_pf94_10k = false;
	result = MatchesAt(clock, source, source_offset, source_size, 0x7fc0,
		kSnesPf94_10kHeader, sizeof(kSnesPf94_10kHeader), &is_pf94_10k,
		absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	bool is_pf94_1m = false;
	result = MatchesAt(clock, source, source_offset, source_size, 0x7fc0,
		kSnesPf94_1mHeader, sizeof(kSnesPf94_1mHeader), &is_pf94_1m,
		absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	const bool is_pf94 = is_pf94_10k || is_pf94_1m;
	if (address == 0) {
		*plan = candidate;
		return MISTER_RESULT_OK;
	}

	uint8_t ram_size = 0;
	uint8_t rom_size_field = 0;
	uint8_t region = 0;
	uint8_t mapper = 0;
	uint8_t rom_type = 0;
	uint8_t company = 0;
	result = ReadHeaderByte(clock, source, source_offset, source_size, address,
		kRamSize, &ram_size, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	result = ReadHeaderByte(clock, source, source_offset, source_size, address,
		kRomSize, &rom_size_field, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	result = ReadHeaderByte(clock, source, source_offset, source_size, address,
		kCartRegion, &region, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	result = ReadHeaderByte(clock, source, source_offset, source_size, address,
		kMapper, &mapper, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	result = ReadHeaderByte(clock, source, source_offset, source_size, address,
		kRomType, &rom_type, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	result = ReadHeaderByte(clock, source, source_offset, source_size, address,
		kCompany, &company, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	if (ram_size >= 0x09) ram_size = 0;
	uint8_t rom_size = 15;
	uint32_t compatibility_size = static_cast<uint32_t>(source_size) - 1;
	if ((compatibility_size & 0xff000000u) == 0) {
		while ((compatibility_size & 0x01000000u) == 0) {
			--rom_size;
			compatibility_size <<= 1;
		}
	}
	bool has_bsx_slot = false;
	if (address >= 14) {
		uint8_t before_14 = 0;
		uint8_t before_13 = 0;
		uint8_t before_11 = 0;
		uint8_t before_10 = 0;
		uint8_t before_4 = 0;
		result = ReadByte(clock, source, source_offset, source_size, address - 14,
			&before_14, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		result = ReadByte(clock, source, source_offset, source_size, address - 13,
			&before_13, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		result = ReadByte(clock, source, source_offset, source_size, address - 11,
			&before_11, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		result = ReadByte(clock, source, source_offset, source_size, address - 10,
			&before_10, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		result = ReadByte(clock, source, source_offset, source_size, address - 4,
			&before_4, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		if (before_14 == 'Z' && before_11 == 'J' &&
			((before_13 >= 'A' && before_13 <= 'Z') ||
			 (before_13 >= '0' && before_13 <= '9')) &&
			(company == 0x33 || (before_10 == 0 && before_4 == 0)))
			has_bsx_slot = true;
	}

	candidate.metadata[1] = address == 0x00ffc0 ? 1 :
		address == 0x40ffc0 ? 2 : has_bsx_slot ? 3 : 0;
	if (is_bsx_bios) {
		candidate.metadata[1] = 0x30;
	} else if (is_sufami_base) {
		const uint8_t rom_size_table[9] = {0, 7, 8, 9, 9, 10, 10, 10, 10};
		const uint8_t ram_size_table[5] = {0, 1, 2, 3, 3};
		candidate.metadata[1] = static_cast<uint8_t>(0x20 |
			(is_sufami_turbo ? 8 : 0) | (is_sufami_bios ? 4 : 0));
		uint8_t value = 0;
		result = ReadByte(clock, source, source_offset, source_size, address + 0x36,
			&value, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		rom_size = value >= 8 ? rom_size_table[8] : rom_size_table[value & 0x0f];
		result = ReadByte(clock, source, source_offset, source_size, address + 0x37,
			&value, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		ram_size = value >= 4 ? ram_size_table[4] : ram_size_table[value & 0x07];
	} else if (is_cc92) {
		candidate.metadata[1] = 0xe4;
		ram_size = 3;
	} else if (is_pf94) {
		candidate.metadata[1] = 0xf4;
		ram_size = 3;
	} else {
		if (mapper == 0x20 && rom_type == 0x03) candidate.metadata[1] |= 0x84;
		else if (mapper == 0x21 && rom_type == 0x03) candidate.metadata[1] |= 0x80;
		else if (mapper == 0x30 && rom_type == 0x05 && company != 0xb2)
			candidate.metadata[1] |= 0x80;
		else if (mapper == 0x31 && (rom_type == 0x03 || rom_type == 0x05))
			candidate.metadata[1] |= 0x80;
		else if (mapper == 0x20 && rom_type == 0x05) candidate.metadata[1] |= 0x90;
		else if (mapper == 0x30 && rom_type == 0x05 && company == 0xb2)
			candidate.metadata[1] |= 0xa0;
		else if (mapper == 0x30 && rom_type == 0x03) candidate.metadata[1] |= 0xb0;
		else if (mapper == 0x30 && rom_type == 0xf6) {
			candidate.metadata[1] |= 0x88;
			ram_size = 1;
			if (rom_size_field < 10) candidate.metadata[1] |= 0x20;
		} else if (mapper == 0x30 && rom_type == 0x25) {
			candidate.metadata[1] |= 0xc0;
		}
		if (mapper == 0x3a && (rom_type == 0xf5 || rom_type == 0xf9)) {
			candidate.metadata[1] |= 0xd0;
			if (rom_type == 0xf9) candidate.metadata[1] |= 0x08;
		}
		if (mapper == 0x35 && rom_type == 0x55) candidate.metadata[1] |= 0x08;
		if (mapper == 0x20 && rom_type == 0xf3) candidate.metadata[1] |= 0x40;
		if (mapper == 0x32 && (rom_type == 0x43 || rom_type == 0x45)) {
			if (rom_size < 14) candidate.metadata[1] |= 0x50;
		}
		if (mapper == 0x23 && (rom_type == 0x32 || rom_type == 0x33 ||
			rom_type == 0x34 || rom_type == 0x35)) candidate.metadata[1] |= 0x60;
		if (mapper == 0x20 && (rom_type == 0x13 || rom_type == 0x14 ||
			rom_type == 0x15 || rom_type == 0x1a)) {
			result = ReadByte(clock, source, source_offset, source_size, address - 3,
				&ram_size, absolute_deadline_ms);
			if (result != MISTER_RESULT_OK) return result;
			if (ram_size == 0xff) ram_size = 5;
			if (ram_size > 6) ram_size = 6;
			candidate.metadata[1] |= 0x70;
		}
	}
	if (((region >= 0x02 && region <= 0x0c) || region == 0x11) &&
		!is_sufami_base && !is_cc92 && !is_pf94)
		candidate.metadata[3] |= 1;
	candidate.metadata[0] = static_cast<uint8_t>((ram_size << 4) | rom_size);
	PutLe32(candidate.metadata + 4, address);
	if (!BeforeDeadline(clock, absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	*plan = candidate;
	return MISTER_RESULT_OK;
}

MisterResult ReadNativeSnesRomWindow(NativeSnesContentSource &source,
	const NativeSnesContentPlan &plan, uint64_t wire_offset, void *bytes,
	size_t count, const NativeClock &clock, uint64_t absolute_deadline_ms)
{
	if (bytes == nullptr || count == 0 || !Contains(plan.rom_wire_size,
		wire_offset, count) || plan.source_size == 0 ||
		plan.source_offset > UINT64_MAX - plan.source_size)
		return MISTER_RESULT_INVALID_ARGUMENT;
	uint8_t *output = static_cast<uint8_t *>(bytes);
	for (size_t index = 0; index < count; ++index) {
		const uint64_t source_position = MirrorOffset(wire_offset + index,
			plan.source_size);
		const MisterResult result = ReadByte(clock, source, plan.source_offset,
			plan.source_size, source_position, output + index,
			absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
	}
	return MISTER_RESULT_OK;
}

} // namespace native
} // namespace mister
