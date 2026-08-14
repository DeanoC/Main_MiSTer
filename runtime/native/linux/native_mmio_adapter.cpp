// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(__linux__) && !defined(_LARGEFILE64_SOURCE)
#define _LARGEFILE64_SOURCE
#endif

#include "runtime/native/linux/native_mmio_adapter.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <limits>
#include <new>

namespace mister {
namespace native {
namespace linux_native {
namespace {

const uint64_t kCoreGpoAddress = 0xff706010u;
const uint64_t kInterfaceModuleAddress = 0xffd08028u;
const uint64_t kSdrPortControlAddress = 0xffc25080u;
const uint64_t kBridgeResetAddress = 0xffd0501cu;
const uint64_t kRemapAddress = 0xff800000u;
const uint64_t kFpgaDataAddress = 0xffb90000u;
const size_t kMinimumPageSize = 4096;

const size_t kManagerStat = 0x00;
const size_t kManagerCtrl = 0x04;
const size_t kManagerDclkCount = 0x08;
const size_t kManagerDclkStatus = 0x0c;
const size_t kManagerGpo = 0x10;
const size_t kManagerGpi = 0x14;
const size_t kManagerMonitorGpio = 0x850;

const uint32_t kInputDataMask = 0x0000ffffu;
const uint32_t kInputStrobeMask = 0x00020000u;
const uint32_t kInputFileSelectMask = 0x00040000u;
const uint32_t kInputUserSelectMask = 0x00100000u;
const uint32_t kInputOwnedMutationMask = kInputStrobeMask |
	kInputFileSelectMask | kInputUserSelectMask;
const uint32_t kInputCoreControlMask = 0xc0000000u;
const uint32_t kInputCoreNormal = 0x80000000u;
const uint32_t kInputFaultMask = 0x80000000u;

bool IsPowerOfTwo(size_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

struct Mapping {
	Mapping() : identity(0) {}
	uintptr_t identity;
};

class LinuxOperations {
public:
	virtual ~LinuxOperations() {}
	virtual size_t PageSize() const = 0;
	virtual uint64_t NowMs() const = 0;
	virtual int Open(const char *path, int flags) = 0;
	virtual int Close(int descriptor) = 0;
	virtual int Map(int descriptor, uint64_t page_offset, size_t length,
		Mapping *mapping) = 0;
	virtual int Unmap(const Mapping &mapping, size_t length) = 0;
	virtual int Read32(const Mapping &mapping, size_t offset,
		uint32_t *value) = 0;
	virtual int Write32(const Mapping &mapping, size_t offset,
		uint32_t value) = 0;
	virtual int OrderingBarrier() = 0;
};

class PosixOperations final : public LinuxOperations {
public:
	size_t PageSize() const override
	{
		const long size = sysconf(_SC_PAGESIZE);
		return size > 0 ? static_cast<size_t>(size) : 0;
	}
	uint64_t NowMs() const override
	{
		struct timespec value = {};
		if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return UINT64_MAX;
		const uint64_t seconds = static_cast<uint64_t>(value.tv_sec);
		if (seconds > (UINT64_MAX -
			static_cast<uint64_t>(value.tv_nsec / 1000000)) / 1000)
			return UINT64_MAX;
		return seconds * 1000 +
			static_cast<uint64_t>(value.tv_nsec / 1000000);
	}
	int Open(const char *path, int flags) override
	{
		return open(path, flags);
	}
	int Close(int descriptor) override
	{
		return close(descriptor);
	}
	int Map(int descriptor, uint64_t page_offset, size_t length,
		Mapping *mapping) override
	{
		if (mapping == nullptr || mapping->identity != 0) return -1;
		void *address = MAP_FAILED;
#if defined(__linux__)
		if (page_offset > static_cast<uint64_t>(
			std::numeric_limits<off64_t>::max())) return -1;
		address = mmap64(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED,
			descriptor, static_cast<off64_t>(page_offset));
#else
		if (page_offset > static_cast<uint64_t>(
			std::numeric_limits<off_t>::max())) return -1;
		address = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED,
			descriptor, static_cast<off_t>(page_offset));
#endif
		mapping->identity = reinterpret_cast<uintptr_t>(address);
		return address == MAP_FAILED ? -1 : 0;
	}
	int Unmap(const Mapping &mapping, size_t length) override
	{
		return munmap(reinterpret_cast<void *>(mapping.identity), length);
	}
	int Read32(const Mapping &mapping, size_t offset,
		uint32_t *value) override
	{
		if (value == nullptr || offset > SIZE_MAX - sizeof(uint32_t)) return -1;
		const uintptr_t base = mapping.identity;
		if (base == 0 || base == std::numeric_limits<uintptr_t>::max() ||
			base > UINTPTR_MAX - offset || ((base + offset) & 3u) != 0)
			return -1;
		const volatile uint32_t *address =
			reinterpret_cast<volatile uint32_t *>(base + offset);
		*value = *address;
		return 0;
	}
	int Write32(const Mapping &mapping, size_t offset,
		uint32_t value) override
	{
		const uintptr_t base = mapping.identity;
		if (base == 0 || base == std::numeric_limits<uintptr_t>::max() ||
			base > UINTPTR_MAX - offset || ((base + offset) & 3u) != 0)
			return -1;
		volatile uint32_t *address =
			reinterpret_cast<volatile uint32_t *>(base + offset);
		*address = value;
		return 0;
	}
	int OrderingBarrier() override
	{
		std::atomic_thread_fence(std::memory_order_seq_cst);
		return 0;
	}
};

#if defined(MISTER_NATIVE_MMIO_TESTING)
class TestOperationsBridge final : public LinuxOperations {
public:
	TestOperationsBridge() : operations_(nullptr) {}
	explicit TestOperationsBridge(NativeMmioTestOperations &operations)
		: operations_(&operations) {}
	size_t PageSize() const override { return operations_->PageSize(); }
	uint64_t NowMs() const override { return operations_->NowMs(); }
	int Open(const char *path, int flags) override
		{ return operations_->Open(path, flags); }
	int Close(int descriptor) override { return operations_->Close(descriptor); }
	int Map(int descriptor, uint64_t page_offset, size_t length,
		Mapping *mapping) override
	{
		NativeMmioTestMapping exposed;
		const int result = operations_->Map(descriptor, page_offset, length,
			&exposed);
		mapping->identity = exposed.identity;
		return result;
	}
	int Unmap(const Mapping &mapping, size_t length) override
	{
		NativeMmioTestMapping exposed;
		exposed.identity = mapping.identity;
		return operations_->Unmap(exposed, length);
	}
	int Read32(const Mapping &mapping, size_t offset,
		uint32_t *value) override
	{
		NativeMmioTestMapping exposed;
		exposed.identity = mapping.identity;
		return operations_->Read32(exposed, offset, value);
	}
	int Write32(const Mapping &mapping, size_t offset,
		uint32_t value) override
	{
		NativeMmioTestMapping exposed;
		exposed.identity = mapping.identity;
		return operations_->Write32(exposed, offset, value);
	}
	int OrderingBarrier() override { return operations_->OrderingBarrier(); }

private:
	NativeMmioTestOperations *operations_;
};
#endif

} // namespace

class NativeLinuxMmioAdapter::Impl final {
public:
	enum class Register : uint8_t {
		core_gpo,
		interface_module,
		sdr_port_control,
		bridge_reset,
		remap,
		fpga_data,
		count
	};
	struct Slot {
		Mapping mapping;
		uint64_t page_offset;
		size_t register_offset;
		size_t length;
		bool held;
	};
	class ProgramSession final : public NativeFpgaProgramSession {
	public:
		explicit ProgramSession(Impl &owner) : owner_(owner) {}

