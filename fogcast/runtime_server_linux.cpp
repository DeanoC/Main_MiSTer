// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fogcast/runtime_server.hpp"

#include "fogcast/runtime_coordinator.hpp"
#include "fogcast/runtime_protocol.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <string.h>
#include <limits.h>
#include <assert.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace fogcast {
namespace {

const uint32_t kFrameLimit = 65536u;
const uint32_t kIoDeadlineMs = 2000u;
thread_local bool g_worker_context = false;

void AssertWorkerContext() { assert(g_worker_context); }

class SystemClock : public ServerClock {
public:
	uint64_t NowMs() override {
		struct timespec value;
		clock_gettime(CLOCK_MONOTONIC, &value);
		return static_cast<uint64_t>(value.tv_sec) * 1000u +
			static_cast<uint64_t>(value.tv_nsec / 1000000u);
	}
};

SystemClock& DefaultClock() { static SystemClock value; return value; }

class SystemCredentials : public ServerCredentials {
public:
	bool PeerUid(int descriptor, uint32_t* uid) override {
#ifdef SO_PEERCRED
		struct ucred credential;
		socklen_t size = static_cast<socklen_t>(sizeof(credential));
		if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credential, &size) != 0 ||
			size != sizeof(credential)) return false;
		*uid = static_cast<uint32_t>(credential.uid);
		return true;
#else
		(void)descriptor; (void)uid;
		return false;
#endif
	}
};

SystemCredentials& DefaultCredentials() { static SystemCredentials value; return value; }

bool Exact(const struct stat& value, mode_t type, mode_t mode, uint32_t uid) {
	return (value.st_mode & S_IFMT) == type && (value.st_mode & 07777) == mode &&
		value.st_uid == static_cast<uid_t>(uid);
}

bool SameInode(const struct stat& left, const struct stat& right) {
	return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool SafeBasename(const std::string& value) {
	return !value.empty() && value.find('\0') == std::string::npos &&
		value != "." && value != ".." && value.find('/') == std::string::npos;
}

bool DescriptorConfigured(int descriptor) {
	const int checked_status = fcntl(descriptor, F_GETFL, 0);
	if (checked_status < 0 || (checked_status & O_NONBLOCK) == 0) return false;
	const int checked_descriptor_flags = fcntl(descriptor, F_GETFD, 0);
	return checked_descriptor_flags >= 0 && (checked_descriptor_flags & FD_CLOEXEC) != 0;
}

bool ConfigureDescriptor(int descriptor) {
	const int status = fcntl(descriptor, F_GETFL, 0);
	if (status < 0 || fcntl(descriptor, F_SETFL, status | O_NONBLOCK) != 0) return false;
	const int descriptor_flags = fcntl(descriptor, F_GETFD, 0);
	return descriptor_flags >= 0 && fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0 &&
		DescriptorConfigured(descriptor);
}

int NewSocket() {
#ifdef __linux__
	const int atomic_descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (atomic_descriptor >= 0) {
		if (DescriptorConfigured(atomic_descriptor)) return atomic_descriptor;
		close(atomic_descriptor); errno = EIO; return -1;
	}
	if (errno != EINVAL && errno != ENOSYS) return -1;
#endif
	const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0) return -1;
	if (ConfigureDescriptor(descriptor)) return descriptor;
	close(descriptor);
	return -1;
}

int AcceptClient(int listener) {
#ifdef __linux__
	const int atomic_descriptor = accept4(listener, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (atomic_descriptor >= 0) {
		if (DescriptorConfigured(atomic_descriptor)) return atomic_descriptor;
		close(atomic_descriptor); errno = EIO; return -1;
	}
	if (errno != ENOSYS && errno != EINVAL) return -1;
#endif
	const int descriptor = accept(listener, 0, 0);
	if (descriptor < 0) return -1;
	if (ConfigureDescriptor(descriptor)) return descriptor;
	close(descriptor);
	errno = EIO;
	return -1;
}

bool NewWakePipe(int descriptors[2]) {
#ifdef __linux__
	if (pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) == 0) {
		if (DescriptorConfigured(descriptors[0]) && DescriptorConfigured(descriptors[1])) return true;
		close(descriptors[0]); close(descriptors[1]); errno = EIO; return false;
	}
	if (errno != ENOSYS && errno != EINVAL) return false;
#endif
	if (pipe(descriptors) != 0) return false;
	if (ConfigureDescriptor(descriptors[0]) && ConfigureDescriptor(descriptors[1])) return true;
	close(descriptors[0]);
	close(descriptors[1]);
	return false;
}

std::string RetainedSocketPath(int directory_fd, const std::string& parent, const std::string& name) {
#ifdef __linux__
	(void)parent;
	return std::string("/proc/self/fd/") + std::to_string(directory_fd) + "/" + name;
#else
	// Host tests run without procfs.  Production Linux never takes this path.
	(void)directory_fd;
	return parent + "/" + name;
#endif
}

uint64_t Deadline(ServerClock* clock) {
	const uint64_t now = clock->NowMs();
	return now > UINT64_MAX - kIoDeadlineMs ? UINT64_MAX : now + kIoDeadlineMs;
}

int Remaining(uint64_t deadline, ServerClock* clock) {
	const uint64_t now = clock->NowMs();
	if (now >= deadline) return 0;
	const uint64_t left = deadline - now;
	return left > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(left);
}

