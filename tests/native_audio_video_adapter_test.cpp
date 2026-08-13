// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_audio_adapter.hpp"
#include "runtime/native/linux/native_video_adapter.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_peripheral_session.hpp"
#include "runtime/native/native_resources.hpp"
#include "tests/native_peripheral_authority_test_peer.hpp"

#include <assert.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace mister {
namespace native {
namespace {

class FakeClock final : public NativeClock {
public:
	explicit FakeClock(uint64_t now_ms) : now_ms_(now_ms) {}
	uint64_t NowMs() const override { return now_ms_; }
	void SetNowMs(uint64_t now_ms) { now_ms_ = now_ms; }
	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t deadline) override { return now_ms_ < deadline; }

private:
	uint64_t now_ms_;
};

class FakeAvOperations final : public linux_native::NativeAvIoTestOperations {
public:
	Result Begin(uint64_t deadline) override
	{
		deadlines.push_back(deadline);
		++begin_calls;
		for (size_t index = 0; index < fail_begin_calls.size(); ++index)
			if (fail_begin_calls[index] == begin_calls)
				return MISTER_RESULT_PLATFORM;
		return fail_begin_call == begin_calls ? MISTER_RESULT_PLATFORM :
			MISTER_RESULT_OK;
	}
	Result SendWord(uint16_t word, uint16_t *ack_low, uint64_t deadline) override
	{
		deadlines.push_back(deadline);
		words.push_back(word);
		if (ack_low != nullptr) *ack_low = 0xa55a;
		++send_calls;
		return fail_send_call == send_calls ? MISTER_RESULT_PLATFORM :
			MISTER_RESULT_OK;
	}
	Result Finish(uint64_t deadline) override
	{
		deadlines.push_back(deadline);
		++finish_calls;
		return fail_finish_call == finish_calls ? MISTER_RESULT_PLATFORM :
			MISTER_RESULT_OK;
	}

	int begin_calls = 0;
	int finish_calls = 0;
	int send_calls = 0;
	int fail_begin_call = 0;
	int fail_send_call = 0;
	int fail_finish_call = 0;
	std::vector<int> fail_begin_calls;
	std::vector<uint16_t> words;
	std::vector<uint64_t> deadlines;
};

void AssertAllDeadlines(const FakeAvOperations &operations, uint64_t deadline)
{
	for (size_t index = 0; index < operations.deadlines.size(); ++index)
		assert(operations.deadlines[index] == deadline);
}

void TestFixtureProfilesHaveOnlyClosedAudioVideoAuthority()
{
	const NativeCoreProfile *snes = FixtureNativeCoreProfile("snes");
	const NativeCoreProfile *megadrive = FixtureNativeCoreProfile("megadrive");
	assert(snes != nullptr);
	assert(megadrive != nullptr);
	assert(snes->audio.recipe == NativeAudioRecipeId::fixture_synthetic_v1);
	assert(megadrive->audio.recipe == NativeAudioRecipeId::fixture_synthetic_v1);
	assert(snes->video.recipe == NativeVideoRecipeId::fixture_synthetic_v1);
	assert(megadrive->video.recipe == NativeVideoRecipeId::fixture_synthetic_v1);
	assert(snes->video.affected_resource_flags ==
		MISTER_RESOURCE_NATIVE_VIDEO);
	assert(megadrive->video.affected_resource_flags ==
		MISTER_RESOURCE_NATIVE_VIDEO);
	assert(ProductionNativeCoreProfile("snes") == nullptr);
}

void TestTypedAudioVideoSessionsCannotBeForgedOrConverted()
{
	static_assert(!std::is_default_constructible<ActiveAudioSession>::value,
		"audio sessions are broker minted");
	static_assert(!std::is_default_constructible<ActiveVideoSession>::value,
		"video sessions are broker minted");
	static_assert(!std::is_default_constructible<ActiveAudioVideoSession>::value,
		"coupled sessions are broker minted");
	static_assert(!std::is_convertible<ActiveAudioSession *,
		ActiveVideoSession *>::value,
		"audio authority cannot become video authority");
	static_assert(!std::is_convertible<ActiveVideoSession *,
		ActiveAudioVideoSession *>::value,
		"video authority cannot become coupled authority");
}

void TestAudioVideoResourcesAreSeparateTypedBoundaries()
{
	static_assert(std::is_abstract<NativeAudioResource>::value,
		"audio has a dedicated typed resource boundary");
	static_assert(std::is_abstract<NativeVideoResource>::value,
		"video has a dedicated typed resource boundary");
	static_assert(std::is_abstract<NativeAudioVideoResource>::value,
		"coupled transmitter authority is distinct from video-only authority");
}

void TestDestroyedCleanupAudioSessionBecomesSameRegistrationRecheckoutable()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeAudioAdapter audio(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio, &lease) ==
		MISTER_RESULT_OK);
	std::unique_ptr<CleanupAudioSessionBundle> first;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &first) == MISTER_RESULT_OK);
	first.reset();
	PeripheralBrokerDisposition disposition = PeripheralBrokerDisposition::live;
	assert(PeripheralAuthorityTestPeer::AudioDisposition(*lease, broker,
		&disposition) ==
		MISTER_RESULT_OK);
	assert(disposition == PeripheralBrokerDisposition::abandoned);
	std::unique_ptr<CleanupAudioSessionBundle> retried;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &retried) == MISTER_RESULT_OK);
	retried.reset();
}

void TestEveryTypedWrapperAllocationFailureLeavesNoLiveRegistration()
{
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	const SafeAudioRecoveryRecord *audio_record =
		FixtureSafeAudioRecoveryRecordForTest(NativeSystem::snes);
	const SafeVideoRecoveryRecord *video_record =
		FixtureSafeVideoRecoveryRecordForTest(NativeSystem::snes);
	const SafeAudioVideoRecoveryRecord *coupled_record =
		FixtureSafeAudioVideoRecoveryRecordForTest(NativeSystem::snes);
	assert(profile != nullptr && audio_record != nullptr && video_record != nullptr &&
		coupled_record != nullptr);

#define ASSERT_ACTIVE_ALLOCATION_ROLLBACK(kind, session_type, acquire, disposition) \
	{ \
		for (size_t allocation_index = 1; allocation_index <= 2; ++allocation_index) { \
		FakeClock clock(100); HardwareBroker broker(clock); FakeAvOperations operations; \
		linux_native::NativeAvIoAdapter io(clock, operations); \
		linux_native::NativeAudioAdapter audio(clock, broker, io); \
		linux_native::NativeVideoAdapter video(clock, broker, io); \
		PlatformGenerationId generation = 0; \
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK); \
		std::unique_ptr<OperationLease> lease; \
		assert(broker.Begin(generation, kind, 1000, &lease) == MISTER_RESULT_OK); \
		std::unique_ptr<session_type> session; \
		SetPeripheralSessionAllocationFailureForTest(allocation_index); \
		assert((acquire) == MISTER_RESULT_PLATFORM); \
		ClearPeripheralSessionAllocationFailureForTest(); \
		PeripheralBrokerDisposition current = PeripheralBrokerDisposition::live; \
		assert((disposition) == MISTER_RESULT_OK); \
		assert(current == PeripheralBrokerDisposition::no_session); \
		} \
	}

	ASSERT_ACTIVE_ALLOCATION_ROLLBACK(OperationKind::audio, ActiveAudioSessionBundle,
		PeripheralAuthorityTestPeer::AcquireActiveAudio(*lease, broker, *profile,
			audio.BackendIdentity(), &session),
		PeripheralAuthorityTestPeer::AudioDisposition(*lease, broker, &current));
	ASSERT_ACTIVE_ALLOCATION_ROLLBACK(OperationKind::video, ActiveVideoSessionBundle,
		PeripheralAuthorityTestPeer::AcquireActiveVideo(*lease, broker, *profile,
			video.BackendIdentity(), &session),
		PeripheralAuthorityTestPeer::VideoDisposition(*lease, broker, &current));
	ASSERT_ACTIVE_ALLOCATION_ROLLBACK(OperationKind::audio_video,
		ActiveAudioVideoSessionBundle,
		PeripheralAuthorityTestPeer::AcquireActiveAudioVideo(*lease, broker,
			*profile, video.BackendIdentity(), &session),
		PeripheralAuthorityTestPeer::AudioVideoDisposition(*lease, broker,
			&current));

