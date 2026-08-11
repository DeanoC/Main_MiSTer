// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fogcast/runtime_protocol.hpp"

#include <algorithm>
#include <limits>
#include <string.h>

namespace fogcast {
namespace {

bool ValidUtf8(const std::string& input) {
	for (size_t i = 0; i < input.size();) {
		const unsigned char c = static_cast<unsigned char>(input[i]);
		if (c < 0x80) { ++i; continue; }
		unsigned int count = 0;
		uint32_t value = 0;
		if (c >= 0xc2 && c <= 0xdf) { count = 2; value = c & 0x1f; }
		else if (c >= 0xe0 && c <= 0xef) { count = 3; value = c & 0x0f; }
		else if (c >= 0xf0 && c <= 0xf4) { count = 4; value = c & 0x07; }
		else return false;
		if (i + count > input.size()) return false;
		for (unsigned int j = 1; j < count; ++j) {
			const unsigned char next = static_cast<unsigned char>(input[i + j]);
			if ((next & 0xc0) != 0x80) return false;
			value = (value << 6) | (next & 0x3f);
		}
		if ((count == 3 && value < 0x800) || (count == 4 && value < 0x10000) ||
			value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
		i += count;
	}
	return true;
}

class Scanner {
public:
	explicit Scanner(const std::string& input) : input_(input), at_(0), nodes_(0) {}

	ErrorClass Parse(size_t cap, detail::Token* output) {
		if (input_.size() > cap) return ErrorClass::bounds;
		if (!ValidUtf8(input_)) return ErrorClass::utf8;
		SkipSpace();
		const ErrorClass result = Value(0, output);
		if (result != ErrorClass::ok) return result;
		SkipSpace();
		return at_ == input_.size() ? ErrorClass::ok : ErrorClass::json;
	}

private:
	void SkipSpace() {
		while (at_ < input_.size() && (input_[at_] == ' ' || input_[at_] == '\n' ||
			input_[at_] == '\r' || input_[at_] == '\t')) ++at_;
	}
	bool Take(char character) {
		SkipSpace();
		if (at_ >= input_.size() || input_[at_] != character) return false;
		++at_;
		return true;
	}
	ErrorClass String(std::string* text, bool* escaped) {
		SkipSpace();
		if (at_ >= input_.size() || input_[at_] != '\"') return ErrorClass::json;
		++at_;
		text->clear();
		*escaped = false;
		while (at_ < input_.size()) {
			const unsigned char c = static_cast<unsigned char>(input_[at_++]);
			if (c == '\"') return ErrorClass::ok;
			if (c < 0x20) return ErrorClass::json;
			if (c == '\\') {
				*escaped = true;
				if (at_ >= input_.size()) return ErrorClass::json;
				const char escape = input_[at_++];
				if (escape == '\"' || escape == '\\' || escape == '/') text->push_back(escape);
				else if (escape == 'b') text->push_back('\b');
				else if (escape == 'f') text->push_back('\f');
				else if (escape == 'n') text->push_back('\n');
				else if (escape == 'r') text->push_back('\r');
				else if (escape == 't') text->push_back('\t');
				else return ErrorClass::json;  // v1 never permits \u spellings.
			} else text->push_back(static_cast<char>(c));
		}
		return ErrorClass::json;
	}
	ErrorClass Number(detail::Token* output) {
		SkipSpace();
		const size_t start = at_;
		if (at_ < input_.size() && input_[at_] == '-') ++at_;
		if (at_ >= input_.size() || input_[at_] < '0' || input_[at_] > '9') return ErrorClass::json;
		if (input_[at_] == '0') {
			++at_;
			if (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9') return ErrorClass::json;
		} else {
			while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9') ++at_;
		}
		if (at_ < input_.size() && (input_[at_] == '.' || input_[at_] == 'e' || input_[at_] == 'E')) return ErrorClass::json;
		output->kind = detail::Token::Kind::number;
		output->text = input_.substr(start, at_ - start);
		return ErrorClass::ok;
	}
	ErrorClass Value(unsigned int depth, detail::Token* output) {
		if (++nodes_ > 8192 || depth > 32) return ErrorClass::bounds;
		SkipSpace();
		if (at_ >= input_.size()) return ErrorClass::json;
		if (input_[at_] == '\"') {
			output->kind = detail::Token::Kind::string;
			return String(&output->text, &output->escaped);
		}
		if (input_[at_] == '{') return Object(depth, output);
		if (input_[at_] == '[') return Array(depth, output);
		if (input_.compare(at_, 4, "true") == 0) { at_ += 4; output->kind = detail::Token::Kind::boolean; output->text = "true"; return ErrorClass::ok; }
		if (input_.compare(at_, 5, "false") == 0) { at_ += 5; output->kind = detail::Token::Kind::boolean; output->text = "false"; return ErrorClass::ok; }
		if (input_.compare(at_, 4, "null") == 0) { at_ += 4; output->kind = detail::Token::Kind::null_value; output->text = "null"; return ErrorClass::ok; }
		return Number(output);
	}
	ErrorClass Object(unsigned int depth, detail::Token* output) {
		if (!Take('{')) return ErrorClass::json;
		output->kind = detail::Token::Kind::object;
		output->object.clear();
		SkipSpace();
		if (Take('}')) return ErrorClass::ok;
		for (;;) {
			std::string key;
			bool escaped = false;
			ErrorClass result = String(&key, &escaped);
			if (result != ErrorClass::ok || escaped || !Take(':')) return ErrorClass::json;
			for (size_t i = 0; i < output->object.size(); ++i) if (output->object[i].first == key) return ErrorClass::json;
			detail::Token value;
			result = Value(depth + 1, &value);
			if (result != ErrorClass::ok) return result;
			output->object.push_back(std::make_pair(key, value));
			if (Take('}')) return ErrorClass::ok;
			if (!Take(',')) return ErrorClass::json;
		}
	}
	ErrorClass Array(unsigned int depth, detail::Token* output) {
		if (!Take('[')) return ErrorClass::json;
		output->kind = detail::Token::Kind::array;
		output->array.clear();
		SkipSpace();
		if (Take(']')) return ErrorClass::ok;
		for (;;) {
			detail::Token item;
			const ErrorClass result = Value(depth + 1, &item);
			if (result != ErrorClass::ok) return result;
			output->array.push_back(item);
			if (Take(']')) return ErrorClass::ok;
			if (!Take(',')) return ErrorClass::json;
		}
	}

	const std::string& input_;
	size_t at_;
	size_t nodes_;
};

bool Keys(const detail::Token& value, const char* const* keys, size_t count) {
	if (value.kind != detail::Token::Kind::object || value.object.size() != count) return false;
	for (size_t i = 0; i < count; ++i) if (detail::Member(value, keys[i]) == 0) return false;
	return true;
}

// Wire responses are signed/recorded protocol artifacts, not loosely ordered
// JSON maps.  Requests remain order-independent and are canonicalized below;
// producer-originated records must already use the normative member order.
bool ExactKeys(const detail::Token& value, const char* const* keys, size_t count) {
	if (value.kind != detail::Token::Kind::object || value.object.size() != count) return false;
	for (size_t i = 0; i < count; ++i) if (value.object[i].first != keys[i]) return false;
	return true;
}

bool EmptyObject(const detail::Token* value) { return value && value->kind == detail::Token::Kind::object && value->object.empty(); }
bool IsString(const detail::Token* value) { return value && value->kind == detail::Token::Kind::string && !value->escaped; }
bool IsNull(const detail::Token* value) { return value && value->kind == detail::Token::Kind::null_value; }
bool StringOneOf(const detail::Token* value, const char* const* words, size_t count) {
	if (!IsString(value)) return false;
	for (size_t i = 0; i < count; ++i) if (value->text == words[i]) return true;
	return false;
}

detail::Token OrderedObject(const detail::Token& source, const char* const* keys, size_t count) {
	detail::Token result;
	result.kind = detail::Token::Kind::object;
	for (size_t i = 0; i < count; ++i) result.object.push_back(std::make_pair(std::string(keys[i]), *detail::Member(source, keys[i])));
	return result;
}

detail::Token CanonicalOwner(const detail::Token& source) {
	if (source.kind == detail::Token::Kind::null_value) return source;
	const char* const keys[] = {"session", "generation", "mode"};
	return OrderedObject(source, keys, 3);
}

detail::Token CanonicalCandidate(const detail::Token& source) {
	const char* const keys[] = {"session", "generation"};
	return OrderedObject(source, keys, 2);
}

detail::Token CanonicalRequest(const detail::Token& root, const std::string& operation) {
	detail::Token result;
	result.kind = detail::Token::Kind::object;
	result.object.push_back(std::make_pair("protocol", *detail::Member(root, "protocol")));
	result.object.push_back(std::make_pair("operation_id", *detail::Member(root, "operation_id")));
	result.object.push_back(std::make_pair("operation", *detail::Member(root, "operation")));
	if (operation == "launch") {
		result.object.push_back(std::make_pair("candidate", CanonicalCandidate(*detail::Member(root, "candidate"))));
	}
	if (operation == "stop" || operation == "recover") result.object.push_back(std::make_pair("owner", CanonicalOwner(*detail::Member(root, "owner"))));
	const detail::Token& precondition = *detail::Member(root, "precondition");
	const char* const precondition_keys[] = {"sequence", "owner"};
	detail::Token canonical_precondition = OrderedObject(precondition, precondition_keys, 2);
	canonical_precondition.object[1].second = CanonicalOwner(canonical_precondition.object[1].second);
	result.object.push_back(std::make_pair("precondition", canonical_precondition));
	const detail::Token& body = *detail::Member(root, "body");
	if (operation == "launch") {
		const char* const body_keys[] = {"game_id", "system", "expected_core", "cache_lease_id", "content"};
		detail::Token canonical_body = OrderedObject(body, body_keys, 5);
		const char* const content_keys[] = {"sha256", "size", "extension"};
		canonical_body.object[4].second = OrderedObject(canonical_body.object[4].second, content_keys, 3);
		result.object.push_back(std::make_pair("body", canonical_body));
	} else if (operation == "recover") {
		const char* const body_keys[] = {"reason"};
		result.object.push_back(std::make_pair("body", OrderedObject(body, body_keys, 1)));
	} else result.object.push_back(std::make_pair("body", body));
	return result;
}

ErrorClass Number(const detail::Token* value, uint64_t maximum, bool positive, uint64_t* result) {
	if (!value || value->kind != detail::Token::Kind::number || value->text.empty()) return ErrorClass::schema;
	if (value->text[0] == '-') return ErrorClass::bounds;
	uint64_t number = 0;
	for (size_t i = 0; i < value->text.size(); ++i) {
		const char c = value->text[i];
		if (c < '0' || c > '9' || number > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(c - '0')) / 10) return ErrorClass::bounds;
		number = number * 10 + static_cast<uint64_t>(c - '0');
	}
	if ((positive && number == 0) || number > maximum) return ErrorClass::bounds;
	if (result) *result = number;
	return ErrorClass::ok;
}

bool GameID(const std::string& text) {
	if (text.empty() || text.size() > 128 || !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= '0' && text[0] <= '9'))) return false;
	if (text.size() > 1 && (text[text.size() - 1] < 'a' || text[text.size() - 1] > 'z') &&
		(text[text.size() - 1] < '0' || text[text.size() - 1] > '9')) return false;
	for (size_t i = 0; i < text.size(); ++i) if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= '0' && text[i] <= '9') || text[i] == '-')) return false;
	return true;
}