bool WaitFd(int fd, short events, uint64_t deadline, ServerClock* clock) {
	struct pollfd item;
	item.fd = fd;
	item.events = events;
	item.revents = 0;
	for (;;) {
		const int result = poll(&item, 1, Remaining(deadline, clock));
		if (result > 0) return (item.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
		if (result == 0) return false;
		if (errno != EINTR) return false;
	}
}

std::string Snapshot(const CoordinatorSnapshot& snapshot) {
	return snapshot.valid ? snapshot.canonical : "null";
}

const char* Code(CoordinatorCode code) {
	const char* value = Coordinator::ProtocolCode(code);
	return value ? value : "INTERNAL";
}

bool SafeOperationId(const std::string& value) {
	if (value.size() != 32) return false;
	for (size_t i = 0; i != value.size(); ++i)
		if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
	return true;
}

enum class RequestClass { close_connection, protocol_mismatch, read_only, mutation };

// Syntax is checked before this function.  It deliberately performs only the
// minimum safe correlation extraction: schema-invalid mutations belong to the
// coordinator, whereas malformed JSON and invalid read-only frames are a
// transport protocol failure and are closed without dispatch.
RequestClass ClassifyRequest(const std::string& bytes, uint64_t* request_id,
	std::string* operation, std::string* operation_id) {
	detail::Token root;
	if (detail::ScanV1Json(bytes, kFrameLimit, &root) != ErrorClass::ok ||
		root.kind != detail::Token::Kind::object ||
		!detail::PositiveUint63(detail::Member(root, "request_id"), request_id))
		return RequestClass::close_connection;
	const detail::Token* action = detail::Member(root, "operation");
	const detail::Token* protocol = detail::Member(root, "protocol");
	if (!action || action->kind != detail::Token::Kind::string || !protocol ||
		protocol->kind != detail::Token::Kind::number) return RequestClass::close_connection;
	*operation = action->text;
	operation_id->clear();
	const detail::Token* id = detail::Member(root, "operation_id");
	if (id && id->kind == detail::Token::Kind::string && SafeOperationId(id->text)) *operation_id = id->text;
	if (protocol->text != "1") return RequestClass::protocol_mismatch;
	if (*operation == "hello" || *operation == "health" || *operation == "status" ||
		*operation == "operation_status") {
		std::string canonical, digest;
		return ParseWire(bytes, &canonical, &digest) == ErrorClass::ok ?
			RequestClass::read_only : RequestClass::close_connection;
	}
	return RequestClass::mutation;
}

bool StatusOperationId(const std::string& bytes, std::string* operation_id) {
	detail::Token root;
	if (detail::ScanV1Json(bytes, kFrameLimit, &root) != ErrorClass::ok || root.kind != detail::Token::Kind::object)
		return false;
	const detail::Token* body = detail::Member(root, "body");
	const detail::Token* id = body ? detail::Member(*body, "operation_id") : 0;
	if (!id || id->kind != detail::Token::Kind::string) return false;
	*operation_id = id->text;
	return true;
}

bool NullMember(const detail::Token& object, const char* name) {
	const detail::Token* value = detail::Member(object, name);
	return value && value->kind == detail::Token::Kind::null_value;
}

bool DrainReady(const Coordinator* coordinator) {
	AssertWorkerContext();
	if (!coordinator || coordinator->phase() != "idle") return false;
	detail::Token record;
	if (detail::ScanV1Json(coordinator->canonical_state(), 262144, &record) != ErrorClass::ok ||
		record.kind != detail::Token::Kind::object || !NullMember(record, "owner") ||
		!NullMember(record, "release_owner") || !NullMember(record, "candidate") ||
		!NullMember(record, "in_flight")) return false;
	const detail::Token* leases = detail::Member(record, "leases");
	return leases && leases->kind == detail::Token::Kind::array && leases->array.empty();
}

std::string ErrorResponse(uint64_t request_id, const std::string& operation_id, const char* code,
	const CoordinatorSnapshot& snapshot) {
	std::string value = "{\"protocol\":1,\"request_id\":" + std::to_string(request_id);
	if (!operation_id.empty()) value += ",\"operation_id\":\"" + operation_id + "\"";
	value += std::string(",\"ok\":false,\"error\":{\"code\":\"") + code +
		"\",\"message\":\"" + RuntimeServer::MessageForCode(code) +
		"\"},\"snapshot\":" + Snapshot(snapshot) + "}";
	return value;
}

std::string MutationResponse(uint64_t request_id, const std::string& operation,
	const std::string& safe_operation_id, const MutationReply& reply) {
	if (reply.kind != MutationReply::Kind::terminal || !reply.ok)
		return ErrorResponse(request_id, reply.operation_id.empty() ? safe_operation_id : reply.operation_id,
			Code(reply.code), reply.snapshot);
	std::string result = "{\"resulting_sequence\":" + std::to_string(reply.resulting_sequence);
	if (operation == "shutdown") result += ",\"quiesced\":true";
	result += "}";
	return "{\"protocol\":1,\"request_id\":" + std::to_string(request_id) +
		",\"operation_id\":\"" + reply.operation_id + "\",\"ok\":true,\"result\":" + result +
		",\"snapshot\":" + Snapshot(reply.snapshot) + "}";
}

std::string OperationStatusResponse(uint64_t request_id, const std::string& operation_id,
	const Coordinator::OperationStatus& status, const CoordinatorSnapshot& current) {
	if (status.kind == Coordinator::OperationStatus::unknown)
		return ErrorResponse(request_id, std::string(), "OPERATION_UNKNOWN", current);
	std::string terminal = "null";
	std::string state = "in_progress";
	if (status.kind == Coordinator::OperationStatus::completed) {
		state = "completed";
		const char* code = Code(status.code);
		const std::string error = status.ok ? "null" : std::string("{\"code\":\"") + code +
			"\",\"message\":\"" + RuntimeServer::MessageForCode(code) + "\"}";
		terminal = std::string("{\"ok\":") + (status.ok ? "true" : "false") +
			",\"error\":" + error + ",\"snapshot\":" + (status.snapshot.empty() ? "null" : status.snapshot) +
			",\"resulting_sequence\":" + std::to_string(status.resulting_sequence) + "}";
	}
	return "{\"protocol\":1,\"request_id\":" + std::to_string(request_id) +
		",\"ok\":true,\"result\":{\"operation_id\":\"" + operation_id +
		"\",\"request_digest\":\"" + status.request_digest + "\",\"state\":\"" + state +
		"\",\"terminal\":" + terminal + "},\"snapshot\":" + Snapshot(current) + "}";
}

}  // namespace