#undef ASSERT_ACTIVE_ALLOCATION_ROLLBACK

#define ASSERT_RETRYABLE_ALLOCATION_ABANDONED(kind, session_type, acquire, disposition) \
	{ \
		for (size_t allocation_index = 1; allocation_index <= 2; ++allocation_index) { \
		FakeClock clock(100); HardwareBroker broker(clock); FakeAvOperations operations; \
		linux_native::NativeAvIoAdapter io(clock, operations); \
		linux_native::NativeAudioAdapter audio(clock, broker, io); \
		linux_native::NativeVideoAdapter video(clock, broker, io); \
		PlatformGenerationId generation = 0; \
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK); \
		assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK); \
		std::unique_ptr<CleanupEpoch> cleanup; \
		assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) == MISTER_RESULT_OK); \
		std::unique_ptr<OperationLease> lease; \
		assert(broker.BeginCleanupOperation(*cleanup, kind, &lease) == MISTER_RESULT_OK); \
		std::unique_ptr<session_type> session; \
		SetPeripheralSessionAllocationFailureForTest(allocation_index); \
		assert((acquire) == MISTER_RESULT_PLATFORM); \
		ClearPeripheralSessionAllocationFailureForTest(); \
		PeripheralBrokerDisposition current = PeripheralBrokerDisposition::live; \
		assert((disposition) == MISTER_RESULT_OK); \
		assert(current == PeripheralBrokerDisposition::abandoned); \
		assert((acquire) == MISTER_RESULT_OK); \
		session.reset(); \
		} \
	}

	ASSERT_RETRYABLE_ALLOCATION_ABANDONED(OperationKind::audio,
		CleanupAudioSessionBundle, PeripheralAuthorityTestPeer::AcquireCleanupAudio(
			*lease, broker, audio.BackendIdentity(), &session),
		PeripheralAuthorityTestPeer::AudioDisposition(*lease, broker, &current));
	ASSERT_RETRYABLE_ALLOCATION_ABANDONED(OperationKind::video,
		CleanupVideoSessionBundle, PeripheralAuthorityTestPeer::AcquireCleanupVideo(
			*lease, broker, video.BackendIdentity(), &session),
		PeripheralAuthorityTestPeer::VideoDisposition(*lease, broker, &current));
	ASSERT_RETRYABLE_ALLOCATION_ABANDONED(OperationKind::audio_video,
		CleanupAudioVideoSessionBundle,
		PeripheralAuthorityTestPeer::AcquireCleanupAudioVideo(*lease, broker,
			video.BackendIdentity(), &session),
		PeripheralAuthorityTestPeer::AudioVideoDisposition(*lease, broker,
			&current));

#undef ASSERT_RETRYABLE_ALLOCATION_ABANDONED

#define ASSERT_RECOVERY_ALLOCATION_ABANDONED(kind, session_type, record, acquire, disposition) \
	{ \
		for (size_t allocation_index = 1; allocation_index <= 2; ++allocation_index) { \
		FakeClock clock(100); HardwareBroker broker(clock); FakeAvOperations operations; \
		linux_native::NativeAvIoAdapter io(clock, operations); \
		linux_native::NativeAudioAdapter audio(clock, broker, io); \
		linux_native::NativeVideoAdapter video(clock, broker, io); \
		std::unique_ptr<RecoveryEpoch> epoch; \
		const uint32_t requested = kind == OperationKind::audio ? MISTER_RESOURCE_NATIVE_AUDIO : \
			(kind == OperationKind::video ? MISTER_RESOURCE_NATIVE_VIDEO : \
			MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_NATIVE_VIDEO); \
		assert(broker.BeginRecovery(requested, 2100, 5100, &epoch) == MISTER_RESULT_OK); \
		std::unique_ptr<OperationLease> lease; \
		assert(broker.BeginRecoveryOperation(*epoch, kind, &lease) == MISTER_RESULT_OK); \
		std::unique_ptr<session_type> session; \
		SetPeripheralSessionAllocationFailureForTest(allocation_index); \
		assert((acquire) == MISTER_RESULT_PLATFORM); \
		ClearPeripheralSessionAllocationFailureForTest(); \
		PeripheralBrokerDisposition current = PeripheralBrokerDisposition::live; \
		assert((disposition) == MISTER_RESULT_OK); \
		assert(current == PeripheralBrokerDisposition::abandoned); \
		assert((acquire) == MISTER_RESULT_OK); \
		session.reset(); \
		} \
	}

	ASSERT_RECOVERY_ALLOCATION_ABANDONED(OperationKind::audio,
		RecoveryAudioSessionBundle, audio_record,
		PeripheralAuthorityTestPeer::AcquireRecoveryAudio(*lease, broker,
			*audio_record, audio.BackendIdentity(), &session),
		PeripheralAuthorityTestPeer::AudioDisposition(*lease, broker, &current));
	ASSERT_RECOVERY_ALLOCATION_ABANDONED(OperationKind::video,
		RecoveryVideoSessionBundle, video_record,
		PeripheralAuthorityTestPeer::AcquireRecoveryVideo(*lease, broker,
			*video_record, video.BackendIdentity(), &session),
		PeripheralAuthorityTestPeer::VideoDisposition(*lease, broker, &current));
	ASSERT_RECOVERY_ALLOCATION_ABANDONED(OperationKind::audio_video,
		RecoveryAudioVideoSessionBundle, coupled_record,
		PeripheralAuthorityTestPeer::AcquireRecoveryAudioVideo(*lease, broker,
			*coupled_record, video.BackendIdentity(), &session),
		PeripheralAuthorityTestPeer::AudioVideoDisposition(*lease, broker,
			&current));

#undef ASSERT_RECOVERY_ALLOCATION_ABANDONED
}

void TestForeignConsumerCannotUseOrCompleteABoundAudioBundle()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeAudioAdapter owner(clock, broker, io);
	linux_native::NativeAudioAdapter foreign(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::audio, 1000, &lease) ==
		MISTER_RESULT_OK);
	std::unique_ptr<ActiveAudioSessionBundle> bundle;
	assert(PeripheralAuthorityTestPeer::AcquireActiveAudio(*lease, broker,
		*profile, owner.BackendIdentity(), &bundle) == MISTER_RESULT_OK);
	const NativePeripheralAcquisitionOutcome rejected = foreign.StartAudio(
		std::move(bundle));
	assert(rejected.result == MISTER_RESULT_INVALID_ARGUMENT);
	assert(!rejected.acquired);
	assert(operations.begin_calls == 0 && operations.send_calls == 0 &&
		operations.finish_calls == 0);
	PeripheralBrokerDisposition disposition = PeripheralBrokerDisposition::live;
	assert(PeripheralAuthorityTestPeer::AudioDisposition(*lease, broker,
		&disposition) == MISTER_RESULT_OK);
	assert(disposition == PeripheralBrokerDisposition::abandoned);
	assert(PeripheralAuthorityTestPeer::AcquireActiveAudio(*lease, broker,
		*profile, owner.BackendIdentity(), &bundle) == MISTER_RESULT_OK);
	assert(owner.StartAudio(std::move(bundle)).result == MISTER_RESULT_OK);
}