bool SystemID(const std::string& text) {
	if (text.empty() || text.size() > 32 || !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= '0' && text[0] <= '9'))) return false;
	for (size_t i = 0; i < text.size(); ++i) if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= '0' && text[i] <= '9') || text[i] == '_' || text[i] == '-')) return false;
	return true;
}

bool CoreName(const std::string& text) {
	if (text.empty() || text.size() > 64 || !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= '0' && text[0] <= '9'))) return false;
	for (size_t i = 0; i < text.size(); ++i) if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= '0' && text[i] <= '9') || text[i] == ' ' || text[i] == '_' || text[i] == '+' || text[i] == '(' || text[i] == ')' || text[i] == '.' || text[i] == '-')) return false;
	return true;
}

ErrorClass Hex(const detail::Token* value, size_t size) {
	return IsString(value) && detail::IsLowerHex(value->text, size) ? ErrorClass::ok : ErrorClass::bounds;
}

ErrorClass Owner(const detail::Token* value, bool nullable) {
	if (nullable && IsNull(value)) return ErrorClass::ok;
	const char* const keys[] = {"session", "generation", "mode"};
	if (!value || !Keys(*value, keys, 3)) return ErrorClass::schema;
	ErrorClass result = Hex(detail::Member(*value, "session"), 32);
	if (result != ErrorClass::ok) return result;
	result = Number(detail::Member(*value, "generation"), 0x7fffffffffffffffULL, true, 0);
	if (result != ErrorClass::ok) return result;
	const detail::Token* mode = detail::Member(*value, "mode");
	return IsString(mode) && mode->text == "fpga_native" ? ErrorClass::ok : ErrorClass::schema;
}

