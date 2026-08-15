// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(__linux__) && !defined(_LARGEFILE64_SOURCE)
#define _LARGEFILE64_SOURCE
#endif

#include "runtime/native/linux/native_core_protocol_io_adapter.hpp"
#include "runtime/native/native_core_protocol_session_state.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <limits>
#include <new>
#include <utility>

namespace mister {
namespace native {
namespace linux_native {
namespace {

const uint64_t kManagerPageAddress = 0xff706000u;
const size_t kGpoOffset = 0x10u;
const size_t kGpiOffset = 0x14u;
const size_t kMinimumPageSize = 4096;
const uint32_t kDataMask = 0x0000ffffu;
const uint32_t kStrobe = 0x00020000u;
const uint32_t kFileIoSelect = 0x00040000u;
const uint32_t kUserIoSelect = 0x00100000u;
const uint32_t kIdentityQuery = 0x80000000u;
const uint32_t kOwnedSpiMask = kStrobe | kFileIoSelect | kUserIoSelect;

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
	int Open(const char *path, int flags) override { return open(path, flags); }
	int Close(int descriptor) override { return close(descriptor); }
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
	int Read32(const Mapping &mapping, size_t offset, uint32_t *value) override
	{
		if (value == nullptr || mapping.identity == 0 ||
			mapping.identity == std::numeric_limits<uintptr_t>::max() ||
			mapping.identity > UINTPTR_MAX - offset ||
			((mapping.identity + offset) & 3u) != 0)
			return -1;
		const volatile uint32_t *address =
			reinterpret_cast<volatile uint32_t *>(mapping.identity + offset);
		*value = *address;
		return 0;
	}
	int Write32(const Mapping &mapping, size_t offset, uint32_t value) override
	{
		if (mapping.identity == 0 ||
			mapping.identity == std::numeric_limits<uintptr_t>::max() ||
			mapping.identity > UINTPTR_MAX - offset ||
			((mapping.identity + offset) & 3u) != 0)
			return -1;
		volatile uint32_t *address =
			reinterpret_cast<volatile uint32_t *>(mapping.identity + offset);
		*address = value;
		return 0;
	}
	int OrderingBarrier() override
	{
		// GCC TSan does not model atomic fences. This fence orders MMIO rather than
		// C++ shared memory, so keep -Wtsan visible without promoting it to an error.
#if defined(__GNUC__) && defined(__SANITIZE_THREAD__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wtsan"
#endif
		std::atomic_thread_fence(std::memory_order_seq_cst);
#if defined(__GNUC__) && defined(__SANITIZE_THREAD__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
		return 0;
	}
};

#if defined(MISTER_NATIVE_CORE_PROTOCOL_IO_TESTING)
class TestOperationsBridge final : public LinuxOperations {
public:
	TestOperationsBridge() : operations_(nullptr) {}
	explicit TestOperationsBridge(NativeCoreProtocolIoTestOperations &operations)
		: operations_(&operations) {}
	size_t PageSize() const override { return operations_->PageSize(); }
	int Open(const char *path, int flags) override
		{ return operations_->Open(path, flags); }
	int Close(int descriptor) override { return operations_->Close(descriptor); }
	int Map(int descriptor, uint64_t page_offset, size_t length,
		Mapping *mapping) override
	{
		NativeCoreProtocolIoTestMapping exposed;
		const int result = operations_->Map(descriptor, page_offset, length,
			&exposed);
		mapping->identity = exposed.identity;
		return result;
	}
	int Unmap(const Mapping &mapping, size_t length) override
	{
		NativeCoreProtocolIoTestMapping exposed;
		exposed.identity = mapping.identity;
		return operations_->Unmap(exposed, length);
	}
	int Read32(const Mapping &mapping, size_t offset, uint32_t *value) override
	{
		NativeCoreProtocolIoTestMapping exposed;
		exposed.identity = mapping.identity;
		return operations_->Read32(exposed, offset, value);
	}
	int Write32(const Mapping &mapping, size_t offset, uint32_t value) override
	{
		NativeCoreProtocolIoTestMapping exposed;
		exposed.identity = mapping.identity;
		return operations_->Write32(exposed, offset, value);
	}
	int OrderingBarrier() override { return operations_->OrderingBarrier(); }

private:
	NativeCoreProtocolIoTestOperations *operations_;
};
#endif

bool ValidTarget(NativeSpiTarget target)
{
	return target == NativeSpiTarget::user_io ||
		target == NativeSpiTarget::file_io;
}

uint32_t TargetBit(NativeSpiTarget target)
{
	return target == NativeSpiTarget::user_io ? kUserIoSelect : kFileIoSelect;
}

CoreProtocolResidue EmptyResidue()
{
	const CoreProtocolResidue residue = {false, false, false, false, false,
		false, false, 0};
	return residue;
}

} // namespace

class LinuxCoreProtocolSessionIoState final : public ProtocolSessionIoState {
public:
	LinuxCoreProtocolSessionIoState()
		: adapter_owner(nullptr), descriptor(-1), mapping(), page_size(0), mapped(false),
		  selected(false), selected_target(NativeSpiTarget::user_io),
		  stop_completed(false), identity_mode_may_be_asserted(false),
		  residue(EmptyResidue()), last_mutation_sequence(0),
		  force_idle_completed(false), unmap_attempted(false),
		  descriptor_close_attempted(false)
	{
	}

