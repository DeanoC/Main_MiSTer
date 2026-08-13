// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "runtime/native/native_snes_content.hpp"

#include <assert.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace mister {
namespace native {
namespace {

class FixedClock final : public NativeClock {
public:
	explicit FixedClock(uint64_t now_ms) : now_ms_(now_ms) {}
	uint64_t NowMs() const override { return now_ms_; }
	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t) override { return false; }
private:
	uint64_t now_ms_;
};

class AdvancingClock final : public NativeClock {
public:
	explicit AdvancingClock(uint64_t now_ms) : now_ms_(now_ms) {}
	uint64_t NowMs() const override { return now_ms_++; }
	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t) override { return false; }
private:
	mutable uint64_t now_ms_;
};

class MemorySource final : public NativeSnesContentSource {
public:
	explicit MemorySource(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}
	MisterResult ReadAt(uint64_t offset, void *output, size_t count,
		uint64_t) override
	{
		if (output == nullptr || offset > bytes_.size() ||
			count > bytes_.size() - static_cast<size_t>(offset))
			return MISTER_RESULT_PLATFORM;
		memcpy(output, bytes_.data() + offset, count);
		return MISTER_RESULT_OK;
	}
private:
	const std::vector<uint8_t> &bytes_;
};

struct SourceRead {
	uint64_t offset;
	size_t count;
};

class RecordingSource final : public NativeSnesContentSource {
public:
	explicit RecordingSource(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}
	MisterResult ReadAt(uint64_t offset, void *output, size_t count,
		uint64_t) override
	{
		reads_.push_back({offset, count});
		if (output == nullptr || offset > bytes_.size() ||
			count > bytes_.size() - static_cast<size_t>(offset))
			return MISTER_RESULT_PLATFORM;
		memcpy(output, bytes_.data() + offset, count);
		return MISTER_RESULT_OK;
	}
	void ClearReads() { reads_.clear(); }
	const std::vector<SourceRead> &reads() const { return reads_; }
private:
	const std::vector<uint8_t> &bytes_;
	std::vector<SourceRead> reads_;
};

class CountingSource final : public NativeSnesContentSource {
public:
	explicit CountingSource(const std::vector<uint8_t> &bytes)
		: bytes_(bytes), reads_(0), out_of_bounds_(false) {}
	MisterResult ReadAt(uint64_t offset, void *output, size_t count,
		uint64_t) override
	{
		++reads_;
		if (output == nullptr || offset > bytes_.size() ||
			count > bytes_.size() - static_cast<size_t>(offset)) {
			out_of_bounds_ = true;
			return MISTER_RESULT_PLATFORM;
		}
		memcpy(output, bytes_.data() + offset, count);
		return MISTER_RESULT_OK;
	}
	size_t reads() const { return reads_; }
	bool out_of_bounds() const { return out_of_bounds_; }
private:
	const std::vector<uint8_t> &bytes_;
	size_t reads_;
	bool out_of_bounds_;
};

std::vector<uint8_t> ValidLoRom(size_t size)
{
	assert(size >= 0x8000);
	std::vector<uint8_t> bytes(size, 0);
	bytes[0] = 0x78;
	const size_t header = 0x7fc0;
	bytes[header + 0x15] = 0x20;
	bytes[header + 0x16] = 0x00;
	bytes[header + 0x17] = 0x08;
	bytes[header + 0x18] = 0x00;
	bytes[header + 0x19] = 0x00;
	bytes[header + 0x1a] = 0x33;
	bytes[header + 0x1c] = 0xff;
	bytes[header + 0x1d] = 0xff;
	bytes[header + 0x1e] = 0x00;
	bytes[header + 0x1f] = 0x00;
	bytes[header + 0x3c] = 0x00;
	bytes[header + 0x3d] = 0x80;
	return bytes;
}