	private:
		NativeFpgaSinkWriteOutcome Write(const unsigned char *bytes,
			size_t count, uint64_t absolute_deadline_ms) override
		{
			return owner_.WriteProgram(bytes, count, absolute_deadline_ms);
		}
		NativeFpgaSinkFinishOutcome Finish(
			uint64_t absolute_deadline_ms) override
		{
			return owner_.FinishProgram(absolute_deadline_ms);
		}
		Impl &owner_;
	};

	Impl()
		: operations_(&posix_), descriptor_(-1), geometry_initialized_(false),
		  release_started_(false), released_(false), descriptor_unknown_(false),
		  programming_started_(false), program_complete_(false),
		  bridges_attempted_(false), core_normal_observed_(false),
		  bridge_authority_(), expected_bytes_(0), programmed_bytes_(0),
		  manager_residue_(false), manager_neutral_step_(0), manager_control_(0),
		  manager_mode_(0), mutation_applied_latch_(false)
	{
		InitializeSlots();
	}
#if defined(MISTER_NATIVE_MMIO_TESTING)
	explicit Impl(NativeMmioTestOperations &operations)
		: test_(operations), operations_(&test_), descriptor_(-1),
		  geometry_initialized_(false), release_started_(false), released_(false),
		  descriptor_unknown_(false), programming_started_(false),
		  program_complete_(false), bridges_attempted_(false),
		  core_normal_observed_(false), bridge_authority_(), expected_bytes_(0),
		  programmed_bytes_(0), manager_residue_(false), manager_neutral_step_(0),
		  manager_control_(0), manager_mode_(0), mutation_applied_latch_(false)
	{
		InitializeSlots();
	}
#endif

