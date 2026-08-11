// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fogcast/runtime_protocol.hpp"

#include <assert.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string HexDecode(const std::string& text) {
	std::string result;
	assert((text.size() % 2) == 0);
	for (size_t i = 0; i < text.size(); i += 2) {
		unsigned int byte = 0;
		std::istringstream parser(text.substr(i, 2));
		parser >> std::hex >> byte;
		assert(!parser.fail());
		result.push_back(static_cast<char>(byte));
	}
	return result;
}

std::string Field(const std::string& line, const char* key) {
	const std::string needle = std::string("\"") + key + "\":";
	const size_t start = line.find(needle);
	assert(start != std::string::npos);
	size_t cursor = start + needle.size();
	if (line.compare(cursor, 4, "null") == 0) return std::string();
	assert(line[cursor] == '\"');
	++cursor;
	const size_t end = line.find('\"', cursor);
	assert(end != std::string::npos);
	return line.substr(cursor, end - cursor);
}

std::string ReplaceOnce(const std::string& input, const std::string& from,
	const std::string& to) {
	const size_t at = input.find(from);
	assert(at != std::string::npos);
	return input.substr(0, at) + to + input.substr(at + from.size());
}

std::string ReplaceAll(const std::string& input, const std::string& from,
	const std::string& to) {
	std::string result = input;
	size_t at = 0;
	while ((at = result.find(from, at)) != std::string::npos) {
		result.replace(at, from.size(), to);
		at += to.size();
	}
	return result;
}

std::string ReplaceLastObjectWithNull(const std::string& input,
	const std::string& key) {
	const size_t key_at = input.rfind(key);
	assert(key_at != std::string::npos);
	const size_t object_at = key_at + key.size();
	assert(input[object_at] == '{');
	unsigned int depth = 0;
	size_t at = object_at;
	for (; at < input.size(); ++at) {
		if (input[at] == '{') ++depth;
		if (input[at] == '}' && --depth == 0) break;
	}
	assert(at < input.size());
	return input.substr(0, object_at) + "null" + input.substr(at + 1);
}

bool BoolField(const std::string& line, const char* key) {
	const std::string needle = std::string("\"") + key + "\":";
	const size_t start = line.find(needle);
	assert(start != std::string::npos);
	return line.compare(start + needle.size(), 4, "true") == 0;
}

fogcast::ErrorClass ExpectedError(const std::string& text) {
	if (text.empty()) return fogcast::ErrorClass::ok;
	if (text == "framing") return fogcast::ErrorClass::framing;
	if (text == "utf8") return fogcast::ErrorClass::utf8;
	if (text == "json") return fogcast::ErrorClass::json;
	if (text == "schema") return fogcast::ErrorClass::schema;
	if (text == "bounds") return fogcast::ErrorClass::bounds;
	if (text == "checksum") return fogcast::ErrorClass::checksum;
	if (text == "transition") return fogcast::ErrorClass::transition;
	assert(false);
	return fogcast::ErrorClass::json;
}

}  // namespace