void TestFixtureAudioVideoAndCoupledSessionsUseClosedRecipes()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeAudioAdapter audio(clock, broker, io);
	linux_native::NativeVideoAdapter video(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);

	std::unique_ptr<OperationLease> lease;
	std::unique_ptr<ActiveAudioSessionBundle> active_audio;
	assert(broker.Begin(generation, OperationKind::audio, 1000, &lease) ==
		MISTER_RESULT_OK);
	assert(PeripheralAuthorityTestPeer::AcquireActiveAudio(*lease, broker,
		*profile, audio.BackendIdentity(), &active_audio) == MISTER_RESULT_OK);
	const NativePeripheralAcquisitionOutcome audio_outcome = audio.StartAudio(
		std::move(active_audio));
	assert(audio_outcome.result == MISTER_RESULT_OK && audio_outcome.acquired);
	lease.reset();
	assert(operations.words.size() == 2);
	assert(operations.words[0] == 0x0026);
	assert(operations.words[1] == profile->audio.active_attenuation);

	std::unique_ptr<ActiveVideoSessionBundle> active_video;
	assert(broker.Begin(generation, OperationKind::video, 1000, &lease) ==
		MISTER_RESULT_OK);
	assert(PeripheralAuthorityTestPeer::AcquireActiveVideo(*lease, broker,
		*profile, video.BackendIdentity(), &active_video) == MISTER_RESULT_OK);
	const NativePeripheralAcquisitionOutcome video_outcome = video.StartVideo(
		std::move(active_video));
	assert(video_outcome.result == MISTER_RESULT_OK && video_outcome.acquired);
	lease.reset();
	assert(operations.words.size() == 7);
	assert(operations.words[2] == 0x002f);
	assert(operations.words[3] == 0);
	assert(operations.words[4] == 0x0020);
	assert(operations.words[5] == profile->video.set_video_words[0]);
	assert(operations.words[6] == profile->video.set_video_words[1]);

	std::unique_ptr<ActiveAudioVideoSessionBundle> active_coupled;
	assert(broker.Begin(generation, OperationKind::audio_video, 1000, &lease) ==
		MISTER_RESULT_OK);
	assert(PeripheralAuthorityTestPeer::AcquireActiveAudioVideo(*lease, broker,
		*profile, video.BackendIdentity(), &active_coupled) == MISTER_RESULT_OK);
	const NativeCoupledAcquisitionOutcome coupled_outcome = video.StartAudioVideo(
		std::move(active_coupled));
	assert(coupled_outcome.result == MISTER_RESULT_OK);
	assert(coupled_outcome.affected_flags ==
		(MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_NATIVE_VIDEO));
	lease.reset();
	assert(operations.words.size() == 9);
	assert(operations.words[7] == 0x0041);
	assert(operations.words[8] == profile->video.power_on_value);

	assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) ==
		MISTER_RESULT_OK);
	std::unique_ptr<CleanupAudioSessionBundle> cleanup_audio;
	assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio, &lease) ==
		MISTER_RESULT_OK);
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &cleanup_audio) == MISTER_RESULT_OK);
	const NativePeripheralReleaseOutcome mute = audio.StopAudio(
		std::move(cleanup_audio));
	assert(mute.result == MISTER_RESULT_OK && mute.local_shutdown_complete);
	assert(!mute.stable_neutral_observed);
	lease.reset();
	assert(operations.words[9] == 0x0026);
	assert(operations.words[10] == profile->audio.mute_attenuation);

	std::unique_ptr<CleanupVideoSessionBundle> cleanup_video;
	assert(broker.BeginCleanupOperation(*cleanup, OperationKind::video, &lease) ==
		MISTER_RESULT_OK);
	assert(PeripheralAuthorityTestPeer::AcquireCleanupVideo(*lease, broker,
		video.BackendIdentity(), &cleanup_video) == MISTER_RESULT_OK);
	const NativePeripheralReleaseOutcome video_stop = video.StopVideo(
		std::move(cleanup_video));
	assert(video_stop.result == MISTER_RESULT_OK);
	lease.reset();
	assert(operations.words.size() == 13);
	assert(operations.words[11] == 0x002f);
	assert(operations.words[12] == 0);
	for (size_t index = 11; index != operations.words.size(); ++index)
		assert(operations.words[index] != 0x0020);
}

void TestCoupledRecoveryKeepsAudioUnknownAndRecordsVideoNeutral()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeVideoAdapter video(clock, broker, io);
	const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(affected, 2100, 5100, &epoch) ==
		MISTER_RESULT_OK);
	const SafeAudioVideoRecoveryRecord *record =
		FixtureSafeAudioVideoRecoveryRecordForTest(NativeSystem::snes);
	assert(record != nullptr);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::audio_video,
		&lease) == MISTER_RESULT_OK);
	std::unique_ptr<RecoveryAudioVideoSessionBundle> session;
	assert(PeripheralAuthorityTestPeer::AcquireRecoveryAudioVideo(*lease, broker,
		*record, video.BackendIdentity(), &session) == MISTER_RESULT_OK);
	const NativeCoupledReleaseOutcome outcome = video.RecoverAudioVideo(
		std::move(session));
	assert(outcome.result == MISTER_RESULT_OK);
	assert(outcome.affected_flags == affected);
	assert(outcome.observed_flags == 0);
	assert(outcome.neutral_flags == MISTER_RESOURCE_NATIVE_VIDEO);
	const CoupledRecoveryReceipt receipt = {MISTER_RESULT_OK, affected,
		outcome.observed_flags, outcome.neutral_flags,
		outcome.local_resources_absent, outcome.closure_unknown,
		broker.mutation_sequence_for_test(), true, 0xa55a};
	assert(PeripheralAuthorityTestPeer::RecordCoupledRecovery(broker, *epoch,
		*lease, receipt, MISTER_RESULT_OK) == MISTER_RESULT_OK);
	lease.reset();
	MisterRecoveryObservationV2 observation = {};
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_NATIVE_VIDEO);
}

void TestFixtureRecoveryVideoUsesTheTeardownGoldenOnly()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeVideoAdapter video(clock, broker, io);
	const SafeVideoRecoveryRecord *record =
		FixtureSafeVideoRecoveryRecordForTest(NativeSystem::snes);
	assert(record != nullptr);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_NATIVE_VIDEO, 2100, 5100,
		&epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::video, &lease) ==
		MISTER_RESULT_OK);
	std::unique_ptr<RecoveryVideoSessionBundle> session;
	assert(PeripheralAuthorityTestPeer::AcquireRecoveryVideo(*lease, broker,
		*record, video.BackendIdentity(), &session) == MISTER_RESULT_OK);
	const NativePeripheralReleaseOutcome outcome = video.RecoverVideo(
		std::move(session));
	assert(outcome.result == MISTER_RESULT_OK);
	assert(operations.words.size() == 2);
	assert(operations.words[0] == 0x002f);
	assert(operations.words[1] == 0);
	for (size_t index = 0; index != operations.words.size(); ++index)
		assert(operations.words[index] != 0x0020);
}