	Result WriteCoreReset(const Access &access, uint32_t mask, uint32_t value)
	{
		if (mask != 0xc0000000u || (value & mask) != 0x40000000u ||
			(value & ~mask) != 0)
			return MISTER_RESULT_INVALID_ARGUMENT;
		uint32_t current = 0;
		Result result = Read(access, Register::core_gpo, &current);
		if (result != MISTER_RESULT_OK) return result;
		return Write(access, Register::core_gpo,
			(current & ~mask) | (value & mask));
	}
	Result WriteInterfaceModule(const Access &access, uint32_t value)
	{
		return value == 0 ? Write(access, Register::interface_module, value) :
			MISTER_RESULT_INVALID_ARGUMENT;
	}
	Result WriteSdrPortControl(const Access &access, uint32_t offset,
		uint32_t value)
	{
		return offset == 0x5080u && value == 0 ?
			Write(access, Register::sdr_port_control, value) :
			MISTER_RESULT_INVALID_ARGUMENT;
	}
	Result WriteBridgeReset(const Access &access, uint32_t value)
	{
		return value == 7 ? Write(access, Register::bridge_reset, value) :
			MISTER_RESULT_INVALID_ARGUMENT;
	}
	Result WriteRemap(const Access &access, uint32_t value)
	{
		return value == 1 ? Write(access, Register::remap, value) :
			MISTER_RESULT_INVALID_ARGUMENT;
	}
	Result ReadCoreGpo(const Access &access, uint32_t *value)
		{ return Read(access, Register::core_gpo, value); }
	Result ReadInterfaceModule(const Access &access, uint32_t *value)
		{ return Read(access, Register::interface_module, value); }
	Result ReadSdrPortControl(const Access &access, uint32_t offset,
		uint32_t *value)
	{
		return offset == 0x5080u ? Read(access, Register::sdr_port_control,
			value) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	Result ReadBridgeReset(const Access &access, uint32_t *value)
		{ return Read(access, Register::bridge_reset, value); }
	Result ReadRemap(const Access &access, uint32_t *value)
		{ return Read(access, Register::remap, value); }

	NativeMappingAcquisitionReceipt AcquireMappings(const Access &access)
	{
		const Result result = EnsureMappings(access, true);
		const bool any = descriptor_ >= 0 || descriptor_unknown_ ||
			!AllMappingsAbsent();
		bool complete = result == MISTER_RESULT_OK && descriptor_ < 0 &&
			!descriptor_unknown_;
		for (const Slot &slot : slots_) complete = complete && slot.held;
		const NativeMappingAcquisitionReceipt receipt = {
			result, any, complete};
		return receipt;
	}

	NativeFpgaSinkStartOutcome Begin(uint64_t expected_bytes,
		uint64_t absolute_deadline_ms,
		std::unique_ptr<NativeFpgaProgramSession> *session)
	{
		(void)ConsumeAppliedMutation();
		NativeFpgaSinkStartOutcome outcome = {
			MISTER_RESULT_INVALID_ARGUMENT, false, false, false};
		if (session == nullptr || session->get() != nullptr || expected_bytes == 0)
			return outcome;
		if (release_started_ || released_ || programming_started_ ||
			bridges_attempted_) {
			outcome.result = MISTER_RESULT_INVALID_STATE;
			return outcome;
		}
		if (CheckDeadlineMs(absolute_deadline_ms) != MISTER_RESULT_OK) {
			outcome.result = MISTER_RESULT_DEADLINE;
			return outcome;
		}
		for (const Slot &slot : slots_) {
			if (!slot.held) {
				outcome.result = MISTER_RESULT_INVALID_STATE;
				return outcome;
			}
		}
		programming_started_ = true;
		expected_bytes_ = expected_bytes;
		programmed_bytes_ = 0;
		outcome.acquired = true;
		outcome.mutation_attempted = true;

		Result result = WriteMaskedRaw(absolute_deadline_ms, Register::core_gpo,
			kManagerGpo, 0xc0000000u, 0x40000000u);
		if (result == MISTER_RESULT_OK) outcome.mutation_applied = true;
		uint32_t observed = 0;
		if (result == MISTER_RESULT_OK)
			result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerGpo, &observed);
		if (result == MISTER_RESULT_OK &&
			(observed & 0xc0000000u) != 0x40000000u)
			result = MISTER_RESULT_PLATFORM;
		if (result == MISTER_RESULT_OK)
			result = WriteRaw(absolute_deadline_ms, Register::interface_module,
				Get(Register::interface_module).register_offset, 0);
		if (result == MISTER_RESULT_OK)
			result = WriteRaw(absolute_deadline_ms, Register::sdr_port_control,
				Get(Register::sdr_port_control).register_offset, 0);
		if (result == MISTER_RESULT_OK)
			result = WriteRaw(absolute_deadline_ms, Register::bridge_reset,
				Get(Register::bridge_reset).register_offset, 7);
		if (result == MISTER_RESULT_OK)
			result = WriteRaw(absolute_deadline_ms, Register::remap,
				Get(Register::remap).register_offset, 1);
		const Register containment_registers[] = {
			Register::interface_module, Register::sdr_port_control,
			Register::bridge_reset, Register::remap};
		const uint32_t expected[] = {0, 0, 7, 1};
		for (size_t index = 0; result == MISTER_RESULT_OK && index != 4;
			++index) {
			result = ReadRaw(absolute_deadline_ms, containment_registers[index],
				Get(containment_registers[index]).register_offset, &observed);
			if (result == MISTER_RESULT_OK && observed != expected[index])
				result = MISTER_RESULT_PLATFORM;
		}
		manager_residue_ = true;
		uint32_t stat = 0;
		uint32_t ctrl = 0;
		if (result == MISTER_RESULT_OK)
			result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerStat, &stat);
		if (result == MISTER_RESULT_OK)
			result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerCtrl, &ctrl);
		if (result == MISTER_RESULT_OK) {
			const uint32_t msel = (stat >> 3) & 0xfu;
			uint32_t ratio = 0;
			if ((msel & 3u) == 1u) ratio = (msel & 8u) ? 2u : 1u;
			else if ((msel & 3u) == 2u) ratio = (msel & 8u) ? 3u : 2u;
			ctrl &= ~(0x300u | 0xc0u | 0x7u);
			if ((msel & 8u) != 0) ctrl |= 0x200u;
			ctrl |= ratio << 6;
			ctrl |= 0x5u;
			result = WriteRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerCtrl, ctrl);
		}
		if (result == MISTER_RESULT_OK)
			result = PollMode(absolute_deadline_ms, 1, false, nullptr);
		if (result == MISTER_RESULT_OK) {
			ctrl &= ~0x4u;
			result = WriteRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerCtrl, ctrl);
		}
		if (result == MISTER_RESULT_OK)
			result = PollMode(absolute_deadline_ms, 2, false, nullptr);
		if (result == MISTER_RESULT_OK) {
			ctrl |= 0x100u;
			result = WriteRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerCtrl, ctrl);
		}
		if (result != MISTER_RESULT_OK) {
			outcome.mutation_applied = ConsumeAppliedMutation();
			outcome.result = result;
			return outcome;
		}
		ProgramSession *created = new (std::nothrow) ProgramSession(*this);
		if (created == nullptr) {
			outcome.mutation_applied = ConsumeAppliedMutation();
			outcome.result = MISTER_RESULT_PLATFORM;
			return outcome;
		}
		session->reset(created);
		outcome.result = MISTER_RESULT_OK;
		outcome.mutation_applied = ConsumeAppliedMutation();
		return outcome;
	}

	NativeFpgaSinkWriteOutcome WriteProgram(const unsigned char *bytes,
		size_t count, uint64_t absolute_deadline_ms)
	{
		(void)ConsumeAppliedMutation();
		NativeFpgaSinkWriteOutcome outcome = {
			MISTER_RESULT_INVALID_ARGUMENT, 0, false, false};
		if (bytes == nullptr || count == 0) return outcome;
		if (!programming_started_ || program_complete_ ||
			programmed_bytes_ > expected_bytes_ ||
			static_cast<uint64_t>(count) > expected_bytes_ - programmed_bytes_) {
			outcome.result = MISTER_RESULT_INVALID_STATE;
			return outcome;
		}
		outcome.mutation_attempted = true;
		size_t offset = 0;
		while (offset < count) {
			uint32_t word = 0;
			const size_t fragment = count - offset < 4 ? count - offset : 4;
			for (size_t index = 0; index != fragment; ++index)
				word |= static_cast<uint32_t>(bytes[offset + index]) << (index * 8);
			const Result result = WriteRaw(absolute_deadline_ms,
				Register::fpga_data, 0, word);
			const bool applied = ConsumeAppliedMutation();
			outcome.mutation_applied = outcome.mutation_applied || applied;
			if (applied) {
				offset += fragment;
				outcome.accepted_bytes += fragment;
				programmed_bytes_ += fragment;
			}
			if (result != MISTER_RESULT_OK) {
				outcome.result = result;
				return outcome;
			}
			if (!applied) {
				outcome.result = MISTER_RESULT_PLATFORM;
				return outcome;
			}
		}
		outcome.result = MISTER_RESULT_OK;
		return outcome;
	}

	NativeFpgaSinkFinishOutcome FinishProgram(uint64_t absolute_deadline_ms)
	{
		(void)ConsumeAppliedMutation();
		NativeFpgaSinkFinishOutcome outcome = {
			MISTER_RESULT_INVALID_STATE, false, false, false, false, false, false};
		if (!programming_started_ || program_complete_ ||
			programmed_bytes_ != expected_bytes_)
			return outcome;
		outcome.mutation_attempted = true;
		uint32_t observed = 0;
		Result result = PollMonitor(absolute_deadline_ms, 0x3u);
		if (result == MISTER_RESULT_OK) outcome.configuration_done_observed = true;
		uint32_t ctrl = 0;
		if (result == MISTER_RESULT_OK)
			result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerCtrl, &ctrl);
		if (result == MISTER_RESULT_OK) {
			ctrl &= ~0x100u;
			result = WriteRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerCtrl, ctrl);
		}
		if (result == MISTER_RESULT_OK)
			result = RunDclk(absolute_deadline_ms, 4);
		if (result == MISTER_RESULT_OK)
			result = PollMode(absolute_deadline_ms, 3, true, &observed);
		if (result == MISTER_RESULT_OK) outcome.initialization_observed = true;
		if (result == MISTER_RESULT_OK)
			result = RunDclk(absolute_deadline_ms, 0x5000u);
		if (result == MISTER_RESULT_OK)
			result = PollMode(absolute_deadline_ms, 4, false, &observed);
		if (result == MISTER_RESULT_OK) outcome.user_mode_observed = true;
		if (result == MISTER_RESULT_OK) {
			ctrl &= ~0x105u;
			ctrl |= 0x2u;
			result = WriteRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerCtrl, ctrl);
		}
		if (result == MISTER_RESULT_OK)
			result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerCtrl, &observed);
		if (result == MISTER_RESULT_OK && (observed & 0x107u) != 0x2u)
			result = MISTER_RESULT_PLATFORM;
		if (result == MISTER_RESULT_OK)
			result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerStat, &observed);
		if (result == MISTER_RESULT_OK && (observed & 7u) != 4u)
			result = MISTER_RESULT_PLATFORM;
		if (result == MISTER_RESULT_OK) {
			outcome.manager_drive_released = true;
			manager_residue_ = false;
			program_complete_ = true;
		}
		outcome.mutation_applied = ConsumeAppliedMutation();
		outcome.result = result;
		return outcome;
	}

	NativeBridgeEnableReceipt EnableBridges(const Access &access)
	{
		(void)ConsumeAppliedMutation();
		NativeBridgeEnableReceipt receipt = {
			MISTER_RESULT_INVALID_STATE, false, false, false, false, false, false, false,
			0, 0};
		if (!program_complete_ || bridges_attempted_ || release_started_ || released_)
			return receipt;
		bridges_attempted_ = true;
		receipt.acquired = true;
		Result result = Write(access, Register::sdr_port_control, 0x3fffu);
		if (result == MISTER_RESULT_OK)
			result = Write(access, Register::bridge_reset, 0);
		if (result == MISTER_RESULT_OK)
			result = Write(access, Register::remap, 0x19u);
		uint32_t observed = 0;
		if (result == MISTER_RESULT_OK) {
			result = Read(access, Register::sdr_port_control, &observed);
			receipt.sdr_ports_observed = result == MISTER_RESULT_OK &&
				observed == 0x3fffu;
			if (result == MISTER_RESULT_OK && !receipt.sdr_ports_observed)
				result = MISTER_RESULT_PLATFORM;
		}
		if (result == MISTER_RESULT_OK) {
			result = Read(access, Register::bridge_reset, &observed);
			receipt.bridge_release_observed = result == MISTER_RESULT_OK &&
				observed == 0;
			if (result == MISTER_RESULT_OK && !receipt.bridge_release_observed)
				result = MISTER_RESULT_PLATFORM;
		}
		if (result == MISTER_RESULT_OK) {
			result = Read(access, Register::remap, &observed);
			receipt.remap_observed = result == MISTER_RESULT_OK && observed == 0x19u;
			if (result == MISTER_RESULT_OK && !receipt.remap_observed)
				result = MISTER_RESULT_PLATFORM;
		}
		if (result == MISTER_RESULT_OK) {
			receipt.core_normal_write_attempted = true;
			result = WriteMaskedRaw(access.absolute_deadline_ms(),
				Register::core_gpo, kManagerGpo, 0xc0000000u, 0x80000000u);
		}
		if (result == MISTER_RESULT_OK) {
			result = ReadRaw(access.absolute_deadline_ms(), Register::core_gpo,
				kManagerGpo, &receipt.observed_core_gpo);
			receipt.core_normal_observed = result == MISTER_RESULT_OK &&
				(receipt.observed_core_gpo & 0xc0000000u) == 0x80000000u;
			if (result == MISTER_RESULT_OK && !receipt.core_normal_observed)
				result = MISTER_RESULT_PLATFORM;
		}
		receipt.result = result;
		receipt.mutation_applied = ConsumeAppliedMutation();
		core_normal_observed_ = result == MISTER_RESULT_OK &&
			receipt.core_normal_observed;
		return receipt;
	}

	Result InstallBridgeActivationAuthority(
		std::unique_ptr<NativeBridgeActivationAuthority> authority)
	{
		if (!authority) return MISTER_RESULT_INVALID_ARGUMENT;
		if (bridge_authority_ || !program_complete_ || !bridges_attempted_ ||
			!core_normal_observed_ || release_started_ || released_ ||
			descriptor_ >= 0 || descriptor_unknown_)
			return MISTER_RESULT_INVALID_STATE;
		for (const Slot &slot : slots_)
			if (!slot.held) return MISTER_RESULT_INVALID_STATE;
		bridge_authority_ = std::move(authority);
		return MISTER_RESULT_OK;
	}
	bool BridgeActivationAuthorityLocallyCurrent() const
	{
		if (!bridge_authority_ || !program_complete_ ||
			!core_normal_observed_ || release_started_ || released_ ||
			descriptor_ >= 0 || descriptor_unknown_)
			return false;
		for (const Slot &slot : slots_)
			if (!slot.held) return false;
		return true;
	}
	const NativeBridgeActivationAuthority &BridgeActivationAuthority() const
	{
		return *bridge_authority_;
	}
	bool HasBridgeActivationAuthority() const
	{
		return bridge_authority_.get() != nullptr;
	}

	NativeSpiMutationResult SelectUserIo(uint64_t absolute_deadline_ms)
	{
		NativeSpiMutationResult result = {
			MISTER_RESULT_INVALID_STATE, false, false, false};
		if (input_target_may_be_selected_) return result;
		uint32_t current = 0;
		result.result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
			kManagerGpo, &current);
		if (result.result != MISTER_RESULT_OK) return result;
		if ((current & kInputCoreControlMask) != kInputCoreNormal) {
			result.result = MISTER_RESULT_INVALID_STATE;
			return result;
		}
		input_target_may_be_selected_ = true;
		return MutateInputGpo(absolute_deadline_ms, current,
			kInputOwnedMutationMask, kInputUserSelectMask,
			kInputOwnedMutationMask | kInputCoreControlMask,
			kInputUserSelectMask | kInputCoreNormal);
	}

	NativeSpiMutationResult WriteInputWord(uint64_t absolute_deadline_ms,
		uint16_t word)
	{
		if (!input_target_may_be_selected_)
			return {MISTER_RESULT_INVALID_STATE, false, false, false};
		uint32_t current = 0;
		const Result read = ReadRaw(absolute_deadline_ms, Register::core_gpo,
			kManagerGpo, &current);
		if (read != MISTER_RESULT_OK) return {read, false, false, false};
		return MutateInputGpo(absolute_deadline_ms, current,
			kInputDataMask | kInputStrobeMask, static_cast<uint32_t>(word),
			kInputDataMask | kInputOwnedMutationMask,
			kInputUserSelectMask | static_cast<uint32_t>(word));
	}

	NativeSpiMutationResult SetInputStrobe(uint64_t absolute_deadline_ms,
		bool high)
	{
		if (!input_target_may_be_selected_)
			return {MISTER_RESULT_INVALID_STATE, false, false, false};
		uint32_t current = 0;
		const Result read = ReadRaw(absolute_deadline_ms, Register::core_gpo,
			kManagerGpo, &current);
		if (read != MISTER_RESULT_OK) return {read, false, false, false};
		NativeSpiMutationResult result = MutateInputGpo(absolute_deadline_ms,
			current, kInputStrobeMask,
			high ? kInputStrobeMask : 0, kInputOwnedMutationMask,
			(current & kInputUserSelectMask) |
				(high ? kInputStrobeMask : 0));
		if (result.applied && result.observed)
			input_target_may_be_selected_ =
				(current & kInputUserSelectMask) != 0 || high;
		return result;
	}

	Result ReadInputAck(uint64_t absolute_deadline_ms,
		NativeSpiAckSample *sample)
	{
		if (sample == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		if (!input_target_may_be_selected_) return MISTER_RESULT_INVALID_STATE;
		uint32_t value = 0;
		const Result result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
			kManagerGpi, &value);
		if (result != MISTER_RESULT_OK) return result;
		sample->ack_high = (value & kInputStrobeMask) != 0;
		sample->fault = (value & kInputFaultMask) != 0;
		sample->response = static_cast<uint16_t>(value & kInputDataMask);
		return MISTER_RESULT_OK;
	}

	Result ObserveInputResidue(uint64_t absolute_deadline_ms,
		bool *user_io_selected, bool *strobe_high)
	{
		if (user_io_selected == nullptr || strobe_high == nullptr)
			return MISTER_RESULT_INVALID_ARGUMENT;
		uint32_t current = 0;
		const Result result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
			kManagerGpo, &current);
		if (result != MISTER_RESULT_OK) return result;
		if ((current & kInputCoreControlMask) != kInputCoreNormal ||
			(current & kInputFileSelectMask) != 0)
			return MISTER_RESULT_INVALID_STATE;
		*user_io_selected = (current & kInputUserSelectMask) != 0;
		*strobe_high = (current & kInputStrobeMask) != 0;
		input_target_may_be_selected_ = *user_io_selected || *strobe_high;
		return MISTER_RESULT_OK;
	}

	NativeSpiMutationResult DeselectUserIo(uint64_t absolute_deadline_ms)
	{
		if (!input_target_may_be_selected_)
			return {MISTER_RESULT_INVALID_STATE, false, false, false};
		uint32_t current = 0;
		const Result read = ReadRaw(absolute_deadline_ms, Register::core_gpo,
			kManagerGpo, &current);
		if (read != MISTER_RESULT_OK) return {read, false, false, false};
		NativeSpiMutationResult result = MutateInputGpo(absolute_deadline_ms,
			current, kInputOwnedMutationMask, 0, kInputOwnedMutationMask, 0);
		if (result.applied && result.observed)
			input_target_may_be_selected_ = false;
		return result;
	}

	NativeManagerNeutralReceipt ReconcileManager(const Access &access)
	{
		(void)ConsumeAppliedMutation();
		NativeManagerNeutralReceipt receipt = {
			MISTER_RESULT_OK, manager_control_, manager_mode_, false, false, false};
		const uint64_t deadline = access.absolute_deadline_ms();
		Result result = MISTER_RESULT_OK;
		if (manager_neutral_step_ == 0) {
			uint32_t stat = 0;
			result = ReadRaw(deadline, Register::core_gpo, kManagerCtrl,
				&manager_control_);
			if (result == MISTER_RESULT_OK)
				result = ReadRaw(deadline, Register::core_gpo, kManagerStat, &stat);
			manager_mode_ = stat & 7u;
			if (result == MISTER_RESULT_OK && manager_mode_ > 4u)
				result = MISTER_RESULT_PLATFORM;
			if (result == MISTER_RESULT_OK)
				manager_neutral_step_ =
					(manager_control_ & 0x107u) == 0x2u ? 6 : 1;
		}
		auto ensure_control = [&](uint32_t controlled, int next_step) -> Result {
			uint32_t current = 0;
			Result local = ReadRaw(deadline, Register::core_gpo, kManagerCtrl,
				&current);
			if (local != MISTER_RESULT_OK) return local;
			if ((current & 0x107u) != controlled) {
				receipt.mutation_attempted = true;
				local = WriteRaw(deadline, Register::core_gpo, kManagerCtrl,
					(current & ~0x107u) | controlled);
				if (local != MISTER_RESULT_OK) return local;
				local = ReadRaw(deadline, Register::core_gpo, kManagerCtrl,
					&current);
				if (local != MISTER_RESULT_OK) return local;
			}
			manager_control_ = current;
			if ((current & 0x107u) != controlled)
				return MISTER_RESULT_PLATFORM;
			manager_neutral_step_ = next_step;
			return MISTER_RESULT_OK;
		};
		if (result == MISTER_RESULT_OK && manager_neutral_step_ == 1)
			result = ensure_control(0x5u, 2);
		if (result == MISTER_RESULT_OK && manager_neutral_step_ == 2) {
			result = PollMode(deadline, 1, false, &manager_mode_);
			if (result == MISTER_RESULT_OK) manager_neutral_step_ = 3;
		}
		if (result == MISTER_RESULT_OK && manager_neutral_step_ == 3)
			result = ensure_control(0x1u, 4);
		if (result == MISTER_RESULT_OK && manager_neutral_step_ == 4) {
			result = PollMode(deadline, 2, false, &manager_mode_);
			if (result == MISTER_RESULT_OK) manager_neutral_step_ = 5;
		}
		if (result == MISTER_RESULT_OK && manager_neutral_step_ == 5)
			result = ensure_control(0x2u, 6);
		if (result == MISTER_RESULT_OK && manager_neutral_step_ == 6) {
			uint32_t stat = 0;
			result = ReadRaw(deadline, Register::core_gpo, kManagerCtrl,
				&manager_control_);
			if (result == MISTER_RESULT_OK)
				result = ReadRaw(deadline, Register::core_gpo, kManagerStat, &stat);
			manager_mode_ = stat & 7u;
			if (result == MISTER_RESULT_OK &&
				((manager_control_ & 0x107u) != 0x2u || manager_mode_ > 4u))
				result = MISTER_RESULT_PLATFORM;
		}
		receipt.result = result;
		receipt.mutation_applied = ConsumeAppliedMutation();
		receipt.observed_control = manager_control_;
		receipt.observed_mode = manager_mode_;
		receipt.neutral_observed = result == MISTER_RESULT_OK &&
			manager_neutral_step_ == 6 &&
			(manager_control_ & 0x107u) == 0x2u && manager_mode_ <= 4u;
		if (receipt.neutral_observed) manager_residue_ = false;
		return receipt;
	}
	Result ReadManagerControl(const Access &access, uint32_t *value)
	{
		return ReadRaw(access.absolute_deadline_ms(), Register::core_gpo,
			kManagerCtrl, value);
	}
	Result ReadManagerMode(const Access &access, uint32_t *value)
	{
		uint32_t stat = 0;
		const Result result = ReadRaw(access.absolute_deadline_ms(),
			Register::core_gpo, kManagerStat, &stat);
		if (result == MISTER_RESULT_OK && value != nullptr) *value = stat & 7u;
		return value == nullptr ? MISTER_RESULT_INVALID_ARGUMENT : result;
	}

	Result ReadRaw(uint64_t absolute_deadline_ms, Register reg, size_t offset,
		uint32_t *value)
	{
		if (value == nullptr || released_ || release_started_)
			return value == nullptr ? MISTER_RESULT_INVALID_ARGUMENT :
				MISTER_RESULT_INVALID_STATE;
		Slot &slot = Get(reg);
		if (!slot.held || offset > slot.length ||
			offset > slot.length - sizeof(uint32_t) || (offset & 3u) != 0)
			return MISTER_RESULT_INVALID_STATE;
		Result result = CheckDeadlineMs(absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		uint32_t observed = 0;
		if (operations_->Read32(slot.mapping, offset, &observed) != 0)
			return MISTER_RESULT_PLATFORM;
		result = CheckDeadlineMs(absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		*value = observed;
		return MISTER_RESULT_OK;
	}
	Result WriteRaw(uint64_t absolute_deadline_ms, Register reg, size_t offset,
		uint32_t value)
	{
		if (released_ || release_started_) return MISTER_RESULT_INVALID_STATE;
		Slot &slot = Get(reg);
		if (!slot.held || offset > slot.length ||
			offset > slot.length - sizeof(uint32_t) || (offset & 3u) != 0)
			return MISTER_RESULT_INVALID_STATE;
		Result result = CheckDeadlineMs(absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		if (operations_->Write32(slot.mapping, offset, value) != 0)
			return MISTER_RESULT_PLATFORM;
		mutation_applied_latch_ = true;
		result = CheckDeadlineMs(absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		if (operations_->OrderingBarrier() != 0) return MISTER_RESULT_PLATFORM;
		return CheckDeadlineMs(absolute_deadline_ms);
	}
	Result WriteMaskedRaw(uint64_t absolute_deadline_ms, Register reg,
		size_t offset, uint32_t mask, uint32_t value)
	{
		uint32_t current = 0;
		Result result = ReadRaw(absolute_deadline_ms, reg, offset, &current);
		if (result != MISTER_RESULT_OK) return result;
		return WriteRaw(absolute_deadline_ms, reg, offset,
			(current & ~mask) | (value & mask));
	}
	Result PollMode(uint64_t absolute_deadline_ms, uint32_t expected,
		bool accept_user, uint32_t *observed)
	{
		for (;;) {
			uint32_t stat = 0;
			const Result result = ReadRaw(absolute_deadline_ms,
				Register::core_gpo, kManagerStat, &stat);
			if (result != MISTER_RESULT_OK) return result;
			const uint32_t mode = stat & 7u;
			if (mode == expected || (accept_user && mode == 4u)) {
				if (observed != nullptr) *observed = mode;
				return MISTER_RESULT_OK;
			}
		}
	}
	Result PollMonitor(uint64_t absolute_deadline_ms, uint32_t mask)
	{
		for (;;) {
			uint32_t value = 0;
			const Result result = ReadRaw(absolute_deadline_ms,
				Register::core_gpo, kManagerMonitorGpio, &value);
			if (result != MISTER_RESULT_OK) return result;
			if ((value & mask) == mask) return MISTER_RESULT_OK;
		}
	}
	Result RunDclk(uint64_t absolute_deadline_ms, uint32_t count)
	{
		uint32_t status = 0;
		Result result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
			kManagerDclkStatus, &status);
		if (result == MISTER_RESULT_OK && status != 0)
			result = WriteRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerDclkStatus, 1);
		if (result == MISTER_RESULT_OK)
			result = WriteRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerDclkCount, count);
		if (result != MISTER_RESULT_OK) return result;
		for (;;) {
			result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
				kManagerDclkStatus, &status);
			if (result != MISTER_RESULT_OK) return result;
			if (status != 0)
				return WriteRaw(absolute_deadline_ms, Register::core_gpo,
					kManagerDclkStatus, 1);
		}
	}

	Result ReleaseMappings(const Access &access)
	{
		if (released_) return MISTER_RESULT_INVALID_STATE;
		release_started_ = true;
		Result first_failure = MISTER_RESULT_OK;
		for (Slot &slot : slots_) {
			if (!slot.held) continue;
			Result result = CheckDeadline(access);
			if (result != MISTER_RESULT_OK) {
				if (first_failure == MISTER_RESULT_OK) first_failure = result;
				break;
			}
			if (operations_->Unmap(slot.mapping, slot.length) != 0) {
				if (first_failure == MISTER_RESULT_OK)
					first_failure = MISTER_RESULT_CLEANUP_INCOMPLETE;
				continue;
			}
			slot.held = false;
			slot.mapping.identity = 0;
			result = CheckDeadline(access);
			if (result != MISTER_RESULT_OK && first_failure == MISTER_RESULT_OK)
				first_failure = result;
		}
		const Result close_result = CloseDescriptor(access);
		if (close_result != MISTER_RESULT_OK && first_failure == MISTER_RESULT_OK)
			first_failure = close_result;
		if (!AllMappingsAbsent() || descriptor_ >= 0 || descriptor_unknown_) {
			return first_failure == MISTER_RESULT_OK ?
				MISTER_RESULT_CLEANUP_INCOMPLETE : first_failure;
		}
		if (first_failure == MISTER_RESULT_OK) {
			released_ = true;
			bridge_authority_.reset();
		}
		return first_failure;
	}
	Result CloseMappingsForProcessExit()
	{
		release_started_ = true;
		Result result = descriptor_unknown_ ? MISTER_RESULT_CLEANUP_INCOMPLETE :
			MISTER_RESULT_OK;
		for (Slot &slot : slots_) {
			if (!slot.held) continue;
			if (operations_->Unmap(slot.mapping, slot.length) != 0) {
				result = MISTER_RESULT_CLEANUP_INCOMPLETE;
				continue;
			}
			slot.held = false;
			slot.mapping.identity = 0;
		}
		if (descriptor_ >= 0) {
			const int descriptor = descriptor_;
			descriptor_ = -1;
			if (operations_->Close(descriptor) != 0) {
				descriptor_unknown_ = true;
				result = MISTER_RESULT_CLEANUP_INCOMPLETE;
			}
		}
		return result;
	}
	bool RecoveryMappingsHeld() const { return !AllMappingsAbsent(); }
	bool ConsumeAppliedMutation()
	{
		const bool applied = mutation_applied_latch_;
		mutation_applied_latch_ = false;
		return applied;
	}