std::vector<uint8_t> ValidRom(size_t size, size_t header)
{
	assert(size >= header + 64);
	std::vector<uint8_t> bytes(size, 0);
	const size_t reset_opcode = (header & ~static_cast<size_t>(0x7fff));
	bytes[reset_opcode] = 0x78;
	bytes[header + 0x15] = header == 0x00ffc0 ? 0x21 :
		header == 0x40ffc0 ? 0x25 : 0x20;
	bytes[header + 0x16] = 0x00;
	bytes[header + 0x17] = 0x08;
	bytes[header + 0x18] = 0x00;
	bytes[header + 0x19] = 0x00;
	bytes[header + 0x1a] = 0x33;
	bytes[header + 0x1c] = 0xff;
	bytes[header + 0x1d] = 0xff;
	bytes[header + 0x1e] = 0x00;
	bytes[header + 0x1f] = 0x00;
	bytes[header + 0x3c] = 0x00;
	bytes[header + 0x3d] = 0x80;
	return bytes;
}

// Frozen synthetic metadata goldens. Their non-zero bytes were recorded from
// the approved Main_MiSTer support/snes/snes.cpp compatibility routine at
// a6fa625ca7c3a9bbafd5f917506aa9c274fb5d30 (SHA-256 71223676dce2d8bc3f324158eb5dd318178c93969bede09743018d0b1f5bf35e).
// Every vector is the full 512-byte native wire header; omitted initializer
// bytes are the compatibility routine's zero-filled tail.
struct GoldenMetadata { uint8_t bytes[512]; };