struct RuntimeServer::State {
	enum class ClientPhase { none, prefix_read, payload_read, dispatch_wait, worker_wait, response_write };
	struct Work {
		bool drain_probe;
		bool protocol_mismatch;
		std::string request;
		uint64_t request_id;
		std::string operation;
		std::string operation_id;
		uint64_t client_token;
		Work() : drain_probe(false), protocol_mismatch(false), request_id(0), client_token(0) {}
	};
	RuntimeServerConfig config;
	Coordinator* coordinator;
	ServerCredentials* credentials;
	ServerClock* clock;
	ServerTestHooks* test_hooks;
	int directory_fd;
	struct stat directory_stat;
	int lock_fd;
	int listener_fd;
	int active_client_fd;
	int wake_read_fd;
	int wake_write_fd;
	uint64_t active_client_token;
	ClientPhase client_phase;
	unsigned char prefix[4];
	size_t prefix_used;
	std::string payload;
	size_t payload_used;
	uint64_t client_deadline;
	std::string write_buffer;
	size_t write_used;
	bool socket_retained;
	bool socket_identity_valid;
	struct stat socket_stat;
	bool start_attempted;
	bool drain;
	volatile sig_atomic_t drain_signal;
	bool drain_ready;
	bool stop_worker;
	bool work_pending;
	bool worker_busy;
	bool response_pending;
	Work work;
	std::string response;
	uint64_t response_token;
	std::mutex mutex;
	std::condition_variable work_available;
	std::thread worker;
	State(const RuntimeServerConfig& value, Coordinator* input, ServerCredentials* peer, ServerClock* time,
		ServerTestHooks* hooks)
		: config(value), coordinator(input), credentials(peer ? peer : &DefaultCredentials()),
		clock(time ? time : &DefaultClock()), test_hooks(hooks), directory_fd(-1), lock_fd(-1), listener_fd(-1), active_client_fd(-1), wake_read_fd(-1), wake_write_fd(-1), active_client_token(0),
		client_phase(ClientPhase::none), prefix_used(0), payload_used(0), client_deadline(0), write_used(0),
		socket_retained(false), socket_identity_valid(false), start_attempted(false), drain(false), drain_signal(0), drain_ready(false), stop_worker(false), work_pending(false), worker_busy(false), response_pending(false), response_token(0) { memset(prefix, 0, sizeof(prefix)); memset(&socket_stat, 0, sizeof(socket_stat)); memset(&directory_stat, 0, sizeof(directory_stat)); }
};