private:
	NativeSpiMutationResult MutateInputGpo(uint64_t absolute_deadline_ms,
		uint32_t current, uint32_t clear_mask, uint32_t set_value,
		uint32_t observe_mask, uint32_t expected)
	{
		NativeSpiMutationResult result = {
			MISTER_RESULT_OK, true, false, false};
		result.result = WriteRaw(absolute_deadline_ms, Register::core_gpo,
			kManagerGpo, (current & ~clear_mask) | (set_value & clear_mask));
		result.applied = ConsumeAppliedMutation();
		if (result.result != MISTER_RESULT_OK) return result;
		if (!result.applied) {
			result.result = MISTER_RESULT_PLATFORM;
			return result;
		}
		uint32_t observed = 0;
		result.result = ReadRaw(absolute_deadline_ms, Register::core_gpo,
			kManagerGpo, &observed);
		if (result.result != MISTER_RESULT_OK) return result;
		result.observed = (observed & observe_mask) == expected;
		if (!result.observed) result.result = MISTER_RESULT_PLATFORM;
		return result;
	}

	void InitializeSlots()
	{
		for (Slot &slot : slots_) {
			slot.page_offset = 0;
			slot.register_offset = 0;
			slot.length = 0;
			slot.held = false;
		}
	}
	Slot &Get(Register reg) { return slots_[static_cast<size_t>(reg)]; }
	Result InitializeGeometry()
	{
		if (geometry_initialized_) return MISTER_RESULT_OK;
		const size_t page_size = operations_->PageSize();
		if (page_size < kMinimumPageSize || !IsPowerOfTwo(page_size) ||
			page_size > static_cast<size_t>(UINT32_MAX))
			return MISTER_RESULT_PLATFORM;
		const uint64_t addresses[] = {
			kCoreGpoAddress, kInterfaceModuleAddress, kSdrPortControlAddress,
			kBridgeResetAddress, kRemapAddress, kFpgaDataAddress
		};
		for (size_t index = 0; index != static_cast<size_t>(Register::count);
			++index) {
			const uint64_t address = addresses[index];
			const uint64_t page_offset = address - address % page_size;
			const uint64_t register_offset = address - page_offset;
			if (page_offset % page_size != 0 || register_offset > SIZE_MAX ||
				register_offset > page_size ||
				register_offset > page_size - sizeof(uint32_t) ||
				(register_offset & 3u) != 0)
				return MISTER_RESULT_PLATFORM;
			slots_[index].page_offset = page_offset;
			slots_[index].register_offset =
				static_cast<size_t>(register_offset);
			slots_[index].length = page_size;
		}
		geometry_initialized_ = true;
		return MISTER_RESULT_OK;
	}
	Result CheckDeadline(const Access &access) const
	{
		return CheckDeadlineMs(access.absolute_deadline_ms());
	}
	Result CheckDeadlineMs(uint64_t absolute_deadline_ms) const
	{
		return operations_->NowMs() >= absolute_deadline_ms ?
			MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
	}
	Result CloseDescriptor(const Access &access)
	{
		if (descriptor_ < 0) return MISTER_RESULT_OK;
		Result result = CheckDeadline(access);
		if (result != MISTER_RESULT_OK) return result;
		const int descriptor = descriptor_;
		descriptor_ = -1;
		if (operations_->Close(descriptor) != 0) {
			descriptor_unknown_ = true;
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		}
		return CheckDeadline(access);
	}
	Result EnsureMappings(const Access &access, bool include_data)
	{
		if (released_ || release_started_) return MISTER_RESULT_INVALID_STATE;
		Result result = InitializeGeometry();
		if (result != MISTER_RESULT_OK) return result;
		result = CheckDeadline(access);
		if (result != MISTER_RESULT_OK) return result;
		bool missing = false;
		for (size_t index = 0; index != static_cast<size_t>(Register::count);
			++index) {
			if (!include_data && index == static_cast<size_t>(Register::fpga_data))
				continue;
			if (!slots_[index].held) missing = true;
		}
		if (missing && descriptor_ < 0) {
			descriptor_ = operations_->Open("/dev/mem",
				O_RDWR | O_SYNC | O_CLOEXEC);
			if (descriptor_ < 0) return MISTER_RESULT_PLATFORM;
			result = CheckDeadline(access);
			if (result != MISTER_RESULT_OK) return result;
		}
		for (size_t index = 0; index != static_cast<size_t>(Register::count);
			++index) {
			if (!include_data && index == static_cast<size_t>(Register::fpga_data))
				continue;
			Slot &slot = slots_[index];
			if (slot.held) continue;
			result = CheckDeadline(access);
			if (result != MISTER_RESULT_OK) break;
			Mapping candidate;
			const int mapped = operations_->Map(descriptor_, slot.page_offset,
				slot.length, &candidate);
			if (mapped != 0 || candidate.identity == 0 || candidate.identity ==
				std::numeric_limits<uintptr_t>::max()) {
				result = MISTER_RESULT_PLATFORM;
				break;
			}
			slot.mapping.identity = candidate.identity;
			slot.held = true;
			result = CheckDeadline(access);
			if (result != MISTER_RESULT_OK) break;
		}
		const Result close_result = CloseDescriptor(access);
		if (result != MISTER_RESULT_OK) return result;
		if (close_result != MISTER_RESULT_OK) return close_result;
		for (size_t index = 0; index != static_cast<size_t>(Register::count);
			++index) {
			if (!include_data && index == static_cast<size_t>(Register::fpga_data))
				continue;
			if (!slots_[index].held) return MISTER_RESULT_PLATFORM;
		}
		if (descriptor_unknown_) return MISTER_RESULT_CLEANUP_INCOMPLETE;
		return MISTER_RESULT_OK;
	}
	Result Read(const Access &access, Register reg, uint32_t *value)
	{
		if (value == nullptr || released_ || release_started_)
			return value == nullptr ? MISTER_RESULT_INVALID_ARGUMENT :
				MISTER_RESULT_INVALID_STATE;
		Result result = EnsureMappings(access, false);
		if (result != MISTER_RESULT_OK) return result;
		Slot &slot = Get(reg);
		if (!slot.held || slot.register_offset > slot.length ||
			slot.register_offset > slot.length - sizeof(uint32_t) ||
			(slot.register_offset & 3u) != 0)
			return MISTER_RESULT_PLATFORM;
		result = CheckDeadline(access);
		if (result != MISTER_RESULT_OK) return result;
		uint32_t observed = 0;
		if (operations_->Read32(slot.mapping, slot.register_offset, &observed)
			!= 0)
			return MISTER_RESULT_PLATFORM;
		result = CheckDeadline(access);
		if (result != MISTER_RESULT_OK) return result;
		*value = observed;
		return MISTER_RESULT_OK;
	}
	Result Write(const Access &access, Register reg, uint32_t value)
	{
		if (released_ || release_started_) return MISTER_RESULT_INVALID_STATE;
		Result result = EnsureMappings(access, false);
		if (result != MISTER_RESULT_OK) return result;
		Slot &slot = Get(reg);
		if (!slot.held || slot.register_offset > slot.length ||
			slot.register_offset > slot.length - sizeof(uint32_t) ||
			(slot.register_offset & 3u) != 0)
			return MISTER_RESULT_PLATFORM;
		result = CheckDeadline(access);
		if (result != MISTER_RESULT_OK) return result;
		if (operations_->Write32(slot.mapping, slot.register_offset, value) != 0)
			return MISTER_RESULT_PLATFORM;
		mutation_applied_latch_ = true;
		result = CheckDeadline(access);
		if (result != MISTER_RESULT_OK) return result;
		if (operations_->OrderingBarrier() != 0) return MISTER_RESULT_PLATFORM;
		return CheckDeadline(access);
	}
	bool AllMappingsAbsent() const
	{
		for (const Slot &slot : slots_)
			if (slot.held) return false;
		return true;
	}
	PosixOperations posix_;
