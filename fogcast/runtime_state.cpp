// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fogcast/runtime_state.hpp"

namespace fogcast {
namespace {

bool ExactTopLevel(const detail::Token& root) {
	static const char* const names[] = {
		"record_version", "protocol", "abi_version", "backend_epoch", "sequence",
		"phase", "mode", "owner", "release_owner", "candidate", "leases",
		"content_lease", "observed_core", "capabilities", "last_error", "in_flight",
		"ledger", "sha256"};
	if (root.kind != detail::Token::Kind::object || root.object.size() != sizeof(names) / sizeof(names[0])) return false;
	for (size_t i = 0; i < root.object.size(); ++i) if (root.object[i].first != names[i]) return false;
	return true;
}

ErrorClass Uint63(const detail::Token* token, uint64_t* value) {
	return detail::PositiveUint63(token, value) ? ErrorClass::ok : ErrorClass::bounds;
}

bool StringOneOf(const detail::Token* token, const char* const* words, size_t count) {
	if (!token || token->kind != detail::Token::Kind::string || token->escaped) return false;
	for (size_t i = 0; i < count; ++i) if (token->text == words[i]) return true;
	return false;
}

bool ExactObject(const detail::Token* value, const char* const* keys, size_t count) {
	if (!value || value->kind != detail::Token::Kind::object || value->object.size() != count) return false;
	for (size_t i = 0; i < count; ++i) if (value->object[i].first != keys[i]) return false;
	return true;
}

bool LowerHex(const detail::Token* value, size_t size) {
	return value && value->kind == detail::Token::Kind::string && !value->escaped && detail::IsLowerHex(value->text, size);
}

bool CoreName(const detail::Token* value) {
	if (!value || value->kind != detail::Token::Kind::string || value->escaped || value->text.empty() || value->text.size() > 64) return false;
	const char first = value->text[0];
	if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) return false;
	for (size_t i = 0; i < value->text.size(); ++i) {
		const char c = value->text[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '+' || c == '(' || c == ')' || c == '.' || c == '-')) return false;
	}
	return true;
}

ErrorClass ValidateOwner(const detail::Token* value, bool nullable) {
	if (nullable && value && value->kind == detail::Token::Kind::null_value) return ErrorClass::ok;
	static const char* const keys[] = {"session", "generation", "mode"};
	static const char* const modes[] = {"fpga_native"};
	if (!ExactObject(value, keys, 3) || !LowerHex(detail::Member(*value, "session"), 32) ||
		Uint63(detail::Member(*value, "generation"), 0) != ErrorClass::ok ||
		!StringOneOf(detail::Member(*value, "mode"), modes, 1)) return ErrorClass::schema;
	return ErrorClass::ok;
}

ErrorClass ValidateSnapshotCandidate(const detail::Token* value, bool nullable) {
	if (nullable && value && value->kind == detail::Token::Kind::null_value) return ErrorClass::ok;
	return ValidateOwner(value, false);
}

ErrorClass ValidateContent(const detail::Token* value) {
	static const char* const keys[] = {"sha256", "size", "extension"};
	if (!ExactObject(value, keys, 3) || !LowerHex(detail::Member(*value, "sha256"), 64) ||
		Uint63(detail::Member(*value, "size"), 0) != ErrorClass::ok) return ErrorClass::bounds;
	uint64_t size = 0;
	if (Uint63(detail::Member(*value, "size"), &size) != ErrorClass::ok || size > 33554432) return ErrorClass::bounds;
	const detail::Token* extension = detail::Member(*value, "extension");
	if (!extension || extension->kind != detail::Token::Kind::string || extension->text.empty() || extension->text.size() > 16) return ErrorClass::bounds;
	for (size_t i = 0; i < extension->text.size(); ++i) if (!((extension->text[i] >= 'a' && extension->text[i] <= 'z') || (extension->text[i] >= '0' && extension->text[i] <= '9'))) return ErrorClass::bounds;
	return ErrorClass::ok;
}

ErrorClass ValidateContentLease(const detail::Token* value) {
	static const char* const keys[] = {"lease_id", "content"};
	if (!ExactObject(value, keys, 2) || !LowerHex(detail::Member(*value, "lease_id"), 32)) return ErrorClass::schema;
	return ValidateContent(detail::Member(*value, "content"));
}