void TestVideoWordFailureCarriesTheAcceptedMutationIntoAtomicFailure()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	operations.fail_send_call = 3;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeVideoAdapter video(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::video, 1000, &lease) ==
		MISTER_RESULT_OK);
	std::unique_ptr<ActiveVideoSessionBundle> session;
	assert(PeripheralAuthorityTestPeer::AcquireActiveVideo(*lease, broker,
		*profile, video.BackendIdentity(), &session) == MISTER_RESULT_OK);
	const NativePeripheralAcquisitionOutcome outcome = video.StartVideo(
		std::move(session));
	assert(outcome.result == MISTER_RESULT_PLATFORM);
	assert(outcome.acquired);
	lease.reset();
	std::unique_ptr<OperationLease> denied;
	assert(broker.Begin(generation, OperationKind::audio, 1000, &denied) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.send_calls == 3);
}

void TestEveryVideoWordFailureAttemptsFullTransactionClose()
{
	for (int failed_word = 1; failed_word <= 5; ++failed_word) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeAvOperations operations;
		operations.fail_send_call = failed_word;
		linux_native::NativeAvIoAdapter io(clock, operations);
		linux_native::NativeVideoAdapter video(clock, broker, io);
		const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
		assert(profile != nullptr);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::video, 1000, &lease) ==
			MISTER_RESULT_OK);
		std::unique_ptr<ActiveVideoSessionBundle> session;
		assert(PeripheralAuthorityTestPeer::AcquireActiveVideo(*lease, broker,
			*profile, video.BackendIdentity(), &session) == MISTER_RESULT_OK);
		const NativePeripheralAcquisitionOutcome outcome = video.StartVideo(
			std::move(session));
		assert(outcome.result == MISTER_RESULT_PLATFORM && outcome.acquired);
		assert(operations.send_calls == failed_word);
		assert(operations.finish_calls == 1);
	}
}

void TestFinishFailureRemainsClosureUnknownAndCannotRecheckout()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	operations.fail_finish_call = 1;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeVideoAdapter video(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginCleanupOperation(*cleanup, OperationKind::video, &lease) ==
		MISTER_RESULT_OK);
	std::unique_ptr<CleanupVideoSessionBundle> session;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupVideo(*lease, broker,
		video.BackendIdentity(), &session) == MISTER_RESULT_OK);
	const NativePeripheralReleaseOutcome outcome = video.StopVideo(
		std::move(session));
	assert(outcome.result == MISTER_RESULT_PLATFORM);
	assert(!outcome.local_resources_absent && outcome.closure_unknown);
	assert(operations.finish_calls == 1);
	PeripheralBrokerDisposition disposition = PeripheralBrokerDisposition::live;
	assert(PeripheralAuthorityTestPeer::VideoDisposition(*lease, broker,
		&disposition) == MISTER_RESULT_OK);
	assert(disposition == PeripheralBrokerDisposition::abandoned);
	std::unique_ptr<CleanupVideoSessionBundle> retry;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupVideo(*lease, broker,
		video.BackendIdentity(), &retry) == MISTER_RESULT_INVALID_STATE);
}

void TestCoupledLocalAbsentFailureAbandonsAtomically()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	operations.fail_send_call = 1;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeVideoAdapter video(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio_video,
		&lease) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupAudioVideoSessionBundle> session;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudioVideo(*lease, broker,
		video.BackendIdentity(), &session) == MISTER_RESULT_OK);
	const NativeCoupledReleaseOutcome outcome = video.StopAudioVideo(
		std::move(session));
	assert(outcome.result == MISTER_RESULT_PLATFORM);
	assert(outcome.local_resources_absent && !outcome.closure_unknown);
	assert(operations.finish_calls == 1);
	PeripheralBrokerDisposition disposition = PeripheralBrokerDisposition::live;
	assert(PeripheralAuthorityTestPeer::AudioVideoDisposition(*lease, broker,
		&disposition) == MISTER_RESULT_OK);
	assert(disposition == PeripheralBrokerDisposition::abandoned);
	std::unique_ptr<CleanupAudioVideoSessionBundle> retry;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudioVideo(*lease, broker,
		video.BackendIdentity(), &retry) == MISTER_RESULT_OK);
}

void TestCleanupAudioRetrySkipsOnlyPositiveAcknowledgements()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	operations.fail_send_call = 2;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeAudioAdapter audio(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio, &lease) ==
		MISTER_RESULT_OK);
	std::unique_ptr<CleanupAudioSessionBundle> first;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &first) == MISTER_RESULT_OK);
	const NativePeripheralReleaseOutcome failed = audio.StopAudio(std::move(first));
	assert(failed.result == MISTER_RESULT_PLATFORM);
	assert(failed.local_resources_absent && !failed.closure_unknown);
	linux_native::NativeAudioAdapter foreign_audio(clock, broker, io);
	std::unique_ptr<CleanupAudioSessionBundle> foreign;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		foreign_audio.BackendIdentity(), &foreign) == MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<CleanupAudioSessionBundle> dropped;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &dropped) == MISTER_RESULT_OK);
	dropped.reset();
	std::unique_ptr<CleanupAudioSessionBundle> retry;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &retry) == MISTER_RESULT_OK);
	const NativePeripheralReleaseOutcome succeeded = audio.StopAudio(std::move(retry));
	assert(succeeded.result == MISTER_RESULT_OK &&
		succeeded.local_shutdown_complete);
	assert(operations.words.size() == 3);
	assert(operations.words[0] == 0x0026);
	assert(operations.words[1] == profile->audio.mute_attenuation);
	assert(operations.words[2] == profile->audio.mute_attenuation);
	assert(operations.finish_calls == 2);
	AssertAllDeadlines(operations, 2100);
}

void TestExpiredRetainedActionCannotRecheckoutOrExtendDeadline()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	operations.fail_send_call = 2;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeAudioAdapter audio(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio, &lease) ==
		MISTER_RESULT_OK);
	std::unique_ptr<CleanupAudioSessionBundle> first;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &first) == MISTER_RESULT_OK);
	assert(audio.StopAudio(std::move(first)).result == MISTER_RESULT_PLATFORM);
	clock.SetNowMs(2100);
	std::unique_ptr<CleanupAudioSessionBundle> retry;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &retry) == MISTER_RESULT_DEADLINE);
	assert(operations.words.size() == 2);
	AssertAllDeadlines(operations, 2100);
}

void TestBeginFailureAfterRetainedAudioSuffixKeepsTheSuffixRetryable()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	operations.fail_send_call = 2;
	operations.fail_begin_call = 2;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeAudioAdapter audio(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio, &lease) ==
		MISTER_RESULT_OK);
	std::unique_ptr<CleanupAudioSessionBundle> first;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &first) == MISTER_RESULT_OK);
	assert(audio.StopAudio(std::move(first)).result == MISTER_RESULT_PLATFORM);
	std::unique_ptr<CleanupAudioSessionBundle> begin_failed;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &begin_failed) == MISTER_RESULT_OK);
	const NativePeripheralReleaseOutcome failed = audio.StopAudio(
		std::move(begin_failed));
	assert(failed.result == MISTER_RESULT_PLATFORM);
	assert(failed.local_resources_absent && !failed.closure_unknown);
	linux_native::NativeAudioAdapter foreign_audio(clock, broker, io);
	std::unique_ptr<CleanupAudioSessionBundle> foreign;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		foreign_audio.BackendIdentity(), &foreign) == MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<CleanupAudioSessionBundle> dropped;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &dropped) == MISTER_RESULT_OK);
	dropped.reset();
	std::unique_ptr<CleanupAudioSessionBundle> retry;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
		audio.BackendIdentity(), &retry) == MISTER_RESULT_OK);
	assert(audio.StopAudio(std::move(retry)).result == MISTER_RESULT_OK);
	assert(operations.words.size() == 3);
	assert(operations.words[0] == 0x0026);
	assert(operations.words[1] == profile->audio.mute_attenuation);
	assert(operations.words[2] == profile->audio.mute_attenuation);
	assert(operations.begin_calls == 3 && operations.finish_calls == 2);
	AssertAllDeadlines(operations, 2100);
}

