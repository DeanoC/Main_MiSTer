// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_core_protocol.hpp"

#include <assert.h>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
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

class MemoryContent final : public NativeCoreProtocolContent {
public:
	MemoryContent(const char *extension, const std::vector<uint8_t> &bytes)
		: extension_(extension), bytes_(bytes) {}
	MisterResult Describe(NativeCoreProtocolContentDescription *description,
		uint64_t) override
	{
		if (description == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		description->size = bytes_.size();
		description->extension = extension_.c_str();
		return MISTER_RESULT_OK;
	}
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
	std::string extension_;
	const std::vector<uint8_t> &bytes_;
};

class FailingContent final : public NativeCoreProtocolContent {
public:
	FailingContent(const char *extension, const std::vector<uint8_t> &bytes,
		size_t fail_after_read)
		: extension_(extension), bytes_(bytes), fail_after_read_(fail_after_read),
		  reads_(0) {}
	MisterResult Describe(NativeCoreProtocolContentDescription *description,
		uint64_t) override
	{
		if (description == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		description->size = bytes_.size();
		description->extension = extension_.c_str();
		return MISTER_RESULT_OK;
	}
	MisterResult ReadAt(uint64_t offset, void *output, size_t count,
		uint64_t) override
	{
		if (++reads_ > fail_after_read_) return MISTER_RESULT_PLATFORM;
		if (output == nullptr || offset > bytes_.size() ||
			count > bytes_.size() - static_cast<size_t>(offset))
			return MISTER_RESULT_PLATFORM;
		memcpy(output, bytes_.data() + offset, count);
		return MISTER_RESULT_OK;
	}
private:
	std::string extension_;
	const std::vector<uint8_t> &bytes_;
	size_t fail_after_read_;
	size_t reads_;
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

struct Trace {
	NativeSpiTarget target;
	bool begins_selected;
	bool ends_selected;
	std::vector<uint16_t> words;
};

class TraceIo final : public NativeActiveCoreProtocolIo {
public:
	TraceIo() : live_{0x005ca623, 0xa8,
		NativeFileIoWidth::little_endian_byte_pairs, 2}, responses_(), traces_() {}
	MisterResult Probe(NativeLiveCoreObservation *observation,
		uint64_t) override
	{
		if (observation == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		*observation = live_;
		return MISTER_RESULT_OK;
	}
	MisterResult Exchange(NativeSpiTarget target,
		const uint16_t *transmit_words, size_t word_count,
		uint16_t *received_words, size_t received_capacity, bool begins_selected,
		bool ends_selected,
		uint64_t) override
	{
		if (transmit_words == nullptr || word_count == 0 ||
			(received_words == nullptr && received_capacity != 0) ||
			(received_words != nullptr && received_capacity < word_count))
			return MISTER_RESULT_INVALID_ARGUMENT;
		traces_.push_back({target, begins_selected, ends_selected,
			std::vector<uint16_t>(transmit_words, transmit_words + word_count)});
		if (received_words != nullptr) {
			for (size_t index = 0; index < word_count; ++index) {
				if (responses_.empty()) return MISTER_RESULT_PLATFORM;
				received_words[index] = responses_.front();
				responses_.erase(responses_.begin());
			}
		}
		return MISTER_RESULT_OK;
	}
	MisterResult CloseSelected(NativeSpiTarget target,
		uint64_t) override
	{
		traces_.push_back({target, false, true, {}});
		return MISTER_RESULT_OK;
	}

	NativeLiveCoreObservation live_;
	std::vector<uint16_t> responses_;
	std::vector<Trace> traces_;
};

class PlanLockedSnesContent final : public NativeCoreProtocolContent {
public:
	PlanLockedSnesContent(const std::vector<uint8_t> &bytes, const TraceIo &io)
		: bytes_(bytes), io_(io), post_start_reads_(0),
		  unexpected_post_start_read_(false) {}
	MisterResult Describe(NativeCoreProtocolContentDescription *description,
		uint64_t) override
	{
		if (description == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		description->size = bytes_.size();
		description->extension = "sfc";
		return MISTER_RESULT_OK;
	}
	MisterResult ReadAt(uint64_t offset, void *output, size_t count,
		uint64_t) override
	{
		if (output == nullptr || offset > bytes_.size() ||
			count > bytes_.size() - static_cast<size_t>(offset))
			return MISTER_RESULT_PLATFORM;
		if (DownloadStarted()) {
			if (count != 1 || offset != post_start_reads_) {
				unexpected_post_start_read_ = true;
				return MISTER_RESULT_PLATFORM;
			}
			++post_start_reads_;
		}
		memcpy(output, bytes_.data() + offset, count);
		return MISTER_RESULT_OK;
	}
	bool unexpected_post_start_read() const { return unexpected_post_start_read_; }
	uint64_t post_start_reads() const { return post_start_reads_; }
private:
	bool DownloadStarted() const
	{
		for (size_t index = 0; index < io_.traces_.size(); ++index) {
			const Trace &trace = io_.traces_[index];
			if (trace.target == NativeSpiTarget::file_io &&
				trace.words.size() == 2 && trace.words[0] == 0x0053 &&
				trace.words[1] == 0x00ff)
				return true;
		}
		return false;
	}
	const std::vector<uint8_t> &bytes_;
	const TraceIo &io_;
	uint64_t post_start_reads_;
	bool unexpected_post_start_read_;
};

void TestMegaDriveRecipeUsesTypedHandshakeAndExactFileWords()
{
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("megadrive");
	assert(profile != nullptr);
	const std::vector<uint8_t> bytes = {0x11, 0x22, 0x33};
	MemoryContent content("md", bytes);
	TraceIo io;
	const char *name = "MegaDrive";
	io.responses_.push_back(0x0000);
	for (size_t index = 0; name[index] != '\0'; ++index)
		io.responses_.push_back(static_cast<uint16_t>(0x5a00u |
			static_cast<uint8_t>(name[index])));
	io.responses_.push_back(0x5a3b);
	io.responses_.push_back(0x0000);
	FixedClock clock(1);
	NativeCoreProtocol protocol(clock, io);
	assert(protocol.ActivateFixtureForTest(*profile, content, 100) ==
		MISTER_RESULT_OK);
	assert(io.traces_.size() == 11);
	assert(io.traces_[0].target == NativeSpiTarget::user_io);
	assert((io.traces_[0].words == std::vector<uint16_t>{0x0031, 0x4321}));
	assert((io.traces_[1].words == std::vector<uint16_t>{0x001e, 0x0001,
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}));
	assert(io.traces_[2].begins_selected);
	assert(io.traces_[2].ends_selected);
	assert(io.traces_[2].words.front() == 0x0014);
	assert(io.traces_[2].words.size() == strlen(name) + 2);
	assert((io.traces_[3].words == std::vector<uint16_t>{0x0055, 0x0000}));
	assert((io.traces_[4].words == std::vector<uint16_t>{0x0056, 0x2e4d, 0x4400}));
	assert((io.traces_[5].words == std::vector<uint16_t>{0x0053, 0x00ff}));
	assert((io.traces_[6].words == std::vector<uint16_t>{0x0054, 0x2211, 0x0033}));
	assert((io.traces_[7].words == std::vector<uint16_t>{0x0029}));
	assert(io.traces_[8].target == NativeSpiTarget::user_io);
	assert(io.traces_[8].words.empty());
	assert((io.traces_[9].words == std::vector<uint16_t>{0x0053, 0x0000}));
	assert((io.traces_[10].words == std::vector<uint16_t>{0x001e, 0x0000,
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}));
}

void TestNewStatusResponseStaysSelectedForEightWordsThenClearsBeforeStop()
{
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("megadrive");
	assert(profile != nullptr);
	const std::vector<uint8_t> bytes = {0x42};
	MemoryContent content("md", bytes);
	TraceIo io;
	io.responses_.push_back(0);
	const char *name = "MegaDrive";
	for (size_t index = 0; name[index] != '\0'; ++index)
		io.responses_.push_back(static_cast<uint8_t>(name[index]));
	io.responses_.push_back(';');
	io.responses_.push_back(0x00a1);
	for (size_t index = 0; index < 8; ++index)
		io.responses_.push_back(index == 0 ? 0x0010 : 0x0000);
	FixedClock clock(1);
	NativeCoreProtocol protocol(clock, io);
	assert(protocol.ActivateFixtureForTest(*profile, content, 100) ==
		MISTER_RESULT_OK);
	assert(io.traces_.size() == 12);
	assert(io.traces_[7].begins_selected && !io.traces_[7].ends_selected);
	assert(!io.traces_[8].begins_selected && io.traces_[8].ends_selected);
	assert((io.traces_[9].words == std::vector<uint16_t>{0x001e, 0x0010,
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}));
	assert((io.traces_[10].words == std::vector<uint16_t>{0x0053, 0x0000}));
	assert((io.traces_[11].words == std::vector<uint16_t>{0x001e, 0x0010,
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}));
}

void TestSnesFixtureActivationIsClosedBeforeAnyHardwareExchange()
{
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	const std::vector<uint8_t> bytes = ValidSnesBytes();
	MemoryContent content("sfc", bytes);
	TraceIo io;
	io.live_ = {0x005ca623, profile->protocol.exact_core_type,
		profile->protocol.file_io_width, profile->input.fpga_io_version};
	io.responses_.push_back(0);
	const char *name = "SNES";
	for (size_t index = 0; name[index] != '\0'; ++index)
		io.responses_.push_back(static_cast<uint8_t>(name[index]));
	io.responses_.push_back(';');
	io.responses_.push_back(0);
	FixedClock clock(1);
	NativeCoreProtocol protocol(clock, io);
	assert(protocol.ActivateFixtureForTest(*profile, content, 100) ==
		MISTER_RESULT_OK);
	assert(io.traces_.size() == 19);
	assert((io.traces_[3].words == std::vector<uint16_t>{0x0055, 0x0000}));
	assert((io.traces_[4].words == std::vector<uint16_t>{0x0056, 0x2e53, 0x4643}));
	assert((io.traces_[5].words == std::vector<uint16_t>{0x0053, 0x00ff}));
	assert(io.traces_[6].words.front() == 0x0054);
	assert(io.traces_[6].words.size() == 513);
	assert(io.traces_[6].words[5] == 0x00c0);
	assert(io.traces_[6].words[6] == 0x007f);
	assert(io.traces_[6].words[9] == 0x0000);
	assert(io.traces_[6].words[10] == 0x0080);
	assert(io.traces_[7].words.front() == 0x0054);
	assert(io.traces_[7].words.size() == 4097);
	assert(io.traces_[14].words.front() == 0x0054);
	assert(io.traces_[14].words.size() == 4097);
	assert((io.traces_[15].words == std::vector<uint16_t>{0x0029}));
	assert(io.traces_[16].target == NativeSpiTarget::user_io);
	assert((io.traces_[17].words == std::vector<uint16_t>{0x0053, 0x0000}));
}

void TestSnesShortAndMutatedContentDoNotReportActivationSuccess()
{
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	const std::vector<uint8_t> short_bytes(0x7fff, 0);
	MemoryContent short_content("sfc", short_bytes);
	TraceIo short_io;
	short_io.live_ = {0x005ca623, profile->protocol.exact_core_type,
		profile->protocol.file_io_width, profile->input.fpga_io_version};
	FixedClock clock(1);
	NativeCoreProtocol short_protocol(clock, short_io);
	assert(short_protocol.ActivateFixtureForTest(*profile, short_content, 100) ==
		MISTER_RESULT_UNSUPPORTED);
	assert(short_io.traces_.empty());

	const std::vector<uint8_t> bytes = ValidSnesBytes();
	FailingContent preflight_content("sfc", bytes, 0);
	TraceIo preflight_io;
	preflight_io.live_ = {0x005ca623, profile->protocol.exact_core_type,
		profile->protocol.file_io_width, profile->input.fpga_io_version};
	NativeCoreProtocol preflight_protocol(clock, preflight_io);
	assert(preflight_protocol.ActivateFixtureForTest(*profile, preflight_content,
		100) == MISTER_RESULT_PLATFORM);
	assert(preflight_io.traces_.empty());

	FailingContent changed_content("sfc", bytes, 35);
	TraceIo changed_io;
	changed_io.live_ = {0x005ca623, profile->protocol.exact_core_type,
		profile->protocol.file_io_width, profile->input.fpga_io_version};
	NativeCoreProtocol changed_protocol(clock, changed_io);
	assert(changed_protocol.ActivateFixtureForTest(*profile, changed_content,
		100) == MISTER_RESULT_PLATFORM);
	assert(!changed_io.traces_.empty());
}

void TestSnesFixtureConsumesThePreflightPlanAfterDownloadStart()
{
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	const std::vector<uint8_t> bytes = ValidSnesBytes();
	TraceIo io;
	io.live_ = {0x005ca623, profile->protocol.exact_core_type,
		profile->protocol.file_io_width, profile->input.fpga_io_version};
	io.responses_.push_back(0);
	const char *name = "SNES";
	for (size_t index = 0; name[index] != '\0'; ++index)
		io.responses_.push_back(static_cast<uint8_t>(name[index]));
	io.responses_.push_back(';');
	io.responses_.push_back(0);
	PlanLockedSnesContent content(bytes, io);
	FixedClock clock(1);
	NativeCoreProtocol protocol(clock, io);
	assert(protocol.ActivateFixtureForTest(*profile, content, 100) ==
		MISTER_RESULT_OK);
	assert(!content.unexpected_post_start_read());
	assert(content.post_start_reads() == bytes.size());
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestMegaDriveRecipeUsesTypedHandshakeAndExactFileWords();
	mister::native::TestNewStatusResponseStaysSelectedForEightWordsThenClearsBeforeStop();
	mister::native::TestSnesFixtureActivationIsClosedBeforeAnyHardwareExchange();
	mister::native::TestSnesShortAndMutatedContentDoNotReportActivationSuccess();
	mister::native::TestSnesFixtureConsumesThePreflightPlanAfterDownloadStart();
	return 0;
}
