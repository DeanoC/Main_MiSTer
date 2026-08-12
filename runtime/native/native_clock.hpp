// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_CLOCK_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_CLOCK_HPP

#include <stdint.h>

#include <condition_variable>
#include <mutex>

namespace mister {
namespace native {

class NativeClock {
public:
	virtual ~NativeClock() {}
	virtual uint64_t NowMs() const = 0;

	// Wait until the condition is notified or the injected monotonic deadline
	// expires. False means the absolute deadline was reached.
	virtual bool WaitUntil(std::condition_variable &condition,
		std::unique_lock<std::mutex> &lock,
		uint64_t absolute_deadline_ms) = 0;
};

} // namespace native
} // namespace mister

#endif
