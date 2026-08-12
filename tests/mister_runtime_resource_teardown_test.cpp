// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/mister_runtime_linux_v2.hpp"

#include <assert.h>

#include <vector>

namespace linux_v2 = mister::linux_v2;

class FakeResources : public linux_v2::ResourceOperations {
public:
	uint64_t now_ms;
	linux_v2::TeardownAction fail_action;
	linux_v2::TeardownAction unobserved_action;
	uint32_t action_delay_ms;
	uint32_t observation_delay_ms;
	std::vector<linux_v2::TeardownAction> events;
	std::vector<uint32_t> budgets;
	std::vector<uint32_t> observation_budgets;
	FakeResources() : now_ms(100), fail_action(linux_v2::TeardownAction::none),
		unobserved_action(linux_v2::TeardownAction::none), action_delay_ms(10),
		observation_delay_ms(0) {}
	uint64_t NowMs() override { return now_ms; }
	MisterResult Run(linux_v2::TeardownAction action, uint32_t deadline_ms) override {
		events.push_back(action);
		budgets.push_back(deadline_ms);
		now_ms += action_delay_ms;
		return action == fail_action ? MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
	}
	MisterResult ObserveNeutral(linux_v2::TeardownAction action, uint32_t deadline_ms,
		bool* neutral) override {
		observation_budgets.push_back(deadline_ms);
		now_ms += observation_delay_ms;
		*neutral = action != unobserved_action;
		return MISTER_RESULT_OK;
	}
};

static const linux_v2::TeardownAction kExpected[] = {
	linux_v2::TeardownAction::scheduler,
	linux_v2::TeardownAction::offload,
	linux_v2::TeardownAction::input,
	linux_v2::TeardownAction::audio,
	linux_v2::TeardownAction::saves,
	linux_v2::TeardownAction::video,
	linux_v2::TeardownAction::content,
	linux_v2::TeardownAction::spi,
	linux_v2::TeardownAction::fpga_reset,
	linux_v2::TeardownAction::bridges,
	linux_v2::TeardownAction::mapping
};