ErrorClass Candidate(const detail::Token* value, bool nullable) {
	if (nullable && IsNull(value)) return ErrorClass::ok;
	const char* const keys[] = {"session", "generation"};
	if (!value || !Keys(*value, keys, 2)) return ErrorClass::schema;
	ErrorClass result = Hex(detail::Member(*value, "session"), 32);
	if (result != ErrorClass::ok) return result;
	return Number(detail::Member(*value, "generation"), 0x7fffffffffffffffULL, true, 0);
}

ErrorClass Content(const detail::Token* value) {
	const char* const keys[] = {"sha256", "size", "extension"};
	if (!value || !Keys(*value, keys, 3)) return ErrorClass::schema;
	ErrorClass result = Hex(detail::Member(*value, "sha256"), 64);
	if (result != ErrorClass::ok) return result;
	result = Number(detail::Member(*value, "size"), 33554432, true, 0);
	if (result != ErrorClass::ok) return result;
	const detail::Token* extension = detail::Member(*value, "extension");
	if (!IsString(extension) || extension->text.empty() || extension->text.size() > 16) return ErrorClass::bounds;
	for (size_t i = 0; i < extension->text.size(); ++i) if (!((extension->text[i] >= 'a' && extension->text[i] <= 'z') || (extension->text[i] >= '0' && extension->text[i] <= '9'))) return ErrorClass::bounds;
	return ErrorClass::ok;
}

ErrorClass ErrorObject(const detail::Token* value) {
	const char* const keys[] = {"code", "message"};
	const char* const codes[] = {"PROTOCOL_MISMATCH", "INVALID_REQUEST", "UNAUTHORIZED_PEER", "BUSY", "NOT_READY", "STALE_ADMISSION", "STALE_GENERATION", "IN_PROGRESS", "OPERATION_REPLAY", "OPERATION_UNKNOWN", "INTERRUPTED", "DEADLINE", "RECOVERY_REQUIRED", "UNSUPPORTED_MODE", "OWNER_NOT_FOUND", "INTERNAL"};
	if (!value || !ExactKeys(*value, keys, 2) || !StringOneOf(detail::Member(*value, "code"), codes, sizeof(codes) / sizeof(codes[0]))) return ErrorClass::schema;
	const detail::Token* message = detail::Member(*value, "message");
	if (!IsString(message) || message->text.size() > 256) return ErrorClass::bounds;
	for (size_t i = 0; i < message->text.size(); ++i) if (!((message->text[i] >= 'A' && message->text[i] <= 'Z') || (message->text[i] >= 'a' && message->text[i] <= 'z') || (message->text[i] >= '0' && message->text[i] <= '9') || message->text[i] == ' ' || message->text[i] == '.' || message->text[i] == ',' || message->text[i] == ':' || message->text[i] == ';' || message->text[i] == '_' || message->text[i] == '+' || message->text[i] == '(' || message->text[i] == ')' || message->text[i] == '=' || message->text[i] == '-')) return ErrorClass::bounds;
	return ErrorClass::ok;
}

ErrorClass Precondition(const detail::Token* value) {
	const char* const keys[] = {"sequence", "owner"};
	if (!value || !Keys(*value, keys, 2)) return ErrorClass::schema;
	ErrorClass result = Number(detail::Member(*value, "sequence"), 0x7fffffffffffffffULL, true, 0);
	if (result != ErrorClass::ok) return result;
	return Owner(detail::Member(*value, "owner"), true);
}

