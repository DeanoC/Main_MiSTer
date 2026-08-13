// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_FPGA_PROGRAMMER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_FPGA_PROGRAMMER_HPP

#include "runtime/native/hardware_broker.hpp"
#include "runtime/native/linux/native_core_artifact_adapter.hpp"
#include "runtime/native/native_clock.hpp"

#include <memory>

namespace mister {
namespace native {
namespace linux_native {

struct NativeFpgaSinkStartOutcome {
	Result result;
	bool acquired;
	bool mutation_attempted;
	bool mutation_applied;
};

struct NativeFpgaSinkWriteOutcome {
	Result result;
	size_t accepted_bytes;
	bool mutation_attempted;
	bool mutation_applied;
};

struct NativeFpgaSinkFinishOutcome {
	Result result;
	bool mutation_attempted;
	bool mutation_applied;
	bool configuration_done_observed;
	bool initialization_observed;
	bool user_mode_observed;
	bool manager_drive_released;
};

class NativeFpgaProgramSession {
public:
	virtual ~NativeFpgaProgramSession() {}
	NativeFpgaProgramSession(const NativeFpgaProgramSession &) = delete;
	NativeFpgaProgramSession &operator=(const NativeFpgaProgramSession &) = delete;

protected:
	NativeFpgaProgramSession() {}

private:
	friend class NativeFpgaProgrammer;
	virtual NativeFpgaSinkWriteOutcome Write(const unsigned char *bytes,
		size_t count, uint64_t absolute_deadline_ms) = 0;
	virtual NativeFpgaSinkFinishOutcome Finish(
		uint64_t absolute_deadline_ms) = 0;
};

class NativeFpgaByteSink {
public:
	virtual ~NativeFpgaByteSink() {}

private:
	friend class NativeFpgaProgrammer;
	virtual NativeFpgaSinkStartOutcome Begin(uint64_t expected_bytes,
		uint64_t absolute_deadline_ms,
		std::unique_ptr<NativeFpgaProgramSession> *session) = 0;
};

struct NativeFpgaProgrammingReceipt {
	Result result;
	bool acquired;
	uint64_t accepted_bytes;
	bool configuration_done_observed;
	bool initialization_observed;
	bool user_mode_observed;
	bool manager_drive_released;
	uint64_t mutation_sequence;
};

class NativeFpgaProgrammer final {
public:
	NativeFpgaProgrammer(HardwareBroker &broker, NativeClock &clock,
		NativeFpgaByteSink &sink);
	NativeFpgaProgrammingReceipt Program(const OperationLease &lease,
		const NativeCoreArtifactHandle &artifact);

private:
	HardwareBroker &broker_;
	NativeClock &clock_;
	NativeFpgaByteSink &sink_;
};

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