void TestFreshCleanupAudioBeginFailureUsesTheCurrentMutationSequence()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeAudioAdapter audio(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active_lease;
	assert(broker.Begin(generation, OperationKind::audio, 1000, &active_lease) ==
		MISTER_RESULT_OK);
	std::unique_ptr<ActiveAudioSessionBundle> active;
	assert(PeripheralAuthorityTestPeer::AcquireActiveAudio(*active_lease, broker,
		*profile, audio.BackendIdentity(), &active) == MISTER_RESULT_OK);
	assert(audio.StartAudio(std::move(active)).result == MISTER_RESULT_OK);
	active_lease.reset();
	assert(broker.mutation_sequence_for_test() != 0);
	assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup_lease;
	assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio,
		&cleanup_lease) == MISTER_RESULT_OK);
	operations.fail_begin_call = operations.begin_calls + 1;
	const size_t before_failure_words = operations.words.size();
	const int before_failure_finishes = operations.finish_calls;
	const size_t before_failure_deadlines = operations.deadlines.size();
	std::unique_ptr<CleanupAudioSessionBundle> first;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*cleanup_lease, broker,
		audio.BackendIdentity(), &first) == MISTER_RESULT_OK);
	const NativePeripheralReleaseOutcome failed = audio.StopAudio(std::move(first));
	assert(failed.result == MISTER_RESULT_PLATFORM);
	assert(failed.local_resources_absent && !failed.closure_unknown);
	assert(operations.words.size() == before_failure_words);
	assert(operations.finish_calls == before_failure_finishes);
	for (size_t index = before_failure_deadlines;
		index < operations.deadlines.size(); ++index)
		assert(operations.deadlines[index] == 2100);
	PeripheralBrokerDisposition disposition = PeripheralBrokerDisposition::live;
	assert(PeripheralAuthorityTestPeer::AudioDisposition(*cleanup_lease, broker,
		&disposition) == MISTER_RESULT_OK);
	assert(disposition == PeripheralBrokerDisposition::abandoned);
	std::unique_ptr<CleanupAudioSessionBundle> retry;
	assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*cleanup_lease, broker,
		audio.BackendIdentity(), &retry) == MISTER_RESULT_OK);
	assert(audio.StopAudio(std::move(retry)).result == MISTER_RESULT_OK);
	assert(operations.words.size() == before_failure_words + 2);
	assert(operations.words[before_failure_words] == 0x0026);
	assert(operations.words[before_failure_words + 1] ==
		profile->audio.mute_attenuation);
}

enum class RetriedPeripheralOperation : uint8_t {
	cleanup_audio,
	recovery_audio,
	cleanup_video,
	recovery_video,
	cleanup_coupled,
	recovery_coupled
};

void AdvanceFixtureAudioMutation(FakeAvOperations &operations,
	HardwareBroker &broker, const NativeCoreProfile &profile,
	linux_native::NativeAudioAdapter &audio, PlatformGenerationId *generation)
{
	assert(generation != nullptr);
	assert(broker.EnterFixtureForTest(profile, generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(*generation, OperationKind::audio, 1000, &lease) ==
		MISTER_RESULT_OK);
	std::unique_ptr<ActiveAudioSessionBundle> session;
	assert(PeripheralAuthorityTestPeer::AcquireActiveAudio(*lease, broker, profile,
		audio.BackendIdentity(), &session) == MISTER_RESULT_OK);
	assert(audio.StartAudio(std::move(session)).result == MISTER_RESULT_OK);
	lease.reset();
	assert(operations.begin_calls == 1);
	assert(broker.mutation_sequence_for_test() != 0);
}

template <typename Bundle, typename Acquire, typename Execute, typename Disposition>
void AssertFreshKnownNoAdmissionThenRetry(FakeAvOperations &operations,
	uint64_t deadline, Acquire acquire, Execute execute, Disposition disposition,
	uint16_t first_word, uint16_t second_word)
{
	operations.fail_begin_call = operations.begin_calls + 1;
	const size_t words_before_failure = operations.words.size();
	const int finishes_before_failure = operations.finish_calls;
	const size_t deadlines_before_failure = operations.deadlines.size();
	std::unique_ptr<Bundle> first;
	assert(acquire(&first) == MISTER_RESULT_OK);
	assert(execute(std::move(first)).result == MISTER_RESULT_PLATFORM);
	assert(operations.words.size() == words_before_failure);
	assert(operations.finish_calls == finishes_before_failure);
	for (size_t index = deadlines_before_failure;
		index < operations.deadlines.size(); ++index)
		assert(operations.deadlines[index] == deadline);
	PeripheralBrokerDisposition current = PeripheralBrokerDisposition::live;
	assert(disposition(&current) == MISTER_RESULT_OK);
	assert(current == PeripheralBrokerDisposition::abandoned);
	std::unique_ptr<Bundle> retry;
	assert(acquire(&retry) == MISTER_RESULT_OK);
	assert(execute(std::move(retry)).result == MISTER_RESULT_OK);
	assert(operations.words.size() == words_before_failure + 2);
	assert(operations.words[words_before_failure] == first_word);
	assert(operations.words[words_before_failure + 1] == second_word);
	for (size_t index = deadlines_before_failure;
		index < operations.deadlines.size(); ++index)
		assert(operations.deadlines[index] == deadline);
}

void TestFreshCleanupBeginFailuresUseTheCurrentMutationSequence()
{
	const RetriedPeripheralOperation operations_to_test[] = {
		RetriedPeripheralOperation::cleanup_audio,
		RetriedPeripheralOperation::cleanup_video,
		RetriedPeripheralOperation::cleanup_coupled};
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	for (size_t index = 0; index < sizeof(operations_to_test) /
		sizeof(operations_to_test[0]); ++index) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeAvOperations operations;
		linux_native::NativeAvIoAdapter io(clock, operations);
		linux_native::NativeAudioAdapter audio(clock, broker, io);
		linux_native::NativeVideoAdapter video(clock, broker, io);
		PlatformGenerationId generation = 0;
		AdvanceFixtureAudioMutation(operations, broker, *profile, audio,
			&generation);
		assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> cleanup;
		assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		switch (operations_to_test[index]) {
		case RetriedPeripheralOperation::cleanup_audio:
			assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio,
				&lease) == MISTER_RESULT_OK);
			AssertFreshKnownNoAdmissionThenRetry<CleanupAudioSessionBundle>(
				operations, 2100,
				[&](std::unique_ptr<CleanupAudioSessionBundle> *session) {
					return PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease,
						broker, audio.BackendIdentity(), session);
				},
				[&](std::unique_ptr<CleanupAudioSessionBundle> &&session) {
					return audio.StopAudio(std::move(session));
				},
				[&](PeripheralBrokerDisposition *disposition) {
					return PeripheralAuthorityTestPeer::AudioDisposition(*lease,
						broker, disposition);
				},
				0x0026, profile->audio.mute_attenuation);
			break;
		case RetriedPeripheralOperation::cleanup_video:
			assert(broker.BeginCleanupOperation(*cleanup, OperationKind::video,
				&lease) == MISTER_RESULT_OK);
			AssertFreshKnownNoAdmissionThenRetry<CleanupVideoSessionBundle>(
				operations, 2100,
				[&](std::unique_ptr<CleanupVideoSessionBundle> *session) {
					return PeripheralAuthorityTestPeer::AcquireCleanupVideo(*lease,
						broker, video.BackendIdentity(), session);
				},
				[&](std::unique_ptr<CleanupVideoSessionBundle> &&session) {
					return video.StopVideo(std::move(session));
				},
				[&](PeripheralBrokerDisposition *disposition) {
					return PeripheralAuthorityTestPeer::VideoDisposition(*lease,
						broker, disposition);
				},
				0x002f, profile->video.framebuffer_disable_word);
			break;
		case RetriedPeripheralOperation::cleanup_coupled:
			assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio_video,
				&lease) == MISTER_RESULT_OK);
			AssertFreshKnownNoAdmissionThenRetry<CleanupAudioVideoSessionBundle>(
				operations, 2100,
				[&](std::unique_ptr<CleanupAudioVideoSessionBundle> *session) {
					return PeripheralAuthorityTestPeer::AcquireCleanupAudioVideo(*lease,
						broker, video.BackendIdentity(), session);
				},
				[&](std::unique_ptr<CleanupAudioVideoSessionBundle> &&session) {
					return video.StopAudioVideo(std::move(session));
				},
				[&](PeripheralBrokerDisposition *disposition) {
					return PeripheralAuthorityTestPeer::AudioVideoDisposition(*lease,
						broker, disposition);
				},
				0x0041, profile->video.power_down_value);
			break;
		default:
			assert(false);
		}
	}
}