	const void *adapter_owner;
	int descriptor;
	Mapping mapping;
	size_t page_size;
	bool mapped;
	bool selected;
	NativeSpiTarget selected_target;
	bool stop_completed;
	bool identity_mode_may_be_asserted;
	CoreProtocolResidue residue;
	uint64_t last_mutation_sequence;
	bool force_idle_completed;
	bool unmap_attempted;
	bool descriptor_close_attempted;
};

class NativeCoreProtocolIoAdapterImpl final {
public:
	NativeCoreProtocolIoAdapterImpl(NativeClock &clock)
		: clock_(clock), operations_(&posix_), state_()
	{
	}
#if defined(MISTER_NATIVE_CORE_PROTOCOL_IO_TESTING)
	NativeCoreProtocolIoAdapterImpl(NativeClock &clock, NativeCoreProtocolIoTestOperations &operations)
		: clock_(clock), test_(operations), operations_(&test_), state_()
	{
	}
#endif

	Result Begin(const std::shared_ptr<ProtocolSessionState> &state,
		uint64_t absolute_deadline_ms)
	{
		if (!state || !state->view ||
			absolute_deadline_ms != state->view->absolute_deadline_ms())
			return MISTER_RESULT_INVALID_STATE;
		if (state_ && state_.get() != state.get()) {
			// An active failure can retain this adapter's physical mapping after
			// its broker view is atomically completed. Cleanup may adopt it only
			// through a newly minted typed session after that old handle was
			// finalized; it never transfers a live view or registration.
			LinuxCoreProtocolSessionIoState *old = Storage();
			const bool retaining_physical_session = old != nullptr &&
				(old->mapped || old->descriptor >= 0);
			if (retaining_physical_session && state_->handle_state.load() !=
				ProtocolSessionHandleState::finalized)
				return MISTER_RESULT_INVALID_STATE;
			if (retaining_physical_session) {
				if (state->io_state) return MISTER_RESULT_INVALID_STATE;
				// Move the exact opaque Linux mapping state—not just a copy of its
				// flags—into the newly-authorized cleanup/recovery session state.
				// Thus a later retry owns the original descriptor, mapping, residue,
				// and cumulative release receipt under its typed session.
				state->io_state = state_->io_state;
				state_->io_state.reset();
			}
		}
		state_ = state;
		if (!state_->io_state) {
			std::shared_ptr<LinuxCoreProtocolSessionIoState> storage(
				new (std::nothrow) LinuxCoreProtocolSessionIoState());
			if (!storage) return MISTER_RESULT_PLATFORM;
			storage->adapter_owner = this;
			state_->io_state = storage;
		}
		if (Storage() == nullptr || Storage()->adapter_owner != this)
			return MISTER_RESULT_INVALID_STATE;
		// A same-registration retry continues only the remaining release step.
		// Its earlier force-low/readback is already positively receipted, so it
		// must not remap, force idle again, or advance mutation sequence.
		if (Storage()->force_idle_completed) return MISTER_RESULT_OK;
		Result result = EnsureMapped(absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		return ForceIdle(absolute_deadline_ms);
	}

	uint64_t CurrentDeadline() const
	{
		return state_ && state_->view ? state_->view->absolute_deadline_ms() : 0;
	}

	Result Probe(NativeLiveCoreObservation *observation,
		uint64_t absolute_deadline_ms)
	{
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (observation == nullptr || storage == nullptr || !storage->mapped) return
			MISTER_RESULT_INVALID_ARGUMENT;
		if (Deadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		Result result = WriteMasked(kIdentityQuery, 0, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) {
			storage->identity_mode_may_be_asserted = true;
			return result;
		}
		uint32_t observed_gpo = 0;
		result = ReadGpo(absolute_deadline_ms, &observed_gpo);
		if (result != MISTER_RESULT_OK || (observed_gpo & kIdentityQuery) != 0) {
			storage->identity_mode_may_be_asserted = true;
			return result == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : result;
		}
		uint32_t identity = 0;
		result = ReadGpi(absolute_deadline_ms, &identity);
		const Result restore = WriteMasked(kIdentityQuery, kIdentityQuery,
			absolute_deadline_ms);
		if (restore == MISTER_RESULT_OK) {
			result = result == MISTER_RESULT_OK ? ReadGpo(absolute_deadline_ms,
				&observed_gpo) : result;
			if (result == MISTER_RESULT_OK && (observed_gpo & kIdentityQuery) == 0)
				result = MISTER_RESULT_PLATFORM;
		}
		if (restore != MISTER_RESULT_OK && result == MISTER_RESULT_OK)
			result = restore;
		storage->identity_mode_may_be_asserted = result != MISTER_RESULT_OK;
		if (result != MISTER_RESULT_OK) return result;
		uint32_t width = 0;
		uint32_t io_version = 0;
		result = ReadGpi(absolute_deadline_ms, &width);
		if (result != MISTER_RESULT_OK) return result;
		result = ReadGpi(absolute_deadline_ms, &io_version);
		if (result != MISTER_RESULT_OK) return result;
		observation->identity_magic = (identity >> 8) & 0x00ffffffu;
		observation->core_type = static_cast<uint8_t>(identity & 0xffu);
		observation->file_io_width = (width & 0x00010000u) != 0 ?
			NativeFileIoWidth::little_endian_byte_pairs :
			NativeFileIoWidth::byte_per_word;
		observation->fpga_io_version = static_cast<uint8_t>((io_version >> 18) & 3u);
		return MISTER_RESULT_OK;
	}

	Result Exchange(NativeSpiTarget target, const uint16_t *transmit_words,
		size_t word_count, uint16_t *received_words, size_t received_capacity,
		bool begins_selected, bool ends_selected, uint64_t absolute_deadline_ms)
	{
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (storage == nullptr || !storage->mapped || !ValidTarget(target) ||
			transmit_words == nullptr || word_count == 0 ||
			(received_words == nullptr ? received_capacity != 0 :
			 received_capacity < word_count) ||
			absolute_deadline_ms != state_->view->absolute_deadline_ms())
			return MISTER_RESULT_INVALID_ARGUMENT;
		const bool continuation = !begins_selected && storage->selected;
		if (continuation && storage->selected_target != target) return MISTER_RESULT_INVALID_STATE;
		const bool is_file_transfer_frame = target == NativeSpiTarget::file_io &&
			word_count >= 2 && transmit_words[0] == 0x0053;
		const bool is_status_frame = target == NativeSpiTarget::user_io &&
			word_count >= 2 && transmit_words[0] == 0x001e;
		const bool fully_deselects = !(begins_selected && !ends_selected);
		Result result = MISTER_RESULT_OK;
		if (!continuation) result = Select(target, absolute_deadline_ms);
		for (size_t index = 0; result == MISTER_RESULT_OK && index < word_count;
			++index) {
			result = WriteMasked(kDataMask | kStrobe, transmit_words[index],
				absolute_deadline_ms);
			if (result != MISTER_RESULT_OK) break;
			result = WriteMasked(kStrobe, kStrobe, absolute_deadline_ms);
			if (result != MISTER_RESULT_OK) break;
			uint32_t gpi = 0;
			result = WaitForAck(true, absolute_deadline_ms, &gpi);
			if (result != MISTER_RESULT_OK) break;
			result = WriteMasked(kStrobe, 0, absolute_deadline_ms);
			if (result != MISTER_RESULT_OK) break;
			result = WaitForAck(false, absolute_deadline_ms, &gpi);
			if (result != MISTER_RESULT_OK) break;
			if (received_words != nullptr)
				received_words[index] = static_cast<uint16_t>(gpi & kDataMask);
		}
		if (result != MISTER_RESULT_OK) {
			const Result forced = ForceIdle(absolute_deadline_ms);
			return result == MISTER_RESULT_OK ? forced : result;
		}
		if (!fully_deselects) return MISTER_RESULT_OK;
		// A completed asserting value/status word may have already changed the
		// core before the final typed deselect begins. Record only these positive
		// effects before the deselect so every later deselect primitive failure
		// retains conservative cleanup evidence. Clear frames remain deliberately
		// deferred until the deselect/readback proves their full completion.
		if (is_file_transfer_frame && transmit_words[1] == 0x00ff)
			storage->residue.download_may_be_active = true;
		if (is_status_frame && (transmit_words[1] & 1u) != 0)
			storage->residue.status_reset_asserted = true;
		result = Deselect(target, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		if (is_file_transfer_frame && transmit_words[1] == 0x0000)
			storage->residue.download_may_be_active = false;
		if (is_status_frame && (transmit_words[1] & 1u) == 0)
			storage->residue.status_reset_asserted = false;
		return MISTER_RESULT_OK;
	}

	Result CloseSelected(NativeSpiTarget target, uint64_t absolute_deadline_ms)
	{
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (storage == nullptr || !storage->selected ||
			storage->selected_target != target) return MISTER_RESULT_INVALID_STATE;
		return Deselect(target, absolute_deadline_ms);
	}

	Result Release(uint64_t absolute_deadline_ms,
		ProtocolMappingReleaseReceipt *receipt)
	{
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (receipt == nullptr || storage == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		*receipt = {MISTER_RESULT_INVALID_STATE, false, false, false, false,
			false, storage->last_mutation_sequence};
		Result first = MISTER_RESULT_OK;
		if (!storage->force_idle_completed) {
			first = ForceIdle(absolute_deadline_ms);
			if (first == MISTER_RESULT_OK) storage->force_idle_completed = true;
		}
		receipt->selected_transaction_closed = storage->force_idle_completed;
		if (first != MISTER_RESULT_OK) {
			receipt->result = first;
			return first;
		}
		receipt->unmap_attempted = storage->unmap_attempted;
		if (storage->mapped) {
			storage->unmap_attempted = true;
			receipt->unmap_attempted = true;
			if (Deadline(absolute_deadline_ms) != MISTER_RESULT_OK)
				first = MISTER_RESULT_DEADLINE;
			else if (operations_->Unmap(storage->mapping, storage->page_size) != 0)
				first = MISTER_RESULT_PLATFORM;
			else {
				storage->mapped = false;
				storage->mapping.identity = 0;
				if (Deadline(absolute_deadline_ms) != MISTER_RESULT_OK)
					first = MISTER_RESULT_DEADLINE;
			}
		}
		receipt->mapping_absent = !storage->mapped;
		receipt->descriptor_close_attempted = storage->descriptor_close_attempted;
		if (storage->descriptor >= 0 && first != MISTER_RESULT_DEADLINE) {
			storage->descriptor_close_attempted = true;
			receipt->descriptor_close_attempted = true;
			if (Deadline(absolute_deadline_ms) != MISTER_RESULT_OK)
				first = MISTER_RESULT_DEADLINE;
			else if (operations_->Close(storage->descriptor) != 0) {
				if (first == MISTER_RESULT_OK) first = MISTER_RESULT_PLATFORM;
			} else {
				storage->descriptor = -1;
			}
		}
		receipt->descriptor_absent = storage->descriptor < 0;
		receipt->mutation_sequence = storage->last_mutation_sequence;
		receipt->result = first;
		return first;
	}

	Result Shutdown(bool download_may_be_active, uint64_t absolute_deadline_ms,
		ProtocolMappingReleaseReceipt *receipt)
	{
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (storage == nullptr) return MISTER_RESULT_INVALID_STATE;
		Result first = MISTER_RESULT_OK;
		if (download_may_be_active && !storage->stop_completed) {
			const uint16_t stop[] = {0x0053, 0x0000};
			first = Exchange(NativeSpiTarget::file_io, stop, 2, nullptr, 0,
				false, false, absolute_deadline_ms);
			if (first == MISTER_RESULT_OK) storage->stop_completed = true;
		}
		ProtocolMappingReleaseReceipt release = {};
		const Result released = Release(absolute_deadline_ms, &release);
		if (first == MISTER_RESULT_OK) first = released;
		release.result = first;
		if (receipt != nullptr) *receipt = release;
		return first;
	}

	void GetResidue(CoreProtocolResidue *residue,
		const ProtocolMappingReleaseReceipt &release) const
	{
		const LinuxCoreProtocolSessionIoState *storage = Storage();
		if (residue == nullptr || storage == nullptr) return;
		*residue = storage->residue;
		residue->mapping_retained = !release.mapping_absent ||
			!release.descriptor_absent;
		residue->identity_mode_may_be_asserted = storage->identity_mode_may_be_asserted;
		residue->user_io_selected = storage->selected &&
			storage->selected_target == NativeSpiTarget::user_io;
		residue->file_io_selected = storage->selected &&
			storage->selected_target == NativeSpiTarget::file_io;
		residue->strobe_may_be_high = false;
		residue->last_mutation_sequence = release.mutation_sequence;
	}

	void CloseForProcessExit()
	{
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (storage != nullptr && storage->mapped) {
			operations_->Unmap(storage->mapping, storage->page_size);
			storage->mapped = false;
			storage->mapping.identity = 0;
		}
		if (storage != nullptr && storage->descriptor >= 0) {
			operations_->Close(storage->descriptor);
			storage->descriptor = -1;
		}
		if (state_) state_->io_state.reset();
		state_.reset();
	}

private:
	LinuxCoreProtocolSessionIoState *Storage()
	{
		if (!state_ || !state_->io_state) return nullptr;
		return static_cast<LinuxCoreProtocolSessionIoState *>(
			state_->io_state.get());
	}
	const LinuxCoreProtocolSessionIoState *Storage() const
	{
		if (!state_ || !state_->io_state) return nullptr;
		return static_cast<const LinuxCoreProtocolSessionIoState *>(
			state_->io_state.get());
	}
	Result Deadline(uint64_t absolute_deadline_ms) const
	{
		return clock_.NowMs() >= absolute_deadline_ms ? MISTER_RESULT_DEADLINE :
			MISTER_RESULT_OK;
	}
	Result EnsureMapped(uint64_t absolute_deadline_ms)
	{
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (storage == nullptr) return MISTER_RESULT_INVALID_STATE;
		if (storage->mapped) return MISTER_RESULT_OK;
		if (storage->page_size == 0) {
			storage->page_size = operations_->PageSize();
			if (storage->page_size < kMinimumPageSize ||
				!IsPowerOfTwo(storage->page_size) ||
				kGpiOffset > storage->page_size - sizeof(uint32_t))
				return MISTER_RESULT_PLATFORM;
		}
		if (Deadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		if (storage->descriptor < 0) {
			storage->descriptor = operations_->Open("/dev/mem",
				O_RDWR | O_SYNC | O_CLOEXEC);
			if (storage->descriptor < 0) return MISTER_RESULT_PLATFORM;
			if (Deadline(absolute_deadline_ms) != MISTER_RESULT_OK)
				return MISTER_RESULT_DEADLINE;
		}
		Mapping candidate;
		if (operations_->Map(storage->descriptor, kManagerPageAddress,
			storage->page_size,
			&candidate) != 0 || candidate.identity == 0 ||
			candidate.identity == std::numeric_limits<uintptr_t>::max())
			return MISTER_RESULT_PLATFORM;
		storage->mapping = candidate;
		storage->mapped = true;
		return Deadline(absolute_deadline_ms);
	}
	Result ReadGpo(uint64_t absolute_deadline_ms, uint32_t *value) const
	{
		const LinuxCoreProtocolSessionIoState *storage = Storage();
		if (value == nullptr || storage == nullptr || !storage->mapped) return value == nullptr ?
			MISTER_RESULT_INVALID_ARGUMENT : MISTER_RESULT_INVALID_STATE;
		if (Deadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		if (operations_->Read32(storage->mapping, kGpoOffset, value) != 0)
			return MISTER_RESULT_PLATFORM;
		return Deadline(absolute_deadline_ms);
	}
	Result ReadGpi(uint64_t absolute_deadline_ms, uint32_t *value) const
	{
		const LinuxCoreProtocolSessionIoState *storage = Storage();
		if (value == nullptr || storage == nullptr || !storage->mapped) return value == nullptr ?
			MISTER_RESULT_INVALID_ARGUMENT : MISTER_RESULT_INVALID_STATE;
		if (Deadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		if (operations_->Read32(storage->mapping, kGpiOffset, value) != 0)
			return MISTER_RESULT_PLATFORM;
		return Deadline(absolute_deadline_ms);
	}
	Result WriteMasked(uint32_t mask, uint32_t value,
		uint64_t absolute_deadline_ms)
	{
		uint32_t current = 0;
		Result result = ReadGpo(absolute_deadline_ms, &current);
		if (result != MISTER_RESULT_OK) return result;
		if (Deadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		const uint32_t next = (current & ~mask) | (value & mask);
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (storage == nullptr) return MISTER_RESULT_INVALID_STATE;
		if (operations_->Write32(storage->mapping, kGpoOffset, next) != 0)
			return MISTER_RESULT_PLATFORM;
		if (operations_->OrderingBarrier() != 0) return MISTER_RESULT_PLATFORM;
		if (state_ && state_->view) {
			const uint64_t sequence = state_->view->RecordMutation();
			if (sequence == 0) return MISTER_RESULT_PLATFORM;
			storage->last_mutation_sequence = sequence;
		}
		return Deadline(absolute_deadline_ms);
	}
	Result ForceIdle(uint64_t absolute_deadline_ms)
	{
		Result result = WriteMasked(kOwnedSpiMask, 0, absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		uint32_t observed = 0;
		result = ReadGpo(absolute_deadline_ms, &observed);
		if (result != MISTER_RESULT_OK) return result;
		if ((observed & kOwnedSpiMask) != 0) return MISTER_RESULT_PLATFORM;
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (storage == nullptr) return MISTER_RESULT_INVALID_STATE;
		storage->selected = false;
		storage->residue.user_io_selected = false;
		storage->residue.file_io_selected = false;
		storage->residue.strobe_may_be_high = false;
		return MISTER_RESULT_OK;
	}
	Result Select(NativeSpiTarget target, uint64_t absolute_deadline_ms)
	{
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (storage == nullptr || storage->selected) return MISTER_RESULT_INVALID_STATE;
		const Result result = WriteMasked(kOwnedSpiMask, TargetBit(target),
			absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		uint32_t observed = 0;
		const Result read = ReadGpo(absolute_deadline_ms, &observed);
		if (read != MISTER_RESULT_OK) return read;
		if ((observed & kOwnedSpiMask) != TargetBit(target)) return MISTER_RESULT_PLATFORM;
		storage->selected = true;
		storage->selected_target = target;
		return MISTER_RESULT_OK;
	}
	Result Deselect(NativeSpiTarget target, uint64_t absolute_deadline_ms)
	{
		LinuxCoreProtocolSessionIoState *storage = Storage();
		if (storage == nullptr || !storage->selected ||
			storage->selected_target != target) return MISTER_RESULT_INVALID_STATE;
		return ForceIdle(absolute_deadline_ms);
	}
	Result WaitForAck(bool high, uint64_t absolute_deadline_ms,
		uint32_t *sample) const
	{
		for (;;) {
			uint32_t observed = 0;
			Result result = ReadGpi(absolute_deadline_ms, &observed);
			if (result != MISTER_RESULT_OK) return result;
			if ((observed & kIdentityQuery) != 0) return MISTER_RESULT_PLATFORM;
			if (((observed & kStrobe) != 0) == high) {
				if (sample != nullptr) *sample = observed;
				return MISTER_RESULT_OK;
			}
		}
	}

	NativeClock &clock_;
	PosixOperations posix_;
#if defined(MISTER_NATIVE_CORE_PROTOCOL_IO_TESTING)
	TestOperationsBridge test_;
#endif
	LinuxOperations *operations_;
	std::shared_ptr<ProtocolSessionState> state_;
};

class NativeCoreProtocolActiveAdapterView final : public NativeActiveCoreProtocolIo {
public:
	explicit NativeCoreProtocolActiveAdapterView(NativeCoreProtocolIoAdapterImpl &impl)
		: impl_(impl) {}

private:
	MisterResult BeginActive(ActiveCoreProtocolSession &session,
		uint64_t deadline) override { return impl_.Begin(session.state_, deadline); }
	MisterResult FinishActive(ActiveCoreProtocolSession &,
		ProtocolMappingReleaseReceipt *receipt) override
		{ return impl_.Release(impl_.CurrentDeadline(), receipt); }
	MisterResult AbortActive(ActiveCoreProtocolSession &, MisterResult,
		CoreProtocolResidue *residue, ProtocolMappingReleaseReceipt *receipt) override
	{
		const Result result = impl_.Release(impl_.CurrentDeadline(), receipt);
		impl_.GetResidue(residue, *receipt);
		return result;
	}
	MisterResult Probe(NativeLiveCoreObservation *observation,
		uint64_t deadline) override { return impl_.Probe(observation, deadline); }
	MisterResult Exchange(NativeSpiTarget target, const uint16_t *words,
		size_t count, uint16_t *responses, size_t capacity, bool begins_selected,
		bool ends_selected, uint64_t deadline) override
		{ return impl_.Exchange(target, words, count, responses, capacity,
			begins_selected, ends_selected, deadline); }
	MisterResult CloseSelected(NativeSpiTarget target, uint64_t deadline) override
		{ return impl_.CloseSelected(target, deadline); }

	NativeCoreProtocolIoAdapterImpl &impl_;
};

class NativeCoreProtocolCleanupAdapterView final : public NativeCleanupCoreProtocolIo {
public:
	explicit NativeCoreProtocolCleanupAdapterView(NativeCoreProtocolIoAdapterImpl &impl)
		: impl_(impl) {}

private:
	MisterResult BeginCleanup(CleanupCoreProtocolSession &session,
		uint64_t deadline) override { return impl_.Begin(session.state_, deadline); }
	MisterResult ShutdownCleanup(CleanupCoreProtocolSession &,
		bool download_may_be_active, ProtocolMappingReleaseReceipt *receipt) override
		{ return impl_.Shutdown(download_may_be_active, impl_.CurrentDeadline(), receipt); }

	NativeCoreProtocolIoAdapterImpl &impl_;
};

class NativeCoreProtocolRecoveryAdapterView final : public NativeRecoveryCoreProtocolIo {
public:
	explicit NativeCoreProtocolRecoveryAdapterView(NativeCoreProtocolIoAdapterImpl &impl)
		: impl_(impl) {}

private:
	MisterResult BeginRecovery(RecoveryCoreProtocolSession &session,
		uint64_t deadline) override { return impl_.Begin(session.state_, deadline); }
	MisterResult DisableRecovery(RecoveryCoreProtocolSession &,
		ProtocolMappingReleaseReceipt *receipt) override
		{ return impl_.Release(impl_.CurrentDeadline(), receipt); }

	NativeCoreProtocolIoAdapterImpl &impl_;
};

class NativeCoreProtocolExitAdapterView final : public NativeCoreProtocolExitIo {
public:
	explicit NativeCoreProtocolExitAdapterView(NativeCoreProtocolIoAdapterImpl &impl)
		: impl_(impl) {}

private:
	void CloseForProcessExit() override { impl_.CloseForProcessExit(); }

	NativeCoreProtocolIoAdapterImpl &impl_;
};

class NativeCoreProtocolIoAdapterCapabilitySet final :
	public NativeCoreProtocolCapabilities::State {
public:
	explicit NativeCoreProtocolIoAdapterCapabilitySet(NativeClock &clock)
		: impl_(clock), active_io_(impl_), cleanup_io_(impl_), recovery_io_(impl_),
		  exit_io_(impl_)
	{
	}
	~NativeCoreProtocolIoAdapterCapabilitySet() override
	{
		impl_.CloseForProcessExit();
	}
#if defined(MISTER_NATIVE_CORE_PROTOCOL_IO_TESTING)
	NativeCoreProtocolIoAdapterCapabilitySet(NativeClock &clock,
		NativeCoreProtocolIoTestOperations &operations)
		: impl_(clock, operations), active_io_(impl_), cleanup_io_(impl_),
		  recovery_io_(impl_), exit_io_(impl_)
	{
	}
#endif
private:
	bool Available() const override { return true; }
	NativeActiveCoreProtocolIo &ActiveIo() override { return active_io_; }
	NativeCleanupCoreProtocolIo &CleanupIo() override { return cleanup_io_; }
	NativeRecoveryCoreProtocolIo &RecoveryIo() override { return recovery_io_; }
	NativeCoreProtocolExitIo &ExitIo() override { return exit_io_; }

	NativeCoreProtocolIoAdapterImpl impl_;
	NativeCoreProtocolActiveAdapterView active_io_;
	NativeCoreProtocolCleanupAdapterView cleanup_io_;
	NativeCoreProtocolRecoveryAdapterView recovery_io_;
	NativeCoreProtocolExitAdapterView exit_io_;
};

NativeCoreProtocolIoAdapterCapabilitySet *AllocateCapabilitySet(NativeClock &clock)
{
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	if (!NativeCoreProtocolAllocationAllowedForTest()) return nullptr;
#endif
	return new (std::nothrow) NativeCoreProtocolIoAdapterCapabilitySet(clock);
}

#if defined(MISTER_NATIVE_CORE_PROTOCOL_IO_TESTING)
NativeCoreProtocolIoAdapterCapabilitySet *AllocateCapabilitySet(NativeClock &clock,
	NativeCoreProtocolIoTestOperations &operations)
{
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	if (!NativeCoreProtocolAllocationAllowedForTest()) return nullptr;
#endif
	return new (std::nothrow) NativeCoreProtocolIoAdapterCapabilitySet(clock,
		operations);
}
#endif

NativeCoreProtocolIoAdapter::NativeCoreProtocolIoAdapter(NativeClock &clock)
	: capabilities_(AllocateCapabilitySet(clock))
{
}

#if defined(MISTER_NATIVE_CORE_PROTOCOL_IO_TESTING)
NativeCoreProtocolIoAdapter::NativeCoreProtocolIoAdapter(NativeClock &clock,
	NativeCoreProtocolIoTestOperations &operations)
	: capabilities_(AllocateCapabilitySet(clock, operations))
{
}
#endif

NativeCoreProtocolIoAdapter::~NativeCoreProtocolIoAdapter()
{
}

bool NativeCoreProtocolIoAdapter::valid() const
	{ return capabilities_.Available(); }
NativeCoreProtocolCapabilities NativeCoreProtocolIoAdapter::capabilities()
	{ return std::move(capabilities_); }

} // namespace linux_native
} // namespace native
} // namespace mister