ErrorClass Request(const detail::Token& root, std::string* canonical, std::string* digest) {
	const detail::Token* protocol = detail::Member(root, "protocol");
	const detail::Token* request_id = detail::Member(root, "request_id");
	const detail::Token* operation = detail::Member(root, "operation");
	const detail::Token* body = detail::Member(root, "body");
	if (!protocol || !request_id || !IsString(operation) || !body ||
		Number(protocol, 1, true, 0) != ErrorClass::ok || protocol->text != "1") return ErrorClass::schema;
	ErrorClass result = Number(request_id, 0x7fffffffffffffffULL, true, 0);
	if (result != ErrorClass::ok) return result;
	const std::string& op = operation->text;
	bool mutating = op == "launch" || op == "stop" || op == "recover" || op == "shutdown";
	if (op != "hello" && op != "health" && op != "status" && op != "operation_status" && !mutating) return ErrorClass::schema;
	if (op == "hello" || op == "health" || op == "status") {
		const char* const keys[] = {"protocol", "request_id", "operation", "body"};
		if (!Keys(root, keys, 4) || !EmptyObject(body)) return ErrorClass::schema;
		*canonical = detail::Encode(root);
		return ErrorClass::ok;
	}
	if (op == "operation_status") {
		const char* const keys[] = {"protocol", "request_id", "operation", "body"};
		const char* const body_keys[] = {"operation_id"};
		if (!Keys(root, keys, 4) || !Keys(*body, body_keys, 1)) return ErrorClass::schema;
		result = Hex(detail::Member(*body, "operation_id"), 32);
		if (result == ErrorClass::ok) *canonical = detail::Encode(root);
		return result;
	}
	const detail::Token* operation_id = detail::Member(root, "operation_id");
	const detail::Token* precondition = detail::Member(root, "precondition");
	result = Hex(operation_id, 32);
	if (result != ErrorClass::ok) return result;
	result = Precondition(precondition);
	if (result != ErrorClass::ok) return result;
	if (op == "launch") {
		const char* const keys[] = {"protocol", "request_id", "operation_id", "operation", "candidate", "precondition", "body"};
		const char* const body_keys[] = {"game_id", "system", "expected_core", "cache_lease_id", "content"};
		if (!Keys(root, keys, 7) || !Keys(*body, body_keys, 5)) return ErrorClass::schema;
		result = Candidate(detail::Member(root, "candidate"), false);
		if (result != ErrorClass::ok) return result;
		const detail::Token* game = detail::Member(*body, "game_id");
		const detail::Token* system = detail::Member(*body, "system");
		const detail::Token* core = detail::Member(*body, "expected_core");
		if (!IsString(game) || !GameID(game->text) || !IsString(system) || !SystemID(system->text) || !IsString(core) || !CoreName(core->text)) return ErrorClass::bounds;
		result = Hex(detail::Member(*body, "cache_lease_id"), 32);
		if (result != ErrorClass::ok) return result;
		result = Content(detail::Member(*body, "content"));
		if (result != ErrorClass::ok) return result;
	} else if (op == "stop") {
		const char* const keys[] = {"protocol", "request_id", "operation_id", "operation", "owner", "precondition", "body"};
		if (!Keys(root, keys, 7) || !EmptyObject(body)) return ErrorClass::schema;
		result = Owner(detail::Member(root, "owner"), false);
		if (result != ErrorClass::ok) return result;
		if (detail::Encode(CanonicalOwner(*detail::Member(root, "owner"))) != detail::Encode(CanonicalOwner(*detail::Member(*precondition, "owner")))) return ErrorClass::schema;
	} else if (op == "recover") {
		const char* const keys[] = {"protocol", "request_id", "operation_id", "operation", "owner", "precondition", "body"};
		const char* const body_keys[] = {"reason"};
		if (!Keys(root, keys, 7) || Owner(detail::Member(root, "owner"), true) != ErrorClass::ok || !Keys(*body, body_keys, 1) || !IsString(detail::Member(*body, "reason")) ||
			(detail::Member(*body, "reason")->text != "ambiguous" && detail::Member(*body, "reason")->text != "failed")) return ErrorClass::schema;
		if (detail::Encode(CanonicalOwner(*detail::Member(root, "owner"))) != detail::Encode(CanonicalOwner(*detail::Member(*precondition, "owner")))) return ErrorClass::schema;
	} else {
		const char* const keys[] = {"protocol", "request_id", "operation_id", "operation", "precondition", "body"};
		if (!Keys(root, keys, 6) || !EmptyObject(body) || !IsNull(detail::Member(*precondition, "owner"))) return ErrorClass::schema;
	}
	*canonical = detail::Encode(CanonicalRequest(root, op));
	*digest = Sha256Hex(*canonical);
	return ErrorClass::ok;
}