ErrorClass ValidateError(const detail::Token* value) {
	static const char* const keys[] = {"code", "message"};
	static const char* const codes[] = {"PROTOCOL_MISMATCH", "INVALID_REQUEST", "UNAUTHORIZED_PEER", "BUSY", "NOT_READY", "STALE_ADMISSION", "STALE_GENERATION", "IN_PROGRESS", "OPERATION_REPLAY", "OPERATION_UNKNOWN", "INTERRUPTED", "DEADLINE", "RECOVERY_REQUIRED", "UNSUPPORTED_MODE", "OWNER_NOT_FOUND", "INTERNAL"};
	if (!ExactObject(value, keys, 2) || !StringOneOf(detail::Member(*value, "code"), codes, sizeof(codes) / sizeof(codes[0]))) return ErrorClass::schema;
	const detail::Token* message = detail::Member(*value, "message");
	if (!message || message->kind != detail::Token::Kind::string || message->text.size() > 256) return ErrorClass::bounds;
	for (size_t i = 0; i < message->text.size(); ++i) if (!((message->text[i] >= 'A' && message->text[i] <= 'Z') || (message->text[i] >= 'a' && message->text[i] <= 'z') || (message->text[i] >= '0' && message->text[i] <= '9') || message->text[i] == ' ' || message->text[i] == '.' || message->text[i] == ',' || message->text[i] == ':' || message->text[i] == ';' || message->text[i] == '_' || message->text[i] == '+' || message->text[i] == '(' || message->text[i] == ')' || message->text[i] == '=' || message->text[i] == '-')) return ErrorClass::bounds;
	return ErrorClass::ok;
}

ErrorClass ValidateLeases(const detail::Token* leases, const detail::Token* owner) {
	if (!leases || leases->kind != detail::Token::Kind::array || leases->array.size() > 16) return ErrorClass::bounds;
	static const char* const resources[] = {"fpga", "bridges", "core_protocol", "native_video", "native_audio", "core_input", "saves", "content"};
	int previous = -1;
	for (size_t i = 0; i < leases->array.size(); ++i) {
		static const char* const keys[] = {"resource", "session", "generation"};
		const detail::Token& lease = leases->array[i];
		const detail::Token* resource = detail::Member(lease, "resource");
		if (!ExactObject(&lease, keys, 3) || !resource || resource->kind != detail::Token::Kind::string ||
			!LowerHex(detail::Member(lease, "session"), 32) || Uint63(detail::Member(lease, "generation"), 0) != ErrorClass::ok) return ErrorClass::schema;
		int ordinal = -1;
		for (size_t j = 0; j < sizeof(resources) / sizeof(resources[0]); ++j) if (resource->text == resources[j]) ordinal = static_cast<int>(j);
		if (ordinal <= previous) return ErrorClass::schema;
		previous = ordinal;
		if (!owner || owner->kind == detail::Token::Kind::null_value || detail::Member(lease, "session")->text != detail::Member(*owner, "session")->text ||
			detail::Member(lease, "generation")->text != detail::Member(*owner, "generation")->text) return ErrorClass::schema;
	}
	return ErrorClass::ok;
}

ErrorClass ValidateInflight(const detail::Token* value) {
	if (value && value->kind == detail::Token::Kind::null_value) return ErrorClass::ok;
	static const char* const keys[] = {"operation_id", "request_digest", "stage"};
	static const char* const stages[] = {"recorded", "transitioning"};
	if (!ExactObject(value, keys, 3) || !LowerHex(detail::Member(*value, "operation_id"), 32) || !LowerHex(detail::Member(*value, "request_digest"), 64) ||
		!StringOneOf(detail::Member(*value, "stage"), stages, 2)) return ErrorClass::schema;
	return ErrorClass::ok;
}

ErrorClass ValidateSnapshot(const detail::Token& snapshot);