#if defined(MISTER_NATIVE_MMIO_TESTING)
	TestOperationsBridge test_;
#endif
	LinuxOperations *operations_;
	Slot slots_[static_cast<size_t>(Register::count)];
	int descriptor_;
	bool geometry_initialized_;
	bool release_started_;
	bool released_;
	bool descriptor_unknown_;
	bool programming_started_;
	bool program_complete_;
	bool bridges_attempted_;
	bool core_normal_observed_;
	std::unique_ptr<NativeBridgeActivationAuthority> bridge_authority_;
	bool input_target_may_be_selected_ = false;
	uint64_t expected_bytes_;
	uint64_t programmed_bytes_;
	bool manager_residue_;
	int manager_neutral_step_;
	uint32_t manager_control_;
	uint32_t manager_mode_;
	bool mutation_applied_latch_;
};

NativeLinuxMmioAdapter::NativeLinuxMmioAdapter()
	: impl_(new (std::nothrow) Impl())
{
}

#if defined(MISTER_NATIVE_MMIO_TESTING)
NativeLinuxMmioAdapter::NativeLinuxMmioAdapter(
	NativeMmioTestOperations &operations)
	: impl_(new (std::nothrow) Impl(operations))
{
}
#endif

NativeLinuxMmioAdapter::~NativeLinuxMmioAdapter()
{
	if (impl_ != nullptr) (void)impl_->CloseMappingsForProcessExit();
	delete impl_;
}

