// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FOGCAST_RUNTIME_PLATFORM_FACTORY_HPP
#define FOGCAST_RUNTIME_PLATFORM_FACTORY_HPP

#include "runtime/mister_runtime.h"

namespace fogcast {

struct RuntimePlatformBinding {
	const MisterPlatformV2* abi_v2;
	void* observation_context;
	bool (*main_absent)(void*);
};

// Each executable links exactly one strong provider.
const RuntimePlatformBinding* RuntimePlatform();

}  // namespace fogcast

#endif