ErrorClass ValidateLedger(const detail::Token* ledger, uint64_t enclosing_sequence,
	bool has_inflight) {
	if (!ledger || ledger->kind != detail::Token::Kind::array) return ErrorClass::schema;
	if (ledger->array.size() > 64) return ErrorClass::bounds;
	uint64_t prior_sequence = 0;
	for (size_t i = 0; i < ledger->array.size(); ++i) {
		static const char* const keys[] = {"operation_id", "request_digest", "ok", "error", "snapshot", "resulting_sequence"};
		const detail::Token& row = ledger->array[i];
		const detail::Token* operation_id = detail::Member(row, "operation_id");
		const detail::Token* request_digest = detail::Member(row, "request_digest");
		const detail::Token* ok = detail::Member(row, "ok");
		if (!ExactObject(&row, keys, 6) || !operation_id || !request_digest || !ok ||
			operation_id->kind != detail::Token::Kind::string || request_digest->kind != detail::Token::Kind::string ||
			!detail::IsLowerHex(operation_id->text, 32) || !detail::IsLowerHex(request_digest->text, 64) ||
			ok->kind != detail::Token::Kind::boolean || Uint63(detail::Member(row, "resulting_sequence"), 0) != ErrorClass::ok) return ErrorClass::schema;
		const detail::Token* error = detail::Member(row, "error");
		const detail::Token* snapshot = detail::Member(row, "snapshot");
		if (!error || !snapshot || snapshot->kind != detail::Token::Kind::object ||
			(ok->text == "true" && error->kind != detail::Token::Kind::null_value) ||
			(ok->text == "false" && (error->kind == detail::Token::Kind::null_value || ValidateError(error) != ErrorClass::ok))) return ErrorClass::schema;
		uint64_t terminal_sequence = 0;
		if (ValidateSnapshot(*snapshot) != ErrorClass::ok || Uint63(detail::Member(row, "resulting_sequence"), &terminal_sequence) != ErrorClass::ok ||
			detail::Member(*snapshot, "sequence")->kind != detail::Token::Kind::number || detail::Member(*snapshot, "sequence")->text != detail::Member(row, "resulting_sequence")->text || (i > 0 && terminal_sequence <= prior_sequence)) return ErrorClass::schema;
		// A terminal written while the current operation is still in flight must
		// be historical.  Once there is no in-flight operation, a ledger entry
		// may describe the enclosing terminal sequence itself.
		if (terminal_sequence > enclosing_sequence ||
			(has_inflight && terminal_sequence >= enclosing_sequence)) return ErrorClass::schema;
		for (size_t earlier = 0; earlier < i; ++earlier) if (detail::Member(ledger->array[earlier], "operation_id")->text == operation_id->text) return ErrorClass::schema;
		prior_sequence = terminal_sequence;
	}
	return ErrorClass::ok;
}

bool CompleteLeases(const detail::Token* leases) { return leases && leases->kind == detail::Token::Kind::array && leases->array.size() == 8; }

bool Same(const detail::Token* left, const detail::Token* right) {
	return left && right && detail::Encode(*left) == detail::Encode(*right);
}

ErrorClass ValidatePhase(const detail::Token& root) {
	const detail::Token* phase = detail::Member(root, "phase");
	const detail::Token* mode = detail::Member(root, "mode");
	const detail::Token* owner = detail::Member(root, "owner");
	const detail::Token* release_owner = detail::Member(root, "release_owner");
	const detail::Token* candidate = detail::Member(root, "candidate");
	const detail::Token* leases = detail::Member(root, "leases");
	const detail::Token* content_lease = detail::Member(root, "content_lease");
	if (phase->text == "idle") {
		if (mode->text != "idle" || owner->kind != detail::Token::Kind::null_value || release_owner->kind != detail::Token::Kind::null_value || candidate->kind != detail::Token::Kind::null_value || !leases->array.empty() || content_lease->kind != detail::Token::Kind::null_value) return ErrorClass::schema;
	} else if (phase->text == "active") {
		if (mode->text != "fpga_native" || owner->kind != detail::Token::Kind::object || release_owner->kind != detail::Token::Kind::null_value || candidate->kind != detail::Token::Kind::null_value || !CompleteLeases(leases) || content_lease->kind != detail::Token::Kind::object) return ErrorClass::schema;
	} else if (phase->text == "intent") {
		if (mode->text != "recovering" || candidate->kind != detail::Token::Kind::object) return ErrorClass::schema;
		if (owner->kind == detail::Token::Kind::null_value) { if (release_owner->kind != detail::Token::Kind::null_value || !leases->array.empty() || content_lease->kind != detail::Token::Kind::null_value) return ErrorClass::schema; }
		else if (release_owner->kind != detail::Token::Kind::object || !Same(owner, release_owner) || !CompleteLeases(leases) || content_lease->kind != detail::Token::Kind::object) return ErrorClass::schema;
	} else if (phase->text == "releasing") {
		// Releasing covers launch/replacement handoff and release-only
		// stop/recover.  Unlike intent, its candidate is optional.
		if (mode->text != "recovering") return ErrorClass::schema;
		if (owner->kind == detail::Token::Kind::null_value) { if (release_owner->kind != detail::Token::Kind::null_value || !leases->array.empty() || content_lease->kind != detail::Token::Kind::null_value) return ErrorClass::schema; }
		else if (release_owner->kind != detail::Token::Kind::object || !Same(owner, release_owner) || !CompleteLeases(leases) || content_lease->kind != detail::Token::Kind::object) return ErrorClass::schema;
	} else if (phase->text == "no_owner") {
		if (mode->text != "recovering" || owner->kind != detail::Token::Kind::null_value || release_owner->kind != detail::Token::Kind::null_value || !leases->array.empty() || content_lease->kind != detail::Token::Kind::null_value) return ErrorClass::schema;
	} else if (phase->text == "transferred") {
		if (mode->text != "recovering" || owner->kind != detail::Token::Kind::object || release_owner->kind != detail::Token::Kind::null_value || candidate->kind != detail::Token::Kind::object || !Same(owner, candidate) || !CompleteLeases(leases) || content_lease->kind != detail::Token::Kind::object) return ErrorClass::schema;
	} else if (phase->text == "unwinding") {
		if (mode->text != "recovering" || owner->kind != detail::Token::Kind::object || release_owner->kind != detail::Token::Kind::object || candidate->kind != detail::Token::Kind::object || !Same(owner, release_owner) || !Same(owner, candidate) || !CompleteLeases(leases) || content_lease->kind != detail::Token::Kind::object) return ErrorClass::schema;
	} else if (phase->text == "failed") {
		if (mode->text != "failed") return ErrorClass::schema;
	}
	return ErrorClass::ok;
}