bool SnapshotShape(const detail::Token& snapshot, ErrorClass* error) {
	if (snapshot.kind != detail::Token::Kind::object) { *error = ErrorClass::schema; return false; }
	const char* const snapshot_keys[] = {"sequence", "backend_epoch", "phase", "mode", "owner", "release_owner", "candidate", "leases", "content_lease", "observed_core", "capabilities", "last_error"};
	if (!ExactKeys(snapshot, snapshot_keys, 12)) { *error = ErrorClass::schema; return false; }
	if (detail::Encode(snapshot).size() > 2048) { *error = ErrorClass::bounds; return false; }
	const detail::Token* sequence = detail::Member(snapshot, "sequence");
	const detail::Token* epoch = detail::Member(snapshot, "backend_epoch");
	if (Number(sequence, 0x7fffffffffffffffULL, true, 0) != ErrorClass::ok || Number(epoch, 0x7fffffffffffffffULL, true, 0) != ErrorClass::ok) { *error = ErrorClass::bounds; return false; }
	const detail::Token* capabilities = detail::Member(snapshot, "capabilities");
	if (!capabilities || capabilities->kind != detail::Token::Kind::number || (capabilities->text != "0" && capabilities->text != "31")) { *error = ErrorClass::schema; return false; }
	const detail::Token* observed = detail::Member(snapshot, "observed_core");
	if (!IsNull(observed) && (!IsString(observed) || !CoreName(observed->text))) { *error = ErrorClass::bounds; return false; }
	const detail::Token* leases = detail::Member(snapshot, "leases");
	if (!leases || leases->kind != detail::Token::Kind::array || leases->array.size() > 16) { *error = ErrorClass::bounds; return false; }
	const detail::Token* owner = detail::Member(snapshot, "owner");
	const detail::Token* release_owner = detail::Member(snapshot, "release_owner");
	const detail::Token* candidate = detail::Member(snapshot, "candidate");
	const char* const owner_keys[] = {"session", "generation", "mode"};
	static const char* const phases[] = {"idle", "intent", "releasing", "no_owner", "transferred", "active", "unwinding", "failed"};
	static const char* const modes[] = {"idle", "fpga_native", "host_cast", "updating", "recovering", "failed"};
	if (!StringOneOf(detail::Member(snapshot, "phase"), phases, sizeof(phases) / sizeof(phases[0])) || !StringOneOf(detail::Member(snapshot, "mode"), modes, sizeof(modes) / sizeof(modes[0]))) { *error = ErrorClass::schema; return false; }
	if ((!IsNull(owner) && !ExactKeys(*owner, owner_keys, 3)) || Owner(owner, true) != ErrorClass::ok) { *error = ErrorClass::schema; return false; }
	if ((!IsNull(release_owner) && !ExactKeys(*release_owner, owner_keys, 3)) || Owner(release_owner, true) != ErrorClass::ok) { *error = ErrorClass::schema; return false; }
	if (!IsNull(candidate) && (!ExactKeys(*candidate, owner_keys, 3) || Owner(candidate, false) != ErrorClass::ok)) { *error = ErrorClass::schema; return false; }
	const detail::Token* content_lease = detail::Member(snapshot, "content_lease");
	if (!IsNull(content_lease)) {
		const char* const content_lease_keys[] = {"lease_id", "content"};
		const char* const content_keys[] = {"sha256", "size", "extension"};
		const detail::Token* content = detail::Member(*content_lease, "content");
		if (!ExactKeys(*content_lease, content_lease_keys, 2) || !content || !ExactKeys(*content, content_keys, 3) || Hex(detail::Member(*content_lease, "lease_id"), 32) != ErrorClass::ok || Content(content) != ErrorClass::ok) { *error = ErrorClass::schema; return false; }
	}
	const detail::Token* last_error = detail::Member(snapshot, "last_error");
	if (!IsNull(last_error) && ErrorObject(last_error) != ErrorClass::ok) { *error = ErrorClass::schema; return false; }
	const detail::Token* owner_session = IsNull(owner) ? 0 : detail::Member(*owner, "session");
	const detail::Token* owner_generation = IsNull(owner) ? 0 : detail::Member(*owner, "generation");
	static const char* const names[] = {"fpga", "bridges", "core_protocol", "native_video", "native_audio", "core_input", "saves", "content"};
	int prior = -1;
	for (size_t i = 0; i < leases->array.size(); ++i) {
		const detail::Token& lease = leases->array[i];
		const char* const keys[] = {"resource", "session", "generation"};
		if (!ExactKeys(lease, keys, 3) || !IsString(detail::Member(lease, "resource")) || Hex(detail::Member(lease, "session"), 32) != ErrorClass::ok || Number(detail::Member(lease, "generation"), 0x7fffffffffffffffULL, true, 0) != ErrorClass::ok) { *error = ErrorClass::schema; return false; }
		int current = -1;
		for (size_t j = 0; j < sizeof(names) / sizeof(names[0]); ++j) if (detail::Member(lease, "resource")->text == names[j]) current = static_cast<int>(j);
		if (current < 0 || current <= prior) { *error = ErrorClass::schema; return false; }
		if (!owner_session || detail::Member(lease, "session")->text != owner_session->text || detail::Member(lease, "generation")->text != owner_generation->text) { *error = ErrorClass::schema; return false; }
		prior = current;
	}
	const std::string& phase = detail::Member(snapshot, "phase")->text;
	const std::string& mode = detail::Member(snapshot, "mode")->text;
	const bool owner_present = !IsNull(owner);
	const bool candidate_present = !IsNull(candidate);
	const bool full = leases->array.size() == 8 && !IsNull(content_lease);
	const bool empty = leases->array.empty() && IsNull(content_lease);
	const bool same_release = owner_present && detail::Encode(*owner) == detail::Encode(*release_owner);
	const bool same_candidate = owner_present && candidate_present && detail::Encode(*owner) == detail::Encode(*candidate);
	if (phase == "idle" && (mode != "idle" || owner_present || !IsNull(release_owner) || candidate_present || !empty)) { *error = ErrorClass::schema; return false; }
	if (phase == "active" && (mode != "fpga_native" || !owner_present || !IsNull(release_owner) || candidate_present || !full)) { *error = ErrorClass::schema; return false; }
	if ((phase == "intent" || phase == "releasing") &&
		(mode != "recovering" || !candidate_present ||
		 (owner_present ? (!same_release || !full) : (!IsNull(release_owner) || !empty)))) { *error = ErrorClass::schema; return false; }
	if (phase == "no_owner" && (mode != "recovering" || owner_present || !IsNull(release_owner) || !empty)) { *error = ErrorClass::schema; return false; }
	if (phase == "transferred" && (mode != "recovering" || !owner_present || !IsNull(release_owner) || !same_candidate || !full)) { *error = ErrorClass::schema; return false; }
	if (phase == "unwinding" && (mode != "recovering" || !owner_present || !same_release || !same_candidate || !full)) { *error = ErrorClass::schema; return false; }
	if (phase == "failed" && mode != "failed") { *error = ErrorClass::schema; return false; }
	*error = ErrorClass::ok;
	return true;
}

