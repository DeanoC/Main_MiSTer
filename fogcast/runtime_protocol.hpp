// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FOGCAST_RUNTIME_PROTOCOL_HPP
#define FOGCAST_RUNTIME_PROTOCOL_HPP

#include <string>
#include <utility>
#include <vector>

namespace fogcast {

enum class ErrorClass {
	ok,
	framing,
	utf8,
	json,
	schema,
	bounds,
	checksum,
	transition,
};

// Parse only C0's versioned IPC object schemas.  On success canonical contains
// the operation's normative canonical bytes, and digest is populated only when
// that schema is checksum-bearing.
ErrorClass ParseWire(const std::string& bytes, std::string* canonical,
	std::string* digest);
ErrorClass ParseFrame(const std::string& bytes, std::string* payload);
ErrorClass ParseBackendFence(const std::string& bytes, std::string* canonical,
	std::string* digest);

class BackendFenceTracker {
public:
	BackendFenceTracker();
	ErrorClass Accept(const std::string& canonical);

private:
	bool present_;
	uint64_t sequence_;
	uint64_t epoch_;
	std::string state_;
};

class ReplayTracker {
public:
	ReplayTracker();
	ErrorClass Accept(const std::string& canonical, const std::string& digest);

private:
	std::vector<std::pair<std::string, std::string> > entries_;
};

std::string Sha256Hex(const std::string& bytes);

namespace detail {

// This is deliberately a bounded token representation, not a general JSON
// API.  It is shared solely by the fixed v1 wire and record validators.
struct Token {
	enum class Kind { null_value, boolean, number, string, array, object };
	Kind kind;
	std::string text;
	std::vector<Token> array;
	std::vector<std::pair<std::string, Token> > object;
	bool escaped;

	Token() : kind(Kind::null_value), escaped(false) {}
};

ErrorClass ScanV1Json(const std::string& bytes, size_t cap, Token* token);
const Token* Member(const Token& object, const char* key);
bool IsAsciiPrintable(const std::string& text, size_t min, size_t max);
bool IsLowerHex(const std::string& text, size_t size);
bool PositiveUint63(const Token* token, uint64_t* value);
std::string Encode(const Token& token);

}  // namespace detail

}  // namespace fogcast

#endif
