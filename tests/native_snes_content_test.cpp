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

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestPlanStripsCopierHeaderAndWritesCompatibilitySizeFields();
	mister::native::TestWireReadMirrorsNonPowerOfTwoSourceWithoutFullImageAllocation();
	mister::native::TestZeroAndShortSourcesFailClosed();
	return 0;
}