ErrorClass ValidateSnapshot(const detail::Token& snapshot) {
	static const char* const keys[] = {"sequence", "backend_epoch", "phase", "mode", "owner", "release_owner", "candidate", "leases", "content_lease", "observed_core", "capabilities", "last_error"};
	if (!ExactObject(&snapshot, keys, 12)) return ErrorClass::schema;
	if (Uint63(detail::Member(snapshot, "sequence"), 0) != ErrorClass::ok || Uint63(detail::Member(snapshot, "backend_epoch"), 0) != ErrorClass::ok) return ErrorClass::bounds;
	static const char* const phases[] = {"idle", "intent", "releasing", "no_owner", "transferred", "active", "unwinding", "failed"};
	static const char* const modes[] = {"idle", "fpga_native", "host_cast", "updating", "recovering", "failed"};
	if (!StringOneOf(detail::Member(snapshot, "phase"), phases, 8) || !StringOneOf(detail::Member(snapshot, "mode"), modes, 6) || ValidateOwner(detail::Member(snapshot, "owner"), true) != ErrorClass::ok || ValidateOwner(detail::Member(snapshot, "release_owner"), true) != ErrorClass::ok || ValidateSnapshotCandidate(detail::Member(snapshot, "candidate"), true) != ErrorClass::ok || ValidateLeases(detail::Member(snapshot, "leases"), detail::Member(snapshot, "owner")) != ErrorClass::ok) return ErrorClass::schema;
	const detail::Token* content = detail::Member(snapshot, "content_lease");
	if (content->kind != detail::Token::Kind::null_value && ValidateContentLease(content) != ErrorClass::ok) return ErrorClass::schema;
	const detail::Token* observed = detail::Member(snapshot, "observed_core");
	if (observed->kind != detail::Token::Kind::null_value && !CoreName(observed)) return ErrorClass::bounds;
	const detail::Token* caps = detail::Member(snapshot, "capabilities");
	if (caps->kind != detail::Token::Kind::number || (caps->text != "0" && caps->text != "31")) return ErrorClass::schema;
	const detail::Token* error = detail::Member(snapshot, "last_error");
	if (error->kind != detail::Token::Kind::null_value && ValidateError(error) != ErrorClass::ok) return ErrorClass::schema;
	return ValidatePhase(snapshot);
}

}  // namespace