#define IMPL_CALL(expression) \
	return impl_ == nullptr ? MISTER_RESULT_PLATFORM : (expression)

Result NativeLinuxMmioAdapter::WriteCoreReset(const Access &access,
	uint32_t mask, uint32_t value)
{
	IMPL_CALL(impl_->WriteCoreReset(access, mask, value));
}
NativeMappingAcquisitionReceipt NativeLinuxMmioAdapter::AcquireMappings(
	const Access &access)
{
	if (impl_ == nullptr) {
		const NativeMappingAcquisitionReceipt receipt = {
			MISTER_RESULT_PLATFORM, false, false};
		return receipt;
	}
	return impl_->AcquireMappings(access);
}
NativeBridgeEnableReceipt NativeLinuxMmioAdapter::EnableBridges(
	const Access &access)
{
	if (impl_ == nullptr) {
		const NativeBridgeEnableReceipt receipt = {
			MISTER_RESULT_PLATFORM, false, false, false, false, false, false, false,
			0, 0};
		return receipt;
	}
	return impl_->EnableBridges(access);
}
Result NativeLinuxMmioAdapter::InstallBridgeActivationAuthority(
	const Access &, std::unique_ptr<NativeBridgeActivationAuthority> authority)
{
	if (impl_ == nullptr) return MISTER_RESULT_PLATFORM;
	return impl_->InstallBridgeActivationAuthority(std::move(authority));
}
Result NativeLinuxMmioAdapter::ValidateBridgeActivationAuthority(
	const HardwareLeaseView &view) const
{
	if (impl_ == nullptr || !impl_->BridgeActivationAuthorityLocallyCurrent())
		return MISTER_RESULT_INVALID_STATE;
	return view.ValidateBridgeActivationAuthority(
		impl_->BridgeActivationAuthority());
}
#if defined(MISTER_NATIVE_MMIO_TESTING)
Result NativeLinuxMmioAdapter::ValidateBridgeActivationAuthorityForTest(
	const HardwareLeaseView &view) const
{
	return ValidateBridgeActivationAuthority(view);
}
Result NativeLinuxMmioAdapter::InstallBridgeActivationAuthorityForTest(
	std::unique_ptr<NativeBridgeActivationAuthority> authority)
{
	if (impl_ == nullptr) return MISTER_RESULT_PLATFORM;
	return impl_->InstallBridgeActivationAuthority(std::move(authority));
}
bool NativeLinuxMmioAdapter::HasBridgeActivationAuthorityForTest() const
{
	return impl_ != nullptr && impl_->HasBridgeActivationAuthority();
}
#endif
NativeManagerNeutralReceipt NativeLinuxMmioAdapter::ReconcileManager(
	const Access &access)
{
	if (impl_ == nullptr) {
		const NativeManagerNeutralReceipt receipt = {
			MISTER_RESULT_PLATFORM, 0, 0, false, false, false};
		return receipt;
	}
	return impl_->ReconcileManager(access);
}
Result NativeLinuxMmioAdapter::ReadManagerControl(const Access &access,
	uint32_t *value)
{
	IMPL_CALL(impl_->ReadManagerControl(access, value));
}
Result NativeLinuxMmioAdapter::ReadManagerMode(const Access &access,
	uint32_t *value)
{
	IMPL_CALL(impl_->ReadManagerMode(access, value));
}
bool NativeLinuxMmioAdapter::RecoveryMappingsHeld(const Access &) const
{
	return impl_ != nullptr && impl_->RecoveryMappingsHeld();
}
bool NativeLinuxMmioAdapter::ConsumeAppliedMutation(const Access &)
{
	return impl_ != nullptr && impl_->ConsumeAppliedMutation();
}
NativeFpgaSinkStartOutcome NativeLinuxMmioAdapter::Begin(
	uint64_t expected_bytes, uint64_t absolute_deadline_ms,
	std::unique_ptr<NativeFpgaProgramSession> *session)
{
	if (impl_ == nullptr) {
		const NativeFpgaSinkStartOutcome outcome = {
			MISTER_RESULT_PLATFORM, false, false, false};
		return outcome;
	}
	return impl_->Begin(expected_bytes, absolute_deadline_ms, session);
}
Result NativeLinuxMmioAdapter::WriteInterfaceModule(const Access &access,
	uint32_t value)
{
	IMPL_CALL(impl_->WriteInterfaceModule(access, value));
}
Result NativeLinuxMmioAdapter::WriteSdrPortControl(const Access &access,
	uint32_t offset, uint32_t value)
{
	IMPL_CALL(impl_->WriteSdrPortControl(access, offset, value));
}
Result NativeLinuxMmioAdapter::WriteBridgeReset(const Access &access,
	uint32_t value)
{
	IMPL_CALL(impl_->WriteBridgeReset(access, value));
}
Result NativeLinuxMmioAdapter::WriteRemap(const Access &access,
	uint32_t value)
{
	IMPL_CALL(impl_->WriteRemap(access, value));
}
Result NativeLinuxMmioAdapter::ReadCoreGpo(const Access &access,
	uint32_t *value)
{
	IMPL_CALL(impl_->ReadCoreGpo(access, value));
}
Result NativeLinuxMmioAdapter::ReadInterfaceModule(const Access &access,
	uint32_t *value)
{
	IMPL_CALL(impl_->ReadInterfaceModule(access, value));
}
Result NativeLinuxMmioAdapter::ReadSdrPortControl(const Access &access,
	uint32_t offset, uint32_t *value)
{
	IMPL_CALL(impl_->ReadSdrPortControl(access, offset, value));
}
Result NativeLinuxMmioAdapter::ReadBridgeReset(const Access &access,
	uint32_t *value)
{
	IMPL_CALL(impl_->ReadBridgeReset(access, value));
}
Result NativeLinuxMmioAdapter::ReadRemap(const Access &access,
	uint32_t *value)
{
	IMPL_CALL(impl_->ReadRemap(access, value));
}
Result NativeLinuxMmioAdapter::ReleaseMappings(const Access &access)
{
	IMPL_CALL(impl_->ReleaseMappings(access));
}
Result NativeLinuxMmioAdapter::CloseMappingsForProcessExit()
{
	IMPL_CALL(impl_->CloseMappingsForProcessExit());
}