ErrorClass Response(const detail::Token& root, std::string* canonical) {
	const char* const keys_ok[] = {"protocol", "request_id", "ok", "result", "snapshot"};
	const char* const keys_error[] = {"protocol", "request_id", "ok", "error", "snapshot"};
	const char* const keys_operation_ok[] = {"protocol", "request_id", "operation_id", "ok", "result", "snapshot"};
	const char* const keys_operation_error[] = {"protocol", "request_id", "operation_id", "ok", "error", "snapshot"};
	const detail::Token* ok = detail::Member(root, "ok");
	if (!ok || ok->kind != detail::Token::Kind::boolean || !detail::Member(root, "protocol") || !detail::Member(root, "request_id")) return ErrorClass::schema;
	if (Number(detail::Member(root, "protocol"), 1, true, 0) != ErrorClass::ok || detail::Member(root, "protocol")->text != "1") return ErrorClass::schema;
	ErrorClass result = Number(detail::Member(root, "request_id"), 0x7fffffffffffffffULL, true, 0);
	if (result != ErrorClass::ok) return result;
	const bool has_operation_id = detail::Member(root, "operation_id") != 0;
	if ((ok->text == "true" && !(has_operation_id ? ExactKeys(root, keys_operation_ok, 6) : ExactKeys(root, keys_ok, 5))) ||
		(ok->text == "false" && !(has_operation_id ? ExactKeys(root, keys_operation_error, 6) : ExactKeys(root, keys_error, 5)))) return ErrorClass::schema;
	if (has_operation_id && Hex(detail::Member(root, "operation_id"), 32) != ErrorClass::ok) return ErrorClass::bounds;
	const detail::Token* snapshot = detail::Member(root, "snapshot");
	if (!IsNull(snapshot)) { ErrorClass snapshot_error = ErrorClass::ok; if (!SnapshotShape(*snapshot, &snapshot_error)) return snapshot_error; }
	if (ok->text == "true") {
		const detail::Token* body = detail::Member(root, "result");
		if (!body || body->kind != detail::Token::Kind::object) return ErrorClass::schema;
		if (body->object.empty() && IsNull(snapshot)) return ErrorClass::schema;
		const detail::Token* capabilities = detail::Member(*body, "capabilities");
		if (capabilities) {
			if (!detail::Member(*body, "abi_version")) {
				const char* const health_keys[] = {"ready", "phase", "mode", "capabilities"};
				if (!ExactKeys(*body, health_keys, 4) || !detail::Member(*body, "ready") || detail::Member(*body, "ready")->kind != detail::Token::Kind::boolean ||
					(!IsString(detail::Member(*body, "phase")) && !IsNull(detail::Member(*body, "phase"))) || (!IsString(detail::Member(*body, "mode")) && !IsNull(detail::Member(*body, "mode"))) || capabilities->kind != detail::Token::Kind::number || (capabilities->text != "0" && capabilities->text != "31")) return ErrorClass::schema;
				if ((detail::Member(*body, "ready")->text == "true") != (capabilities->text == "31")) return ErrorClass::schema;
				if (IsNull(snapshot)) {
					if (detail::Member(*body, "ready")->text != "false" || !IsNull(detail::Member(*body, "phase")) || !IsNull(detail::Member(*body, "mode")) || capabilities->text != "0") return ErrorClass::schema;
				} else if (!IsString(detail::Member(*body, "phase")) || !IsString(detail::Member(*body, "mode")) || detail::Member(*body, "phase")->text != detail::Member(*snapshot, "phase")->text || detail::Member(*body, "mode")->text != detail::Member(*snapshot, "mode")->text || capabilities->text != detail::Member(*snapshot, "capabilities")->text) return ErrorClass::schema;
			} else {
			const char* const hello_keys[] = {"protocol", "abi_version", "ready", "max_frame_bytes", "max_connections", "max_replay_entries", "capabilities"};
			if (!ExactKeys(*body, hello_keys, 7) || !detail::Member(*body, "ready") || detail::Member(*body, "ready")->kind != detail::Token::Kind::boolean ||
				detail::Member(*body, "protocol")->kind != detail::Token::Kind::number || detail::Member(*body, "protocol")->text != "1" ||
				detail::Member(*body, "abi_version")->kind != detail::Token::Kind::number || detail::Member(*body, "abi_version")->text != "2" ||
				detail::Member(*body, "max_frame_bytes")->kind != detail::Token::Kind::number || detail::Member(*body, "max_frame_bytes")->text != "65536" ||
				detail::Member(*body, "max_connections")->kind != detail::Token::Kind::number || detail::Member(*body, "max_connections")->text != "1" ||
				detail::Member(*body, "max_replay_entries")->kind != detail::Token::Kind::number || detail::Member(*body, "max_replay_entries")->text != "64" ||
				capabilities->kind != detail::Token::Kind::number || (capabilities->text != "0" && capabilities->text != "31")) return ErrorClass::schema;
			if ((detail::Member(*body, "ready")->text == "true") != (capabilities->text == "31")) return ErrorClass::schema;
			if (!IsNull(snapshot)) return ErrorClass::schema;
			}
		}
		if (detail::Member(*body, "request_digest")) {
			const char* const status_keys[] = {"operation_id", "request_digest", "state", "terminal"};
			if (!ExactKeys(*body, status_keys, 4) || Hex(detail::Member(*body, "operation_id"), 32) != ErrorClass::ok || Hex(detail::Member(*body, "request_digest"), 64) != ErrorClass::ok || !IsString(detail::Member(*body, "state"))) return ErrorClass::bounds;
			if (detail::Member(*body, "state")->text != "in_progress" && detail::Member(*body, "state")->text != "completed") return ErrorClass::schema;
			if (detail::Member(*body, "state")->text == "in_progress" && !IsNull(detail::Member(*body, "terminal"))) return ErrorClass::schema;
			if (detail::Member(*body, "state")->text == "completed") {
				const detail::Token* terminal = detail::Member(*body, "terminal");
				if (IsNull(snapshot)) return ErrorClass::schema;
				const char* const terminal_keys[] = {"ok", "error", "snapshot", "resulting_sequence"};
				if (!terminal || !ExactKeys(*terminal, terminal_keys, 4) || detail::Member(*terminal, "ok")->kind != detail::Token::Kind::boolean || IsNull(detail::Member(*terminal, "snapshot")) || Number(detail::Member(*terminal, "resulting_sequence"), 0x7fffffffffffffffULL, true, 0) != ErrorClass::ok) return ErrorClass::schema;
				ErrorClass terminal_snapshot_error = ErrorClass::ok;
				if (!SnapshotShape(*detail::Member(*terminal, "snapshot"), &terminal_snapshot_error) || detail::Member(*terminal, "resulting_sequence")->text != detail::Member(*detail::Member(*terminal, "snapshot"), "sequence")->text) return ErrorClass::schema;
				const detail::Token* terminal_error = detail::Member(*terminal, "error");
				if ((detail::Member(*terminal, "ok")->text == "true" && !IsNull(terminal_error)) || (detail::Member(*terminal, "ok")->text == "false" && ErrorObject(terminal_error) != ErrorClass::ok)) return ErrorClass::schema;
			}
		} else if (detail::Member(*body, "resulting_sequence")) {
			const detail::Token* sequence = detail::Member(*body, "resulting_sequence");
			if (IsNull(snapshot) || Number(sequence, 0x7fffffffffffffffULL, true, 0) != ErrorClass::ok || sequence->text != detail::Member(*snapshot, "sequence")->text) return ErrorClass::schema;
			const detail::Token* quiesced = detail::Member(*body, "quiesced");
			if (quiesced && (quiesced->kind != detail::Token::Kind::boolean || quiesced->text != "true")) return ErrorClass::schema;
			const char* const mutation_keys[] = {"resulting_sequence"};
			const char* const mutation_quiesced_keys[] = {"resulting_sequence", "quiesced"};
			if (!(quiesced ? ExactKeys(*body, mutation_quiesced_keys, 2) : ExactKeys(*body, mutation_keys, 1))) return ErrorClass::schema;
		} else if (!capabilities && !body->object.empty()) return ErrorClass::schema;
		}
	if (ok->text == "false") {
		const detail::Token* error = detail::Member(root, "error");
		if (ErrorObject(error) != ErrorClass::ok) return ErrorObject(error);
	}
	*canonical = detail::Encode(root);
	return ErrorClass::ok;
}

