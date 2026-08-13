// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_core_protocol.hpp"
#include "runtime/native/linux/native_core_protocol_io_adapter.hpp"

#include <type_traits>

namespace mister {
namespace native {
namespace {

template <typename...> struct Void { typedef void type; };
template <typename... Types> using VoidT = typename Void<Types...>::type;

template <typename Type, typename = void> struct HasProbe : std::false_type {};
template <typename Type> struct HasProbe<Type,
	VoidT<decltype(&Type::Probe)> > : std::true_type {};

template <typename Type, typename = void> struct HasExchange : std::false_type {};
template <typename Type> struct HasExchange<Type,
	VoidT<decltype(&Type::Exchange)> > : std::true_type {};

template <typename Type, typename = void> struct HasCloseSelected : std::false_type {};
template <typename Type> struct HasCloseSelected<Type,
	VoidT<decltype(&Type::CloseSelected)> > : std::true_type {};

template <typename Type, typename = void> struct HasBeginActive : std::false_type {};
template <typename Type> struct HasBeginActive<Type,
	VoidT<decltype(&Type::BeginActive)> > : std::true_type {};

template <typename Type, typename = void> struct HasBeginCleanup : std::false_type {};
template <typename Type> struct HasBeginCleanup<Type,
	VoidT<decltype(&Type::BeginCleanup)> > : std::true_type {};

template <typename Type, typename = void> struct HasBeginRecovery : std::false_type {};
template <typename Type> struct HasBeginRecovery<Type,
	VoidT<decltype(&Type::BeginRecovery)> > : std::true_type {};

static_assert(HasProbe<NativeActiveCoreProtocolIo>::value,
	"active capability must expose probing");
static_assert(HasExchange<NativeActiveCoreProtocolIo>::value,
	"active capability must expose word exchange");
static_assert(HasCloseSelected<NativeActiveCoreProtocolIo>::value,
	"active capability must expose selected-transaction closure");
static_assert(HasBeginActive<NativeActiveCoreProtocolIo>::value,
	"active capability must begin only active sessions");
static_assert(!HasBeginCleanup<NativeActiveCoreProtocolIo>::value &&
	!HasBeginRecovery<NativeActiveCoreProtocolIo>::value,
	"active capability must not convert to cleanup or recovery authority");

static_assert(HasBeginCleanup<NativeCleanupCoreProtocolIo>::value,
	"cleanup capability must begin cleanup sessions");
static_assert(!HasProbe<NativeCleanupCoreProtocolIo>::value &&
	!HasExchange<NativeCleanupCoreProtocolIo>::value &&
	!HasCloseSelected<NativeCleanupCoreProtocolIo>::value &&
	!HasBeginActive<NativeCleanupCoreProtocolIo>::value &&
	!HasBeginRecovery<NativeCleanupCoreProtocolIo>::value,
	"cleanup capability must not name active/recovery actions");

static_assert(HasBeginRecovery<NativeRecoveryCoreProtocolIo>::value,
	"recovery capability must begin recovery sessions");
static_assert(!HasProbe<NativeRecoveryCoreProtocolIo>::value &&
	!HasExchange<NativeRecoveryCoreProtocolIo>::value &&
	!HasCloseSelected<NativeRecoveryCoreProtocolIo>::value &&
	!HasBeginActive<NativeRecoveryCoreProtocolIo>::value &&
	!HasBeginCleanup<NativeRecoveryCoreProtocolIo>::value,
	"recovery capability must not name active/cleanup actions");

static_assert(!std::is_convertible<NativeActiveCoreProtocolIo *,
	NativeCleanupCoreProtocolIo *>::value &&
	!std::is_convertible<NativeActiveCoreProtocolIo *,
	NativeRecoveryCoreProtocolIo *>::value &&
	!std::is_convertible<NativeCleanupCoreProtocolIo *,
	NativeActiveCoreProtocolIo *>::value &&
	!std::is_convertible<NativeRecoveryCoreProtocolIo *,
	NativeActiveCoreProtocolIo *>::value,
	"core protocol capability types must be non-convertible");

static_assert(!std::is_convertible<linux_native::NativeCoreProtocolIoAdapter *,
	NativeActiveCoreProtocolIo *>::value &&
	!std::is_convertible<linux_native::NativeCoreProtocolIoAdapter *,
	NativeCleanupCoreProtocolIo *>::value &&
	!std::is_convertible<linux_native::NativeCoreProtocolIoAdapter *,
	NativeRecoveryCoreProtocolIo *>::value,
	"the adapter container must not itself become an authority capability");

static_assert(!std::is_convertible<NativeCoreProtocolCapabilities *,
	NativeActiveCoreProtocolIo *>::value &&
	!std::is_convertible<NativeCoreProtocolCapabilities *,
	NativeCleanupCoreProtocolIo *>::value &&
	!std::is_convertible<NativeCoreProtocolCapabilities *,
	NativeRecoveryCoreProtocolIo *>::value &&
	!std::is_convertible<NativeCoreProtocolCapabilities *,
	NativeCoreProtocolExitIo *>::value,
	"the cohesive bundle must not expose a phase capability");

static_assert(!std::is_convertible<linux_native::NativeCoreProtocolIoAdapter *,
	NativeCoreProtocolCapabilities *>::value,
	"the adapter container must vend, not become, its cohesive bundle");

static_assert(!std::is_copy_constructible<NativeCoreProtocolCapabilities>::value &&
	!std::is_copy_assignable<NativeCoreProtocolCapabilities>::value &&
	std::is_move_constructible<NativeCoreProtocolCapabilities>::value &&
	std::is_move_assignable<NativeCoreProtocolCapabilities>::value,
	"the cohesive bundle must be a move-only single-consumer token");

static_assert(!std::is_constructible<NativeCoreProtocol, NativeClock &,
	HardwareBroker &, NativeActiveCoreProtocolIo &, NativeCleanupCoreProtocolIo &,
	NativeRecoveryCoreProtocolIo &, NativeCoreProtocolExitIo &>::value,
	"active A with teardown B must never construct a protocol");

static_assert(!std::is_constructible<NativeCoreProtocol, NativeClock &,
	HardwareBroker &, NativeActiveCoreProtocolIo &, NativeCleanupCoreProtocolIo &,
	NativeRecoveryCoreProtocolIo &, NativeCoreProtocolExitIo &>::value,
	"active B with teardown A must never construct a protocol");

static_assert(!std::is_constructible<NativeCoreProtocol, NativeClock &,
	HardwareBroker &, NativeCoreProtocolCapabilities &>::value &&
	!std::is_constructible<NativeCoreProtocol, NativeClock &,
	HardwareBroker &, const NativeCoreProtocolCapabilities &>::value &&
	std::is_constructible<NativeCoreProtocol, NativeClock &, HardwareBroker &,
		NativeCoreProtocolCapabilities &&>::value,
	"only a moved cohesive bundle may construct one protocol");

#if defined(MISTER_NATIVE_CORE_PROTOCOL_FIXTURE_CONSTRUCTOR_REACHABILITY_PROBE)
void FixtureConstructorMustBeUnavailableInProduction(NativeClock &clock,
	NativeActiveCoreProtocolIo &active_io)
{
	NativeCoreProtocol protocol(clock, active_io);
}
#endif

} // namespace
} // namespace native
} // namespace mister

int main()
{
	return 0;
}
