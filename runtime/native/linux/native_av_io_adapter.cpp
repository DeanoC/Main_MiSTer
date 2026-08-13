// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_av_io_adapter.hpp"

#include "runtime/native/hardware_broker.hpp"

namespace mister {
namespace native {
namespace linux_native {
namespace {

#if defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
// These fixture-only command words are compatibility-authorized by the closed
// synthetic profile.  Production has no raw-command recipe in this adapter.
const uint16_t kFixtureUioSetFbuf = 0x002f;
const uint16_t kFixtureUioSetVideo = 0x0020;
#endif

bool BeforeDeadline(const NativeClock &clock, uint64_t deadline)
{
	return clock.NowMs() < deadline;
}

PeripheralCompletionReceipt EmptyReceipt(Result result, uint64_t mutation)
{
	// No transaction was admitted yet, but the operation-local fixture backend
	// holds no mapping or descriptor. This is sufficient residue proof for an
	// atomic pre-mutation active failure, never a neutrality observation.
	const PeripheralCompletionReceipt receipt = {result, true, true, true,
		true, false, mutation, 0};
	return receipt;
}

#if defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
PeripheralCompletionReceipt TransactionReceipt(Result result, bool closed,
	uint64_t mutation, uint16_t residue)
{
	return {result, closed, true, true, closed, !closed, mutation, residue};
}

Result TransactionResult(Result primary, bool closed, bool before_deadline)
{
	if (primary != MISTER_RESULT_OK) return primary;
	if (!closed) return MISTER_RESULT_PLATFORM;
	return before_deadline ? MISTER_RESULT_OK : MISTER_RESULT_DEADLINE;
}

#endif

} // namespace

NativeAvIoAdapter::NativeAvIoAdapter(NativeClock &clock)
	: clock_(clock)
#if defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
	, operations_(nullptr)
#endif
{
}

#if defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
NativeAvIoAdapter::NativeAvIoAdapter(NativeClock &clock,
	NativeAvIoTestOperations &operations)
	: clock_(clock), operations_(&operations)
{
}
#endif

NativeAvIoAdapter::~NativeAvIoAdapter() = default;

#if defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
Result NativeAvIoAdapter::SendFixtureActionWords(HardwareBroker &broker,
	const std::shared_ptr<PeripheralSessionState> &state,
	PeripheralSessionAction action, const uint16_t *words, uint8_t word_count,
	uint8_t next_word_index, uint16_t *residue, uint64_t deadline)
{
	for (uint8_t index = next_word_index; index < word_count; ++index) {
		const Result sent = operations_->SendWord(words[index], residue, deadline);
		if (sent != MISTER_RESULT_OK) return sent;
		const Result acknowledged = broker.RecordPeripheralActionWord(state,
			action, index);
		if (acknowledged != MISTER_RESULT_OK) return acknowledged;
	}
	return MISTER_RESULT_OK;
}

Result NativeAvIoAdapter::CloseFixtureAction(HardwareBroker &broker,
	const std::shared_ptr<PeripheralSessionState> &state,
	PeripheralSessionAction action, bool closed)
{
	return closed ? broker.ClosePeripheralAction(state, action) :
		MISTER_RESULT_OK;
}
#endif

Result NativeAvIoAdapter::SendAudio(HardwareBroker &broker,
	const std::shared_ptr<PeripheralSessionState> &state,
	const NativeAudioProfile &profile, uint8_t attenuation,
	PeripheralCompletionReceipt *receipt)
{
	if (receipt == nullptr || !state || !BeforeDeadline(clock_,
		state->absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	*receipt = EmptyReceipt(MISTER_RESULT_PLATFORM,
		state->last_mutation_sequence);
#if !defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
	(void)broker;
	(void)profile;
	(void)attenuation;
	return MISTER_RESULT_UNSUPPORTED;
#else
	if (operations_ == nullptr)
		return MISTER_RESULT_UNSUPPORTED;
	const uint64_t deadline = state->absolute_deadline_ms;
	const uint16_t words[] = {0x0026, attenuation};
	uint8_t next_word_index = 0;
	const Result prepared = broker.PreparePeripheralAction(state,
		PeripheralSessionAction::audio_attenuation, &profile, 2,
		&next_word_index);
	if (prepared != MISTER_RESULT_OK) return prepared;
	if (operations_->Begin(deadline) != MISTER_RESULT_OK)
		return MISTER_RESULT_PLATFORM;
	const Result admitted = broker.AdmitPeripheralAction(state,
		PeripheralSessionAction::audio_attenuation);
	if (admitted != MISTER_RESULT_OK) {
		const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
		*receipt = TransactionReceipt(admitted, closed,
			state->last_mutation_sequence, 0);
		return admitted;
	}
	uint64_t mutation = 0;
	const Result recorded = broker.RecordPeripheralMutation(state, &mutation);
	if (recorded != MISTER_RESULT_OK) {
		const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
		const Result closed_result = CloseFixtureAction(broker, state,
			PeripheralSessionAction::audio_attenuation, closed);
		const Result result = closed_result == MISTER_RESULT_OK ? recorded :
			closed_result;
		*receipt = TransactionReceipt(result, closed, mutation, 0);
		return result;
	}
	uint16_t residue = 0;
	const Result primary = SendFixtureActionWords(broker, state,
		PeripheralSessionAction::audio_attenuation, words, 2, next_word_index,
		&residue, deadline);
	const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
	const Result closed_result = CloseFixtureAction(broker, state,
		PeripheralSessionAction::audio_attenuation, closed);
	const Result result = closed_result == MISTER_RESULT_OK ?
		TransactionResult(primary, closed, BeforeDeadline(clock_, deadline)) :
		closed_result;
	*receipt = TransactionReceipt(result, closed, mutation, residue);
	return result;
#endif
}

Result NativeAvIoAdapter::SendVideoActivation(HardwareBroker &broker,
	const std::shared_ptr<PeripheralSessionState> &state,
	const NativeVideoProfile &profile, PeripheralCompletionReceipt *receipt)
{
	if (receipt == nullptr || !state || !BeforeDeadline(clock_,
		state->absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	*receipt = EmptyReceipt(MISTER_RESULT_PLATFORM,
		state->last_mutation_sequence);
#if !defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
	(void)broker;
	(void)profile;
	return MISTER_RESULT_UNSUPPORTED;
#else
	if (operations_ == nullptr || profile.recipe !=
		NativeVideoRecipeId::fixture_synthetic_v1 ||
		profile.affected_resource_flags != MISTER_RESOURCE_NATIVE_VIDEO)
		return MISTER_RESULT_UNSUPPORTED;
	const uint64_t deadline = state->absolute_deadline_ms;
	const uint16_t words[] = {kFixtureUioSetFbuf,
		profile.framebuffer_disable_word, kFixtureUioSetVideo,
		profile.set_video_words[0], profile.set_video_words[1]};
	uint8_t next_word_index = 0;
	const Result prepared = broker.PreparePeripheralAction(state,
		PeripheralSessionAction::video_activation, &profile, 5,
		&next_word_index);
	if (prepared != MISTER_RESULT_OK) return prepared;
	if (operations_->Begin(deadline) != MISTER_RESULT_OK)
		return MISTER_RESULT_PLATFORM;
	const Result admitted = broker.AdmitPeripheralAction(state,
		PeripheralSessionAction::video_activation);
	if (admitted != MISTER_RESULT_OK) {
		const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
		*receipt = TransactionReceipt(admitted, closed,
			state->last_mutation_sequence, 0);
		return admitted;
	}
	uint64_t mutation = 0;
	const Result recorded = broker.RecordPeripheralMutation(state, &mutation);
	if (recorded != MISTER_RESULT_OK) {
		const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
		const Result closed_result = CloseFixtureAction(broker, state,
			PeripheralSessionAction::video_activation, closed);
		const Result result = closed_result == MISTER_RESULT_OK ? recorded :
			closed_result;
		*receipt = TransactionReceipt(result, closed, mutation, 0);
		return result;
	}
	uint16_t residue = 0;
	const Result primary = SendFixtureActionWords(broker, state,
		PeripheralSessionAction::video_activation, words, 5, next_word_index,
		&residue, deadline);
	const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
	const Result closed_result = CloseFixtureAction(broker, state,
		PeripheralSessionAction::video_activation, closed);
	const Result result = closed_result == MISTER_RESULT_OK ?
		TransactionResult(primary, closed, BeforeDeadline(clock_, deadline)) :
		closed_result;
	*receipt = TransactionReceipt(result, closed, mutation, residue);
	return result;
#endif
}

Result NativeAvIoAdapter::SendVideoTeardown(HardwareBroker &broker,
	const std::shared_ptr<PeripheralSessionState> &state,
	const NativeVideoProfile &profile, PeripheralCompletionReceipt *receipt)
{
	if (receipt == nullptr || !state || !BeforeDeadline(clock_,
		state->absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	*receipt = EmptyReceipt(MISTER_RESULT_PLATFORM,
		state->last_mutation_sequence);
#if !defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
	(void)broker;
	(void)profile;
	return MISTER_RESULT_UNSUPPORTED;
#else
	if (operations_ == nullptr || profile.recipe !=
		NativeVideoRecipeId::fixture_synthetic_v1 ||
		profile.affected_resource_flags != MISTER_RESOURCE_NATIVE_VIDEO)
		return MISTER_RESULT_UNSUPPORTED;
	const uint64_t deadline = state->absolute_deadline_ms;
	const uint16_t words[] = {kFixtureUioSetFbuf,
		profile.framebuffer_disable_word};
	uint8_t next_word_index = 0;
	const Result prepared = broker.PreparePeripheralAction(state,
		PeripheralSessionAction::video_teardown, &profile, 2,
		&next_word_index);
	if (prepared != MISTER_RESULT_OK) return prepared;
	if (operations_->Begin(deadline) != MISTER_RESULT_OK)
		return MISTER_RESULT_PLATFORM;
	const Result admitted = broker.AdmitPeripheralAction(state,
		PeripheralSessionAction::video_teardown);
	if (admitted != MISTER_RESULT_OK) {
		const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
		*receipt = TransactionReceipt(admitted, closed,
			state->last_mutation_sequence, 0);
		return admitted;
	}
	uint64_t mutation = 0;
	const Result recorded = broker.RecordPeripheralMutation(state, &mutation);
	if (recorded != MISTER_RESULT_OK) {
		const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
		const Result closed_result = CloseFixtureAction(broker, state,
			PeripheralSessionAction::video_teardown, closed);
		const Result result = closed_result == MISTER_RESULT_OK ? recorded :
			closed_result;
		*receipt = TransactionReceipt(result, closed, mutation, 0);
		return result;
	}
	uint16_t residue = 0;
	const Result primary = SendFixtureActionWords(broker, state,
		PeripheralSessionAction::video_teardown, words, 2, next_word_index,
		&residue, deadline);
	const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
	const Result closed_result = CloseFixtureAction(broker, state,
		PeripheralSessionAction::video_teardown, closed);
	const Result result = closed_result == MISTER_RESULT_OK ?
		TransactionResult(primary, closed, BeforeDeadline(clock_, deadline)) :
		closed_result;
	*receipt = TransactionReceipt(result, closed, mutation, residue);
	return result;
#endif
}

#define MISTER_DEFINE_AV_IO_METHOD(name, session_type, impl, profile_type) \
Result NativeAvIoAdapter::name(HardwareBroker &broker, session_type &session, \
	const profile_type &profile, PeripheralCompletionReceipt *receipt) \
{ \
	return impl(broker, session.state_, profile, receipt); \
}

Result NativeAvIoAdapter::StartAudio(HardwareBroker &broker,
	ActiveAudioSession &session, const NativeAudioProfile &profile,
	PeripheralCompletionReceipt *receipt)
{
	return SendAudio(broker, session.state_, profile, profile.active_attenuation,
		receipt);
}

Result NativeAvIoAdapter::StopAudio(HardwareBroker &broker,
	CleanupAudioSession &session, const NativeAudioProfile &profile,
	PeripheralCompletionReceipt *receipt)
{
	return SendAudio(broker, session.state_, profile, profile.mute_attenuation,
		receipt);
}

Result NativeAvIoAdapter::RecoverAudio(HardwareBroker &broker,
	RecoveryAudioSession &session, const NativeAudioProfile &profile,
	PeripheralCompletionReceipt *receipt)
{
	return SendAudio(broker, session.state_, profile, profile.mute_attenuation,
		receipt);
}

MISTER_DEFINE_AV_IO_METHOD(StartVideo, ActiveVideoSession, SendVideoActivation,
	NativeVideoProfile)
MISTER_DEFINE_AV_IO_METHOD(StopVideo, CleanupVideoSession, SendVideoTeardown,
	NativeVideoProfile)
MISTER_DEFINE_AV_IO_METHOD(RecoverVideo, RecoveryVideoSession, SendVideoTeardown,
	NativeVideoProfile)

#undef MISTER_DEFINE_AV_IO_METHOD

Result NativeAvIoAdapter::SendCoupled(HardwareBroker &broker,
	const std::shared_ptr<PeripheralSessionState> &state,
	const NativeVideoProfile &profile, bool power_down,
	CoupledCompletionReceipt *receipt)
{
	if (receipt == nullptr || !state || !BeforeDeadline(clock_,
		state->absolute_deadline_ms)) return MISTER_RESULT_DEADLINE;
	const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	*receipt = {MISTER_RESULT_PLATFORM, affected, 0, 0, true, false,
		state->last_mutation_sequence, true, 0};
#if !defined(MISTER_NATIVE_AUDIO_VIDEO_TESTING)
	(void)broker;
	(void)profile;
	(void)power_down;
	return MISTER_RESULT_UNSUPPORTED;
#else
	if (operations_ == nullptr || !profile.coupled_transmitter ||
		profile.adv7513_main_address != 0x39)
		return MISTER_RESULT_UNSUPPORTED;
	const uint64_t deadline = state->absolute_deadline_ms;
	const uint16_t command = 0x0041;
	const uint16_t value = power_down ? profile.power_down_value :
		profile.power_on_value;
	const uint16_t words[] = {command, value};
	uint8_t next_word_index = 0;
	const Result prepared = broker.PreparePeripheralAction(state,
		PeripheralSessionAction::coupled_transmitter, &profile, 2,
		&next_word_index);
	if (prepared != MISTER_RESULT_OK) return prepared;
	if (operations_->Begin(deadline) != MISTER_RESULT_OK)
		return MISTER_RESULT_PLATFORM;
	const Result admitted = broker.AdmitPeripheralAction(state,
		PeripheralSessionAction::coupled_transmitter);
	if (admitted != MISTER_RESULT_OK) {
		const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
		*receipt = {admitted, affected, 0, 0, closed, !closed,
			state->last_mutation_sequence, closed, 0};
		return admitted;
	}
	uint64_t mutation = 0;
	const Result recorded = broker.RecordPeripheralMutation(state, &mutation);
	if (recorded != MISTER_RESULT_OK) {
		const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
		const Result closed_result = CloseFixtureAction(broker, state,
			PeripheralSessionAction::coupled_transmitter, closed);
		const Result result = closed_result == MISTER_RESULT_OK ? recorded :
			closed_result;
		*receipt = {result, affected, 0, 0, closed, !closed, mutation, closed, 0};
		return result;
	}
	uint16_t residue = 0;
	const Result primary = SendFixtureActionWords(broker, state,
		PeripheralSessionAction::coupled_transmitter, words, 2, next_word_index,
		&residue, deadline);
	const bool closed = operations_->Finish(deadline) == MISTER_RESULT_OK;
	const Result closed_result = CloseFixtureAction(broker, state,
		PeripheralSessionAction::coupled_transmitter, closed);
	const Result result = closed_result == MISTER_RESULT_OK ?
		TransactionResult(primary, closed, BeforeDeadline(clock_, deadline)) :
		closed_result;
	*receipt = {result, affected, 0,
		power_down ? static_cast<uint32_t>(MISTER_RESOURCE_NATIVE_VIDEO) : 0u,
		closed, !closed, mutation, closed, residue};
	return result;
#endif
}

Result NativeAvIoAdapter::StartAudioVideo(HardwareBroker &broker,
	ActiveAudioVideoSession &session, const NativeVideoProfile &profile,
	CoupledAcquisitionReceipt *receipt)
{
	if (receipt == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	CoupledCompletionReceipt completion = {};
	const Result result = SendCoupled(broker, session.state_, profile, false,
		&completion);
	*receipt = {result, completion.affected_flags, completion.local_resources_absent,
		completion.transaction_closed, completion.closure_unknown,
		completion.mutation_sequence, completion.transaction_residue};
	return result;
}

Result NativeAvIoAdapter::StopAudioVideo(HardwareBroker &broker,
	CleanupAudioVideoSession &session, const NativeVideoProfile &profile,
	CoupledCompletionReceipt *receipt)
{
	return SendCoupled(broker, session.state_, profile, true, receipt);
}

Result NativeAvIoAdapter::RecoverAudioVideo(HardwareBroker &broker,
	RecoveryAudioVideoSession &session, const NativeVideoProfile &profile,
	CoupledRecoveryReceipt *receipt)
{
	return SendCoupled(broker, session.state_, profile, true, receipt);
}

} // namespace linux_native
} // namespace native
} // namespace mister