namespace {

bool ExactSocket(const struct stat& value, uint32_t uid) {
	return Exact(value, S_IFSOCK, 0600, uid) && value.st_nlink == 1;
}

bool VerifyConfiguredParent(const RuntimeServer::State* state) {
	const int reopened = open(state->config.parent_directory.c_str(),
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (reopened < 0) return false;
	struct stat current;
	const bool valid = fstat(reopened, &current) == 0 && SameInode(current, state->directory_stat) &&
		Exact(current, S_IFDIR, 0700, state->config.service_uid);
	close(reopened);
	return valid;
}

bool VerifyLock(const RuntimeServer::State* state) {
	struct stat held, entry;
	return fstat(state->lock_fd, &held) == 0 &&
		Exact(held, S_IFREG, 0600, state->config.service_uid) && held.st_nlink == 1 &&
		fstatat(state->directory_fd, state->config.lock_name.c_str(), &entry, AT_SYMLINK_NOFOLLOW) == 0 &&
		SameInode(held, entry) && Exact(entry, S_IFREG, 0600, state->config.service_uid) &&
		entry.st_nlink == 1;
}

bool VerifyAnchors(const RuntimeServer::State* state) {
	return VerifyConfiguredParent(state) && VerifyLock(state);
}

bool VerifySocketEntry(const RuntimeServer::State* state) {
	struct stat entry;
	return fstatat(state->directory_fd, state->config.socket_name.c_str(), &entry,
		AT_SYMLINK_NOFOLLOW) == 0 && SameInode(entry, state->socket_stat) &&
		ExactSocket(entry, state->config.service_uid);
}

bool StatPostBindSocket(RuntimeServer::State* state, struct stat* value) {
	if (state->test_hooks && state->test_hooks->FailFirstPostBindEntryStat()) {
		errno = EIO;
		return false;
	}
	return fstatat(state->directory_fd, state->config.socket_name.c_str(), value,
		AT_SYMLINK_NOFOLLOW) == 0;
}

bool CaptureSocketForCleanup(RuntimeServer::State* state) {
	struct stat entry;
	// Do not use the fault seam here: this is the real, independent recheck
	// used only to establish an exact unlink identity after the first stat
	// failed. If it cannot establish one, Close leaves the path untouched.
	if (fstatat(state->directory_fd, state->config.socket_name.c_str(), &entry,
		AT_SYMLINK_NOFOLLOW) != 0 || !ExactSocket(entry, state->config.service_uid)) return false;
	state->socket_stat = entry;
	state->socket_identity_valid = true;
	return true;
}

std::string WorkerResponse(RuntimeServer::State::Work const& work, Coordinator* coordinator) {
	AssertWorkerContext();
	const CoordinatorSnapshot snapshot = coordinator->snapshot();
	if (work.protocol_mismatch)
		return ErrorResponse(work.request_id, work.operation_id, "PROTOCOL_MISMATCH", snapshot);
	if (work.operation == "status") {
		if (!snapshot.valid) return ErrorResponse(work.request_id, work.operation_id, "NOT_READY", snapshot);
		return "{\"protocol\":1,\"request_id\":" + std::to_string(work.request_id) + ",\"ok\":true,\"result\":{},\"snapshot\":" + Snapshot(snapshot) + "}";
	}
	if (work.operation == "hello") return "{\"protocol\":1,\"request_id\":" + std::to_string(work.request_id) + ",\"ok\":true,\"result\":{\"protocol\":1,\"abi_version\":2,\"ready\":" + std::string(coordinator->ready() ? "true" : "false") + ",\"max_frame_bytes\":65536,\"max_connections\":1,\"max_replay_entries\":64,\"capabilities\":" + (coordinator->ready() ? "31" : "0") + "},\"snapshot\":null}";
	if (work.operation == "health") {
		std::string phase = "null", mode = "null";
		std::string capabilities = "0";
		detail::Token record;
		const bool available = coordinator->ready() && snapshot.valid &&
			detail::ScanV1Json(snapshot.canonical, 2048, &record) == ErrorClass::ok;
		if (available) {
			const detail::Token* value = detail::Member(record, "phase");
			if (!value || value->kind != detail::Token::Kind::string) return ErrorResponse(work.request_id, work.operation_id, "NOT_READY", CoordinatorSnapshot());
			phase = detail::Encode(*value);
			value = detail::Member(record, "mode");
			if (!value || value->kind != detail::Token::Kind::string) return ErrorResponse(work.request_id, work.operation_id, "NOT_READY", CoordinatorSnapshot());
			mode = detail::Encode(*value);
			value = detail::Member(record, "capabilities");
			if (!value || value->kind != detail::Token::Kind::number) return ErrorResponse(work.request_id, work.operation_id, "NOT_READY", CoordinatorSnapshot());
			capabilities = value->text;
		}
		return "{\"protocol\":1,\"request_id\":" + std::to_string(work.request_id) + ",\"ok\":true,\"result\":{\"ready\":" + std::string(available ? "true" : "false") + ",\"phase\":" + phase + ",\"mode\":" + mode + ",\"capabilities\":" + capabilities + "},\"snapshot\":" + (available ? Snapshot(snapshot) : "null") + "}";
	}
	if (work.operation == "operation_status") {
		std::string queried;
		if (!StatusOperationId(work.request, &queried)) return ErrorResponse(work.request_id, std::string(), "INVALID_REQUEST", snapshot);
		return OperationStatusResponse(work.request_id, queried, coordinator->operation_status(queried), snapshot);
	}
	return MutationResponse(work.request_id, work.operation, work.operation_id,
		coordinator->ExecuteMutation(work.request));
}

void RunWorker(RuntimeServer::State* state) {
	g_worker_context = true;
	for (;;) {
		RuntimeServer::State::Work work;
		{
			std::unique_lock<std::mutex> lock(state->mutex);
			state->work_available.wait(lock, [state] { return state->stop_worker || state->work_pending; });
			if (state->stop_worker) { g_worker_context = false; return; }
			work = state->work;
			state->work_pending = false;
			state->worker_busy = true;
		}
		std::string response;
		const bool drain_ready = work.drain_probe ? DrainReady(state->coordinator) : false;
		if (!work.drain_probe) response = WorkerResponse(work, state->coordinator);
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->worker_busy = false;
			if (work.drain_probe) state->drain_ready = drain_ready;
			else { state->response = response; state->response_token = work.client_token; state->response_pending = true; }
		}
		if (state->wake_write_fd >= 0) { const char byte = 'w'; (void)write(state->wake_write_fd, &byte, 1); }
	}
}

bool Enqueue(RuntimeServer::State* state, const RuntimeServer::State::Work& work) {
	std::lock_guard<std::mutex> lock(state->mutex);
	if (state->work_pending || state->worker_busy || state->response_pending) return false;
	state->work = work;
	state->work_pending = true;
	if (!work.drain_probe) state->drain_ready = false;
	state->work_available.notify_one();
	return true;
}

void StartPrefixRead(RuntimeServer::State* state) {
	state->client_phase = RuntimeServer::State::ClientPhase::prefix_read;
	state->prefix_used = 0;
	state->payload.clear();
	state->payload_used = 0;
	state->write_buffer.clear();
	state->write_used = 0;
	state->client_deadline = Deadline(state->clock);
}

void DropClient(RuntimeServer::State* state) {
	if (state->active_client_fd >= 0) close(state->active_client_fd);
	state->active_client_fd = -1;
	state->client_phase = RuntimeServer::State::ClientPhase::none;
	state->prefix_used = 0;
	state->payload.clear();
	state->payload_used = 0;
	state->write_buffer.clear();
	state->write_used = 0;
	state->client_deadline = 0;
}

bool BeginResponse(RuntimeServer::State* state, const std::string& response) {
	if (response.empty() || response.size() > kFrameLimit) return false;
	state->write_buffer.assign(4, '\0');
	state->write_buffer[0] = static_cast<char>(response.size() >> 24);
	state->write_buffer[1] = static_cast<char>(response.size() >> 16);
	state->write_buffer[2] = static_cast<char>(response.size() >> 8);
	state->write_buffer[3] = static_cast<char>(response.size());
	state->write_buffer += response;
	state->write_used = 0;
	state->client_phase = RuntimeServer::State::ClientPhase::response_write;
	// The prefix and JSON payload deliberately share this one deadline.
	state->client_deadline = Deadline(state->clock);
	return true;
}

void CollectWorkerResponse(RuntimeServer::State* state) {
	std::string response;
	uint64_t token = 0;
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (!state->response_pending) return;
		response.swap(state->response);
		token = state->response_token;
		state->response_pending = false;
	}
	if (state->active_client_fd >= 0 && token == state->active_client_token &&
		state->client_phase == RuntimeServer::State::ClientPhase::worker_wait) {
		if (!BeginResponse(state, response)) DropClient(state);
	}
}

