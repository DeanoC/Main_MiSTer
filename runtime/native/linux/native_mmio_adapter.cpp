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
const size_t kMinimumPageSize = 4096;

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
		count
	};
	struct Slot {
		Mapping mapping;
		uint64_t page_offset;
		size_t register_offset;
		size_t length;
		bool held;
	};

	Impl()
		: operations_(&posix_), descriptor_(-1), geometry_initialized_(false),
		  release_started_(false), released_(false)
	{
		InitializeSlots();
	}
#if defined(MISTER_NATIVE_MMIO_TESTING)
	explicit Impl(NativeMmioTestOperations &operations)
		: test_(operations), operations_(&test_), descriptor_(-1),
		  geometry_initialized_(false), release_started_(false), released_(false)
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
		if (!AllMappingsAbsent() || descriptor_ >= 0) {
			return first_failure == MISTER_RESULT_OK ?
				MISTER_RESULT_CLEANUP_INCOMPLETE : first_failure;
		}
		if (first_failure == MISTER_RESULT_OK) released_ = true;
		return first_failure;
	}

private:
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
			kBridgeResetAddress, kRemapAddress
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
		return operations_->NowMs() >= access.absolute_deadline_ms() ?
			MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
	}
	Result CloseDescriptor(const Access &access)
	{
		if (descriptor_ < 0) return MISTER_RESULT_OK;
		Result result = CheckDeadline(access);
		if (result != MISTER_RESULT_OK) return result;
		if (operations_->Close(descriptor_) != 0) return MISTER_RESULT_PLATFORM;
		descriptor_ = -1;
		return CheckDeadline(access);
	}
	Result EnsureMappings(const Access &access)
	{
		if (released_ || release_started_) return MISTER_RESULT_INVALID_STATE;
		Result result = InitializeGeometry();
		if (result != MISTER_RESULT_OK) return result;
		result = CheckDeadline(access);
		if (result != MISTER_RESULT_OK) return result;
		bool missing = false;
		for (const Slot &slot : slots_)
			if (!slot.held) missing = true;
		if (missing && descriptor_ < 0) {
			descriptor_ = operations_->Open("/dev/mem",
				O_RDWR | O_SYNC | O_CLOEXEC);
			if (descriptor_ < 0) return MISTER_RESULT_PLATFORM;
			result = CheckDeadline(access);
			if (result != MISTER_RESULT_OK) return result;
		}
		for (Slot &slot : slots_) {
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
		for (const Slot &slot : slots_)
			if (!slot.held) return MISTER_RESULT_PLATFORM;
		return MISTER_RESULT_OK;
	}
	Result Read(const Access &access, Register reg, uint32_t *value)
	{
		if (value == nullptr || released_ || release_started_)
			return value == nullptr ? MISTER_RESULT_INVALID_ARGUMENT :
				MISTER_RESULT_INVALID_STATE;
		Result result = EnsureMappings(access);
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
		Result result = EnsureMappings(access);
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
	delete impl_;
}

#define IMPL_CALL(expression) \
	return impl_ == nullptr ? MISTER_RESULT_PLATFORM : (expression)

Result NativeLinuxMmioAdapter::WriteCoreReset(const Access &access,
	uint32_t mask, uint32_t value)
{
	IMPL_CALL(impl_->WriteCoreReset(access, mask, value));
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
