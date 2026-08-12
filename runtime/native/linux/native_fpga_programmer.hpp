// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_FPGA_PROGRAMMER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_FPGA_PROGRAMMER_HPP

#include "runtime/native/hardware_broker.hpp"
#include "runtime/native/linux/native_core_artifact_adapter.hpp"
#include "runtime/native/native_clock.hpp"

namespace mister {
namespace native {
namespace linux_native {

class NativeFpgaByteSink {
public:
	virtual ~NativeFpgaByteSink() {}
	virtual Result Write(const unsigned char *bytes, size_t count,
		uint64_t absolute_deadline_ms, size_t *accepted) = 0;
};

struct NativeFpgaProgrammingReceipt {
	Result result;
	uint64_t accepted_bytes;
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