void DispatchPayload(RuntimeServer::State* state) {
	uint64_t request_id = 0;
	std::string operation, operation_id;
	const RequestClass kind = ClassifyRequest(state->payload, &request_id, &operation, &operation_id);
	if (kind == RequestClass::close_connection) { DropClient(state); return; }
	RuntimeServer::State::Work work;
	work.request = state->payload;
	work.request_id = request_id;
	work.operation = operation;
	work.operation_id = operation_id;
	work.client_token = state->active_client_token;
	work.protocol_mismatch = kind == RequestClass::protocol_mismatch;
	state->client_phase = Enqueue(state, work) ? RuntimeServer::State::ClientPhase::worker_wait :
		RuntimeServer::State::ClientPhase::dispatch_wait;
}

ssize_t ClientRead(RuntimeServer::State* state, void* bytes, size_t length) {
	if (state->test_hooks && state->test_hooks->InterruptRead()) { errno = EINTR; return -1; }
	return read(state->active_client_fd, bytes, length);
}

ssize_t ClientSend(RuntimeServer::State* state, const void* bytes, size_t length) {
	if (state->test_hooks && state->test_hooks->InterruptSend()) { errno = EINTR; return -1; }
	return send(state->active_client_fd, bytes, length, MSG_NOSIGNAL);
}

void ReadClient(RuntimeServer::State* state) {
	for (;;) {
		if (state->client_phase == RuntimeServer::State::ClientPhase::prefix_read) {
			const ssize_t count = ClientRead(state, state->prefix + state->prefix_used,
				sizeof(state->prefix) - state->prefix_used);
			if (count > 0) {
				state->prefix_used += static_cast<size_t>(count);
				if (state->prefix_used != sizeof(state->prefix)) continue;
				const uint32_t length = (static_cast<uint32_t>(state->prefix[0]) << 24) |
					(static_cast<uint32_t>(state->prefix[1]) << 16) |
					(static_cast<uint32_t>(state->prefix[2]) << 8) | state->prefix[3];
				if (!RuntimeServer::ValidFrameLength(length)) { DropClient(state); return; }
				state->payload.assign(length, '\0');
				state->payload_used = 0;
				state->client_phase = RuntimeServer::State::ClientPhase::payload_read;
				state->client_deadline = Deadline(state->clock);
				continue;
			}
			if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) { DropClient(state); return; }
			if (errno == EINTR) return;
			return;
		}
		if (state->client_phase == RuntimeServer::State::ClientPhase::payload_read) {
			const ssize_t count = ClientRead(state, &state->payload[state->payload_used],
				state->payload.size() - state->payload_used);
			if (count > 0) {
				state->payload_used += static_cast<size_t>(count);
				if (state->payload_used != state->payload.size()) continue;
				DispatchPayload(state);
				return;
			}
			if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) { DropClient(state); return; }
			if (errno == EINTR) return;
			return;
		}
		return;
	}
}

void WriteClient(RuntimeServer::State* state) {
	while (state->write_used != state->write_buffer.size()) {
		if (state->test_hooks && state->test_hooks->StallSend()) { errno = EAGAIN; return; }
		size_t remaining = state->write_buffer.size() - state->write_used;
		if (state->test_hooks) {
			const uint32_t asked = remaining > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(remaining);
			const uint32_t allowed = state->test_hooks->SendAllowance(asked);
			if (allowed == 0) { errno = EAGAIN; return; }
			if (remaining > allowed) remaining = allowed;
		}
		const ssize_t count = ClientSend(state,
			state->write_buffer.data() + state->write_used,
			remaining);
		if (count > 0) { state->write_used += static_cast<size_t>(count); continue; }
		if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) { DropClient(state); return; }
		if (errno == EINTR) return;
		return;
	}
	StartPrefixRead(state);
}