ErrorClass ParseStateRecord(const std::string& bytes, StateRecord* record,
	std::string* canonical, std::string* digest) {
	if (!record || !canonical || !digest) return ErrorClass::schema;
	canonical->clear();
	digest->clear();
	detail::Token root;
	ErrorClass result = detail::ScanV1Json(bytes, 256 * 1024, &root);
	if (result != ErrorClass::ok) return result;
	if (!ExactTopLevel(root)) return ErrorClass::schema;
	const detail::Token* version = detail::Member(root, "record_version");
	const detail::Token* protocol = detail::Member(root, "protocol");
	const detail::Token* abi = detail::Member(root, "abi_version");
	if (!version || version->kind != detail::Token::Kind::number || version->text != "1" ||
		!protocol || protocol->kind != detail::Token::Kind::number || protocol->text != "1" ||
		!abi || abi->kind != detail::Token::Kind::number || abi->text != "2") return ErrorClass::schema;
	uint64_t sequence = 0;
	uint64_t epoch = 0;
	result = Uint63(detail::Member(root, "backend_epoch"), &epoch);
	if (result != ErrorClass::ok) return result;
	result = Uint63(detail::Member(root, "sequence"), &sequence);
	if (result != ErrorClass::ok) return result;
	static const char* const phases[] = {"idle", "intent", "releasing", "no_owner", "transferred", "active", "unwinding", "failed"};
	static const char* const modes[] = {"idle", "fpga_native", "host_cast", "updating", "recovering", "failed"};
	if (!StringOneOf(detail::Member(root, "phase"), phases, sizeof(phases) / sizeof(phases[0])) ||
		!StringOneOf(detail::Member(root, "mode"), modes, sizeof(modes) / sizeof(modes[0]))) return ErrorClass::schema;
	result = ValidateOwner(detail::Member(root, "owner"), true);
	if (result != ErrorClass::ok) return result;
	result = ValidateOwner(detail::Member(root, "release_owner"), true);
	if (result != ErrorClass::ok) return result;
	result = ValidateSnapshotCandidate(detail::Member(root, "candidate"), true);
	if (result != ErrorClass::ok) return result;
	result = ValidateLeases(detail::Member(root, "leases"), detail::Member(root, "owner"));
	if (result != ErrorClass::ok) return result;
	const detail::Token* content_lease = detail::Member(root, "content_lease");
	if (content_lease->kind != detail::Token::Kind::null_value) {
		result = ValidateContentLease(content_lease);
		if (result != ErrorClass::ok) return result;
	}
	const detail::Token* observed_core = detail::Member(root, "observed_core");
	if (observed_core->kind != detail::Token::Kind::null_value && !CoreName(observed_core)) return ErrorClass::bounds;
	const detail::Token* capabilities = detail::Member(root, "capabilities");
	if (!capabilities || capabilities->kind != detail::Token::Kind::number || (capabilities->text != "0" && capabilities->text != "31")) return ErrorClass::schema;
	const detail::Token* last_error = detail::Member(root, "last_error");
	if (last_error->kind != detail::Token::Kind::null_value && ValidateError(last_error) != ErrorClass::ok) return ErrorClass::schema;
	result = ValidatePhase(root);
	if (result != ErrorClass::ok) return result;
	result = ValidateInflight(detail::Member(root, "in_flight"));
	if (result != ErrorClass::ok) return result;
	const detail::Token* in_flight = detail::Member(root, "in_flight");
	const std::string& phase_name = detail::Member(root, "phase")->text;
	const bool transitional = phase_name == "intent" || phase_name == "releasing" ||
		phase_name == "no_owner" || phase_name == "transferred" || phase_name == "unwinding";
	if (transitional && in_flight->kind != detail::Token::Kind::object) return ErrorClass::schema;
	result = ValidateLedger(detail::Member(root, "ledger"), sequence,
		in_flight->kind == detail::Token::Kind::object);
	if (result != ErrorClass::ok) return result;
	if (in_flight->kind == detail::Token::Kind::object) {
		const std::string& stage = detail::Member(*in_flight, "stage")->text;
		if ((phase_name == "idle" || phase_name == "active" || phase_name == "failed") && stage != "recorded") return ErrorClass::schema;
		if (transitional && stage != "transitioning") return ErrorClass::schema;
		const detail::Token* ledger = detail::Member(root, "ledger");
		for (size_t i = 0; i < ledger->array.size(); ++i) if (detail::Member(ledger->array[i], "operation_id")->text == detail::Member(*in_flight, "operation_id")->text) return ErrorClass::schema;
	}
	const detail::Token* checksum = detail::Member(root, "sha256");
	if (!checksum || checksum->kind != detail::Token::Kind::string || !detail::IsLowerHex(checksum->text, 64)) return ErrorClass::checksum;
	detail::Token copy = root;
	copy.object.pop_back();
	*canonical = detail::Encode(copy);
	*digest = Sha256Hex(*canonical);
	if (*digest != checksum->text) return ErrorClass::checksum;
	record->sequence = sequence;
	record->backend_epoch = epoch;
	record->phase = detail::Member(root, "phase")->text;
	record->canonical = *canonical;
	record->digest = *digest;
	return ErrorClass::ok;
}

}  // namespace fogcast