// Compact public-domain style SHA-256 implementation kept local to avoid a
// target dependency.  It hashes canonical v1 bytes only.
uint32_t Rotate(uint32_t value, uint32_t count) { return (value >> count) | (value << (32 - count)); }
void Transform(const unsigned char block[64], uint32_t state[8]) {
	static const uint32_t k[64] = {0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
	uint32_t w[64];
	for (unsigned int i = 0; i < 16; ++i) w[i] = (static_cast<uint32_t>(block[i*4]) << 24) | (static_cast<uint32_t>(block[i*4+1]) << 16) | (static_cast<uint32_t>(block[i*4+2]) << 8) | block[i*4+3];
	for (unsigned int i = 16; i < 64; ++i) { const uint32_t s0 = Rotate(w[i-15],7)^Rotate(w[i-15],18)^(w[i-15]>>3); const uint32_t s1 = Rotate(w[i-2],17)^Rotate(w[i-2],19)^(w[i-2]>>10); w[i] = w[i-16]+s0+w[i-7]+s1; }
	uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
	for (unsigned int i = 0; i < 64; ++i) { const uint32_t s1=Rotate(e,6)^Rotate(e,11)^Rotate(e,25); const uint32_t choice=(e&f)^((~e)&g); const uint32_t t1=h+s1+choice+k[i]+w[i]; const uint32_t s0=Rotate(a,2)^Rotate(a,13)^Rotate(a,22); const uint32_t majority=(a&b)^(a&c)^(b&c); h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+s0+majority; }
	state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}

}  // namespace