NativeSpiMutationResult NativeLinuxMmioAdapter::Select(
	const HardwareLeaseView &view, NativeSpiTarget target)
{
	if (target != NativeSpiTarget::user_io)
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false};
	const Result authority = ValidateBridgeActivationAuthority(view);
	if (authority != MISTER_RESULT_OK)
		return {authority, false, false, false};
	return impl_ == nullptr ?
		NativeSpiMutationResult{MISTER_RESULT_PLATFORM, false, false, false} :
		impl_->SelectUserIo(view.absolute_deadline_ms());
}
NativeSpiMutationResult NativeLinuxMmioAdapter::WriteWordWithStrobeLow(
	const HardwareLeaseView &view, uint16_t word)
{
	const Result authority = ValidateBridgeActivationAuthority(view);
	if (authority != MISTER_RESULT_OK)
		return {authority, false, false, false};
	return impl_ == nullptr ?
		NativeSpiMutationResult{MISTER_RESULT_PLATFORM, false, false, false} :
		impl_->WriteInputWord(view.absolute_deadline_ms(), word);
}
NativeSpiMutationResult NativeLinuxMmioAdapter::SetStrobe(
	const HardwareLeaseView &view, bool high)
{
	const Result authority = ValidateBridgeActivationAuthority(view);
	if (authority != MISTER_RESULT_OK)
		return {authority, false, false, false};
	return impl_ == nullptr ?
		NativeSpiMutationResult{MISTER_RESULT_PLATFORM, false, false, false} :
		impl_->SetInputStrobe(view.absolute_deadline_ms(), high);
}
Result NativeLinuxMmioAdapter::ReadAckSample(const HardwareLeaseView &view,
	NativeSpiAckSample *sample)
{
	const Result authority = ValidateBridgeActivationAuthority(view);
	if (authority != MISTER_RESULT_OK) return authority;
	return impl_ == nullptr ? MISTER_RESULT_PLATFORM :
		impl_->ReadInputAck(view.absolute_deadline_ms(), sample);
}
NativeSpiMutationResult NativeLinuxMmioAdapter::Deselect(
	const HardwareLeaseView &view, NativeSpiTarget target,
	uint64_t absolute_deadline_ms)
{
	if (target != NativeSpiTarget::user_io ||
		absolute_deadline_ms != view.absolute_deadline_ms())
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false};
	const Result authority = ValidateBridgeActivationAuthority(view);
	if (authority != MISTER_RESULT_OK)
		return {authority, false, false, false};
	return impl_ == nullptr ?
		NativeSpiMutationResult{MISTER_RESULT_PLATFORM, false, false, false} :
		impl_->DeselectUserIo(absolute_deadline_ms);
}