void TestFreshRecoveryBeginFailuresUseTheCurrentMutationSequence()
{
	const RetriedPeripheralOperation operations_to_test[] = {
		RetriedPeripheralOperation::recovery_audio,
		RetriedPeripheralOperation::recovery_video,
		RetriedPeripheralOperation::recovery_coupled};
	const SafeAudioRecoveryRecord *audio_record =
		FixtureSafeAudioRecoveryRecordForTest(NativeSystem::snes);
	const SafeVideoRecoveryRecord *video_record =
		FixtureSafeVideoRecoveryRecordForTest(NativeSystem::snes);
	const SafeAudioVideoRecoveryRecord *coupled_record =
		FixtureSafeAudioVideoRecoveryRecordForTest(NativeSystem::snes);
	assert(audio_record != nullptr && video_record != nullptr &&
		coupled_record != nullptr);
	for (size_t index = 0; index < sizeof(operations_to_test) /
		sizeof(operations_to_test[0]); ++index) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeAvOperations operations;
		linux_native::NativeAvIoAdapter io(clock, operations);
		linux_native::NativeAudioAdapter audio(clock, broker, io);
		linux_native::NativeVideoAdapter video(clock, broker, io);
		const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO;
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested, 2100, 5100, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> mutation_lease;
		if (operations_to_test[index] == RetriedPeripheralOperation::recovery_audio) {
			assert(broker.BeginRecoveryOperation(*epoch, OperationKind::video,
				&mutation_lease) == MISTER_RESULT_OK);
			std::unique_ptr<RecoveryVideoSessionBundle> mutation_session;
			assert(PeripheralAuthorityTestPeer::AcquireRecoveryVideo(*mutation_lease,
				broker, *video_record, video.BackendIdentity(), &mutation_session) ==
				MISTER_RESULT_OK);
			assert(video.RecoverVideo(std::move(mutation_session)).result ==
				MISTER_RESULT_OK);
		} else {
			assert(broker.BeginRecoveryOperation(*epoch, OperationKind::audio,
				&mutation_lease) == MISTER_RESULT_OK);
			std::unique_ptr<RecoveryAudioSessionBundle> mutation_session;
			assert(PeripheralAuthorityTestPeer::AcquireRecoveryAudio(*mutation_lease,
				broker, *audio_record, audio.BackendIdentity(), &mutation_session) ==
				MISTER_RESULT_OK);
			assert(audio.RecoverAudio(std::move(mutation_session)).result ==
				MISTER_RESULT_OK);
		}
		mutation_lease.reset();
		assert(broker.mutation_sequence_for_test() != 0);
		std::unique_ptr<OperationLease> lease;
		switch (operations_to_test[index]) {
		case RetriedPeripheralOperation::recovery_audio:
			assert(broker.BeginRecoveryOperation(*epoch, OperationKind::audio,
				&lease) == MISTER_RESULT_OK);
			AssertFreshKnownNoAdmissionThenRetry<RecoveryAudioSessionBundle>(
				operations, 2100,
				[&](std::unique_ptr<RecoveryAudioSessionBundle> *session) {
					return PeripheralAuthorityTestPeer::AcquireRecoveryAudio(*lease,
						broker, *audio_record, audio.BackendIdentity(), session);
				},
				[&](std::unique_ptr<RecoveryAudioSessionBundle> &&session) {
					return audio.RecoverAudio(std::move(session));
				},
				[&](PeripheralBrokerDisposition *disposition) {
					return PeripheralAuthorityTestPeer::AudioDisposition(*lease,
						broker, disposition);
				},
				0x0026, audio_record->audio.mute_attenuation);
			break;
		case RetriedPeripheralOperation::recovery_video:
			assert(broker.BeginRecoveryOperation(*epoch, OperationKind::video,
				&lease) == MISTER_RESULT_OK);
			AssertFreshKnownNoAdmissionThenRetry<RecoveryVideoSessionBundle>(
				operations, 2100,
				[&](std::unique_ptr<RecoveryVideoSessionBundle> *session) {
					return PeripheralAuthorityTestPeer::AcquireRecoveryVideo(*lease,
						broker, *video_record, video.BackendIdentity(), session);
				},
				[&](std::unique_ptr<RecoveryVideoSessionBundle> &&session) {
					return video.RecoverVideo(std::move(session));
				},
				[&](PeripheralBrokerDisposition *disposition) {
					return PeripheralAuthorityTestPeer::VideoDisposition(*lease,
						broker, disposition);
				},
				0x002f, video_record->video.framebuffer_disable_word);
			break;
		case RetriedPeripheralOperation::recovery_coupled:
			assert(broker.BeginRecoveryOperation(*epoch, OperationKind::audio_video,
				&lease) == MISTER_RESULT_OK);
			AssertFreshKnownNoAdmissionThenRetry<RecoveryAudioVideoSessionBundle>(
				operations, 2100,
				[&](std::unique_ptr<RecoveryAudioVideoSessionBundle> *session) {
					return PeripheralAuthorityTestPeer::AcquireRecoveryAudioVideo(*lease,
						broker, *coupled_record, video.BackendIdentity(), session);
				},
				[&](std::unique_ptr<RecoveryAudioVideoSessionBundle> &&session) {
					return video.RecoverAudioVideo(std::move(session));
				},
				[&](PeripheralBrokerDisposition *disposition) {
					return PeripheralAuthorityTestPeer::AudioVideoDisposition(*lease,
						broker, disposition);
				},
				0x0041, coupled_record->video.power_down_value);
			break;
		default:
			assert(false);
		}
	}
}