void AcceptAll(RuntimeServer::State* state) {
	for (;;) {
		const int accepted = AcceptClient(state->listener_fd);
		if (accepted < 0) {
			if (errno == EINTR) continue;
			return;
		}
		uint32_t uid = 0;
		if (!state->credentials->PeerUid(accepted, &uid) || uid != state->config.service_uid ||
			state->active_client_fd >= 0) { close(accepted); continue; }
		state->active_client_fd = accepted;
		++state->active_client_token;
		StartPrefixRead(state);
	}
}

}  // namespace

RuntimeServer::RuntimeServer(const RuntimeServerConfig& config, Coordinator* coordinator,
	ServerCredentials* credentials, ServerClock* clock, ServerTestHooks* test_hooks)
	: state_(new State(config, coordinator, credentials, clock, test_hooks)) {}

RuntimeServer::~RuntimeServer() { Close(); delete state_; }

bool RuntimeServer::ValidFrameLength(uint32_t length) { return length != 0 && length <= kFrameLimit; }

const char* RuntimeServer::MessageForCode(const char* code) {
	if (!code) return "internal error";
	if (strcmp(code, "INVALID_REQUEST") == 0) return "invalid request";
	if (strcmp(code, "UNAUTHORIZED_PEER") == 0) return "unauthorized peer";
	if (strcmp(code, "PROTOCOL_MISMATCH") == 0) return "protocol mismatch";
	if (strcmp(code, "BUSY") == 0) return "operation busy";
	if (strcmp(code, "NOT_READY") == 0) return "runtime not ready";
	if (strcmp(code, "STALE_ADMISSION") == 0) return "stale admission";
	if (strcmp(code, "STALE_GENERATION") == 0) return "stale generation";
	if (strcmp(code, "IN_PROGRESS") == 0) return "operation in progress";
	if (strcmp(code, "OPERATION_REPLAY") == 0) return "operation replay";
	if (strcmp(code, "OPERATION_UNKNOWN") == 0) return "operation unknown";
	if (strcmp(code, "INTERRUPTED") == 0) return "operation interrupted";
	if (strcmp(code, "DEADLINE") == 0) return "operation deadline exceeded";
	if (strcmp(code, "RECOVERY_REQUIRED") == 0) return "recovery required";
	if (strcmp(code, "UNSUPPORTED_MODE") == 0) return "unsupported mode";
	if (strcmp(code, "OWNER_NOT_FOUND") == 0) return "owner not found";
	return "internal error";
}