int main() {
	FakeResources resources;
	linux_v2::ResourceTransaction transaction(MISTER_RESOURCE_V2_KNOWN);
	assert(transaction.acquired_mask() == MISTER_RESOURCE_V2_KNOWN);
	assert(transaction.neutral_mask() == 0);

	assert(transaction.Stop(resources, 2000) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(resources.events.size() == 7);
	assert(transaction.neutral_mask() == (MISTER_RESOURCE_NATIVE_VIDEO |
		MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_SAVES | MISTER_RESOURCE_CONTENT));
	assert(transaction.active_mask() == (MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL));
	for (size_t i = 0; i < resources.events.size(); ++i) {
		assert(resources.events[i] == kExpected[i]);
		assert(resources.budgets[i] == 2000 - i * 10);
	}

	assert(transaction.Stop(resources, 5000) == MISTER_RESULT_OK);
	assert(resources.events.size() == sizeof(kExpected) / sizeof(kExpected[0]));
	for (size_t i = 0; i < resources.events.size(); ++i) assert(resources.events[i] == kExpected[i]);
	assert(resources.budgets[7] == 4930);
	assert(resources.budgets[8] == 4920);
	assert(resources.budgets[9] == 4910);
	assert(resources.budgets[10] == 4900);
	assert(transaction.active_mask() == 0);
	assert(transaction.neutral_mask() == MISTER_RESOURCE_V2_KNOWN);
	assert(transaction.Stop(resources, 5000) == MISTER_RESULT_OK);
	assert(resources.events.size() == sizeof(kExpected) / sizeof(kExpected[0]));

	FakeResources retry_resources;
	retry_resources.fail_action = linux_v2::TeardownAction::audio;
	linux_v2::ResourceTransaction retry(MISTER_RESOURCE_V2_KNOWN);
	assert(retry.Stop(retry_resources, 2000) == MISTER_RESULT_DEADLINE);
	assert(retry_resources.events.back() == linux_v2::TeardownAction::audio);
	retry_resources.fail_action = linux_v2::TeardownAction::none;
	assert(retry.Stop(retry_resources, 2000) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(retry_resources.events[4] == linux_v2::TeardownAction::audio);
	assert(retry_resources.budgets[4] == 1960);
	assert(retry.Stop(retry_resources, 5000) == MISTER_RESULT_OK);

	FakeResources expired_resources;
	linux_v2::ResourceTransaction expired(MISTER_RESOURCE_V2_KNOWN);
	assert(expired.Stop(expired_resources, 5) == MISTER_RESULT_DEADLINE);
	assert(expired_resources.events.size() == 1);
	assert(expired.neutral_mask() == 0);

	FakeResources unobserved_resources;
	unobserved_resources.unobserved_action = linux_v2::TeardownAction::input;
	linux_v2::ResourceTransaction unobserved(MISTER_RESOURCE_CORE_INPUT);
	assert(unobserved.Stop(unobserved_resources, 2000) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(unobserved.active_mask() == MISTER_RESOURCE_CORE_INPUT);
	assert(unobserved_resources.events.size() == 1);
	assert(unobserved_resources.events[0] == linux_v2::TeardownAction::input);
	unobserved_resources.unobserved_action = linux_v2::TeardownAction::none;
	assert(unobserved.Stop(unobserved_resources, 2000) == MISTER_RESULT_OK);
	assert(unobserved_resources.events.size() == 2);

	FakeResources content_resources;
	linux_v2::ResourceTransaction content_only(MISTER_RESOURCE_CONTENT);
	assert(content_only.Stop(content_resources, 2000) == MISTER_RESULT_OK);
	assert(content_resources.events.size() == 1);
	assert(content_resources.events[0] == linux_v2::TeardownAction::content);

	FakeResources zero_clock_resources;
	zero_clock_resources.now_ms = 0;
	zero_clock_resources.fail_action = linux_v2::TeardownAction::input;
	linux_v2::ResourceTransaction zero_clock(MISTER_RESOURCE_CORE_INPUT);
	assert(zero_clock.Stop(zero_clock_resources, 2000) == MISTER_RESULT_DEADLINE);
	zero_clock_resources.now_ms = 1000;
	zero_clock_resources.fail_action = linux_v2::TeardownAction::none;
	assert(zero_clock.Stop(zero_clock_resources, 2000) == MISTER_RESULT_OK);
	assert(zero_clock_resources.budgets.back() == 1000);

	FakeResources supporting_resources;
	linux_v2::ResourceTransaction supporting_only(0);
	assert(supporting_only.AcquireSupporting(linux_v2::TeardownAction::scheduler) ==
		MISTER_RESULT_OK);
	assert(supporting_only.AcquireSupporting(linux_v2::TeardownAction::input) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(supporting_only.Stop(supporting_resources, 2000) == MISTER_RESULT_OK);
	assert(supporting_resources.events.size() == 1);
	assert(supporting_resources.events[0] == linux_v2::TeardownAction::scheduler);
	assert(supporting_only.AcquireSupporting(linux_v2::TeardownAction::scheduler) ==
		MISTER_RESULT_INVALID_STATE);

	FakeResources short_observation_overrun;
	short_observation_overrun.observation_delay_ms = 1990;
	linux_v2::ResourceTransaction short_boundary(MISTER_RESOURCE_CONTENT);
	assert(short_boundary.Stop(short_observation_overrun, 2000) == MISTER_RESULT_DEADLINE);
	assert(short_boundary.neutral_mask() == 0);
	assert(short_observation_overrun.observation_budgets.size() == 1);
	assert(short_observation_overrun.observation_budgets[0] == 1990);

	FakeResources mapping_resources;
	linux_v2::ResourceTransaction mapping_only(0);
	assert(mapping_only.AcquireSupporting(linux_v2::TeardownAction::mapping) ==
		MISTER_RESULT_OK);
	assert(mapping_only.acquired_mask() == 0);
	assert(mapping_only.Stop(mapping_resources, 2000) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(mapping_resources.events.empty());
	mapping_resources.observation_delay_ms = 4990;
	assert(mapping_only.Stop(mapping_resources, 5000) == MISTER_RESULT_DEADLINE);
	assert(mapping_resources.events.size() == 1);
	assert(mapping_resources.events[0] == linux_v2::TeardownAction::mapping);
	assert(mapping_resources.observation_budgets[0] == 4990);
	const size_t expired_events = mapping_resources.events.size();
	const size_t expired_observations = mapping_resources.observation_budgets.size();
	assert(mapping_resources.now_ms == 5100);
	assert(mapping_only.Stop(mapping_resources, 5000) == MISTER_RESULT_DEADLINE);
	assert(mapping_resources.now_ms == 5100);
	assert(mapping_resources.events.size() == expired_events);
	assert(mapping_resources.observation_budgets.size() == expired_observations);

	FakeResources mapping_retry_resources;
	linux_v2::ResourceTransaction mapping_retry(0);
	assert(mapping_retry.AcquireSupporting(linux_v2::TeardownAction::mapping) ==
		MISTER_RESULT_OK);
	assert(mapping_retry.Stop(mapping_retry_resources, 2000) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	mapping_retry_resources.fail_action = linux_v2::TeardownAction::mapping;
	assert(mapping_retry.Stop(mapping_retry_resources, 5000) == MISTER_RESULT_DEADLINE);
	assert(mapping_retry_resources.now_ms == 110);
	assert(mapping_retry_resources.events.size() == 1);
	assert(mapping_retry_resources.observation_budgets.empty());
	mapping_retry_resources.fail_action = linux_v2::TeardownAction::none;
	assert(mapping_retry.Stop(mapping_retry_resources, 5000) == MISTER_RESULT_OK);
	assert(mapping_retry_resources.now_ms == 120);
	assert(mapping_retry_resources.events.size() == 2);
	assert(mapping_retry_resources.observation_budgets.size() == 1);

	FakeResources short_action_overrun;
	short_action_overrun.action_delay_ms = 2000;
	linux_v2::ResourceTransaction short_action_boundary(MISTER_RESOURCE_CONTENT);
	assert(short_action_boundary.Stop(short_action_overrun, 2000) == MISTER_RESULT_DEADLINE);
	assert(short_action_boundary.neutral_mask() == 0);
	assert(short_action_overrun.observation_budgets.empty());

	FakeResources mapping_action_overrun;
	linux_v2::ResourceTransaction mapping_action_boundary(0);
	assert(mapping_action_boundary.AcquireSupporting(linux_v2::TeardownAction::mapping) ==
		MISTER_RESULT_OK);
	assert(mapping_action_boundary.Stop(mapping_action_overrun, 2000) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	mapping_action_overrun.action_delay_ms = 5000;
	assert(mapping_action_boundary.Stop(mapping_action_overrun, 5000) ==
		MISTER_RESULT_DEADLINE);
	assert(mapping_action_overrun.observation_budgets.empty());
	return 0;
}