const GoldenMetadata kGoldenLo = {{
	0x05, 0x00, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenLoRam3 = {{
	0x35, 0x00, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenHi = {{
	0x06, 0x01, 0x00, 0x00, 0xc0, 0xff, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x00
}};
const GoldenMetadata kGoldenEx = {{
	0x0d, 0x02, 0x00, 0x00, 0xc0, 0xff, 0x40, 0x00,
	0x00, 0x00, 0x41, 0x00
}};
const GoldenMetadata kGoldenPal = {{
	0x05, 0x00, 0x00, 0x01, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenDsp1 = {{
	0x05, 0x84, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenDsp1b = {{
	0x05, 0x80, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenDsp2 = {{
	0x05, 0x90, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenDsp3 = {{
	0x05, 0xa0, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenDsp4 = {{
	0x05, 0xb0, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenSt010 = {{
	0x15, 0xa8, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenSt011 = {{
	0x15, 0x88, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenCx4 = {{
	0x05, 0x40, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenSdd1 = {{
	0x05, 0x50, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenSa1 = {{
	0x05, 0x60, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenSpc7110 = {{
	0x05, 0xd0, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenSpc7110Rtc = {{
	0x05, 0xd8, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenSrtc = {{
	0x05, 0x08, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenGsu = {{
	0x55, 0x70, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenObc1 = {{
	0x05, 0xc0, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenCc92 = {{
	0x35, 0xe4, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenPf94 = {{
	0x35, 0xf4, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenBsxBios = {{
	0x05, 0x30, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenBsxSlot = {{
	0x05, 0x03, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00,
	0x00, 0x80, 0x00, 0x00
}};
const GoldenMetadata kGoldenSufami = {{
	0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
	0x00, 0x00, 0x20, 0x00
}};
const GoldenMetadata kGoldenSufamiTurbo = {{
	0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
	0x00, 0x00, 0x30, 0x00
}};

NativeSnesContentPlan Prepare(const std::vector<uint8_t> &bytes,
	uint64_t maximum_source_bytes = 16 * 1024 * 1024,
	uint64_t maximum_wire_bytes = 16 * 1024 * 1024)
{
	MemorySource source(bytes);
	FixedClock clock(1);
	NativeSnesContentPlan plan = {};
	assert(PrepareNativeSnesContent(source, bytes.size(), 1,
		maximum_source_bytes, maximum_wire_bytes, clock, 100, &plan) ==
		MISTER_RESULT_OK);
	return plan;
}

void AssertGoldenMetadata(const NativeSnesContentPlan &plan,
	const GoldenMetadata &expected)
{
	assert(memcmp(plan.metadata, expected.bytes, sizeof(expected.bytes)) == 0);
}

struct GoldenMirrorRun {
	uint32_t wire_offset;
	uint32_t source_offset;
	uint32_t count;
};

bool PayloadReadsMatch(const RecordingSource &source, size_t begin,
	uint64_t expected_source_offset, size_t count)
{
	const std::vector<SourceRead> &reads = source.reads();
	if (begin > reads.size() || count > reads.size() - begin) return false;
	for (size_t index = 0; index < count; ++index) {
		const SourceRead &read = reads[begin + index];
		if (read.count != 1 || read.offset != expected_source_offset + index)
			return false;
	}
	return true;
}

void AssertGoldenMirrorRuns(const std::vector<uint8_t> &rom,
	const GoldenMirrorRun *runs, size_t run_count)
{
	RecordingSource source(rom);
	FixedClock clock(1);
	NativeSnesContentPlan plan = {};
	assert(PrepareNativeSnesContent(source, rom.size(), 1,
		16 * 1024 * 1024, 16 * 1024 * 1024, clock, 100, &plan) ==
		MISTER_RESULT_OK);
	// Planning/header reads must not dilute the exact streamed-read evidence.
	source.ClearReads();
	assert(source.reads().empty());
	uint64_t expected_wire_offset = 0;
	for (size_t run = 0; run < run_count; ++run) {
		assert(runs[run].wire_offset == expected_wire_offset);
		for (uint32_t offset = 0; offset < runs[run].count;) {
			const size_t count = runs[run].count - offset > 4096 ? 4096 :
				runs[run].count - offset;
			uint8_t actual[4096] = {};
			const size_t read_begin = source.reads().size();
			assert(ReadNativeSnesRomWindow(source, plan,
				runs[run].wire_offset + offset, actual, count, clock, 100) ==
				MISTER_RESULT_OK);
			assert(source.reads().size() == read_begin + count);
			assert(PayloadReadsMatch(source, read_begin,
				plan.source_offset + runs[run].source_offset + offset, count));
			for (size_t index = 0; index < count; ++index)
				assert(actual[index] == rom[runs[run].source_offset + offset + index]);
			offset += count;
		}
		expected_wire_offset += runs[run].count;
	}
	assert(expected_wire_offset == plan.rom_wire_size);
}

void ReadModuloSurrogate(RecordingSource &source,
	const NativeSnesContentPlan &plan, uint64_t wire_offset, uint8_t *output,
	size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		assert(source.ReadAt(plan.source_offset +
			(wire_offset + index) % plan.source_size, output + index, 1, 100) ==
			MISTER_RESULT_OK);
	}
}

class FailingSource final : public NativeSnesContentSource {
public:
	explicit FailingSource(const std::vector<uint8_t> &bytes)
		: bytes_(bytes), reads_(0) {}
	MisterResult ReadAt(uint64_t offset, void *output, size_t count,
		uint64_t) override
	{
		if (++reads_ == 2) return MISTER_RESULT_PLATFORM;
		if (output == nullptr || offset > bytes_.size() ||
			count > bytes_.size() - static_cast<size_t>(offset))
			return MISTER_RESULT_PLATFORM;
		memcpy(output, bytes_.data() + offset, count);
		return MISTER_RESULT_OK;
	}
private:
	const std::vector<uint8_t> &bytes_;
	size_t reads_;
};

void TestPlanStripsCopierHeaderAndWritesCompatibilitySizeFields()
{
	std::vector<uint8_t> rom = ValidLoRom(0x8000);
	std::vector<uint8_t> wrapped(512 + rom.size(), 0xee);
	std::copy(rom.begin(), rom.end(), wrapped.begin() + 512);
	MemorySource source(wrapped);
	FixedClock clock(1);
	NativeSnesContentPlan plan = {};
	assert(PrepareNativeSnesContent(source, wrapped.size(), 1,
		16 * 1024 * 1024, 16 * 1024 * 1024, clock, clock.NowMs() + 10,
		&plan) == MISTER_RESULT_OK);
	assert(plan.source_offset == 512);
	assert(plan.source_size == 0x8000);
	assert(plan.rom_wire_size == 0x8000);
	assert(plan.metadata[4] == 0xc0);
	assert(plan.metadata[5] == 0x7f);
	assert(plan.metadata[6] == 0x00);
	assert(plan.metadata[7] == 0x00);
	assert(plan.metadata[8] == 0x00);
	assert(plan.metadata[9] == 0x80);
	assert(plan.metadata[10] == 0x00);
	assert(plan.metadata[11] == 0x00);
}

void TestWireReadMirrorsNonPowerOfTwoSourceWithoutFullImageAllocation()
{
	std::vector<uint8_t> rom = ValidLoRom(0x8001);
	rom[0x8000] = 0x5a;
	MemorySource source(rom);
	NativeSnesContentPlan plan = {};
	FixedClock clock(1);
	assert(PrepareNativeSnesContent(source, rom.size(), 1,
		16 * 1024 * 1024, 16 * 1024 * 1024, clock, 100, &plan) ==
		MISTER_RESULT_OK);
	assert(plan.rom_wire_size == 0x10000);
	uint8_t bytes[4] = {};
	assert(ReadNativeSnesRomWindow(source, plan, 0x8000, bytes,
		sizeof(bytes), clock, 100) == MISTER_RESULT_OK);
	assert(bytes[0] == 0x5a);
	assert(bytes[1] == 0x5a);
	assert(bytes[2] == 0x5a);
	assert(bytes[3] == 0x5a);
}

void TestZeroAndShortSourcesFailClosed()
{
	const std::vector<uint8_t> empty;
	MemorySource empty_source(empty);
	NativeSnesContentPlan plan = {};
	FixedClock clock(1);
	assert(PrepareNativeSnesContent(empty_source, 0, 1, 1024, 1024,
		clock, 100, &plan) == MISTER_RESULT_UNSUPPORTED);
	const std::vector<uint8_t> short_source_bytes(0x7fff, 0);
	MemorySource short_source(short_source_bytes);
	assert(PrepareNativeSnesContent(short_source, short_source_bytes.size(),
		1, 16 * 1024 * 1024, 16 * 1024 * 1024, clock, 100, &plan) ==
		MISTER_RESULT_UNSUPPORTED);
}

void TestPlanLimitsCandidatesAndDeadlineFailBeforeUnsafeRead()
{
	FixedClock clock(1);
	NativeSnesContentPlan plan = {};
	std::vector<uint8_t> minimum = ValidLoRom(0x8000);
	CountingSource minimum_source(minimum);
	assert(PrepareNativeSnesContent(minimum_source, minimum.size(), 0x8000,
		0x800000, 0x1000000, clock, 100, &plan) == MISTER_RESULT_OK);
	assert(minimum_source.reads() != 0 && !minimum_source.out_of_bounds());

	std::vector<uint8_t> below_minimum(0x7fff, 0);
	CountingSource below_minimum_source(below_minimum);
	assert(PrepareNativeSnesContent(below_minimum_source, below_minimum.size(),
		0x8000, 0x800000, 0x1000000, clock, 100, &plan) ==
		MISTER_RESULT_UNSUPPORTED);
	assert(below_minimum_source.reads() == 0);

	std::vector<uint8_t> maximum = ValidLoRom(0x800000);
	CountingSource maximum_source(maximum);
	assert(PrepareNativeSnesContent(maximum_source, maximum.size(), 0x8000,
		0x800000, 0x1000000, clock, 100, &plan) == MISTER_RESULT_OK);
	assert(!maximum_source.out_of_bounds());
	maximum.push_back(0);
	CountingSource above_maximum_source(maximum);
	assert(PrepareNativeSnesContent(above_maximum_source, maximum.size(),
		0x8000, 0x800000, 0x1000000, clock, 100, &plan) ==
		MISTER_RESULT_UNSUPPORTED);
	assert(above_maximum_source.reads() == 0);

	std::vector<uint8_t> exact_lo_header = ValidLoRom(0x8000);
	CountingSource exact_lo_source(exact_lo_header);
	assert(PrepareNativeSnesContent(exact_lo_source, exact_lo_header.size(), 1,
		0x8000, 0x1000000, clock, 100, &plan) == MISTER_RESULT_OK);
	assert(!exact_lo_source.out_of_bounds());
	std::vector<uint8_t> ex_header_inside = ValidRom(0x410000, 0x40ffc0);
	CountingSource ex_header_inside_source(ex_header_inside);
	assert(PrepareNativeSnesContent(ex_header_inside_source,
		ex_header_inside.size(), 1, 0x800000, 0x1000000, clock, 100,
		&plan) == MISTER_RESULT_OK);
	assert(!ex_header_inside_source.out_of_bounds());
	std::vector<uint8_t> ex_header_outside = ValidLoRom(0x40ffff);
	CountingSource ex_header_outside_source(ex_header_outside);
	assert(PrepareNativeSnesContent(ex_header_outside_source,
		ex_header_outside.size(), 1, 0x800000, 0x1000000, clock, 100,
		&plan) == MISTER_RESULT_OK);
	assert(!ex_header_outside_source.out_of_bounds());

	CountingSource uint32_ceiling_source(minimum);
	assert(PrepareNativeSnesContent(uint32_ceiling_source,
		static_cast<uint64_t>(UINT32_MAX) + 1, 1, UINT64_MAX, UINT64_MAX,
		clock, 100, &plan) == MISTER_RESULT_UNSUPPORTED);
	assert(uint32_ceiling_source.reads() == 0);

	MemorySource deadline_source(minimum);
	assert(PrepareNativeSnesContent(deadline_source, minimum.size(), 1,
		0x800000, 0x1000000, clock, 1, &plan) == MISTER_RESULT_DEADLINE);
	AdvancingClock advancing_clock(0);
	assert(PrepareNativeSnesContent(deadline_source, minimum.size(), 1,
		0x800000, 0x1000000, advancing_clock, 1, &plan) ==
		MISTER_RESULT_DEADLINE);
}

void TestCompatibilityHeaderMappingsAndSpecialChips()
{
	struct Case {
		uint8_t mapper;
		uint8_t type;
		uint8_t company;
		uint8_t declared_ram;
		uint8_t declared_rom;
		uint8_t ram_before_header;
		const GoldenMetadata *golden;
	};
	const Case cases[] = {
		{0x20, 0x00, 0x33, 3, 8, 0, &kGoldenLoRam3},
		{0x30, 0x00, 0x33, 0, 8, 0, &kGoldenLo},
		{0x20, 0x03, 0x33, 0, 8, 0, &kGoldenDsp1},
		{0x21, 0x03, 0x33, 0, 8, 0, &kGoldenDsp1b},
		{0x30, 0x05, 0x33, 0, 8, 0, &kGoldenDsp1b},
		{0x30, 0x05, 0xb2, 0, 8, 0, &kGoldenDsp3},
		{0x31, 0x03, 0x33, 0, 8, 0, &kGoldenDsp1b},
		{0x31, 0x05, 0x33, 0, 8, 0, &kGoldenDsp1b},
		{0x20, 0x05, 0x33, 0, 8, 0, &kGoldenDsp2},
		{0x30, 0x03, 0x33, 0, 8, 0, &kGoldenDsp4},
		{0x30, 0xf6, 0x33, 0, 9, 0, &kGoldenSt010},
		{0x30, 0xf6, 0x33, 0, 10, 0, &kGoldenSt011},
		{0x20, 0xf3, 0x33, 0, 8, 0xff, &kGoldenCx4},
		{0x32, 0x43, 0x33, 0, 8, 0, &kGoldenSdd1},
		{0x32, 0x45, 0x33, 0, 8, 0, &kGoldenSdd1},
		{0x23, 0x32, 0x33, 0, 8, 0, &kGoldenSa1},
		{0x23, 0x33, 0x33, 0, 8, 0, &kGoldenSa1},
		{0x23, 0x34, 0x33, 0, 8, 0, &kGoldenSa1},
		{0x23, 0x35, 0x33, 0, 8, 0, &kGoldenSa1},
		{0x3a, 0xf5, 0x33, 0, 8, 0, &kGoldenSpc7110},
		{0x3a, 0xf9, 0x33, 0, 8, 0, &kGoldenSpc7110Rtc},
		{0x35, 0x55, 0x33, 0, 8, 0, &kGoldenSrtc},
		{0x20, 0x13, 0x33, 0, 8, 0xff, &kGoldenGsu},
		{0x20, 0x14, 0x33, 0, 8, 0xff, &kGoldenGsu},
		{0x20, 0x15, 0x33, 0, 8, 0xff, &kGoldenGsu},
		{0x20, 0x1a, 0x33, 0, 8, 0xff, &kGoldenGsu},
		{0x30, 0x25, 0x33, 0, 8, 0, &kGoldenObc1},
	};
	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		std::vector<uint8_t> rom = ValidLoRom(0x8000);
		rom[0x7fc0 + 0x15] = cases[index].mapper;
		rom[0x7fc0 + 0x16] = cases[index].type;
		rom[0x7fc0 + 0x18] = cases[index].declared_ram;
		rom[0x7fc0 + 0x17] = cases[index].declared_rom;
		rom[0x7fc0 + 0x1a] = cases[index].company;
		rom[0x7fc0 - 3] = cases[index].ram_before_header;
		const NativeSnesContentPlan plan = Prepare(rom);
		AssertGoldenMetadata(plan, *cases[index].golden);
	}
}

void TestCompatibilityHeaderLocationsPalAndTitleOverrides()
{
	std::vector<uint8_t> hi = ValidRom(0x10000, 0x00ffc0);
	AssertGoldenMetadata(Prepare(hi), kGoldenHi);
	std::vector<uint8_t> ex = ValidRom(0x410000, 0x40ffc0);
	AssertGoldenMetadata(Prepare(ex), kGoldenEx);
	std::vector<uint8_t> pal = ValidLoRom(0x8000);
	pal[0x7fc0 + 0x19] = 0x02;
	AssertGoldenMetadata(Prepare(pal), kGoldenPal);
	pal[0x7fc0 + 0x19] = 0x11;
	AssertGoldenMetadata(Prepare(pal), kGoldenPal);
	std::vector<uint8_t> cc92 = ValidLoRom(0x8000);
	const uint8_t cc92_header[] = {
		0x00, 0x08, 0x22, 0x02, 0x1c, 0x00, 0x10, 0x00,
		0x08, 0x65, 0x80, 0x84, 0x20, 0x00, 0x22, 0x25,
		0x00, 0x83, 0x0c, 0x80, 0x10, 0x00, 0x00, 0xa0,
		0x80, 0x01, 0x80, 0x80, 0x00, 0x01, 0x02, 0x2d};
	memcpy(cc92.data() + 0x7fc0, cc92_header, sizeof(cc92_header));
	cc92[0] = 0x78;
	cc92[0x7fc0 + 0x3c] = 0x00;
	cc92[0x7fc0 + 0x3d] = 0x80;
	const NativeSnesContentPlan cc92_plan = Prepare(cc92);
	AssertGoldenMetadata(cc92_plan, kGoldenCc92);
	std::vector<uint8_t> pf94 = ValidLoRom(0x8000);
	const uint8_t pf94_header[] = {
		0xc9, 0x80, 0x80, 0x44, 0x15, 0x00, 0x62, 0x09,
		0x29, 0xa0, 0x52, 0x70, 0x50, 0x12, 0x05, 0x35,
		0x31, 0x63, 0xc0, 0x22, 0x01, 0x80, 0xc2, 0x3a,
		0x6c, 0xb0, 0xe8, 0x4a, 0x11, 0x20, 0xc0, 0xf8};
	memcpy(pf94.data() + 0x7fc0, pf94_header, sizeof(pf94_header));
	pf94[0] = 0x78;
	pf94[0x7fc0 + 0x3c] = 0x00;
	pf94[0x7fc0 + 0x3d] = 0x80;
	AssertGoldenMetadata(Prepare(pf94), kGoldenPf94);
	std::vector<uint8_t> pf94_1m = ValidLoRom(0x8000);
	const uint8_t pf94_1m_header[] = {
		0x50, 0x52, 0x45, 0x48, 0x49, 0x53, 0x54, 0x4f,
		0x52, 0x49, 0x4b, 0x20, 0x4d, 0x41, 0x4e, 0x20,
		0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x00, 0x0a,
		0x00, 0x01, 0x33, 0x00, 0xff, 0xff, 0x00, 0x00,
		0xff, 0xff, 0xff, 0xff, 0x2b, 0x80, 0x2b, 0x80,
		0x2b, 0x80, 0xfe, 0x91, 0x2b, 0x80, 0xa4, 0xf7,
		0xff, 0xff, 0xff, 0xff, 0x2b, 0x80, 0x2b, 0x80,
		0x2b, 0x80, 0x75, 0xf7, 0x00, 0x80, 0xa4, 0xf7};
	memcpy(pf94_1m.data() + 0x7fc0, pf94_1m_header, sizeof(pf94_1m_header));
	AssertGoldenMetadata(Prepare(pf94_1m), kGoldenPf94);
	std::vector<uint8_t> bsx = ValidLoRom(0x8000);
	memcpy(bsx.data() + 0x7fc0, "Satellaview BS-X     ", 21);
	AssertGoldenMetadata(Prepare(bsx), kGoldenBsxBios);
	std::vector<uint8_t> bsx_slot = ValidLoRom(0x8000);
	bsx_slot[0x7fc0 - 14] = 'Z';
	bsx_slot[0x7fc0 - 13] = 'A';
	bsx_slot[0x7fc0 - 11] = 'J';
	AssertGoldenMetadata(Prepare(bsx_slot), kGoldenBsxSlot);
	std::vector<uint8_t> sufami = ValidLoRom(0x200000);
	memcpy(sufami.data(), "BANDAI SFC-ADX", 14);
	memcpy(sufami.data() + 0x10, "SFC-ADX BACKUP", 14);
	memcpy(sufami.data() + 0x100000, "BANDAI SFC-ADX", 14);
	AssertGoldenMetadata(Prepare(sufami), kGoldenSufami);
	std::vector<uint8_t> sufami_turbo = ValidLoRom(0x300000);
	memcpy(sufami_turbo.data(), "BANDAI SFC-ADX", 14);
	memcpy(sufami_turbo.data() + 0x10, "SFC-ADX BACKUP", 14);
	memcpy(sufami_turbo.data() + 0x100000, "BANDAI SFC-ADX", 14);
	memcpy(sufami_turbo.data() + 0x200000, "BANDAI SFC-ADX", 14);
	AssertGoldenMetadata(Prepare(sufami_turbo), kGoldenSufamiTurbo);
}

void TestCompatibilityGoldenMirrorRunsCoverEachBoundary()
{
	// These complete run maps were frozen from the approved compatibility
	// `snes_get_mirrored_rom` output. They are intentionally declarative rather
	// than a second implementation of its mask walk.
	const GoldenMirrorRun size_9000[] = {{0, 0, 0x9000},
		{0x9000, 0x8000, 0x1000}, {0xa000, 0x8000, 0x1000},
		{0xb000, 0x8000, 0x1000}, {0xc000, 0x8000, 0x1000},
		{0xd000, 0x8000, 0x1000}, {0xe000, 0x8000, 0x1000},
		{0xf000, 0x8000, 0x1000}};
	const GoldenMirrorRun size_a000[] = {{0, 0, 0xa000},
		{0xa000, 0x8000, 0x2000}, {0xc000, 0x8000, 0x2000},
		{0xe000, 0x8000, 0x2000}};
	const GoldenMirrorRun size_c000[] = {{0, 0, 0xc000},
		{0xc000, 0x8000, 0x4000}};
	const GoldenMirrorRun size_e000[] = {{0, 0, 0xe000},
		{0xe000, 0xc000, 0x2000}};
	const GoldenMirrorRun size_fdff[] = {{0, 0, 0xfdff},
		{0xfdff, 0xfdfe, 1}, {0xfe00, 0xfc00, 0x1ff},
		{0xffff, 0xfdfe, 1}};
	const struct {
		size_t size;
		const GoldenMirrorRun *runs;
		size_t run_count;
	} cases[] = {{0x9000, size_9000, sizeof(size_9000) / sizeof(size_9000[0])},
		{0xa000, size_a000, sizeof(size_a000) / sizeof(size_a000[0])},
		{0xc000, size_c000, sizeof(size_c000) / sizeof(size_c000[0])},
		{0xe000, size_e000, sizeof(size_e000) / sizeof(size_e000[0])},
		{0xfdff, size_fdff, sizeof(size_fdff) / sizeof(size_fdff[0])}};
	for (size_t test = 0; test < sizeof(cases) / sizeof(cases[0]); ++test) {
		std::vector<uint8_t> rom = ValidLoRom(cases[test].size);
		for (size_t index = 0; index < rom.size(); ++index)
			rom[index] = static_cast<uint8_t>(index * 17u + test * 31u);
		rom[0] = 0x78;
		rom[0x7fc0 + 0x15] = 0x20;
		rom[0x7fc0 + 0x1a] = 0x33;
		rom[0x7fc0 + 0x1c] = 0xff;
		rom[0x7fc0 + 0x1d] = 0xff;
		rom[0x7fc0 + 0x3c] = 0x00;
		rom[0x7fc0 + 0x3d] = 0x80;
		AssertGoldenMirrorRuns(rom, cases[test].runs, cases[test].run_count);
	}
}

void TestFrozenReadOffsetsRejectAliasingModuloSurrogate()
{
	std::vector<uint8_t> rom = ValidLoRom(0x9000);
	for (size_t index = 0; index < rom.size(); ++index)
		rom[index] = static_cast<uint8_t>(index * 17u + 31u);
	rom[0] = 0x78;
	rom[0x7fc0 + 0x15] = 0x20;
	rom[0x7fc0 + 0x1a] = 0x33;
	rom[0x7fc0 + 0x1c] = 0xff;
	rom[0x7fc0 + 0x1d] = 0xff;
	rom[0x7fc0 + 0x3c] = 0x00;
	rom[0x7fc0 + 0x3d] = 0x80;
	RecordingSource source(rom);
	FixedClock clock(1);
	NativeSnesContentPlan plan = {};
	assert(PrepareNativeSnesContent(source, rom.size(), 1,
		16 * 1024 * 1024, 16 * 1024 * 1024, clock, 100, &plan) ==
		MISTER_RESULT_OK);
	source.ClearReads();
	assert(source.reads().empty());
	uint8_t actual[4096] = {};
	ReadModuloSurrogate(source, plan, 0xa000, actual, sizeof(actual));
	for (size_t index = 0; index < sizeof(actual); ++index)
		assert(actual[index] == rom[0x8000 + index]);
	assert(source.reads().size() == sizeof(actual));
	assert(!PayloadReadsMatch(source, 0, plan.source_offset + 0x8000,
		sizeof(actual)));
}

void TestReadFailureAndWireOverflowFailBeforePlanPublication()
{
	const std::vector<uint8_t> rom = ValidLoRom(0x8001);
	FailingSource failing(rom);
	FixedClock clock(1);
	NativeSnesContentPlan plan = {};
	assert(PrepareNativeSnesContent(failing, rom.size(), 1,
		16 * 1024 * 1024, 16 * 1024 * 1024, clock, 100, &plan) ==
		MISTER_RESULT_PLATFORM);
	MemorySource source(rom);
	assert(PrepareNativeSnesContent(source, rom.size(), 1,
		0x8001, 0x8001, clock, 100, &plan) ==
		MISTER_RESULT_UNSUPPORTED);
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestPlanStripsCopierHeaderAndWritesCompatibilitySizeFields();
	mister::native::TestWireReadMirrorsNonPowerOfTwoSourceWithoutFullImageAllocation();
	mister::native::TestZeroAndShortSourcesFailClosed();
	mister::native::TestPlanLimitsCandidatesAndDeadlineFailBeforeUnsafeRead();
	mister::native::TestCompatibilityHeaderMappingsAndSpecialChips();
	mister::native::TestCompatibilityHeaderLocationsPalAndTitleOverrides();
	mister::native::TestCompatibilityGoldenMirrorRunsCoverEachBoundary();
	mister::native::TestFrozenReadOffsetsRejectAliasingModuloSurrogate();
	mister::native::TestReadFailureAndWireOverflowFailBeforePlanPublication();
	return 0;
}