void ExerciseAcknowledgedSuffixRetry(RetriedPeripheralOperation operation,
	int failed_word, bool finish_failure = false,
	int retained_begin_failures = 0)
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeAvOperations operations;
	operations.fail_send_call = failed_word;
	operations.fail_finish_call = finish_failure ? 1 : 0;
	for (int failure = 0; failure < retained_begin_failures; ++failure)
		operations.fail_begin_calls.push_back(2 + failure);
	linux_native::NativeAvIoAdapter io(clock, operations);
	linux_native::NativeAudioAdapter audio(clock, broker, io);
	linux_native::NativeVideoAdapter video(clock, broker, io);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	const SafeAudioRecoveryRecord *audio_record =
		FixtureSafeAudioRecoveryRecordForTest(NativeSystem::snes);
	const SafeVideoRecoveryRecord *video_record =
		FixtureSafeVideoRecoveryRecordForTest(NativeSystem::snes);
	const SafeAudioVideoRecoveryRecord *coupled_record =
		FixtureSafeAudioVideoRecoveryRecordForTest(NativeSystem::snes);
	assert(profile != nullptr && audio_record != nullptr && video_record != nullptr &&
		coupled_record != nullptr);
	std::vector<uint16_t> recipe;
	switch (operation) {
	case RetriedPeripheralOperation::cleanup_audio: {
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> cleanup;
		assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio, &lease) ==
			MISTER_RESULT_OK);
		std::unique_ptr<CleanupAudioSessionBundle> first;
		assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
			audio.BackendIdentity(), &first) == MISTER_RESULT_OK);
		assert(audio.StopAudio(std::move(first)).result == MISTER_RESULT_PLATFORM);
		for (int failure = 0; failure < retained_begin_failures; ++failure) {
			std::unique_ptr<CleanupAudioSessionBundle> begin_failed;
			assert(PeripheralAuthorityTestPeer::AcquireCleanupAudio(*lease, broker,
				audio.BackendIdentity(), &begin_failed) == MISTER_RESULT_OK);
			assert(audio.StopAudio(std::move(begin_failed)).result ==
				MISTER_RESULT_PLATFORM);
		}
		std::unique_ptr<CleanupAudioSessionBundle> retry;
		const Result retry_result = PeripheralAuthorityTestPeer::AcquireCleanupAudio(
			*lease, broker, audio.BackendIdentity(), &retry);
		assert(finish_failure ? retry_result == MISTER_RESULT_INVALID_STATE :
			retry_result == MISTER_RESULT_OK);
		if (!finish_failure)
			assert(audio.StopAudio(std::move(retry)).result == MISTER_RESULT_OK);
		recipe = {0x0026, profile->audio.mute_attenuation};
		break;
	}
	case RetriedPeripheralOperation::recovery_audio: {
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_NATIVE_AUDIO, 2100, 5100,
			&epoch) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginRecoveryOperation(*epoch, OperationKind::audio, &lease) ==
			MISTER_RESULT_OK);
		std::unique_ptr<RecoveryAudioSessionBundle> first;
		assert(PeripheralAuthorityTestPeer::AcquireRecoveryAudio(*lease, broker,
			*audio_record, audio.BackendIdentity(), &first) == MISTER_RESULT_OK);
		assert(audio.RecoverAudio(std::move(first)).result == MISTER_RESULT_PLATFORM);
		for (int failure = 0; failure < retained_begin_failures; ++failure) {
			std::unique_ptr<RecoveryAudioSessionBundle> begin_failed;
			assert(PeripheralAuthorityTestPeer::AcquireRecoveryAudio(*lease, broker,
				*audio_record, audio.BackendIdentity(), &begin_failed) ==
				MISTER_RESULT_OK);
			assert(audio.RecoverAudio(std::move(begin_failed)).result ==
				MISTER_RESULT_PLATFORM);
		}
		std::unique_ptr<RecoveryAudioSessionBundle> retry;
		const Result retry_result = PeripheralAuthorityTestPeer::AcquireRecoveryAudio(
			*lease, broker, *audio_record, audio.BackendIdentity(), &retry);
		assert(finish_failure ? retry_result == MISTER_RESULT_INVALID_STATE :
			retry_result == MISTER_RESULT_OK);
		if (!finish_failure)
			assert(audio.RecoverAudio(std::move(retry)).result == MISTER_RESULT_OK);
		recipe = {0x0026, audio_record->audio.mute_attenuation};
		break;
	}
	case RetriedPeripheralOperation::cleanup_video: {
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> cleanup;
		assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginCleanupOperation(*cleanup, OperationKind::video, &lease) ==
			MISTER_RESULT_OK);
		std::unique_ptr<CleanupVideoSessionBundle> first;
		assert(PeripheralAuthorityTestPeer::AcquireCleanupVideo(*lease, broker,
			video.BackendIdentity(), &first) == MISTER_RESULT_OK);
		assert(video.StopVideo(std::move(first)).result == MISTER_RESULT_PLATFORM);
		for (int failure = 0; failure < retained_begin_failures; ++failure) {
			std::unique_ptr<CleanupVideoSessionBundle> begin_failed;
			assert(PeripheralAuthorityTestPeer::AcquireCleanupVideo(*lease, broker,
				video.BackendIdentity(), &begin_failed) == MISTER_RESULT_OK);
			assert(video.StopVideo(std::move(begin_failed)).result ==
				MISTER_RESULT_PLATFORM);
		}
		std::unique_ptr<CleanupVideoSessionBundle> retry;
		const Result retry_result = PeripheralAuthorityTestPeer::AcquireCleanupVideo(
			*lease, broker, video.BackendIdentity(), &retry);
		assert(finish_failure ? retry_result == MISTER_RESULT_INVALID_STATE :
			retry_result == MISTER_RESULT_OK);
		if (!finish_failure)
			assert(video.StopVideo(std::move(retry)).result == MISTER_RESULT_OK);
		recipe = {0x002f, profile->video.framebuffer_disable_word};
		break;
	}
	case RetriedPeripheralOperation::recovery_video: {
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_NATIVE_VIDEO, 2100, 5100,
			&epoch) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginRecoveryOperation(*epoch, OperationKind::video, &lease) ==
			MISTER_RESULT_OK);
		std::unique_ptr<RecoveryVideoSessionBundle> first;
		assert(PeripheralAuthorityTestPeer::AcquireRecoveryVideo(*lease, broker,
			*video_record, video.BackendIdentity(), &first) == MISTER_RESULT_OK);
		assert(video.RecoverVideo(std::move(first)).result == MISTER_RESULT_PLATFORM);
		for (int failure = 0; failure < retained_begin_failures; ++failure) {
			std::unique_ptr<RecoveryVideoSessionBundle> begin_failed;
			assert(PeripheralAuthorityTestPeer::AcquireRecoveryVideo(*lease, broker,
				*video_record, video.BackendIdentity(), &begin_failed) ==
				MISTER_RESULT_OK);
			assert(video.RecoverVideo(std::move(begin_failed)).result ==
				MISTER_RESULT_PLATFORM);
		}
		std::unique_ptr<RecoveryVideoSessionBundle> retry;
		const Result retry_result = PeripheralAuthorityTestPeer::AcquireRecoveryVideo(
			*lease, broker, *video_record, video.BackendIdentity(), &retry);
		assert(finish_failure ? retry_result == MISTER_RESULT_INVALID_STATE :
			retry_result == MISTER_RESULT_OK);
		if (!finish_failure)
			assert(video.RecoverVideo(std::move(retry)).result == MISTER_RESULT_OK);
		recipe = {0x002f, video_record->video.framebuffer_disable_word};
		break;
	}
	case RetriedPeripheralOperation::cleanup_coupled: {
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		assert(broker.Quiesce(generation, 500) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> cleanup;
		assert(broker.BeginCleanup(generation, 2100, 5100, &cleanup) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginCleanupOperation(*cleanup, OperationKind::audio_video,
			&lease) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupAudioVideoSessionBundle> first;
		assert(PeripheralAuthorityTestPeer::AcquireCleanupAudioVideo(*lease, broker,
			video.BackendIdentity(), &first) == MISTER_RESULT_OK);
		assert(video.StopAudioVideo(std::move(first)).result == MISTER_RESULT_PLATFORM);
		for (int failure = 0; failure < retained_begin_failures; ++failure) {
			std::unique_ptr<CleanupAudioVideoSessionBundle> begin_failed;
			assert(PeripheralAuthorityTestPeer::AcquireCleanupAudioVideo(*lease, broker,
				video.BackendIdentity(), &begin_failed) == MISTER_RESULT_OK);
			assert(video.StopAudioVideo(std::move(begin_failed)).result ==
				MISTER_RESULT_PLATFORM);
		}
		std::unique_ptr<CleanupAudioVideoSessionBundle> retry;
		const Result retry_result =
			PeripheralAuthorityTestPeer::AcquireCleanupAudioVideo(*lease, broker,
				video.BackendIdentity(), &retry);
		assert(finish_failure ? retry_result == MISTER_RESULT_INVALID_STATE :
			retry_result == MISTER_RESULT_OK);
		if (!finish_failure)
			assert(video.StopAudioVideo(std::move(retry)).result == MISTER_RESULT_OK);
		recipe = {0x0041, profile->video.power_down_value};
		break;
	}
	case RetriedPeripheralOperation::recovery_coupled: {
		const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO;
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested, 2100, 5100, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginRecoveryOperation(*epoch, OperationKind::audio_video,
			&lease) == MISTER_RESULT_OK);
		std::unique_ptr<RecoveryAudioVideoSessionBundle> first;
		assert(PeripheralAuthorityTestPeer::AcquireRecoveryAudioVideo(*lease, broker,
			*coupled_record, video.BackendIdentity(), &first) == MISTER_RESULT_OK);
		assert(video.RecoverAudioVideo(std::move(first)).result ==
			MISTER_RESULT_PLATFORM);
		for (int failure = 0; failure < retained_begin_failures; ++failure) {
			std::unique_ptr<RecoveryAudioVideoSessionBundle> begin_failed;
			assert(PeripheralAuthorityTestPeer::AcquireRecoveryAudioVideo(*lease,
				broker, *coupled_record, video.BackendIdentity(), &begin_failed) ==
				MISTER_RESULT_OK);
			assert(video.RecoverAudioVideo(std::move(begin_failed)).result ==
				MISTER_RESULT_PLATFORM);
		}
		std::unique_ptr<RecoveryAudioVideoSessionBundle> retry;
		const Result retry_result =
			PeripheralAuthorityTestPeer::AcquireRecoveryAudioVideo(*lease, broker,
				*coupled_record, video.BackendIdentity(), &retry);
		assert(finish_failure ? retry_result == MISTER_RESULT_INVALID_STATE :
			retry_result == MISTER_RESULT_OK);
		if (!finish_failure)
			assert(video.RecoverAudioVideo(std::move(retry)).result == MISTER_RESULT_OK);
		recipe = {0x0041, coupled_record->video.power_down_value};
		break;
	}
	}
	assert(recipe.size() == 2);
	const std::vector<uint16_t> expected = finish_failure ? recipe :
		failed_word == 1 ? std::vector<uint16_t>{recipe[0], recipe[0], recipe[1]} :
		std::vector<uint16_t>{recipe[0], recipe[1], recipe[1]};
	assert(operations.words == expected);
	assert(operations.finish_calls == (finish_failure ? 1 : 2));
	assert(operations.begin_calls == (finish_failure ? 1 :
		2 + retained_begin_failures));
	AssertAllDeadlines(operations, 2100);
}