ServerResult RuntimeServer::Start() {
	if (state_->start_attempted) return ServerResult::invalid_argument;
	state_->start_attempted = true;
	if (!state_->coordinator || state_->config.parent_directory.empty() ||
		state_->config.parent_directory.find('\0') != std::string::npos ||
		!SafeBasename(state_->config.socket_name) || !SafeBasename(state_->config.lock_name))
		return ServerResult::invalid_argument;
	state_->directory_fd = open(state_->config.parent_directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	struct stat directory;
	if (state_->directory_fd < 0 || fstat(state_->directory_fd, &directory) != 0 ||
		!Exact(directory, S_IFDIR, 0700, state_->config.service_uid)) { Close(); return ServerResult::unsafe_parent; }
	state_->directory_stat = directory;
	const mode_t original_umask = umask(0077);
	state_->lock_fd = openat(state_->directory_fd, state_->config.lock_name.c_str(),
		O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	const int create_error = errno;
	(void)umask(original_umask);
	errno = create_error;
	if (state_->lock_fd < 0 && errno == EEXIST)
		state_->lock_fd = openat(state_->directory_fd, state_->config.lock_name.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	if (state_->lock_fd < 0) { Close(); return ServerResult::unsafe_lock; }
	struct stat lock;
	if (fstat(state_->lock_fd, &lock) != 0 || !Exact(lock, S_IFREG, 0600, state_->config.service_uid) || lock.st_nlink != 1) {
		Close(); return ServerResult::unsafe_lock;
	}
	if (flock(state_->lock_fd, LOCK_EX | LOCK_NB) != 0) { Close(); return ServerResult::locked; }
	// This is deliberately the last lock validation before any socket operation.
	// It rejects a replaced configured parent or lock directory entry while the
	// retained descriptors still identify the original objects.
	if (!VerifyAnchors(state_)) { Close(); return ServerResult::unsafe_lock; }
	struct stat before;
	if (fstatat(state_->directory_fd, state_->config.socket_name.c_str(), &before, AT_SYMLINK_NOFOLLOW) == 0) {
		if (!ExactSocket(before, state_->config.service_uid) || !VerifyAnchors(state_)) { Close(); return ServerResult::unsafe_socket; }
		int probe = NewSocket();
		if (probe < 0) { Close(); return ServerResult::io_error; }
		struct sockaddr_un address; memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
		const std::string path = RetainedSocketPath(state_->directory_fd, state_->config.parent_directory, state_->config.socket_name);
		if (path.size() >= sizeof(address.sun_path)) { close(probe); Close(); return ServerResult::invalid_argument; }
		strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
		// connect(2) has no *at form.  On Linux this /proc path resolves through
		// the retained directory descriptor; every following mutation rechecks the
		// configured path and lock entry.
		const int result = connect(probe, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address));
		if (result == 0) { close(probe); Close(); return ServerResult::unsafe_socket; }
		if (errno == EINPROGRESS) {
			const uint64_t probe_deadline = state_->clock->NowMs() > UINT64_MAX - 1000u ?
				UINT64_MAX : state_->clock->NowMs() + 1000u;
			if (!WaitFd(probe, POLLOUT, probe_deadline, state_->clock)) {
				close(probe); Close(); return ServerResult::unsafe_socket;
			}
			int error = 0;
			socklen_t length = sizeof(error);
			if (getsockopt(probe, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || length != sizeof(error) || error == 0) {
				close(probe); Close(); return ServerResult::unsafe_socket;
			}
			errno = error;
		}
		const int connect_error = errno; close(probe);
		struct stat after, repeated;
		if (connect_error != ECONNREFUSED || !VerifyAnchors(state_) ||
			fstatat(state_->directory_fd, state_->config.socket_name.c_str(), &after, AT_SYMLINK_NOFOLLOW) != 0 ||
			!ExactSocket(after, state_->config.service_uid) || !SameInode(before, after) ||
			fstatat(state_->directory_fd, state_->config.socket_name.c_str(), &repeated, AT_SYMLINK_NOFOLLOW) != 0 ||
			!ExactSocket(repeated, state_->config.service_uid) || !SameInode(after, repeated) ||
			unlinkat(state_->directory_fd, state_->config.socket_name.c_str(), 0) != 0) { Close(); return ServerResult::unsafe_socket; }
	} else if (errno != ENOENT) { Close(); return ServerResult::unsafe_socket; }
	if (!VerifyAnchors(state_)) { Close(); return ServerResult::unsafe_parent; }
	state_->listener_fd = NewSocket();
	if (state_->listener_fd < 0) { Close(); return ServerResult::io_error; }
	struct sockaddr_un address; memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
	if (state_->config.socket_name.size() >= sizeof(address.sun_path)) { Close(); return ServerResult::invalid_argument; }
	const std::string path = RetainedSocketPath(state_->directory_fd, state_->config.parent_directory, state_->config.socket_name);
	strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
	if (!VerifyAnchors(state_) || path.size() >= sizeof(address.sun_path)) { Close(); return ServerResult::bind_failed; }
	// Socket bind observes umask.  Creating it with 0600 from the outset avoids
	// a pathname chmod operation (and therefore a symlink-follow window) while
	// retaining the required restrictive publication mode on older Linux kernels.
	const mode_t socket_umask = umask(0177);
	const int bind_result = bind(state_->listener_fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address));
	const int bind_error = errno;
	(void)umask(socket_umask);
	errno = bind_error;
	if (bind_result != 0) { Close(); return ServerResult::bind_failed; }
	// bind(2) has created a path now. Make cleanup ownership visible before the
	// first fallible directory-entry inspection. Once its path identity is
	// captured, Close will remove only that exact entry.
	state_->socket_retained = true;
	struct stat bound_entry;
	if (!StatPostBindSocket(state_, &bound_entry) || !ExactSocket(bound_entry, state_->config.service_uid)) {
		(void)CaptureSocketForCleanup(state_);
		Close(); return ServerResult::bind_failed;
	}
	state_->socket_stat = bound_entry;
	state_->socket_identity_valid = true;
	if (state_->test_hooks && state_->test_hooks->FailPostBindValidation()) {
		Close(); return ServerResult::bind_failed;
	}
	struct stat checked_socket;
	if (!VerifyAnchors(state_) || fstatat(state_->directory_fd, state_->config.socket_name.c_str(), &checked_socket,
		AT_SYMLINK_NOFOLLOW) != 0 || !SameInode(state_->socket_stat, checked_socket) ||
		!ExactSocket(checked_socket, state_->config.service_uid) ||
		listen(state_->listener_fd, 8) != 0 || !VerifySocketEntry(state_)) { Close(); return ServerResult::bind_failed; }
	int wake[2];
	if (!NewWakePipe(wake)) { Close(); return ServerResult::io_error; }
	state_->wake_read_fd = wake[0]; state_->wake_write_fd = wake[1];
	if (!VerifyAnchors(state_) || !VerifySocketEntry(state_)) { Close(); return ServerResult::unsafe_socket; }
	state_->worker = std::thread(&RunWorker, state_);
	return ServerResult::ok;
}

ServerResult RuntimeServer::Poll(uint32_t timeout_ms) {
	if (state_->listener_fd < 0) return ServerResult::invalid_argument;
	if (!VerifyConfiguredParent(state_)) { Close(); return ServerResult::unsafe_parent; }
	if (!VerifyLock(state_)) { Close(); return ServerResult::unsafe_lock; }
	if (!VerifySocketEntry(state_)) { Close(); return ServerResult::unsafe_socket; }
	CollectWorkerResponse(state_);
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		if (state_->drain_signal) { state_->drain = true; state_->drain_signal = 0; }
	}
	if (state_->client_phase == State::ClientPhase::dispatch_wait) DispatchPayload(state_);
	if (state_->active_client_fd >= 0 && state_->client_phase != State::ClientPhase::worker_wait &&
		state_->client_phase != State::ClientPhase::dispatch_wait && state_->client_deadline != 0 &&
		Remaining(state_->client_deadline, state_->clock) == 0) DropClient(state_);

	struct pollfd items[3];
	items[0].fd = state_->listener_fd; items[0].events = POLLIN; items[0].revents = 0;
	items[1].fd = state_->active_client_fd;
	if (state_->client_phase == State::ClientPhase::response_write) items[1].events = POLLOUT | POLLHUP;
	else if (state_->client_phase == State::ClientPhase::worker_wait) {
		// A pipelined request is intentionally unread until the previous response
		// is committed. Polling for POLLIN here would immediately wake forever.
		items[1].events = POLLHUP;
#ifdef POLLRDHUP
		items[1].events |= POLLRDHUP;
#endif
	} else items[1].events = POLLIN | POLLHUP;
	items[1].revents = 0;
	items[2].fd = state_->wake_read_fd; items[2].events = POLLIN; items[2].revents = 0;
	int poll_timeout = timeout_ms > static_cast<uint32_t>(INT_MAX) ? INT_MAX : static_cast<int>(timeout_ms);
	if (state_->active_client_fd >= 0 && state_->client_phase != State::ClientPhase::worker_wait) {
		const int deadline_remaining = Remaining(state_->client_deadline, state_->clock);
		if (deadline_remaining < poll_timeout) poll_timeout = deadline_remaining;
	}
	const int ready = state_->test_hooks && state_->test_hooks->InterruptPoll() ?
		(errno = EINTR, -1) : poll(items, 3u, poll_timeout);
	if (ready < 0) {
		if (errno == EINTR) return ServerResult::ok;  // deadlines are absolute and remain unchanged.
		return ServerResult::io_error;
	}
	if (ready > 0 && (items[2].revents & POLLIN) != 0) {
		char bytes[64]; while (read(state_->wake_read_fd, bytes, sizeof(bytes)) > 0) {}
	}
	if (ready > 0 && (items[0].revents & POLLIN) != 0) AcceptAll(state_);
	const short client_events = items[1].revents;
	if (state_->active_client_fd >= 0 && state_->client_phase == State::ClientPhase::response_write && (client_events & POLLOUT) != 0)
		WriteClient(state_);
	else if (state_->active_client_fd >= 0 &&
		(state_->client_phase == State::ClientPhase::prefix_read || state_->client_phase == State::ClientPhase::payload_read) &&
		(client_events & POLLIN) != 0) ReadClient(state_);
	if (state_->active_client_fd >= 0 && (client_events & (POLLERR | POLLNVAL)) != 0) DropClient(state_);
	else if (state_->active_client_fd >= 0 && state_->client_phase == State::ClientPhase::worker_wait &&
		(client_events & (POLLHUP
#ifdef POLLRDHUP
			| POLLRDHUP
#endif
		)) != 0) DropClient(state_);
	else if (state_->active_client_fd >= 0 && state_->client_phase != State::ClientPhase::dispatch_wait &&
		(client_events & POLLHUP) != 0 &&
		(client_events & (POLLIN | POLLOUT)) == 0) DropClient(state_);
	CollectWorkerResponse(state_);
	if (state_->client_phase == State::ClientPhase::dispatch_wait) DispatchPayload(state_);
	bool schedule_probe = false;
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		const bool partial_frame = state_->prefix_used != 0 || state_->payload_used != 0 ||
			state_->client_phase == State::ClientPhase::dispatch_wait ||
			state_->client_phase == State::ClientPhase::worker_wait ||
			state_->client_phase == State::ClientPhase::response_write;
		schedule_probe = state_->drain && !state_->drain_ready && !partial_frame &&
			!state_->work_pending && !state_->worker_busy && !state_->response_pending;
		if (schedule_probe) {
			state_->work = State::Work(); state_->work.drain_probe = true; state_->work_pending = true;
			state_->work_available.notify_one();
		}
	}
	bool drain_ready = false;
	bool response_pending = false;
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		drain_ready = state_->drain && state_->drain_ready;
		response_pending = state_->response_pending;
	}
	// A verified drain exits only after every readable or queued client frame
	// was given priority over the probe, and after any response can be sent.
	if (drain_ready && !response_pending && state_->client_phase != State::ClientPhase::response_write &&
		state_->client_phase != State::ClientPhase::worker_wait &&
		state_->client_phase != State::ClientPhase::dispatch_wait) { Close(); return ServerResult::ok; }
	return ServerResult::ok;
}