namespace detail {

ErrorClass ScanV1Json(const std::string& bytes, size_t cap, Token* token) { return Scanner(bytes).Parse(cap, token); }
const Token* Member(const Token& object, const char* key) { if (object.kind != Token::Kind::object) return 0; for (size_t i = 0; i < object.object.size(); ++i) if (object.object[i].first == key) return &object.object[i].second; return 0; }
bool IsAsciiPrintable(const std::string& text, size_t min, size_t max) { if (text.size() < min || text.size() > max) return false; for (size_t i=0;i<text.size();++i) if (static_cast<unsigned char>(text[i]) < 0x20 || static_cast<unsigned char>(text[i]) > 0x7e) return false; return true; }
bool IsLowerHex(const std::string& text, size_t size) { if (text.size()!=size) return false; for (size_t i=0;i<text.size();++i) if (!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f'))) return false; return true; }
bool PositiveUint63(const Token* token, uint64_t* value) { return Number(token, 0x7fffffffffffffffULL, true, value) == ErrorClass::ok; }
std::string Encode(const Token& token) { std::string out; if (token.kind==Token::Kind::null_value || token.kind==Token::Kind::boolean || token.kind==Token::Kind::number) return token.text; if (token.kind==Token::Kind::string) return std::string("\"")+token.text+"\""; if (token.kind==Token::Kind::array) { out="["; for(size_t i=0;i<token.array.size();++i){if(i)out+=',';out+=Encode(token.array[i]);} return out+"]"; } out="{"; for(size_t i=0;i<token.object.size();++i){if(i)out+=',';out+='\"';out+=token.object[i].first;out+="\":";out+=Encode(token.object[i].second);} return out+"}"; }

}  // namespace detail

std::string Sha256Hex(const std::string& bytes) {
	uint32_t state[8]={0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
	const unsigned char* input=reinterpret_cast<const unsigned char*>(bytes.data()); size_t at=0;
	while (at+64<=bytes.size()) { Transform(input+at,state); at+=64; }
	unsigned char block[128]; memset(block,0,sizeof(block)); const size_t remain=bytes.size()-at; if(remain)memcpy(block,input+at,remain); block[remain]=0x80; const uint64_t bits=static_cast<uint64_t>(bytes.size())*8; const size_t final_block=remain>=56?64:0; for(unsigned int i=0;i<8;++i) block[final_block+63-i]=static_cast<unsigned char>(bits>>(i*8)); Transform(block,state); if(final_block)Transform(block+64,state);
	static const char hex[]="0123456789abcdef"; std::string output(64,'0'); for(unsigned int i=0;i<8;++i)for(unsigned int j=0;j<4;++j){const unsigned char b=static_cast<unsigned char>(state[i]>>(24-j*8));output[(i*4+j)*2]=hex[b>>4];output[(i*4+j)*2+1]=hex[b&15];} return output;
}

ErrorClass ParseWire(const std::string& bytes, std::string* canonical, std::string* digest) {
	if (!canonical || !digest) return ErrorClass::schema;
	canonical->clear(); digest->clear(); detail::Token root; const ErrorClass result=detail::ScanV1Json(bytes,65536,&root); if(result!=ErrorClass::ok)return result;
	if(root.kind!=detail::Token::Kind::object)return ErrorClass::schema;
	return detail::Member(root,"operation") ? Request(root,canonical,digest) : Response(root,canonical);
}

ErrorClass ParseFrame(const std::string& bytes, std::string* payload) {
	if (!payload || bytes.size()<4) return ErrorClass::framing;
	const uint32_t size=(static_cast<uint32_t>(static_cast<unsigned char>(bytes[0]))<<24)|(static_cast<uint32_t>(static_cast<unsigned char>(bytes[1]))<<16)|(static_cast<uint32_t>(static_cast<unsigned char>(bytes[2]))<<8)|static_cast<unsigned char>(bytes[3]);
	if(size==0 || size>65536 || bytes.size()!=size+4)return ErrorClass::framing;
	const std::string raw = bytes.substr(4);
	std::string digest;
	return ParseWire(raw, payload, &digest);
}

ErrorClass ParseBackendFence(const std::string& bytes, std::string* canonical, std::string* digest) {
	if(!canonical||!digest)return ErrorClass::schema; canonical->clear();digest->clear();detail::Token root; ErrorClass result=detail::ScanV1Json(bytes,262144,&root);if(result!=ErrorClass::ok)return result;
	const char* const keys[]={"record_version","sequence","state","authority_epoch","sha256"}; if(!ExactKeys(root,keys,5))return ErrorClass::schema;
	if(!detail::Member(root,"record_version")||detail::Member(root,"record_version")->kind!=detail::Token::Kind::number||detail::Member(root,"record_version")->text!="1")return ErrorClass::schema;
	result=Number(detail::Member(root,"sequence"),0x7fffffffffffffffULL,true,0);if(result!=ErrorClass::ok)return result;result=Number(detail::Member(root,"authority_epoch"),0x7fffffffffffffffULL,true,0);if(result!=ErrorClass::ok)return result;
	const detail::Token* state=detail::Member(root,"state");if(!IsString(state)||(state->text!="legacy"&&state->text!="native"&&state->text!="transitioning"&&state->text!="native_quiescing"))return ErrorClass::schema;
	if(Hex(detail::Member(root,"sha256"),64)!=ErrorClass::ok)return ErrorClass::checksum; detail::Token copy=root;copy.object.pop_back();*canonical=detail::Encode(copy);*digest=Sha256Hex(*canonical);if(*digest!=detail::Member(root,"sha256")->text)return ErrorClass::checksum;return ErrorClass::ok;
}

BackendFenceTracker::BackendFenceTracker() : present_(false), sequence_(0), epoch_(0) {}

ErrorClass BackendFenceTracker::Accept(const std::string& canonical) {
	detail::Token root;
	if (detail::ScanV1Json(canonical, 262144, &root) != ErrorClass::ok) return ErrorClass::json;
	uint64_t sequence = 0;
	uint64_t epoch = 0;
	if (!detail::PositiveUint63(detail::Member(root, "sequence"), &sequence) || !detail::PositiveUint63(detail::Member(root, "authority_epoch"), &epoch)) return ErrorClass::bounds;
	const detail::Token* state = detail::Member(root, "state");
	if (!state || state->kind != detail::Token::Kind::string) return ErrorClass::schema;
	if (present_) {
		if (sequence != sequence_ + 1) return ErrorClass::transition;
		const bool same_epoch = epoch == epoch_;
		const bool new_authority = epoch == epoch_ + 1;
		bool legal = false;
		if ((state_ == "native" && state->text == "native_quiescing") ||
			(state_ == "native_quiescing" && state->text == "transitioning") ||
			(state_ == "legacy" && state->text == "transitioning")) legal = same_epoch;
		if (state_ == "transitioning" && (state->text == "legacy" || state->text == "native")) legal = new_authority;
		if (!legal) return ErrorClass::transition;
	}
	present_ = true;
	sequence_ = sequence;
	epoch_ = epoch;
	state_ = state->text;
	return ErrorClass::ok;
}

ReplayTracker::ReplayTracker() {}

ErrorClass ReplayTracker::Accept(const std::string& canonical, const std::string& digest) {
	detail::Token root;
	if (detail::ScanV1Json(canonical, 65536, &root) != ErrorClass::ok) return ErrorClass::json;
	const detail::Token* operation_id = detail::Member(root, "operation_id");
	if (!operation_id || operation_id->kind != detail::Token::Kind::string) return ErrorClass::schema;
	for (size_t i = 0; i < entries_.size(); ++i) {
		if (entries_[i].first != operation_id->text) continue;
		return entries_[i].second == digest ? ErrorClass::ok : ErrorClass::transition;
	}
	entries_.push_back(std::make_pair(operation_id->text, digest));
	if (entries_.size() > 64) entries_.erase(entries_.begin());
	return ErrorClass::ok;
}

}  // namespace fogcast
