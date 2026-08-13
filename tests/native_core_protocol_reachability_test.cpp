// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MISTER_NATIVE_INPUT_FILE_IO_REACHABILITY_PROBE) || \
	defined(MISTER_NATIVE_INPUT_GENERIC_SPI_REACHABILITY_PROBE)
#define private public
#include "runtime/native/native_input.hpp"
#undef private

namespace mister {
namespace native {

Result AttemptForbiddenInputSpiRoute(NativeInput &input,
	HardwareLeaseView &view, const SpiWords &words, SpiReceipt *receipt)
{
	return input.bus_.ExchangeWithHardwareLeaseView(view,
#if defined(MISTER_NATIVE_INPUT_FILE_IO_REACHABILITY_PROBE)
		NativeSpiTarget::file_io,
#else
		NativeSpiTarget::user_io,
#endif
		words, receipt, nullptr);
}

} // namespace native
} // namespace mister

int main() { return 0; }
#else

#include "runtime/native/native_core_protocol.hpp"
#include "runtime/native/native_input.hpp"

#include <type_traits>

namespace mister {
namespace native {

static_assert(!std::is_default_constructible<ActiveCoreProtocolSession>::value,
	"active protocol authority must be broker minted");
static_assert(!std::is_default_constructible<CleanupCoreProtocolSession>::value,
	"cleanup protocol authority must be broker minted");
static_assert(!std::is_default_constructible<RecoveryCoreProtocolSession>::value,
	"recovery protocol authority must be broker minted");
static_assert(!std::is_convertible<ActiveCoreProtocolSession *,
	CleanupCoreProtocolSession *>::value,
	"active protocol authority must not convert to cleanup authority");
static_assert(!std::is_convertible<CleanupCoreProtocolSession *,
	RecoveryCoreProtocolSession *>::value,
	"cleanup protocol authority must not convert to recovery authority");
static_assert(!std::is_constructible<NativeInput, NativeSpiBus &>::value,
	"NativeInput must not retain a generic SPI bus capability");

} // namespace native
} // namespace mister

int main()
{
	return 0;
}
#endif
