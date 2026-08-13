// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_core_protocol.hpp"
#include "runtime/native/native_core_protocol_session_state.hpp"
#include "runtime/native/native_recovery.hpp"
#include "runtime/native/native_resources.hpp"

#include <string.h>

#include <atomic>
#include <new>
#include <utility>
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

#if defined(MISTER_NATIVE_PROFILE_TESTING)
std::atomic<size_t> g_allocation_failure_index(0);
std::atomic<size_t> g_allocation_attempt_index(0);
#endif

CoreProtocolResidue EmptyResidue()
{
	const CoreProtocolResidue residue = {false, false, false, false, false,
		false, false, 0};
	return residue;
}

class RetainedContentBridge final : public NativeCoreProtocolContent {
public:
	explicit RetainedContentBridge(NativeContentResource &content)
		: content_(content), description_()
	{
		description_[0] = '\0';
	}
	MisterResult Describe(NativeCoreProtocolContentDescription *description,
		uint64_t absolute_deadline_ms) override
	{
		if (description == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		NativeRetainedContentDescription retained = {};
		const Result result = content_.DescribeRetained(&retained,
			absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		if (retained.extension_length == 0 || retained.extension_length > 3 ||
			retained.extension[retained.extension_length] != '\0')
			return MISTER_RESULT_PLATFORM;
		for (size_t index = 0; index <= retained.extension_length; ++index)
			description_[index] = retained.extension[index];
		description->size = retained.size;
		description->extension = description_;
		return MISTER_RESULT_OK;
	}
	MisterResult ReadAt(uint64_t offset, void *bytes, size_t count,
		uint64_t absolute_deadline_ms) override
	{
		return content_.ReadRetainedAt(offset, bytes, count,
			absolute_deadline_ms);
	}

private:
	NativeContentResource &content_;
	char description_[4];
};

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

#if defined(MISTER_NATIVE_PROFILE_TESTING)
void SetNativeCoreProtocolAllocationFailureForTest(size_t allocation_index)
{
	g_allocation_attempt_index.store(0, std::memory_order_relaxed);
	g_allocation_failure_index.store(allocation_index, std::memory_order_relaxed);
}

void ClearNativeCoreProtocolAllocationFailureForTest()
{
	g_allocation_failure_index.store(0, std::memory_order_relaxed);
	g_allocation_attempt_index.store(0, std::memory_order_relaxed);
}

bool NativeCoreProtocolAllocationAllowedForTest()
{
	const size_t attempt = g_allocation_attempt_index.fetch_add(1,
		std::memory_order_relaxed) + 1;
	const size_t failure = g_allocation_failure_index.load(
		std::memory_order_relaxed);
	return failure == 0 || attempt != failure;
}
#endif

// Teardown owns only the narrowly-authorized cleanup/recovery/exit capability
// surfaces. It deliberately cannot name active probing, word exchange, status,
// or selected-transaction actions.
class NativeCoreProtocolTeardown final {
public:
	NativeCoreProtocolTeardown(HardwareBroker &broker,
		NativeCleanupCoreProtocolIo &cleanup_io,
		NativeRecoveryCoreProtocolIo &recovery_io,
		NativeCoreProtocolExitIo &exit_io)
		: broker_(broker), cleanup_io_(cleanup_io), recovery_io_(recovery_io),
		  exit_io_(exit_io)
	{
	}

	Result Shutdown(const OperationLease &cleanup_lease,
		bool download_may_be_active, const NativeCoreProfile *exact_profile)
	{
		std::unique_ptr<CleanupCoreProtocolSession> session;
		Result result = cleanup_lease.AcquireCleanupCoreProtocolSession(broker_,
			&session);
		if (result != MISTER_RESULT_OK) return result;
		result = cleanup_io_.BeginCleanup(*session,
			cleanup_lease.absolute_deadline_ms());
		if (result != MISTER_RESULT_OK) return result;
		ProtocolMappingReleaseReceipt receipt = {};
		// The saved profile is created only by the exact active recipe. Cleanup
		// can therefore issue the fixed stop frame but receives no active probe
		// or word-exchange capability.
		result = cleanup_io_.ShutdownCleanup(*session,
			download_may_be_active && exact_profile != nullptr, &receipt);
		if (result != MISTER_RESULT_OK) return result;
		return cleanup_lease.CompleteCleanupCoreProtocolSession(broker_,
			std::move(session), receipt);
	}

	Result Disable(const OperationLease &recovery_lease,
		RecoveryResourceState *state)
	{
		if (state == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		*state = RecoveryResourceState::unknown;
		std::unique_ptr<RecoveryCoreProtocolSession> session;
		Result result = recovery_lease.AcquireRecoveryCoreProtocolSession(broker_,
			&session);
		if (result != MISTER_RESULT_OK) return result;
		result = recovery_io_.BeginRecovery(*session,
			recovery_lease.absolute_deadline_ms());
		if (result != MISTER_RESULT_OK) return result;
		ProtocolMappingReleaseReceipt receipt = {};
		result = recovery_io_.DisableRecovery(*session, &receipt);
		if (result != MISTER_RESULT_OK) return result;
		result = recovery_lease.CompleteRecoveryCoreProtocolSession(broker_,
			std::move(session), receipt);
		if (result == MISTER_RESULT_OK) *state = RecoveryResourceState::neutral;
		return result;
	}

	void CloseForProcessExit() { exit_io_.CloseForProcessExit(); }

private:
	HardwareBroker &broker_;
	NativeCleanupCoreProtocolIo &cleanup_io_;
	NativeRecoveryCoreProtocolIo &recovery_io_;
	NativeCoreProtocolExitIo &exit_io_;
};

#if defined(MISTER_NATIVE_PROFILE_TESTING)
NativeCoreProtocol::NativeCoreProtocol(NativeClock &clock,
	NativeActiveCoreProtocolIo &active_io)
	: clock_(clock), broker_(nullptr), active_io_(&active_io), capabilities_(),
	  teardown_(),
	  capabilities_available_(active_io.Available()),
	  last_status_sequence_(0),
	  active_profile_(nullptr), residue_(EmptyResidue())
{
}
#endif

NativeCoreProtocol::NativeCoreProtocol(NativeClock &clock,
	HardwareBroker &broker, NativeCoreProtocolCapabilities &&capabilities)
	: clock_(clock), broker_(&broker), active_io_(nullptr),
	  capabilities_(std::move(capabilities)),
	  teardown_(),
	  capabilities_available_(capabilities_.Available()),
	  last_status_sequence_(0),
	  active_profile_(nullptr), residue_(EmptyResidue())
{
	if (!capabilities_available_) return;
	active_io_ = &capabilities_.active_io();
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	if (!NativeCoreProtocolAllocationAllowedForTest()) {
		capabilities_available_ = false;
		return;
	}
#endif
	teardown_.reset(new (std::nothrow) NativeCoreProtocolTeardown(broker,
		capabilities_.cleanup_io(), capabilities_.recovery_io(),
		capabilities_.exit_io()));
	if (!teardown_) capabilities_available_ = false;
}

NativeCoreProtocol::~NativeCoreProtocol() = default;

MisterResult NativeActiveCoreProtocolIo::BeginActive(ActiveCoreProtocolSession &,
	uint64_t)
{
	return MISTER_RESULT_UNSUPPORTED;
}

bool NativeActiveCoreProtocolIo::Available() const { return true; }
bool NativeCleanupCoreProtocolIo::Available() const { return true; }
bool NativeRecoveryCoreProtocolIo::Available() const { return true; }
bool NativeCoreProtocolExitIo::Available() const { return true; }

MisterResult NativeCleanupCoreProtocolIo::BeginCleanup(CleanupCoreProtocolSession &,
	uint64_t)
{
	return MISTER_RESULT_UNSUPPORTED;
}

MisterResult NativeRecoveryCoreProtocolIo::BeginRecovery(RecoveryCoreProtocolSession &,
	uint64_t)
{
	return MISTER_RESULT_UNSUPPORTED;
}

MisterResult NativeActiveCoreProtocolIo::FinishActive(ActiveCoreProtocolSession &,
	ProtocolMappingReleaseReceipt *)
{
	return MISTER_RESULT_UNSUPPORTED;
}

MisterResult NativeActiveCoreProtocolIo::AbortActive(ActiveCoreProtocolSession &,
	MisterResult, CoreProtocolResidue *, ProtocolMappingReleaseReceipt *)
{
	return MISTER_RESULT_UNSUPPORTED;
}

MisterResult NativeCleanupCoreProtocolIo::ShutdownCleanup(CleanupCoreProtocolSession &,
	bool, ProtocolMappingReleaseReceipt *)
{
	return MISTER_RESULT_UNSUPPORTED;
}

MisterResult NativeRecoveryCoreProtocolIo::DisableRecovery(RecoveryCoreProtocolSession &,
	ProtocolMappingReleaseReceipt *)
{
	return MISTER_RESULT_UNSUPPORTED;
}

void NativeCoreProtocolExitIo::CloseForProcessExit()
{
}

NativeCoreProtocolCapabilities::NativeCoreProtocolCapabilities() : state_(nullptr)
{
}

NativeCoreProtocolCapabilities::NativeCoreProtocolCapabilities(
	State *state)
	: state_(state)
{
	if (state_ != nullptr) state_->Retain();
}

NativeCoreProtocolCapabilities::NativeCoreProtocolCapabilities(
	NativeCoreProtocolCapabilities &&other) noexcept
	: state_(other.state_)
{
	other.state_ = nullptr;
}

NativeCoreProtocolCapabilities &NativeCoreProtocolCapabilities::operator=(
	NativeCoreProtocolCapabilities &&other) noexcept
{
	if (this == &other) return *this;
	if (state_ != nullptr) state_->Release();
	state_ = other.state_;
	other.state_ = nullptr;
	return *this;
}

NativeCoreProtocolCapabilities::~NativeCoreProtocolCapabilities()
{
	if (state_ != nullptr) state_->Release();
}

bool NativeCoreProtocolCapabilities::Available() const
{
	return state_ && state_->Available();
}

NativeActiveCoreProtocolIo &NativeCoreProtocolCapabilities::active_io() const
{
	return state_->ActiveIo();
}

NativeCleanupCoreProtocolIo &NativeCoreProtocolCapabilities::cleanup_io() const
{
	return state_->CleanupIo();
}

NativeRecoveryCoreProtocolIo &NativeCoreProtocolCapabilities::recovery_io() const
{
	return state_->RecoveryIo();
}

NativeCoreProtocolExitIo &NativeCoreProtocolCapabilities::exit_io() const
{
	return state_->ExitIo();
}

MisterResult NativeCoreProtocol::Exchange(NativeSpiTarget target,
	const uint16_t *words, size_t count, uint16_t *responses,
	size_t response_capacity, bool begins_selected, bool ends_selected,
	uint64_t absolute_deadline_ms)
{
	if (!capabilities_available_) return MISTER_RESULT_PLATFORM;
	if (words == nullptr || count == 0 ||
		(responses == nullptr && response_capacity != 0) ||
		(responses != nullptr && response_capacity < count))
		return MISTER_RESULT_INVALID_ARGUMENT;
	if (!BeforeDeadline(clock_, absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	const MisterResult result = active_io_->Exchange(target, words, count, responses,
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
	if (!capabilities_available_) return MISTER_RESULT_PLATFORM;
	if (!BeforeDeadline(clock_, absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	const MisterResult result = active_io_->CloseSelected(target, absolute_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;
	return BeforeDeadline(clock_, absolute_deadline_ms) ? MISTER_RESULT_OK :
		MISTER_RESULT_DEADLINE;
}

MisterResult NativeCoreProtocol::ValidateLive(const NativeCoreProfile &profile,
	uint64_t absolute_deadline_ms)
{
	if (!capabilities_available_) return MISTER_RESULT_PLATFORM;
	NativeLiveCoreObservation live = {};
	if (!BeforeDeadline(clock_, absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	MisterResult result = active_io_->Probe(&live, absolute_deadline_ms);
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
	if (!capabilities_available_) return MISTER_RESULT_PLATFORM;
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

NativeCoreProtocolOutcome NativeCoreProtocol::Activate(
	const OperationLease &active_lease, const NativeCoreProfile &profile,
	NativeContentResource &content)
{
	NativeCoreProtocolOutcome outcome = {MISTER_RESULT_INVALID_STATE, false,
		false};
	if (!capabilities_available_) {
		outcome.result = MISTER_RESULT_PLATFORM;
		return outcome;
	}
	if (broker_ == nullptr || !ValidateNativeCoreProfileRecord(profile)) {
		outcome.result = MISTER_RESULT_UNSUPPORTED;
		return outcome;
	}
	// D6.4 owns the full SNES transform. Do not acquire a session, describe
	// content, or map hardware until that exact transform is available.
	if (profile.protocol.transform ==
		NativeContentTransform::snes_header_and_mirror) {
		outcome.result = MISTER_RESULT_UNSUPPORTED;
		return outcome;
	}
	RetainedContentBridge retained(content);
	NativeCoreProtocolContentDescription description = {};
	outcome.result = retained.Describe(&description,
		active_lease.absolute_deadline_ms());
	if (outcome.result != MISTER_RESULT_OK) return outcome;
	if (description.extension == nullptr || description.size == 0 ||
		description.size < profile.protocol.minimum_source_bytes ||
		description.size > profile.protocol.maximum_source_bytes ||
		!NativeCoreProfileAcceptsExtension(profile, description.extension)) {
		outcome.result = MISTER_RESULT_UNSUPPORTED;
		return outcome;
	}

	std::unique_ptr<ActiveCoreProtocolSession> session;
	outcome.result = active_lease.AcquireActiveCoreProtocolSession(*broker_,
		profile, &session);
	if (outcome.result != MISTER_RESULT_OK) return outcome;
	outcome.acquired = true;
	outcome.result = active_io_->BeginActive(*session,
		active_lease.absolute_deadline_ms());
	if (outcome.result == MISTER_RESULT_OK)
		outcome.result = ActivateFixtureForTest(profile, retained,
			active_lease.absolute_deadline_ms());
	if (outcome.result == MISTER_RESULT_OK) {
		ProtocolMappingReleaseReceipt release = {};
		outcome.result = active_io_->FinishActive(*session, &release);
		if (outcome.result == MISTER_RESULT_OK)
			outcome.result = active_lease.CompleteSuccessfulCoreProtocolSession(
				*broker_, std::move(session), release);
		if (outcome.result == MISTER_RESULT_OK) {
			active_profile_ = &profile;
			residue_ = EmptyResidue();
			return outcome;
		}
	}

	const MisterResult primary = outcome.result;
	CoreProtocolResidue residue = EmptyResidue();
	ProtocolMappingReleaseReceipt release = {};
	const MisterResult aborted = active_io_->AbortActive(*session, primary, &residue,
		&release);
	if (aborted != MISTER_RESULT_OK && release.result == MISTER_RESULT_OK)
		release.result = aborted;
	if (release.mutation_sequence == 0 && session->state_ &&
		session->state_->view)
		release.mutation_sequence = session->state_->view->RecordMutation();
	if (release.mutation_sequence == 0) outcome.result = MISTER_RESULT_PLATFORM;
	residue.last_mutation_sequence = release.mutation_sequence;
	const ActiveProtocolFailureReceipt receipt = {primary, residue, release,
		release.mutation_sequence};
	const Result completed = active_lease.CompleteFailedCoreProtocolSession(
		*broker_, std::move(session), receipt);
	if (completed == MISTER_RESULT_OK) {
		residue_ = residue;
		active_profile_ = &profile;
		outcome.broker_failure_completed = true;
	} else {
		outcome.result = completed;
	}
	return outcome;
}

bool NativeCoreProtocol::valid() const
{
	return capabilities_available_;
}

Result NativeCoreProtocol::ShutdownLive(const OperationLease &cleanup_lease)
{
	if (!capabilities_available_ || broker_ == nullptr || !teardown_)
		return MISTER_RESULT_PLATFORM;
	const Result result = teardown_->Shutdown(cleanup_lease,
		residue_.download_may_be_active, active_profile_);
	if (result == MISTER_RESULT_OK) {
		residue_ = EmptyResidue();
		active_profile_ = nullptr;
		last_status_sequence_ = 0;
	}
	return result;
}

Result NativeCoreProtocol::DisableStateless(const OperationLease &recovery_lease,
	RecoveryResourceState *state)
{
	if (state == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	if (!capabilities_available_ || broker_ == nullptr || !teardown_)
		return MISTER_RESULT_PLATFORM;
	return teardown_->Disable(recovery_lease, state);
}

void NativeCoreProtocol::CloseForProcessExit()
{
	if (teardown_) teardown_->CloseForProcessExit();
}

} // namespace native
} // namespace mister