int main() {
	// Break caught: accepting a malformed fixture or emitting bytes different
	// from the Go-owned canonical corpus would break cross-runtime replay.
	std::ifstream fixture("tests/testdata/fogcast_protocol_v1.jsonl");
	assert(fixture.good());
	std::string line;
	unsigned int rows = 0;
	std::string launch_canonical;
	std::string health_response;
	std::string launch_response;
	std::string operation_status_response;
	std::string fence;
	fogcast::BackendFenceTracker fences;
	fogcast::ReplayTracker replay;
	while (std::getline(fixture, line)) {
		const std::string kind = Field(line, "kind");
		if (kind == "state_record") continue;
		const std::string input = HexDecode(Field(line, "input_hex"));
		const bool valid = BoolField(line, "valid");
		const std::string want_canonical = HexDecode(Field(line, "canonical_hex"));
		const std::string want_digest = Field(line, "sha256");
		std::string canonical;
		std::string digest;
		fogcast::ErrorClass actual = fogcast::ErrorClass::json;
		if (kind == "wire_request" || kind == "wire_response") {
			actual = fogcast::ParseWire(input, &canonical, &digest);
			const std::string id = Field(line, "id");
			if (kind == "wire_request" && actual == fogcast::ErrorClass::ok && id.find("replay-digest") != std::string::npos) actual = replay.Accept(canonical, digest);
		} else if (kind == "frame") {
			actual = fogcast::ParseFrame(input, &canonical);
		} else {
			const std::string id = Field(line, "id");
			if (id.find("-0-before") != std::string::npos) fences = fogcast::BackendFenceTracker();
			actual = fogcast::ParseBackendFence(input, &canonical, &digest);
			if (actual == fogcast::ErrorClass::ok && id.find("-after") != std::string::npos) actual = fences.Accept(canonical);
			if (actual == fogcast::ErrorClass::ok && id.find("-0-before") != std::string::npos) actual = fences.Accept(canonical);
		}
		if (actual != ExpectedError(Field(line, "error"))) {
			std::fprintf(stderr, "fixture failure: %s expected %d got %d\n",
				Field(line, "id").c_str(), static_cast<int>(ExpectedError(Field(line, "error"))),
				static_cast<int>(actual));
			assert(false);
		}
		if (valid) {
			if (canonical != want_canonical) {
				std::fprintf(stderr, "canonical failure: %s expected %zu got %zu\n", Field(line, "id").c_str(), want_canonical.size(), canonical.size());
				assert(false);
			}
			assert(digest == want_digest);
		}
		if (Field(line, "id") == "valid-launch-request") launch_canonical = want_canonical;
		if (Field(line, "id") == "valid-health-response") health_response = input;
		if (Field(line, "id") == "valid-launch-response") launch_response = input;
		if (Field(line, "id") == "valid-operation-status-response") operation_status_response = input;
		if (Field(line, "id") == "valid-fence-native-grant") fence = input;
		++rows;
	}
	assert(rows == 101);  // State-record rows are exercised by the state suite.
	assert(!launch_canonical.empty() && !health_response.empty() && !launch_response.empty() && !operation_status_response.empty() && !fence.empty());
	// Break caught: retaining source order in a semantic request makes Go/C++
	// replay digests diverge for equivalent, differently ordered objects.
	const std::string permuted_launch = "{\"body\":{\"content\":{\"extension\":\"md\",\"size\":1048576,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"cache_lease_id\":\"00112233445566778899aabbccddeeff\",\"expected_core\":\"MegaDrive\",\"system\":\"megadrive\",\"game_id\":\"synthetic-game\"},\"precondition\":{\"owner\":null,\"sequence\":9},\"candidate\":{\"generation\":42,\"session\":\"0123456789abcdef0123456789abcdef\"},\"operation\":\"launch\",\"operation_id\":\"fedcba9876543210fedcba9876543210\",\"request_id\":5,\"protocol\":1}";
	std::string canonical;
	std::string digest;
	assert(fogcast::ParseWire(permuted_launch, &canonical, &digest) == fogcast::ErrorClass::ok);
	assert(canonical == launch_canonical);
	assert(digest == fogcast::Sha256Hex(launch_canonical));
	std::string frame("\0\0\0\2{}", 6);
	assert(fogcast::ParseFrame(frame, &canonical) == fogcast::ErrorClass::schema);
	assert(fogcast::ParseWire("{\"protocol\":1,\"request_id\":1,\"ok\":true,\"result\":{\"arbitrary\":1},\"snapshot\":null}", &canonical, &digest) == fogcast::ErrorClass::schema);
	assert(fogcast::ParseWire("{\"protocol\":1,\"request_id\":1,\"ok\":false,\"error\":{\"code\":\"UNKNOWN\",\"message\":\"safe\"},\"snapshot\":null}", &canonical, &digest) == fogcast::ErrorClass::schema);
	// Nested owner member order is not semantic; structural equality is.
	const std::string permuted_stop = "{\"protocol\":1,\"request_id\":6,\"operation_id\":\"fedcba9876543210fedcba9876543210\",\"operation\":\"stop\",\"owner\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42,\"mode\":\"fpga_native\"},\"precondition\":{\"sequence\":16,\"owner\":{\"mode\":\"fpga_native\",\"generation\":42,\"session\":\"0123456789abcdef0123456789abcdef\"}},\"body\":{}}";
	assert(fogcast::ParseWire(permuted_stop, &canonical, &digest) == fogcast::ErrorClass::ok);
	assert(fogcast::ParseWire("{\"protocol\":1,\"request_id\":-1,\"operation\":\"hello\",\"body\":{}}", &canonical, &digest) == fogcast::ErrorClass::bounds);
	// Producer records have a normative order; accepting permutations would make
	// independently serialized terminals/fences non-deterministic.
	assert(fogcast::ParseWire(ReplaceOnce(health_response, "{\"ready\":true,\"phase\":\"idle\"", "{\"phase\":\"idle\",\"ready\":true"), &canonical, &digest) == fogcast::ErrorClass::schema);
	assert(fogcast::ParseWire(ReplaceOnce(operation_status_response, "\"ok\":true,\"error\":null", "\"error\":null,\"ok\":true"), &canonical, &digest) == fogcast::ErrorClass::schema);
	assert(fogcast::ParseWire(ReplaceOnce(operation_status_response, "\"sequence\":9,\"backend_epoch\":1", "\"backend_epoch\":1,\"sequence\":9"), &canonical, &digest) == fogcast::ErrorClass::schema);
	assert(fogcast::ParseBackendFence(ReplaceOnce(fence, "\"sequence\":2,\"state\":\"native\"", "\"state\":\"native\",\"sequence\":2"), &canonical, &digest) == fogcast::ErrorClass::schema);
	assert(fogcast::ParseWire(ReplaceOnce(health_response, "\"release_owner\":null", "\"release_owner\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42,\"mode\":\"fpga_native\"}"), &canonical, &digest) == fogcast::ErrorClass::schema);
	assert(fogcast::ParseWire(ReplaceOnce(launch_response, "\"candidate\":null", "\"candidate\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42,\"mode\":\"fpga_native\"}"), &canonical, &digest) == fogcast::ErrorClass::schema);
	assert(fogcast::ParseWire(ReplaceOnce(launch_response, "\"phase\":\"active\"", "\"phase\":\"intent\""), &canonical, &digest) == fogcast::ErrorClass::schema);
	assert(fogcast::ParseWire(ReplaceLastObjectWithNull(operation_status_response, "\"snapshot\":"), &canonical, &digest) == fogcast::ErrorClass::schema);
	const std::string failed_idle_status = ReplaceAll(operation_status_response, "\"phase\":\"idle\"", "\"phase\":\"failed\"");
	assert(fogcast::ParseWire(failed_idle_status, &canonical, &digest) == fogcast::ErrorClass::schema);
	std::string failed_status = ReplaceAll(failed_idle_status, "\"mode\":\"idle\"", "\"mode\":\"failed\"");
	assert(fogcast::ParseWire(failed_status, &canonical, &digest) == fogcast::ErrorClass::ok);
	// The replay window retains exactly 64 operation identities.  It must reject
	// an altered in-window replay, then evict the oldest only on entry 65.
	fogcast::ReplayTracker replay_window;
	char operation_id[33];
	for (unsigned int i = 0; i < 64; ++i) {
		std::snprintf(operation_id, sizeof(operation_id), "%032x", i + 1);
		const std::string operation = std::string("{\"operation_id\":\"") + operation_id + "\"}";
		assert(replay_window.Accept(operation, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == fogcast::ErrorClass::ok);
	}
	std::snprintf(operation_id, sizeof(operation_id), "%032x", 1);
	assert(replay_window.Accept(std::string("{\"operation_id\":\"") + operation_id + "\"}", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == fogcast::ErrorClass::transition);
	std::snprintf(operation_id, sizeof(operation_id), "%032x", 65);
	assert(replay_window.Accept(std::string("{\"operation_id\":\"") + operation_id + "\"}", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == fogcast::ErrorClass::ok);
	std::snprintf(operation_id, sizeof(operation_id), "%032x", 1);
	assert(replay_window.Accept(std::string("{\"operation_id\":\"") + operation_id + "\"}", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == fogcast::ErrorClass::ok);
	return 0;
}
