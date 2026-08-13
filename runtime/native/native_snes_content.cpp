// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "runtime/native/native_snes_content.hpp"

#include <limits.h>
#include <string.h>

namespace mister {
namespace native {
namespace {

enum HeaderField : uint64_t {
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
	if (value == 0 || result == nullptr) return MISTER_RESULT_UNSUPPORTED;
	if (value > (UINT64_C(1) << 63)) return MISTER_RESULT_UNSUPPORTED;
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
	if (retained_size < source_offset || retained_size - source_offset < 0x8000 ||
		retained_size - source_offset < minimum_source_bytes ||
		retained_size - source_offset > maximum_source_bytes ||
		retained_size - source_offset > UINT32_MAX)
		return MISTER_RESULT_UNSUPPORTED;
	const uint64_t source_size = retained_size - source_offset;
	uint64_t wire_size = 0;
	MisterResult result = NextPowerOfTwo(source_size, &wire_size);
	if (result != MISTER_RESULT_OK || wire_size > maximum_wire_bytes)
		return MISTER_RESULT_UNSUPPORTED;
	NativeSnesContentPlan candidate = {};
	candidate.source_offset = source_offset;
	candidate.source_size = source_size;
	candidate.rom_wire_size = wire_size;
	PutLe32(candidate.metadata + 8, static_cast<uint32_t>(source_size));
	uint32_t header_address = 0;
	result = FindHeader(clock, source, source_offset, source_size,
		&header_address, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	PutLe32(candidate.metadata + 4, header_address);
	if (header_address == 0) {
		*plan = candidate;
		return MISTER_RESULT_OK;
	}
	uint8_t ram_size = 0;
	uint8_t region = 0;
	result = ReadByte(clock, source, source_offset, source_size,
		header_address + kRamSize, &ram_size, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	result = ReadByte(clock, source, source_offset, source_size,
		header_address + kCartRegion, &region, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	if (ram_size >= 0x09) ram_size = 0;
	uint8_t rom_size = 15;
	uint64_t size_for_rom_size = source_size - 1;
	while ((size_for_rom_size & 0x1000000u) == 0) {
		if (rom_size == 0 || size_for_rom_size > UINT64_MAX / 2)
			return MISTER_RESULT_UNSUPPORTED;
		--rom_size;
		size_for_rom_size <<= 1;
	}
	candidate.metadata[1] = header_address == 0x00ffc0 ? 1 :
		header_address == 0x40ffc0 ? 2 : 0;
	candidate.metadata[0] = static_cast<uint8_t>((ram_size << 4) | rom_size);
	if ((region >= 0x02 && region <= 0x0c) || region == 0x11)
		candidate.metadata[3] = 1;
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
