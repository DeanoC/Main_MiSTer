// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fogcast/runtime_state.hpp"
#include "fogcast/runtime_store.hpp"

#include <assert.h>
#include <cstdio>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <string>

namespace {

class FakeSyscalls : public fogcast::StoreSyscalls {
public:
	bool short_write = false;
	bool fail_sync = false;
	int fail_sync_call = 0;
	bool fail_rename = false;
	mutable int sync_calls = 0;
	mutable int rename_calls = 0;

	long Write(int file, const char* bytes, unsigned long size) const override {
		if (short_write) return 0;
		return static_cast<long>(write(file, bytes, static_cast<size_t>(size)));
	}
	int Sync(int file) const override {
		++sync_calls;
		if (fail_sync || (fail_sync_call != 0 && sync_calls == fail_sync_call)) { errno = EIO; return -1; }
		return fsync(file);
	}
	int Rename(const char* from, const char* to) const override {
		++rename_calls;
		if (fail_rename) { errno = EIO; return -1; }
		return rename(from, to);
	}
	int Unlink(const char* path) const override { return unlink(path); }
};

class ScopedUmask {
public:
	explicit ScopedUmask(mode_t replacement) : prior_(umask(replacement)) {}
	~ScopedUmask() { umask(prior_); }

private:
	mode_t prior_;
};

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

std::string ReplaceOnce(const std::string& input, const std::string& from, const std::string& to) {
	const size_t at = input.find(from);
	assert(at != std::string::npos);
	return input.substr(0, at) + to + input.substr(at + from.size());
}

std::string Checksummed(const std::string& raw) {
	const size_t at = raw.rfind(",\"sha256\":");
	assert(at != std::string::npos);
	const std::string canonical = raw.substr(0, at) + "}";
	return raw.substr(0, at) + ",\"sha256\":\"" + fogcast::Sha256Hex(canonical) + "\"}";
}

std::string ReadFile(const std::string& path) {
	std::ifstream input(path.c_str(), std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

std::string ReplaceAt(const std::string& input, size_t at, size_t length,
	const std::string& to) {
	assert(at != std::string::npos);
	return input.substr(0, at) + to + input.substr(at + length);
}

std::string RootSequence(const std::string& input, const std::string& value) {
	const size_t at = input.find("\"backend_epoch\":");
	assert(at != std::string::npos);
	const size_t number = input.find("\"sequence\":", at);
	assert(number != std::string::npos);
	const size_t begin = number + std::string("\"sequence\":").size();
	const size_t end = input.find(',', begin);
	return ReplaceAt(input, begin, end - begin, value);
}

std::string LastResultingSequence(const std::string& input) {
	const size_t at = input.rfind("\"resulting_sequence\":");
	assert(at != std::string::npos);
	const size_t begin = at + std::string("\"resulting_sequence\":").size();
	const size_t end = input.find('}', begin);
	assert(end != std::string::npos);
	return input.substr(begin, end - begin);
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

bool BoolField(const std::string& line, const char* key) {
	const std::string needle = std::string("\"") + key + "\":";
	const size_t start = line.find(needle);
	assert(start != std::string::npos);
	return line.compare(start + needle.size(), 4, "true") == 0;
}

fogcast::ErrorClass ExpectedError(const std::string& text) {
	if (text.empty()) return fogcast::ErrorClass::ok;
	if (text == "json") return fogcast::ErrorClass::json;
	if (text == "schema") return fogcast::ErrorClass::schema;
	if (text == "bounds") return fogcast::ErrorClass::bounds;
	assert(false);
	return fogcast::ErrorClass::json;
}

}  // namespace

int main() {
	std::ifstream fixture("tests/testdata/fogcast_protocol_v1.jsonl");
	assert(fixture.good());
	std::string line;
	std::string idle_raw;
	std::string active_raw;
	std::string ledger_raw;
	fogcast::StateRecord active_written;
	fogcast::StateRecord ledger_written;
	std::string ignored_canonical;
	std::string ignored_digest;
	unsigned int rows = 0;
	while (std::getline(fixture, line)) {
		if (Field(line, "kind") != "state_record") continue;
		fogcast::StateRecord record;
		std::string canonical;
		std::string digest;
		const fogcast::ErrorClass actual = fogcast::ParseStateRecord(HexDecode(Field(line, "input_hex")), &record, &canonical, &digest);
		assert(actual == ExpectedError(Field(line, "error")));
		if (BoolField(line, "valid")) {
			assert(canonical == HexDecode(Field(line, "canonical_hex")));
			assert(digest == Field(line, "sha256"));
		}
		if (Field(line, "id") == "valid-idle-state-record") idle_raw = HexDecode(Field(line, "input_hex"));
		if (Field(line, "id") == "valid-active-state-record") active_raw = HexDecode(Field(line, "input_hex"));
		if (Field(line, "id") == "valid-state-record-ledger-64") ledger_raw = HexDecode(Field(line, "input_hex"));
		++rows;
	}
	assert(rows == 10);
	assert(!idle_raw.empty() && !active_raw.empty() && !ledger_raw.empty());
	assert(fogcast::ParseStateRecord(active_raw, &active_written, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);
	assert(fogcast::ParseStateRecord(ledger_raw, &ledger_written, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);
	fogcast::StateRecord probe;
	assert(fogcast::ParseStateRecord(Checksummed(active_raw), &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);

	// These records retain a correct checksum.  They prove that state safety is
	// semantic, rather than merely a successful parse/checksum calculation.
	std::string contradictory = ReplaceOnce(idle_raw, "\"owner\":null", "\"owner\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42,\"mode\":\"fpga_native\"}");
	contradictory = Checksummed(contradictory);
	assert(fogcast::ParseStateRecord(contradictory, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	std::string active_idle = ReplaceOnce(active_raw, "\"mode\":\"fpga_native\"", "\"mode\":\"idle\"");
	active_idle = Checksummed(active_idle);
	assert(fogcast::ParseStateRecord(active_idle, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	std::string empty_content_lease = Checksummed(ReplaceOnce(idle_raw, "\"content_lease\":null", "\"content_lease\":{}"));
	assert(fogcast::ParseStateRecord(empty_content_lease, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	std::string bad_core = Checksummed(ReplaceOnce(idle_raw, "\"observed_core\":null", "\"observed_core\":\"bad/path\""));
	assert(fogcast::ParseStateRecord(bad_core, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::bounds);
	std::string good_core = Checksummed(ReplaceOnce(idle_raw, "\"observed_core\":null", "\"observed_core\":\"Mega Drive+\""));
	assert(fogcast::ParseStateRecord(good_core, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);
	std::string transitional = ReplaceOnce(active_raw, "\"phase\":\"active\"", "\"phase\":\"intent\"");
	transitional = ReplaceOnce(transitional, "\"mode\":\"fpga_native\"", "\"mode\":\"recovering\"");
	const std::string old_owner = "\"release_owner\":null";
	const std::string release_owner = "\"release_owner\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42,\"mode\":\"fpga_native\"}";
	transitional = ReplaceOnce(transitional, old_owner, release_owner);
	transitional = ReplaceOnce(transitional, "\"candidate\":null", "\"candidate\":{\"session\":\"fedcba9876543210fedcba9876543210\",\"generation\":43,\"mode\":\"fpga_native\"}");
	transitional = ReplaceOnce(transitional, "\"in_flight\":null", "\"in_flight\":{\"operation_id\":\"fedcba9876543210fedcba9876543210\",\"request_digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"stage\":\"transitioning\"}");
	transitional = Checksummed(transitional);
	const fogcast::ErrorClass transitional_result = fogcast::ParseStateRecord(transitional, &probe, &ignored_canonical, &ignored_digest);
	if (transitional_result != fogcast::ErrorClass::ok) std::fprintf(stderr, "transitional=%d\n", static_cast<int>(transitional_result));
	assert(transitional_result == fogcast::ErrorClass::ok);
	std::string recorded_intent = Checksummed(ReplaceOnce(transitional, "\"stage\":\"transitioning\"", "\"stage\":\"recorded\""));
	assert(fogcast::ParseStateRecord(recorded_intent, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	std::string null_intent = Checksummed(ReplaceOnce(transitional, "\"in_flight\":{\"operation_id\":\"fedcba9876543210fedcba9876543210\",\"request_digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"stage\":\"transitioning\"}", "\"in_flight\":null"));
	assert(fogcast::ParseStateRecord(null_intent, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	// An idle-origin launch has no old hardware owner.  Intent and releasing
	// deliberately retain that empty shape until no_owner, while still carrying
	// the candidate and a transitioning operation.
	std::string idle_origin = ReplaceOnce(idle_raw, "\"phase\":\"idle\"", "\"phase\":\"intent\"");
	idle_origin = ReplaceOnce(idle_origin, "\"mode\":\"idle\"", "\"mode\":\"recovering\"");
	idle_origin = ReplaceOnce(idle_origin, "\"candidate\":null", "\"candidate\":{\"session\":\"fedcba9876543210fedcba9876543210\",\"generation\":43,\"mode\":\"fpga_native\"}");
	idle_origin = ReplaceOnce(idle_origin, "\"in_flight\":null", "\"in_flight\":{\"operation_id\":\"fedcba9876543210fedcba9876543210\",\"request_digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"stage\":\"transitioning\"}");
	idle_origin = Checksummed(idle_origin);
	assert(fogcast::ParseStateRecord(idle_origin, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);
	std::string idle_releasing = Checksummed(ReplaceOnce(idle_origin, "\"phase\":\"intent\"", "\"phase\":\"releasing\""));
	assert(fogcast::ParseStateRecord(idle_releasing, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);
	// Releasing has four distinct approved shapes: existing and idle-origin
	// launches retain a candidate; release-only and recovery/drain do not.
	std::string existing_launch_releasing = Checksummed(ReplaceOnce(transitional, "\"phase\":\"intent\"", "\"phase\":\"releasing\""));
	assert(fogcast::ParseStateRecord(existing_launch_releasing, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);
	std::string release_only = ReplaceOnce(active_raw, "\"phase\":\"active\"", "\"phase\":\"releasing\"");
	release_only = ReplaceOnce(release_only, "\"mode\":\"fpga_native\"", "\"mode\":\"recovering\"");
	release_only = ReplaceOnce(release_only, "\"release_owner\":null", "\"release_owner\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42,\"mode\":\"fpga_native\"}");
	release_only = ReplaceOnce(release_only, "\"in_flight\":null", "\"in_flight\":{\"operation_id\":\"fedcba9876543210fedcba9876543210\",\"request_digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"stage\":\"transitioning\"}");
	release_only = Checksummed(release_only);
	assert(fogcast::ParseStateRecord(release_only, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);
	std::string recovery_releasing = ReplaceOnce(idle_raw, "\"phase\":\"idle\"", "\"phase\":\"releasing\"");
	recovery_releasing = ReplaceOnce(recovery_releasing, "\"mode\":\"idle\"", "\"mode\":\"recovering\"");
	recovery_releasing = ReplaceOnce(recovery_releasing, "\"in_flight\":null", "\"in_flight\":{\"operation_id\":\"fedcba9876543210fedcba9876543210\",\"request_digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"stage\":\"transitioning\"}");
	recovery_releasing = Checksummed(recovery_releasing);
	assert(fogcast::ParseStateRecord(recovery_releasing, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);
	std::string release_mismatched_owner = Checksummed(ReplaceOnce(release_only, "\"release_owner\":{\"session\":\"0123456789abcdef0123456789abcdef\"", "\"release_owner\":{\"session\":\"fedcba9876543210fedcba9876543210\""));
	assert(fogcast::ParseStateRecord(release_mismatched_owner, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	std::string recovery_with_lease = Checksummed(ReplaceOnce(recovery_releasing, "\"leases\":[]", "\"leases\":[{}]"));
	assert(fogcast::ParseStateRecord(recovery_with_lease, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	std::string mixed_origin = Checksummed(ReplaceOnce(idle_origin, "\"release_owner\":null", "\"release_owner\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42,\"mode\":\"fpga_native\"}"));
	assert(fogcast::ParseStateRecord(mixed_origin, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	std::string transferred = ReplaceOnce(active_raw, "\"phase\":\"active\"", "\"phase\":\"transferred\"");
	transferred = ReplaceOnce(transferred, "\"mode\":\"fpga_native\"", "\"mode\":\"recovering\"");
	transferred = ReplaceOnce(transferred, "\"candidate\":null", "\"candidate\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42,\"mode\":\"fpga_native\"}");
	transferred = ReplaceOnce(transferred, "\"in_flight\":null", "\"in_flight\":{\"operation_id\":\"fedcba9876543210fedcba9876543210\",\"request_digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"stage\":\"transitioning\"}");
	transferred = Checksummed(transferred);
	assert(fogcast::ParseStateRecord(transferred, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);
	std::string wrong_transferred_owner = Checksummed(ReplaceOnce(transferred, "\"candidate\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42", "\"candidate\":{\"session\":\"fedcba9876543210fedcba9876543210\",\"generation\":43"));
	assert(fogcast::ParseStateRecord(wrong_transferred_owner, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	// The 64-row ledger fixture exercises recursive snapshot validation and the
	// sequence fence rather than merely the outer terminal fields.
	assert(fogcast::ParseStateRecord(ledger_raw, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::ok);
	const size_t ledger_start = ledger_raw.find("\"ledger\":[{");
	const size_t nested_release = ledger_raw.find("\"release_owner\":null", ledger_start);
	assert(nested_release != std::string::npos);
	std::string malformed_nested = Checksummed(ReplaceAt(ledger_raw, nested_release, std::string("\"release_owner\":null").size(), "\"release_owner\":{\"session\":\"0123456789abcdef0123456789abcdef\",\"generation\":42,\"mode\":\"fpga_native\"}"));
	assert(fogcast::ParseStateRecord(malformed_nested, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	std::string future_terminal = Checksummed(RootSequence(ledger_raw, "1"));
	assert(fogcast::ParseStateRecord(future_terminal, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);
	std::string terminal_equals_record = RootSequence(ledger_raw, LastResultingSequence(ledger_raw));
	terminal_equals_record = ReplaceOnce(terminal_equals_record, "\"in_flight\":null", "\"in_flight\":{\"operation_id\":\"ffffffffffffffffffffffffffffffff\",\"request_digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"stage\":\"recorded\"}");
	terminal_equals_record = Checksummed(terminal_equals_record);
	assert(fogcast::ParseStateRecord(terminal_equals_record, &probe, &ignored_canonical, &ignored_digest) == fogcast::ErrorClass::schema);

	// Break caught: skipping secure, fsync-backed replacement can resurrect a
	// stale owner after daemon restart, or allow an attacker-owned symlink.
	char temporary[] = "/tmp/fogcast-state-XXXXXX";
	assert(mkdtemp(temporary) != 0);
	fogcast::StateStore store(temporary);
	fogcast::StateRecord written;
	std::string state_canonical;
	std::string state_digest;
	fixture.clear();
	fixture.seekg(0);
	while (std::getline(fixture, line)) {
		if (Field(line, "id") != "valid-idle-state-record") continue;
		assert(fogcast::ParseStateRecord(HexDecode(Field(line, "input_hex")), &written, &state_canonical, &state_digest) == fogcast::ErrorClass::ok);
		break;
	}
	assert(written.sequence == 21);
	assert(store.Commit(written) == fogcast::ErrorClass::ok);
	fogcast::StateRecord loaded;
	assert(store.Load(&loaded) == fogcast::ErrorClass::ok);
	assert(loaded.sequence == written.sequence);
	const std::string record_path = std::string(temporary) + "/state.json";
	const std::string prior_bytes = ReadFile(record_path);
	std::string bad_checksum = prior_bytes;
	bad_checksum[bad_checksum.size() - 3] = bad_checksum[bad_checksum.size() - 3] == '0' ? '1' : '0';
	{ std::ofstream output(record_path.c_str(), std::ios::binary | std::ios::trunc); output << bad_checksum; }
	assert(store.Load(&loaded) == fogcast::ErrorClass::checksum);
	{ std::ofstream output(record_path.c_str(), std::ios::binary | std::ios::trunc); output << prior_bytes; }
	// A stale generation must never overwrite a newer durable commit.
	assert(store.Commit(written) == fogcast::ErrorClass::schema);
	fogcast::StateRecord tampered = written;
	tampered.sequence = 22;
	assert(store.Commit(tampered) == fogcast::ErrorClass::schema);
	assert(ReadFile(record_path) == prior_bytes);
	assert(chmod(record_path.c_str(), 0400) == 0);
	assert(store.Load(&loaded) == fogcast::ErrorClass::schema);
	assert(chmod(record_path.c_str(), 0600) == 0);
	assert(chmod(temporary, 0755) == 0);
	assert(store.Load(&loaded) == fogcast::ErrorClass::schema);
	assert(chmod(temporary, 0700) == 0);
	assert(unlink(record_path.c_str()) == 0);
	assert(symlink("/dev/null", record_path.c_str()) == 0);
	assert(store.Load(&loaded) == fogcast::ErrorClass::schema);
	assert(unlink(record_path.c_str()) == 0);
	assert(rmdir(temporary) == 0);

	// A restrictive final mode must survive a maximally restrictive process
	// umask.  The scope restores the process umask before any assertion can
	// abort, while still proving Commit and Load work inside the changed umask.
	// Failed file sync never reaches rename, and a stale temp is neither consumed nor
	// modified by a later commit.
	char durable[] = "/tmp/fogcast-state-durable-XXXXXX";
	assert(mkdtemp(durable) != 0);
	struct stat durable_status;
	const std::string durable_path = std::string(durable) + "/state.json";
	bool restrictive_commit = false;
	bool restrictive_mode = false;
	bool restrictive_load = false;
	{
		ScopedUmask restrictive(0777);
		restrictive_commit = fogcast::StateStore(durable).Commit(written) == fogcast::ErrorClass::ok;
		restrictive_mode = stat(durable_path.c_str(), &durable_status) == 0 && (durable_status.st_mode & 0777) == 0600;
		fogcast::StateRecord restrictive_record;
		restrictive_load = fogcast::StateStore(durable).Load(&restrictive_record) == fogcast::ErrorClass::ok && restrictive_record.sequence == written.sequence;
	}
	assert(restrictive_commit && restrictive_mode && restrictive_load);
	const std::string durable_prior = ReadFile(durable_path);
	FakeSyscalls regular_sync_failure;
	regular_sync_failure.fail_sync_call = 1;
	assert(ledger_written.sequence > written.sequence);
	assert(fogcast::StateStore(durable, &regular_sync_failure).Commit(ledger_written) == fogcast::ErrorClass::schema);
	assert(regular_sync_failure.rename_calls == 0 && ReadFile(durable_path) == durable_prior);
	const std::string stale_path = std::string(durable) + "/state.json.tmp";
	const int stale = open(stale_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(stale >= 0 && write(stale, "stale", 5) == 5 && close(stale) == 0);
	assert(fogcast::StateStore(durable).Commit(ledger_written) == fogcast::ErrorClass::schema);
	assert(ReadFile(stale_path) == "stale");
	assert(unlink(stale_path.c_str()) == 0 && unlink(durable_path.c_str()) == 0 && rmdir(durable) == 0);

	// These fakes exercise failures which a real filesystem rarely produces on
	// demand: a short write, file/directory fsync, and atomic rename failure.
	char injected[] = "/tmp/fogcast-state-injected-XXXXXX";
	assert(mkdtemp(injected) != 0);
	FakeSyscalls short_write;
	short_write.short_write = true;
	assert(fogcast::StateStore(injected, &short_write).Commit(written) == fogcast::ErrorClass::schema);
	FakeSyscalls sync_failure;
	sync_failure.fail_sync = true;
	assert(fogcast::StateStore(injected, &sync_failure).Commit(written) == fogcast::ErrorClass::schema);
	FakeSyscalls rename_failure;
	rename_failure.fail_rename = true;
	assert(fogcast::StateStore(injected, &rename_failure).Commit(written) == fogcast::ErrorClass::schema);
	FakeSyscalls directory_sync_failure;
	directory_sync_failure.fail_sync_call = 2;
	assert(fogcast::StateStore(injected, &directory_sync_failure).Commit(written) == fogcast::ErrorClass::schema);
	assert(unlink((std::string(injected) + "/state.json").c_str()) == 0);
	assert(rmdir(injected) == 0);
	return 0;
}