void TestEveryCleanupRecoveryActionRetrySkipsOnlyAcknowledgedWords()
{
	const RetriedPeripheralOperation operations[] = {
		RetriedPeripheralOperation::cleanup_audio,
		RetriedPeripheralOperation::recovery_audio,
		RetriedPeripheralOperation::cleanup_video,
		RetriedPeripheralOperation::recovery_video,
		RetriedPeripheralOperation::cleanup_coupled,
		RetriedPeripheralOperation::recovery_coupled};
	for (size_t operation = 0; operation < sizeof(operations) /
		sizeof(operations[0]); ++operation)
		for (int failed_word = 1; failed_word <= 2; ++failed_word)
			ExerciseAcknowledgedSuffixRetry(operations[operation], failed_word);
}

void TestEveryCleanupRecoveryActionFinishFailureClosesRetryAuthority()
{
	const RetriedPeripheralOperation operations[] = {
		RetriedPeripheralOperation::cleanup_audio,
		RetriedPeripheralOperation::recovery_audio,
		RetriedPeripheralOperation::cleanup_video,
		RetriedPeripheralOperation::recovery_video,
		RetriedPeripheralOperation::cleanup_coupled,
		RetriedPeripheralOperation::recovery_coupled};
	for (size_t operation = 0; operation < sizeof(operations) /
		sizeof(operations[0]); ++operation)
		ExerciseAcknowledgedSuffixRetry(operations[operation], 0, true);
}

void TestBeginFailuresAtEveryRetainedSuffixIndexPreserveEveryAction()
{
	const RetriedPeripheralOperation operations[] = {
		RetriedPeripheralOperation::cleanup_audio,
		RetriedPeripheralOperation::recovery_audio,
		RetriedPeripheralOperation::cleanup_video,
		RetriedPeripheralOperation::recovery_video,
		RetriedPeripheralOperation::cleanup_coupled,
		RetriedPeripheralOperation::recovery_coupled};
	for (size_t operation = 0; operation < sizeof(operations) /
		sizeof(operations[0]); ++operation)
		for (int failed_word = 1; failed_word <= 2; ++failed_word)
			ExerciseAcknowledgedSuffixRetry(operations[operation], failed_word,
				false, 2);
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestFixtureProfilesHaveOnlyClosedAudioVideoAuthority();
	mister::native::TestTypedAudioVideoSessionsCannotBeForgedOrConverted();
	mister::native::TestAudioVideoResourcesAreSeparateTypedBoundaries();
	mister::native::TestDestroyedCleanupAudioSessionBecomesSameRegistrationRecheckoutable();
	mister::native::TestEveryTypedWrapperAllocationFailureLeavesNoLiveRegistration();
	mister::native::TestForeignConsumerCannotUseOrCompleteABoundAudioBundle();
	mister::native::TestFixtureAudioVideoAndCoupledSessionsUseClosedRecipes();
	mister::native::TestCoupledRecoveryKeepsAudioUnknownAndRecordsVideoNeutral();
	mister::native::TestFixtureRecoveryVideoUsesTheTeardownGoldenOnly();
	mister::native::TestVideoWordFailureCarriesTheAcceptedMutationIntoAtomicFailure();
	mister::native::TestEveryVideoWordFailureAttemptsFullTransactionClose();
	mister::native::TestFinishFailureRemainsClosureUnknownAndCannotRecheckout();
	mister::native::TestCoupledLocalAbsentFailureAbandonsAtomically();
	mister::native::TestCleanupAudioRetrySkipsOnlyPositiveAcknowledgements();
	mister::native::TestExpiredRetainedActionCannotRecheckoutOrExtendDeadline();
	mister::native::TestBeginFailureAfterRetainedAudioSuffixKeepsTheSuffixRetryable();
	mister::native::TestFreshCleanupAudioBeginFailureUsesTheCurrentMutationSequence();
	mister::native::TestFreshCleanupBeginFailuresUseTheCurrentMutationSequence();
	mister::native::TestFreshRecoveryBeginFailuresUseTheCurrentMutationSequence();
	mister::native::TestEveryCleanupRecoveryActionRetrySkipsOnlyAcknowledgedWords();
	mister::native::TestEveryCleanupRecoveryActionFinishFailureClosesRetryAuthority();
	mister::native::TestBeginFailuresAtEveryRetainedSuffixIndexPreserveEveryAction();
	return 0;
}