void RuntimeServer::RequestDrain() {
	const int saved_errno = errno;
	state_->drain_signal = 1;
	if (state_->wake_write_fd >= 0) { const char byte = 'd'; (void)write(state_->wake_write_fd, &byte, 1); }
	errno = saved_errno;
}
bool RuntimeServer::draining() const { return state_->drain || state_->drain_signal; }
bool RuntimeServer::running() const { return state_->listener_fd >= 0; }

void RuntimeServer::Close() {
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		state_->stop_worker = true;
		state_->work_available.notify_one();
	}
	if (state_->worker.joinable()) state_->worker.join();
	if (state_->active_client_fd >= 0) { close(state_->active_client_fd); state_->active_client_fd = -1; }
	if (state_->wake_read_fd >= 0) { close(state_->wake_read_fd); state_->wake_read_fd = -1; }
	if (state_->wake_write_fd >= 0) { close(state_->wake_write_fd); state_->wake_write_fd = -1; }
	if (state_->listener_fd >= 0) { close(state_->listener_fd); state_->listener_fd = -1; }
	if (state_->socket_retained && state_->socket_identity_valid && state_->directory_fd >= 0) {
		struct stat current;
		if (fstatat(state_->directory_fd, state_->config.socket_name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
			SameInode(current, state_->socket_stat) && ExactSocket(current, state_->config.service_uid))
			(void)unlinkat(state_->directory_fd, state_->config.socket_name.c_str(), 0);
	}
	state_->socket_retained = false;
	state_->socket_identity_valid = false;
	if (state_->lock_fd >= 0) { (void)flock(state_->lock_fd, LOCK_UN); close(state_->lock_fd); state_->lock_fd = -1; }
	if (state_->directory_fd >= 0) { close(state_->directory_fd); state_->directory_fd = -1; }
}

}  // namespace fogcast
