// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_input_adapter.hpp"

#include <fcntl.h>
#include <errno.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <dirent.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

namespace mister {
namespace native {
namespace linux_native {

namespace {

constexpr size_t kDeviceCapacity = 8;
constexpr size_t kReadBatchCapacity = 16;

struct InputCandidate {
	uint64_t identity;
	char path[128];
};

#if defined(__linux__) || defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
struct InputSourceProbe {
	bool character_device;
	bool identity_available;
	uint16_t bus_type;
	bool name_identifies_uinput;
	bool topology_available;
	bool virtual_topology;
	size_t recognized_gamepad_buttons;
};

bool IsKnownControllerBus(uint16_t bus_type)
{
	return bus_type == 0x02 || bus_type == 0x03 || bus_type == 0x05;
}

NativeLinuxInputCapabilities ApplyInputSourcePolicy(
	NativeLinuxInputCapabilities capabilities, const InputSourceProbe &probe)
{
	capabilities.digital_gamepad_buttons =
		probe.recognized_gamepad_buttons >= 2 ||
		(probe.recognized_gamepad_buttons != 0 && capabilities.digital_hat);
	capabilities.uinput = capabilities.uinput || probe.bus_type == 0x06 ||
		probe.name_identifies_uinput || probe.virtual_topology;
	capabilities.fifo_or_signal = capabilities.fifo_or_signal ||
		!probe.character_device;
	capabilities.unknown = capabilities.unknown || !probe.identity_available ||
		!probe.topology_available || !IsKnownControllerBus(probe.bus_type);
	return capabilities;
}
#endif

#if defined(__linux__)
bool NameIdentifiesUinput(const char *name)
{
	if (name == nullptr) return false;
	constexpr char needle[] = "uinput";
	for (size_t start = 0; name[start] != '\0'; ++start) {
		size_t offset = 0;
		for (; needle[offset] != '\0' && name[start + offset] != '\0'; ++offset) {
			char value = name[start + offset];
			if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + 32);
			if (value != needle[offset]) break;
		}
		if (needle[offset] == '\0') return true;
	}
	return false;
}
#endif

class InputOperations {
public:
	virtual ~InputOperations() {}
	virtual Result OpenDirectory(uint64_t deadline, int *descriptor) = 0;
	virtual Result OpenWatch(uint64_t deadline, int *descriptor) = 0;
	virtual Result AddWatch(int descriptor, uint64_t deadline, int *watch) = 0;
	virtual Result DiscoverDevice(size_t index, InputCandidate *candidate) = 0;
	virtual Result OpenDevice(const InputCandidate &candidate, uint64_t deadline,
		int *descriptor) = 0;
	virtual Result QueryCapabilities(int descriptor,
		NativeLinuxInputCapabilities *capabilities) = 0;
	virtual Result SetGrab(int descriptor, bool grab) = 0;
	virtual Result ReadEvents(int descriptor, NativeLinuxInputEvent *events,
		size_t capacity, size_t *count, bool *partial) = 0;
	virtual Result RemoveWatch(int descriptor, int watch) = 0;
	virtual Result Close(int descriptor) = 0;
	virtual uint64_t NowMs() const = 0;
};

class PosixInputOperations final : public InputOperations {
public:
	explicit PosixInputOperations(NativeClock &clock)
		: clock_(clock)
#if defined(__linux__)
		, directory_(nullptr), directory_descriptor_(-1), next_identity_(1)
#endif
	{}
	~PosixInputOperations() override {}