Result NativeLinuxMmioAdapter::CleanupValidateDigitalNeutralAuthority(
	const CleanupInputReplayView &view)
{
	if (impl_ == nullptr || !impl_->BridgeActivationAuthorityLocallyCurrent())
		return MISTER_RESULT_INVALID_STATE;
	return view.ValidateBridgeActivationAuthority(
		impl_->BridgeActivationAuthority());
}

Result NativeLinuxMmioAdapter::CleanupObserveDigitalNeutralResidue(
	const CleanupInputReplayView &view, bool *user_io_selected,
	bool *strobe_high)
{
	const Result authority = CleanupValidateDigitalNeutralAuthority(view);
	if (authority != MISTER_RESULT_OK) return authority;
	return impl_->ObserveInputResidue(view.absolute_deadline_ms(),
		user_io_selected, strobe_high);
}

NativeSpiMutationResult NativeLinuxMmioAdapter::CleanupSelectUserIo(
	const CleanupInputReplayView &view)
{
	const Result authority = CleanupValidateDigitalNeutralAuthority(view);
	if (authority != MISTER_RESULT_OK)
		return {authority, false, false, false};
	return impl_->SelectUserIo(view.absolute_deadline_ms());
}

NativeSpiMutationResult NativeLinuxMmioAdapter::CleanupWriteDigitalNeutralWord(
	const CleanupInputReplayView &view, uint8_t authorized_word_index)
{
	const Result authority = CleanupValidateDigitalNeutralAuthority(view);
	if (authority != MISTER_RESULT_OK)
		return {authority, false, false, false};
	uint16_t word = 0;
	const Result authorized = view.AuthorizedWord(authorized_word_index, &word);
	if (authorized != MISTER_RESULT_OK)
		return {authorized, false, false, false};
	return impl_->WriteInputWord(view.absolute_deadline_ms(), word);
}

NativeSpiMutationResult NativeLinuxMmioAdapter::CleanupSetStrobe(
	const CleanupInputReplayView &view, bool high)
{
	const Result authority = CleanupValidateDigitalNeutralAuthority(view);
	if (authority != MISTER_RESULT_OK)
		return {authority, false, false, false};
	return impl_->SetInputStrobe(view.absolute_deadline_ms(), high);
}

Result NativeLinuxMmioAdapter::CleanupReadAckSample(
	const CleanupInputReplayView &view, NativeSpiAckSample *sample)
{
	const Result authority = CleanupValidateDigitalNeutralAuthority(view);
	if (authority != MISTER_RESULT_OK) return authority;
	return impl_->ReadInputAck(view.absolute_deadline_ms(), sample);
}

NativeSpiMutationResult NativeLinuxMmioAdapter::CleanupDeselectUserIo(
	const CleanupInputReplayView &view, uint64_t absolute_deadline_ms)
{
	if (absolute_deadline_ms != view.absolute_deadline_ms())
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false};
	const Result authority = CleanupValidateDigitalNeutralAuthority(view);
	if (authority != MISTER_RESULT_OK)
		return {authority, false, false, false};
	return impl_->DeselectUserIo(absolute_deadline_ms);
}

#undef IMPL_CALL

#if defined(MISTER_NATIVE_MMIO_TESTING)
bool NativeLinuxMmioProductionPrimitivesRejectInvalidAccessForTest()
{
	PosixOperations operations;
	Mapping failed;
	if (operations.Map(-1, 0, 4096, &failed) == 0 ||
		failed.identity != std::numeric_limits<uintptr_t>::max())
		return false;
	alignas(uint32_t) uint32_t register_value = 0x11223344u;
	Mapping aligned;
	aligned.identity = reinterpret_cast<uintptr_t>(&register_value);
	uint32_t observed = 0;
	if (operations.Read32(aligned, 0, &observed) != 0 ||
		observed != register_value ||
		operations.Write32(aligned, 0, 0x55667788u) != 0 ||
		register_value != 0x55667788u)
		return false;
	Mapping misaligned;
	misaligned.identity = aligned.identity + 1;
	Mapping overflow;
	overflow.identity = UINTPTR_MAX - 1;
	return operations.Read32(misaligned, 0, &observed) != 0 &&
		operations.Write32(misaligned, 0, 0) != 0 &&
		operations.Read32(overflow, 4, &observed) != 0 &&
		operations.Write32(overflow, 4, 0) != 0;
}
#endif

} // namespace linux_native
} // namespace native
} // namespace mister