	Result OpenDirectory(uint64_t deadline, int *descriptor) override
	{
		if (descriptor == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		if (NowMs() >= deadline) return MISTER_RESULT_DEADLINE;
#if defined(__linux__)
		directory_ = opendir("/dev/input");
		if (directory_ == nullptr) return MISTER_RESULT_PLATFORM;
		directory_descriptor_ = dirfd(directory_);
		if (directory_descriptor_ < 0) {
			closedir(directory_); directory_ = nullptr;
			return MISTER_RESULT_PLATFORM;
		}
		*descriptor = directory_descriptor_;
		return MISTER_RESULT_OK;
#else
		(void)descriptor;
		return MISTER_RESULT_UNSUPPORTED;
#endif
	}

	Result OpenWatch(uint64_t deadline, int *descriptor) override
	{
		if (descriptor == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		if (NowMs() >= deadline) return MISTER_RESULT_DEADLINE;
#if defined(__linux__)
		const int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
		if (fd < 0) return MISTER_RESULT_PLATFORM;
		*descriptor = fd;
		return MISTER_RESULT_OK;
#else
		(void)descriptor;
		return MISTER_RESULT_UNSUPPORTED;
#endif
	}

	Result AddWatch(int descriptor, uint64_t deadline, int *watch) override
	{
		if (watch == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		if (NowMs() >= deadline) return MISTER_RESULT_DEADLINE;
#if defined(__linux__)
		const int value = inotify_add_watch(descriptor, "/dev/input",
			IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
		if (value < 0) return MISTER_RESULT_PLATFORM;
		*watch = value;
		return MISTER_RESULT_OK;
#else
		(void)descriptor;
		return MISTER_RESULT_UNSUPPORTED;
#endif
	}

	Result DiscoverDevice(size_t index, InputCandidate *candidate) override
	{
		if (candidate == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
#if defined(__linux__)
		if (directory_ == nullptr) return MISTER_RESULT_INVALID_STATE;
		if (index == 0) rewinddir(directory_);
		for (;;) {
			dirent *entry = readdir(directory_);
			if (entry == nullptr) return MISTER_RESULT_UNSUPPORTED;
			if (strncmp(entry->d_name, "event", 5) != 0) continue;
			const int count = snprintf(candidate->path, sizeof(candidate->path),
				"/dev/input/%s", entry->d_name);
			if (count <= 0 || static_cast<size_t>(count) >= sizeof(candidate->path))
				return MISTER_RESULT_PLATFORM;
			candidate->identity = next_identity_++;
			return MISTER_RESULT_OK;
		}
#else
		(void)index;
		return MISTER_RESULT_UNSUPPORTED;
#endif
	}

	Result OpenDevice(const InputCandidate &candidate, uint64_t deadline,
		int *descriptor) override
	{
		if (descriptor == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		if (NowMs() >= deadline) return MISTER_RESULT_DEADLINE;
#if defined(__linux__)
		const int fd = open(candidate.path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) return MISTER_RESULT_PLATFORM;
		*descriptor = fd;
		return MISTER_RESULT_OK;
#else
		(void)candidate;
		return MISTER_RESULT_UNSUPPORTED;
#endif
	}

	Result QueryCapabilities(int descriptor,
		NativeLinuxInputCapabilities *caps) override
	{
		if (caps == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		*caps = {};
#if defined(__linux__)
		unsigned long event_bits[(EV_MAX + sizeof(unsigned long) * 8) /
			sizeof(unsigned long) / 8] = {};
		unsigned long key_bits[(KEY_MAX + sizeof(unsigned long) * 8) /
			sizeof(unsigned long) / 8] = {};
		unsigned long abs_bits[(ABS_MAX + sizeof(unsigned long) * 8) /
			sizeof(unsigned long) / 8] = {};
		unsigned long rel_bits[(REL_MAX + sizeof(unsigned long) * 8) /
			sizeof(unsigned long) / 8] = {};
		if (ioctl(descriptor, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0 ||
			ioctl(descriptor, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 ||
			ioctl(descriptor, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0 ||
			ioctl(descriptor, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) < 0)
			return MISTER_RESULT_PLATFORM;
		auto bit = [](const unsigned long *bits, size_t value) {
			const size_t width = sizeof(unsigned long) * 8;
			return (bits[value / width] & (1UL << (value % width))) != 0;
		};
		const uint16_t gamepad_buttons[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH,
			BTN_WEST, BTN_SELECT, BTN_START, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
			BTN_THUMBL, BTN_THUMBR, BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT,
			BTN_DPAD_RIGHT};
		size_t recognized_gamepad_buttons = 0;
		for (uint16_t code : gamepad_buttons)
			recognized_gamepad_buttons += bit(key_bits, code) ? 1u : 0u;
		caps->digital_hat = bit(abs_bits, ABS_HAT0X) && bit(abs_bits, ABS_HAT0Y);
		caps->keyboard = bit(key_bits, KEY_A) || bit(key_bits, KEY_ENTER);
		caps->mouse = bit(key_bits, BTN_MOUSE);
		caps->analog_absolute = bit(abs_bits, ABS_X) || bit(abs_bits, ABS_Y) ||
			bit(abs_bits, ABS_RX) || bit(abs_bits, ABS_RY);
		caps->touch = bit(key_bits, BTN_TOUCH);
		caps->wheel = bit(rel_bits, REL_WHEEL) || bit(rel_bits, REL_HWHEEL);
		caps->rumble_or_output = bit(event_bits, EV_FF) ||
			bit(event_bits, EV_SND);
		caps->bluetooth_admin = bit(key_bits, KEY_BLUETOOTH);
		caps->led = bit(event_bits, EV_LED);
		struct stat descriptor_status = {};
		const bool status_available = fstat(descriptor, &descriptor_status) == 0;
		const bool character_device = status_available &&
			S_ISCHR(descriptor_status.st_mode);
		input_id source_id = {};
		const bool identity_available =
			ioctl(descriptor, EVIOCGID, &source_id) == 0;
		char name[128] = {};
		(void)ioctl(descriptor, EVIOCGNAME(sizeof(name)), name);
		char topology_path[64] = {};
		char topology[256] = {};
		bool topology_available = false;
		bool virtual_topology = false;
		if (status_available) {
			const int length = snprintf(topology_path, sizeof(topology_path),
				"/sys/dev/char/%u:%u", major(descriptor_status.st_rdev),
				minor(descriptor_status.st_rdev));
			if (length > 0 && static_cast<size_t>(length) < sizeof(topology_path)) {
				const ssize_t bytes = readlink(topology_path, topology,
					sizeof(topology) - 1);
				if (bytes > 0) {
					topology[bytes] = '\0';
					topology_available = true;
					virtual_topology = strstr(topology, "/virtual/") != nullptr;
				}
			}
		}
		*caps = ApplyInputSourcePolicy(*caps, {character_device,
			identity_available, source_id.bustype, NameIdentifiesUinput(name),
			topology_available, virtual_topology, recognized_gamepad_buttons});
		return MISTER_RESULT_OK;
#else
		(void)descriptor;
		return MISTER_RESULT_UNSUPPORTED;
#endif
	}

	Result SetGrab(int descriptor, bool grab) override
	{
#if defined(__linux__)
		return ioctl(descriptor, EVIOCGRAB, grab ? 1 : 0) == 0 ?
			MISTER_RESULT_OK : MISTER_RESULT_PLATFORM;
#else
		(void)descriptor; (void)grab;
		return MISTER_RESULT_UNSUPPORTED;
#endif
	}

	Result ReadEvents(int descriptor, NativeLinuxInputEvent *events,
		size_t capacity, size_t *count, bool *partial) override
	{
		if (events == nullptr || count == nullptr || partial == nullptr ||
			capacity == 0) return MISTER_RESULT_INVALID_ARGUMENT;
		*count = 0; *partial = false;
#if defined(__linux__)
		input_event raw[kReadBatchCapacity] = {};
		const size_t bounded = capacity < kReadBatchCapacity ? capacity :
			kReadBatchCapacity;
		const ssize_t bytes = read(descriptor, raw, bounded * sizeof(raw[0]));
		if (bytes < 0) return errno == EAGAIN ? MISTER_RESULT_OK :
			MISTER_RESULT_PLATFORM;
		*partial = (static_cast<size_t>(bytes) % sizeof(raw[0])) != 0;
		*count = static_cast<size_t>(bytes) / sizeof(raw[0]);
		for (size_t i = 0; i < *count; ++i) {
			events[i] = {NativeLinuxInputEventKind::unsupported, raw[i].code,
				raw[i].value, 0};
			if (raw[i].type == EV_KEY) {
				uint16_t code = UINT16_MAX;
				switch (raw[i].code) {
				case BTN_SOUTH: code = NativeLinuxInputCode::south; break;
				case BTN_EAST: code = NativeLinuxInputCode::east; break;
				case BTN_SELECT: code = NativeLinuxInputCode::select; break;
				case BTN_START: code = NativeLinuxInputCode::start; break;
				case BTN_DPAD_UP: code = NativeLinuxInputCode::up; break;
				case BTN_DPAD_DOWN: code = NativeLinuxInputCode::down; break;
				case BTN_DPAD_LEFT: code = NativeLinuxInputCode::left; break;
				case BTN_DPAD_RIGHT: code = NativeLinuxInputCode::right; break;
				case BTN_TL: code = NativeLinuxInputCode::left_shoulder; break;
				case BTN_TR: code = NativeLinuxInputCode::right_shoulder; break;
				case BTN_NORTH: code = NativeLinuxInputCode::north; break;
				case BTN_WEST: code = NativeLinuxInputCode::west; break;
				case BTN_TL2: code = NativeLinuxInputCode::left_trigger; break;
				case BTN_TR2: code = NativeLinuxInputCode::right_trigger; break;
				case BTN_THUMBL: code = NativeLinuxInputCode::left_thumb; break;
				case BTN_THUMBR: code = NativeLinuxInputCode::right_thumb; break;
				default: break;
				}
				if (code != UINT16_MAX) {
					events[i].kind = NativeLinuxInputEventKind::digital_button;
					events[i].code = code;
				}
			} else if (raw[i].type == EV_ABS && raw[i].code == ABS_HAT0X) {
				events[i].kind = NativeLinuxInputEventKind::digital_hat_x;
			} else if (raw[i].type == EV_ABS && raw[i].code == ABS_HAT0Y) {
				events[i].kind = NativeLinuxInputEventKind::digital_hat_y;
			} else if (raw[i].type == EV_SYN && raw[i].code == SYN_REPORT) {
				events[i].kind = NativeLinuxInputEventKind::syn_report;
			} else if (raw[i].type == EV_SYN && raw[i].code == SYN_DROPPED) {
				events[i].kind = NativeLinuxInputEventKind::syn_dropped;
			}
		}
		return MISTER_RESULT_OK;
#else
		(void)descriptor;
		return MISTER_RESULT_UNSUPPORTED;
#endif
	}

	Result RemoveWatch(int descriptor, int watch) override
	{
#if defined(__linux__)
		return inotify_rm_watch(descriptor, watch) == 0 ? MISTER_RESULT_OK :
			MISTER_RESULT_PLATFORM;
#else
		(void)descriptor; (void)watch;
		return MISTER_RESULT_UNSUPPORTED;
#endif
	}

	Result Close(int descriptor) override
	{
#if defined(__linux__)
		if (directory_ != nullptr && descriptor == directory_descriptor_) {
			const int result = closedir(directory_);
			directory_ = nullptr; directory_descriptor_ = -1;
			return result == 0 ? MISTER_RESULT_OK : MISTER_RESULT_PLATFORM;
		}
#endif
		return close(descriptor) == 0 ? MISTER_RESULT_OK : MISTER_RESULT_PLATFORM;
	}
	uint64_t NowMs() const override { return clock_.NowMs(); }
private:
	NativeClock &clock_;
#if defined(__linux__)
	DIR *directory_;
	int directory_descriptor_;
	uint64_t next_identity_;
#endif
};

#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
class InjectedInputOperations final : public InputOperations {
public:
	InjectedInputOperations() : operations_(nullptr) {}
	explicit InjectedInputOperations(NativeLinuxInputOperations &operations)
		: operations_(&operations) {}
	Result OpenDirectory(uint64_t d, int *fd) override
	{
		return operations_->OpenDirectory(d, fd);
	}
	Result OpenWatch(uint64_t d, int *fd) override
	{
		return operations_->OpenWatch(d, fd);
	}
	Result AddWatch(int fd, uint64_t d, int *watch) override
	{
		return operations_->AddWatch(fd, d, watch);
	}
	Result DiscoverDevice(size_t index, InputCandidate *candidate) override
	{
		NativeLinuxInputCandidate value = {};
		const Result result = operations_->DiscoverDevice(index, &value);
		if (result == MISTER_RESULT_OK) {
			candidate->identity = value.identity;
			memcpy(candidate->path, value.path, sizeof(candidate->path));
		}
		return result;
	}
	Result OpenDevice(const InputCandidate &candidate, uint64_t d,
		int *fd) override
	{
		NativeLinuxInputCandidate value = {};
		value.identity = candidate.identity;
		memcpy(value.path, candidate.path, sizeof(value.path));
		return operations_->OpenDevice(value, d, fd);
	}
	Result QueryCapabilities(int fd, NativeLinuxInputCapabilities *caps) override
	{
		return operations_->QueryCapabilities(fd, caps);
	}
	Result SetGrab(int fd, bool grab) override
	{
		return operations_->SetGrab(fd, grab);
	}
	Result ReadEvents(int fd, NativeLinuxInputEvent *events, size_t capacity,
		size_t *count, bool *partial) override
	{
		return operations_->ReadEvents(fd, events, capacity, count, partial);
	}
	Result RemoveWatch(int fd, int watch) override
	{
		return operations_->RemoveWatch(fd, watch);
	}
	Result Close(int fd) override { return operations_->Close(fd); }
	uint64_t NowMs() const override { return operations_->NowMs(); }
private:
	NativeLinuxInputOperations *operations_;
};
#endif

bool IsDigitalOnly(const NativeLinuxInputCapabilities &caps)
{
	const bool positive = caps.digital_gamepad_buttons || caps.digital_hat;
	const bool denied = caps.keyboard || caps.mouse || caps.analog_absolute ||
		caps.touch || caps.wheel || caps.rumble_or_output ||
		caps.bluetooth_admin || caps.uinput || caps.fifo_or_signal || caps.led ||
		caps.unknown;
	return positive && !denied;
}

} // namespace

#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
NativeLinuxInputCapabilities ClassifyNativeLinuxInputSourceForTest(
	NativeLinuxInputCapabilities capabilities,
	const NativeLinuxInputSourceProbe &probe)
{
	return ApplyInputSourcePolicy(capabilities, {probe.character_device,
		probe.identity_available, probe.bus_type, probe.name_identifies_uinput,
		probe.topology_available, probe.virtual_topology,
		probe.recognized_gamepad_buttons});
}
#endif

class NativeInputAdapter::Impl {
public:
	struct Device {
		int descriptor;
		uint64_t identity;
		uint8_t player;
		bool opened;
		bool admitted;
		bool grabbed;
		uint16_t button_map;
		uint16_t hat_map;
	};
	explicit Impl(NativeClock &clock)
		: local(clock),
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
		  injected(),
#endif
		  operations(&local), mutex(), directory(-1), watch_descriptor(-1),
		  watch(-1), watch_registered(false), devices{}, opening(false),
		  open_complete(false), cleanup_started(false), polling(false),
		  uncertain{false, false}, sequence{0, 0} {}
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
	Impl(NativeClock &clock, NativeLinuxInputOperations &ops)
		: local(clock), injected(ops), operations(&injected), mutex(),
		  directory(-1), watch_descriptor(-1), watch(-1),
		  watch_registered(false), devices{}, opening(false),
		  open_complete(false), cleanup_started(false), polling(false),
		  uncertain{false, false}, sequence{0, 0} {}
#endif
	PosixInputOperations local;
#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
	InjectedInputOperations injected;
#endif
	InputOperations *operations;
	mutable std::mutex mutex;
	int directory;
	int watch_descriptor;
	int watch;
	bool watch_registered;
	Device devices[kDeviceCapacity];
	bool opening;
	bool open_complete;
	bool cleanup_started;
	bool polling;
	bool uncertain[kNativePlayerCount];
	uint64_t sequence[kNativePlayerCount];
};

NativeInputAdapter::NativeInputAdapter(HardwareBroker &broker,
	NativeClock &clock, const NativeCoreProfile &profile, NativeInput &input)
	: broker_(broker), profile_(profile), input_(input),
	  impl_(new (std::nothrow) Impl(clock))
{
}

#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
NativeInputAdapter::NativeInputAdapter(HardwareBroker &broker,
	NativeClock &clock, const NativeCoreProfile &profile, NativeInput &input,
	NativeLinuxInputOperations &operations)
	: broker_(broker), profile_(profile), input_(input),
	  impl_(new (std::nothrow) Impl(clock, operations))
{
}
#endif

NativeInputAdapter::~NativeInputAdapter()
{
	CloseInputDescriptorsForProcessExit();
	delete impl_;
}

NativeAcquisitionOutcome NativeInputAdapter::OpenInputDescriptors(
	const OperationLease &lease)
{
	if (impl_ == nullptr) return {MISTER_RESULT_PLATFORM, false};
	std::unique_ptr<ProcessOperationGuard> guard;
	Result result = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::input_descriptors, &profile_, &guard);
	if (result != MISTER_RESULT_OK) return {result, false};
	if (guard->authority() != LeaseAuthority::active_generation)
		return {MISTER_RESULT_INVALID_STATE, false};
	std::lock_guard<std::mutex> lock(impl_->mutex);
	auto acquired = [this]() {
		if (impl_->directory >= 0 || impl_->watch_descriptor >= 0 ||
			impl_->watch_registered) return true;
		for (const auto &device : impl_->devices) if (device.opened) return true;
		return false;
	};
	if (impl_->opening || impl_->open_complete || impl_->cleanup_started ||
		acquired())
		return {MISTER_RESULT_INVALID_STATE, acquired()};
	impl_->opening = true;
	auto finish = [this, &acquired](Result value) {
		impl_->opening = false;
		return NativeAcquisitionOutcome{value, acquired()};
	};
	auto expired = [this, &guard]() {
		return impl_->operations->NowMs() >= guard->absolute_deadline_ms();
	};

	int value = -1;
	result = impl_->operations->OpenDirectory(guard->absolute_deadline_ms(), &value);
	if (result != MISTER_RESULT_OK)
		return finish(expired() ? MISTER_RESULT_DEADLINE : result);
	impl_->directory = value;
	if (expired()) return finish(MISTER_RESULT_DEADLINE);
	value = -1;
	result = impl_->operations->OpenWatch(guard->absolute_deadline_ms(), &value);
	if (result != MISTER_RESULT_OK)
		return finish(expired() ? MISTER_RESULT_DEADLINE : result);
	impl_->watch_descriptor = value;
	if (expired()) return finish(MISTER_RESULT_DEADLINE);
	value = -1;
	result = impl_->operations->AddWatch(impl_->watch_descriptor,
		guard->absolute_deadline_ms(), &value);
	if (result != MISTER_RESULT_OK)
		return finish(expired() ? MISTER_RESULT_DEADLINE : result);
	impl_->watch = value;
	impl_->watch_registered = true;
	if (expired()) return finish(MISTER_RESULT_DEADLINE);

	size_t admitted = 0;
	for (size_t index = 0; index < kDeviceCapacity; ++index) {
		if (expired()) return finish(MISTER_RESULT_DEADLINE);
		InputCandidate candidate = {};
		result = impl_->operations->DiscoverDevice(index, &candidate);
		if (expired()) return finish(MISTER_RESULT_DEADLINE);
		if (result == MISTER_RESULT_UNSUPPORTED) break;
		if (result != MISTER_RESULT_OK) return finish(result);
		Impl::Device *slot = nullptr;
		for (auto &device : impl_->devices) {
			if (!device.opened) { slot = &device; break; }
		}
		if (slot == nullptr) return finish(MISTER_RESULT_PLATFORM);
		value = -1;
		result = impl_->operations->OpenDevice(candidate,
			guard->absolute_deadline_ms(), &value);
		if (result != MISTER_RESULT_OK)
			return finish(expired() ? MISTER_RESULT_DEADLINE : result);
		*slot = {value, candidate.identity, 0, true, false, false, 0, 0};
		if (expired()) return finish(MISTER_RESULT_DEADLINE);
		NativeLinuxInputCapabilities caps = {};
		result = impl_->operations->QueryCapabilities(value, &caps);
		if (expired()) return finish(MISTER_RESULT_DEADLINE);
		if (result != MISTER_RESULT_OK) return finish(result);
		if (!IsDigitalOnly(caps) || admitted >= kNativePlayerCount) {
			result = impl_->operations->Close(value);
			if (expired()) return finish(MISTER_RESULT_DEADLINE);
			if (result != MISTER_RESULT_OK) return finish(result);
			*slot = {};
			slot->descriptor = -1;
			continue;
		}
		result = impl_->operations->SetGrab(value, true);
		if (expired()) return finish(MISTER_RESULT_DEADLINE);
		if (result != MISTER_RESULT_OK) return finish(result);
		slot->grabbed = true;
		slot->admitted = true;
		slot->player = static_cast<uint8_t>(admitted++);
		if (expired()) return finish(MISTER_RESULT_DEADLINE);
	}
	if (expired()) return finish(MISTER_RESULT_DEADLINE);
	impl_->open_complete = true;
	impl_->polling = true;
	return finish(MISTER_RESULT_OK);
}

Result NativeInputAdapter::PollInput(const OperationLease &lease)
{
	if (impl_ == nullptr) return MISTER_RESULT_PLATFORM;
	DeliveredInput delivered[kNativePlayerCount] = {};
	bool delivered_valid[kNativePlayerCount] = {false, false};
	Result result = input_.SnapshotDeliveredInput(broker_, &profile_, lease,
		delivered, delivered_valid, kNativePlayerCount);
	if (result != MISTER_RESULT_OK) {
		if (result == MISTER_RESULT_UNSUPPORTED) {
			std::lock_guard<std::mutex> lock(impl_->mutex);
			for (const auto &device : impl_->devices) {
				if (device.opened && device.admitted && device.grabbed)
					impl_->uncertain[device.player] = true;
			}
			impl_->polling = false;
		}
		return result;
	}
	const uint64_t deadline = lease.absolute_deadline_ms();
	if (impl_->operations->NowMs() >= deadline) return MISTER_RESULT_DEADLINE;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	if (!impl_->open_complete || !impl_->polling || impl_->cleanup_started)
		return MISTER_RESULT_INVALID_STATE;
	for (auto &device : impl_->devices) {
		if (!device.opened || !device.admitted || !device.grabbed) continue;
		if (impl_->operations->NowMs() >= deadline)
			return MISTER_RESULT_DEADLINE;
		NativeLinuxInputEvent events[kReadBatchCapacity] = {};
		size_t count = 0;
		bool partial = false;
		result = impl_->operations->ReadEvents(device.descriptor, events,
			kReadBatchCapacity, &count, &partial);
		if (impl_->operations->NowMs() >= deadline) {
			impl_->uncertain[device.player] = true;
			impl_->polling = false;
			return MISTER_RESULT_DEADLINE;
		}
		if (result != MISTER_RESULT_OK || partial) {
			impl_->uncertain[device.player] = true;
			impl_->polling = false;
			return result == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : result;
		}
		for (size_t index = 0; index < count; ++index) {
			const NativeLinuxInputEvent &event = events[index];
			if ((event.device_identity != 0 &&
				event.device_identity != device.identity) ||
				event.kind == NativeLinuxInputEventKind::syn_dropped ||
				event.kind == NativeLinuxInputEventKind::device_removed) {
				impl_->uncertain[device.player] = true;
				impl_->polling = false;
				return MISTER_RESULT_PLATFORM;
			}
			if (event.kind == NativeLinuxInputEventKind::unsupported ||
				event.kind == NativeLinuxInputEventKind::syn_report) continue;
			const uint16_t previous = static_cast<uint16_t>(
				device.button_map | device.hat_map);
			if (event.kind == NativeLinuxInputEventKind::digital_button) {
				if (event.code > NativeLinuxInputCode::right_thumb ||
					(event.value != 0 && event.value != 1)) continue;
				const uint16_t bit = static_cast<uint16_t>(1u << event.code);
				device.button_map = event.value == 0 ?
					static_cast<uint16_t>(device.button_map & ~bit) :
					static_cast<uint16_t>(device.button_map | bit);
			} else if (event.kind == NativeLinuxInputEventKind::digital_hat_x) {
				device.hat_map &= static_cast<uint16_t>(~(
					(1u << NativeLinuxInputCode::left) |
					(1u << NativeLinuxInputCode::right)));
				if (event.value < 0)
					device.hat_map |= 1u << NativeLinuxInputCode::left;
				if (event.value > 0)
					device.hat_map |= 1u << NativeLinuxInputCode::right;
			} else if (event.kind == NativeLinuxInputEventKind::digital_hat_y) {
				device.hat_map &= static_cast<uint16_t>(~(
					(1u << NativeLinuxInputCode::up) |
					(1u << NativeLinuxInputCode::down)));
				if (event.value < 0)
					device.hat_map |= 1u << NativeLinuxInputCode::up;
				if (event.value > 0)
					device.hat_map |= 1u << NativeLinuxInputCode::down;
			}
			const uint16_t next = static_cast<uint16_t>(
				device.button_map | device.hat_map);
			if (next == previous) continue;
			if (impl_->sequence[device.player] == UINT64_MAX) {
				impl_->uncertain[device.player] = true;
				impl_->polling = false;
				return MISTER_RESULT_PLATFORM;
			}
			const uint64_t sequence = impl_->sequence[device.player] + 1;
			NativeInputEvent translated = {NativeInputKind::digital, device.player,
				next, next != 0, false, event.code,
				{device.player, sequence}};
			SpiReceipt receipt = {};
			result = input_.Deliver(&profile_, lease, translated, &receipt);
			if (result != MISTER_RESULT_OK) {
				impl_->uncertain[device.player] = true;
				impl_->polling = false;
				return result;
			}
			impl_->sequence[device.player] = sequence;
		}
	}
	return impl_->operations->NowMs() >= deadline ? MISTER_RESULT_DEADLINE :
		MISTER_RESULT_OK;
}

Result NativeInputAdapter::CaptureDigitalNeutral(const OperationLease &lease,
	NativeDigitalNeutral *values, bool *valid, size_t count)
{
	if (impl_ == nullptr || values == nullptr || valid == nullptr ||
		count < kNativePlayerCount) return MISTER_RESULT_INVALID_ARGUMENT;
	DeliveredInput delivered[kNativePlayerCount] = {};
	bool delivered_valid[kNativePlayerCount] = {false, false};
	const Result result = input_.SnapshotDeliveredInput(broker_, &profile_, lease,
		delivered, delivered_valid, kNativePlayerCount);
	if (result != MISTER_RESULT_OK) return result;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	for (size_t player = 0; player < kNativePlayerCount; ++player) {
		valid[player] = delivered_valid[player] || impl_->uncertain[player];
		values[player] = {static_cast<uint8_t>(player),
			{profile_.input.player_command[player], 0}};
	}
	return MISTER_RESULT_OK;
}

Result NativeInputAdapter::CloseInputDescriptors(const OperationLease &lease)
{
	if (impl_ == nullptr) return MISTER_RESULT_PLATFORM;
	std::unique_ptr<ProcessOperationGuard> guard;
	Result admission = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::input_descriptors, &profile_, &guard);
	if (admission != MISTER_RESULT_OK) return admission;
	if (guard->authority() != LeaseAuthority::cleanup_epoch)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->cleanup_started = true;
	impl_->polling = false;
	auto expired = [this, &guard]() {
		return impl_->operations->NowMs() >= guard->absolute_deadline_ms();
	};
	if (expired()) return MISTER_RESULT_DEADLINE;
	Result failure = MISTER_RESULT_OK;
	auto note = [&failure](Result result) {
		if (result != MISTER_RESULT_OK && failure == MISTER_RESULT_OK)
			failure = result;
	};
	if (impl_->watch_registered) {
		const Result result = impl_->operations->RemoveWatch(
			impl_->watch_descriptor, impl_->watch);
		note(result);
		if (result == MISTER_RESULT_OK) {
			impl_->watch_registered = false;
			impl_->watch = -1;
		}
		if (expired()) return MISTER_RESULT_DEADLINE;
	}
	for (auto &device : impl_->devices) {
		if (!device.opened) continue;
		if (device.grabbed) {
			if (expired()) return MISTER_RESULT_DEADLINE;
			const Result result = impl_->operations->SetGrab(device.descriptor, false);
			note(result);
			if (result == MISTER_RESULT_OK) device.grabbed = false;
			if (expired()) return MISTER_RESULT_DEADLINE;
		}
		if (!device.grabbed) {
			if (expired()) return MISTER_RESULT_DEADLINE;
			const Result result = impl_->operations->Close(device.descriptor);
			note(result);
			if (result == MISTER_RESULT_OK) {
				device = {};
				device.descriptor = -1;
			}
			if (expired()) return MISTER_RESULT_DEADLINE;
		}
	}
	if (!impl_->watch_registered && impl_->watch_descriptor >= 0) {
		if (expired()) return MISTER_RESULT_DEADLINE;
		const Result result = impl_->operations->Close(impl_->watch_descriptor);
		note(result);
		if (result == MISTER_RESULT_OK) impl_->watch_descriptor = -1;
		if (expired()) return MISTER_RESULT_DEADLINE;
	}
	if (impl_->directory >= 0) {
		if (expired()) return MISTER_RESULT_DEADLINE;
		const Result result = impl_->operations->Close(impl_->directory);
		note(result);
		if (result == MISTER_RESULT_OK) impl_->directory = -1;
		if (expired()) return MISTER_RESULT_DEADLINE;
	}
	if (failure != MISTER_RESULT_OK) return failure == MISTER_RESULT_DEADLINE ?
		MISTER_RESULT_DEADLINE : MISTER_RESULT_CLEANUP_INCOMPLETE;
	impl_->open_complete = false;
	return MISTER_RESULT_OK;
}

void NativeInputAdapter::CloseInputDescriptorsForProcessExit()
{
	if (impl_ == nullptr) return;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->cleanup_started = true;
	impl_->polling = false;
	if (impl_->watch_registered)
		(void)impl_->operations->RemoveWatch(impl_->watch_descriptor, impl_->watch);
	for (auto &device : impl_->devices) {
		if (!device.opened) continue;
		if (device.grabbed) (void)impl_->operations->SetGrab(device.descriptor, false);
		(void)impl_->operations->Close(device.descriptor);
		device = {};
		device.descriptor = -1;
	}
	if (impl_->watch_descriptor >= 0)
		(void)impl_->operations->Close(impl_->watch_descriptor);
	if (impl_->directory >= 0) (void)impl_->operations->Close(impl_->directory);
	impl_->directory = -1;
	impl_->watch_descriptor = -1;
	impl_->watch = -1;
	impl_->watch_registered = false;
}

#if defined(MISTER_NATIVE_LINUX_EXECUTION_TESTING)
size_t NativeInputAdapter::admitted_devices_for_test() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	size_t count = 0;
	for (const auto &device : impl_->devices) count += device.admitted ? 1 : 0;
	return count;
}

size_t NativeInputAdapter::grabbed_devices_for_test() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	size_t count = 0;
	for (const auto &device : impl_->devices) count += device.grabbed ? 1 : 0;
	return count;
}

size_t NativeInputAdapter::owned_descriptors_for_test() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	size_t count = (impl_->directory >= 0 ? 1u : 0u) +
		(impl_->watch_descriptor >= 0 ? 1u : 0u);
	for (const auto &device : impl_->devices) count += device.opened ? 1 : 0;
	return count;
}

bool NativeInputAdapter::player_uncertain_for_test(size_t player) const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return player < kNativePlayerCount && impl_->uncertain[player];
}

void NativeInputAdapter::set_player_sequence_for_test(size_t player,
	uint64_t sequence)
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	if (player < kNativePlayerCount) impl_->sequence[player] = sequence;
}
#endif

} // namespace linux_native
} // namespace native
} // namespace mister
