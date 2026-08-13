// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_core_artifact_adapter.hpp"
#include "runtime/native/linux/native_content_adapter.hpp"
#include "runtime/native/linux/native_fpga_programmer.hpp"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <type_traits>
#include <vector>
#include <functional>

using namespace mister::native::linux_native;
using mister::native::NativeAcquisitionOutcome;
using namespace mister::native;

namespace {

static_assert(std::is_abstract<NativeFpgaProgramSession>::value,
	"FPGA programming must use one abstract duration-held session");
static_assert(!std::is_default_constructible<NativeFpgaProgramSession>::value,
	"callers must not construct FPGA programming authority");
static_assert(!std::is_copy_constructible<NativeFpgaProgramSession>::value,
	"FPGA programming authority must not be copyable");

std::string TemporaryFile(const char *bytes, size_t size, int *descriptor)
{
	char path[] = "/tmp/fogcast-native-sha.XXXXXX";
	*descriptor = mkstemp(path);
	assert(*descriptor >= 0);
	size_t completed = 0;
	while (completed != size) {
		const ssize_t count = write(*descriptor, bytes + completed,
			size - completed);
		assert(count > 0);
		completed += static_cast<size_t>(count);
	}
	assert(unlink(path) == 0);
	return path;
}

void CheckSha256(const std::string &bytes, const char *expected)
{
	int descriptor = -1;
	TemporaryFile(bytes.data(), bytes.size(), &descriptor);
	NativePosixFileSystem filesystem;
	char digest[65] = {};
	assert(NativeSha256DescriptorForTest(filesystem, descriptor, bytes.size(),
		digest) ==
		NativeArtifactResult::ok);
	assert(strcmp(digest, expected) == 0);
	assert(close(descriptor) == 0);
}

void TestSha256KnownAnswersAndExactLength()
{
	CheckSha256("",
		"e3b0c44298fc1c149afbf4c8996fb924"
		"27ae41e4649b934ca495991b7852b855");
	CheckSha256("abc",
		"ba7816bf8f01cfea414140de5dae2223"
		"b00361a396177a9cb410ff61f20015ad");
	CheckSha256(std::string(64, 'a'),
		"ffe054fe7ae0cb6dc65c3af9b61d5209"
		"f439851db43d0ba5997337df154668eb");
	CheckSha256(std::string(1000, 'a'),
		"41edece42d63e8d9bf515a9ba6932e1c"
		"20cbc9f5a5d134645adb5db1b9737ea3");

	int descriptor = -1;
	TemporaryFile("abc", 3, &descriptor);
	NativePosixFileSystem filesystem;
	char digest[65] = {};
	assert(NativeSha256DescriptorForTest(filesystem, descriptor, 2, digest) ==
		NativeArtifactResult::changed);
	assert(NativeSha256DescriptorForTest(filesystem, descriptor, 4, digest) ==
		NativeArtifactResult::changed);
	assert(close(descriptor) == 0);
}

std::string Join(const std::string &left, const std::string &right)
{
	return left + "/" + right;
}

std::string TemporaryRoot()
{
	char path[] = "/tmp/fogcast-private-root-sentinel.XXXXXX";
	char *created = mkdtemp(path);
	assert(created != nullptr);
	char resolved[4096] = {};
	assert(realpath(created, resolved) != nullptr);
	return resolved;
}

void WriteFile(const std::string &path, const std::string &bytes)
{
	const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(descriptor >= 0);
	size_t completed = 0;
	while (completed != bytes.size()) {
		const ssize_t count = write(descriptor, bytes.data() + completed,
			bytes.size() - completed);
		assert(count > 0);
		completed += static_cast<size_t>(count);
	}
	assert(close(descriptor) == 0);
}

NativeArtifactAuthority HelloAuthority(const char *system = "snes",
	size_t system_length = 4)
{
	NativeArtifactAuthority authority = {};
	authority.system = system;
	authority.system_length = system_length;
	authority.sha256 =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
	authority.sha256_length = 64;
	authority.size = 5;
	authority.extension = "rbf";
	authority.extension_length = 3;
	return authority;
}

NativeArtifactResult ResolveSnesFixtureForTest(
	NativeCoreArtifactAdapter &adapter, uint64_t deadline,
	NativeCoreArtifactHandle *artifact)
{
	return adapter.ResolveFixtureForTest(*FixtureNativeCoreProfile("snes"),
		HelloAuthority(), deadline, artifact);
}

void RemoveArtifactTree(const std::string &root, const std::string &system,
	const std::string &name)
{
	if (!name.empty()) assert(unlink(Join(Join(root, system), name).c_str()) == 0);
	assert(rmdir(Join(root, system).c_str()) == 0);
	assert(rmdir(root.c_str()) == 0);
}

void TestSecureArtifactResolutionAndComponents()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem filesystem;
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeCoreArtifactHandle artifact;
	const uint64_t deadline = filesystem.NowMs() + 1000;
	assert(adapter.Resolve(HelloAuthority(), deadline, &artifact) ==
		NativeArtifactResult::ok);
	assert(artifact.valid());
	assert(artifact.owns_descriptors());
	assert(artifact.size() == 5);
	assert(artifact.Close() == NativeArtifactResult::ok);
	assert(!artifact.owns_descriptors());
	char mutable_root[4096] = {};
	assert(root.size() < sizeof(mutable_root));
	strcpy(mutable_root, root.c_str());
	NativeCoreArtifactAdapter copied_root_adapter(mutable_root, filesystem);
	mutable_root[1] = mutable_root[1] == 'x' ? 'y' : 'x';
	NativeCoreArtifactHandle copied_root_artifact;
	assert(copied_root_adapter.Resolve(HelloAuthority(), deadline,
		&copied_root_artifact) == NativeArtifactResult::ok);
	assert(copied_root_artifact.Close() == NativeArtifactResult::ok);

	NativeCoreArtifactHandle rejected;
	NativeArtifactAuthority authority = HelloAuthority("/snes", 5);
	assert(adapter.Resolve(authority, deadline, &rejected) ==
		NativeArtifactResult::invalid_identity);
	authority = HelloAuthority(".", 1);
	assert(adapter.Resolve(authority, deadline, &rejected) ==
		NativeArtifactResult::invalid_identity);
	authority = HelloAuthority("..", 2);
	assert(adapter.Resolve(authority, deadline, &rejected) ==
		NativeArtifactResult::invalid_identity);
	authority = HelloAuthority("sn/es", 5);
	assert(adapter.Resolve(authority, deadline, &rejected) ==
		NativeArtifactResult::invalid_identity);
	const char embedded_nul[] = {'s', 'n', '\0', 'e', 's'};
	authority = HelloAuthority(embedded_nul, sizeof(embedded_nul));
	assert(adapter.Resolve(authority, deadline, &rejected) ==
		NativeArtifactResult::invalid_identity);
	authority = HelloAuthority();
	authority.sha256 =
		"2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824";
	assert(adapter.Resolve(authority, deadline, &rejected) ==
		NativeArtifactResult::invalid_identity);
	assert(adapter.Resolve(HelloAuthority(), filesystem.NowMs(), &rejected) ==
		NativeArtifactResult::deadline);

	RemoveArtifactTree(root, "snes", name);
}

void TestContentRetainsExactDescriptorAndRechecksEntry()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.sfc";
	const std::string path = Join(Join(root, "snes"), name);
	WriteFile(path, "hello");
	NativePosixFileSystem filesystem;
	NativeContentAdapter content(root.c_str(), filesystem);
	NativeArtifactAuthority authority = HelloAuthority();
	authority.extension = "sfc";
	authority.extension_length = 3;
	assert(content.Configure(authority) == MISTER_RESULT_OK);
	const uint64_t deadline = filesystem.NowMs() + 1000;
	NativeAcquisitionOutcome retained = content.RetainContent(deadline);
	assert(retained.result == MISTER_RESULT_OK);
	assert(retained.acquired);
	NativeSaveKey save_key = {};
	assert(content.DeriveSaveKey(*FixtureNativeCoreProfile("snes"), &save_key) ==
		MISTER_RESULT_OK);
	assert(save_key.system_id == NativeSystem::snes);
	char bytes[5] = {};
	assert(content.ReadAt(0, bytes, sizeof(bytes), deadline) == MISTER_RESULT_OK);
	assert(memcmp(bytes, "hello", sizeof(bytes)) == 0);
	assert(unlink(path.c_str()) == 0);
	WriteFile(path, "world");
	assert(content.ReadAt(0, bytes, sizeof(bytes), deadline) ==
		MISTER_RESULT_PLATFORM);
	assert(content.active());
	assert(content.CloseContent(deadline) == MISTER_RESULT_OK);
	assert(!content.active());
	RemoveArtifactTree(root, "snes", name);
}

void TestRetainedContentSystemMustMatchTheAdmittedSaveProfile()
{
	struct Case {
		const char *retained_system;
		size_t retained_system_length;
		const char *extension;
		const char *admitted_system;
	};
	const Case cases[] = {
		{"snes", 4, "sfc", "megadrive"},
		{"megadrive", 9, "gen", "snes"}
	};
	for (const Case &test : cases) {
		const std::string root = TemporaryRoot();
		assert(mkdir(Join(root, test.retained_system).c_str(), 0700) == 0);
		const std::string name =
			"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824." +
			std::string(test.extension);
		WriteFile(Join(Join(root, test.retained_system), name), "hello");
		NativePosixFileSystem filesystem;
		NativeContentAdapter content(root.c_str(), filesystem);
		NativeArtifactAuthority authority = HelloAuthority(test.retained_system,
			test.retained_system_length);
		authority.extension = test.extension;
		authority.extension_length = strlen(test.extension);
		assert(content.Configure(authority) == MISTER_RESULT_OK);
		const uint64_t deadline = filesystem.NowMs() + 1000;
		assert(content.RetainContent(deadline).result == MISTER_RESULT_OK);
		NativeSaveKey key = {};
		assert(content.DeriveSaveKey(*FixtureNativeCoreProfile(test.admitted_system),
			&key) == MISTER_RESULT_UNSUPPORTED);
		assert(content.CloseContent(deadline) == MISTER_RESULT_OK);
		RemoveArtifactTree(root, test.retained_system, name);
	}
}

class TestClock final : public NativeClock {
public:
	explicit TestClock(uint64_t now) : now_(now) {}
	uint64_t NowMs() const override { return now_; }
	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t deadline) override { return now_ < deadline; }
	void SetNow(uint64_t now) { now_ = now; }
private:
	uint64_t now_;
};

class FaultFileSystem final : public NativeFileSystem {
public:
	enum ForeignKind { foreign_pipe, foreign_file, foreign_socket };
	explicit FaultFileSystem(NativeFileSystem &delegate)
		: delegate_(delegate), call_(0), fail_at_(0), close_failures_(0),
		  opens_(0), read_calls_(0), read_fail_at_(0), read_short_at_(0),
		  read_overlong_at_(0), read_at_calls_(0), read_at_fail_at_(0),
		  read_at_short_at_(0), read_at_overlong_at_(0), now_override_(0),
		  advance_after_read_to_(0), advance_after_read_at_to_(0),
		  advance_after_close_to_(0), fail_close_at_(0), reuse_close_at_(0),
		  foreign_descriptor_(-1), foreign_peer_(-1),
		  foreign_kind_(foreign_pipe), now_calls_(0), expire_now_at_(0),
		  expire_now_to_(0), after_hash_(), opened_descriptors_(),
		  close_attempts_(), during_close_() {}
	uint64_t NowMs() const override
	{
		++now_calls_;
		if (expire_now_at_ != 0 && now_calls_ >= expire_now_at_)
			return expire_now_to_;
		return now_override_ == 0 ? delegate_.NowMs() : now_override_;
	}
	int OpenAt(int parent, const char *name, int flags, mode_t mode) override
	{
		if (Fail()) return -1;
		const int result = delegate_.OpenAt(parent, name, flags, mode);
		if (result >= 0) {
			++opens_;
			opened_descriptors_.push_back(result);
		}
		return result;
	}
	int StatAt(int parent, const char *name, struct stat *info,
		int flags) override
	{ if (Fail()) return -1; return delegate_.StatAt(parent, name, info, flags); }
	int Stat(int descriptor, struct stat *info) override
	{ if (Fail()) return -1; return delegate_.Stat(descriptor, info); }
	off_t Seek(int descriptor, off_t offset, int whence) override
	{ if (Fail()) return -1; return delegate_.Seek(descriptor, offset, whence); }
	ssize_t Read(int descriptor, void *bytes, size_t count) override
	{
		if (Fail()) return -1;
		++read_calls_;
		if (read_fail_at_ == read_calls_) return -1;
		const ssize_t result = delegate_.Read(descriptor, bytes, count);
		if (read_short_at_ == read_calls_ && result > 0) return result - 1;
		if (read_overlong_at_ == read_calls_)
			return static_cast<ssize_t>(count + 1);
		if (result > 0 && advance_after_read_to_ != 0)
			now_override_ = advance_after_read_to_;
		if (result == 0 && after_hash_) {
			std::function<void()> mutation = after_hash_;
			after_hash_ = std::function<void()>();
			mutation();
		}
		return result;
	}
	ssize_t ReadAt(int descriptor, void *bytes, size_t count,
		off_t offset) override
	{
		if (Fail()) return -1;
		++read_at_calls_;
		if (read_at_fail_at_ == read_at_calls_) return -1;
		const bool short_read = read_at_short_at_ == read_at_calls_ && count > 1;
		const ssize_t result = delegate_.ReadAt(descriptor, bytes,
			short_read ? count - 1 : count, offset);
		if (read_at_overlong_at_ == read_at_calls_)
			return static_cast<ssize_t>(count + 1);
		if (result > 0 && advance_after_read_at_to_ != 0)
			now_override_ = advance_after_read_at_to_;
		return result;
	}
	int Close(int descriptor) override
	{
		++call_;
		close_attempts_.push_back(descriptor);
		if (during_close_) {
			std::function<void()> callback = during_close_;
			during_close_ = std::function<void()>();
			callback();
		}
		if (reuse_close_at_ == close_attempts_.size()) {
			assert(delegate_.Close(descriptor) == 0);
			InstallForeignDescriptor(descriptor);
			if (advance_after_close_to_ != 0)
				now_override_ = advance_after_close_to_;
			return -1;
		}
		if (fail_close_at_ == close_attempts_.size()) return -1;
		if (close_failures_ != 0) { --close_failures_; return -1; }
		if (fail_at_ != 0 && call_ == fail_at_) return -1;
		const int result = delegate_.Close(descriptor);
		if (advance_after_close_to_ != 0) now_override_ = advance_after_close_to_;
		return result;
	}
	void InstallForeignDescriptor(int descriptor)
	{
		int pair[2] = {-1, -1};
		if (foreign_kind_ == foreign_socket)
			assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
		else if (foreign_kind_ == foreign_pipe)
			assert(pipe(pair) == 0);
		else {
			char path[] = "/tmp/fogcast-foreign-fd.XXXXXX";
			pair[0] = mkstemp(path);
			assert(pair[0] >= 0);
			assert(unlink(path) == 0);
		}
		assert(pair[0] == descriptor);
		foreign_descriptor_ = pair[0];
		foreign_peer_ = pair[1];
	}
	void AssertForeignUsable()
	{
		const char value = 'z';
		char observed = 0;
		if (foreign_kind_ == foreign_file) {
			assert(write(foreign_descriptor_, &value, 1) == 1);
			assert(lseek(foreign_descriptor_, 0, SEEK_SET) == 0);
			assert(read(foreign_descriptor_, &observed, 1) == 1);
		} else {
			assert(write(foreign_peer_, &value, 1) == 1);
			assert(read(foreign_descriptor_, &observed, 1) == 1);
		}
		assert(observed == value);
	}
	void CloseForeign()
	{
		assert(close(foreign_descriptor_) == 0);
		if (foreign_peer_ >= 0) assert(close(foreign_peer_) == 0);
		foreign_descriptor_ = -1;
		foreign_peer_ = -1;
	}
	bool Fail()
	{
		++call_;
		return fail_at_ != 0 && call_ == fail_at_;
	}
	NativeFileSystem &delegate_;
	unsigned call_;
	unsigned fail_at_;
	unsigned close_failures_;
	unsigned opens_;
	unsigned read_calls_;
	unsigned read_fail_at_;
	unsigned read_short_at_;
	unsigned read_overlong_at_;
	unsigned read_at_calls_;
	unsigned read_at_fail_at_;
	unsigned read_at_short_at_;
	unsigned read_at_overlong_at_;
	mutable uint64_t now_override_;
	uint64_t advance_after_read_to_;
	uint64_t advance_after_read_at_to_;
	uint64_t advance_after_close_to_;
	size_t fail_close_at_;
	size_t reuse_close_at_;
	int foreign_descriptor_;
	int foreign_peer_;
	ForeignKind foreign_kind_;
	mutable unsigned now_calls_;
	unsigned expire_now_at_;
	uint64_t expire_now_to_;
	std::function<void()> after_hash_;
	std::vector<int> opened_descriptors_;
	std::vector<int> close_attempts_;
	std::function<void()> during_close_;
};

void TestRetainedCoreCloseIsOrderedAndIdempotent()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem posix;
	FaultFileSystem filesystem(posix);
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeCoreArtifactHandle artifact;
	assert(ResolveSnesFixtureForTest(adapter, posix.NowMs() + 1000, &artifact) ==
		NativeArtifactResult::ok);
	const std::vector<int> opened = filesystem.opened_descriptors_;
	const unsigned opens = filesystem.opens_;
	assert(opened.size() >= 4);
	filesystem.during_close_ = [&]() {
		assert(!artifact.valid());
		assert(artifact.size() == 0);
		assert(artifact.owns_descriptors());
		assert(!artifact.closure_unknown());
	};
	assert(artifact.CloseRetainedBefore(posix.NowMs() + 1000) ==
		NativeArtifactResult::ok);
	assert(filesystem.close_attempts_.size() == opened.size());
	for (size_t index = 0; index < opened.size(); ++index)
		assert(filesystem.close_attempts_[index] ==
			opened[opened.size() - index - 1]);
	assert(!artifact.owns_descriptors());
	assert(!artifact.closure_unknown());
	assert(!artifact.valid());
	assert(artifact.size() == 0);
	assert(artifact.CloseRetainedBefore(posix.NowMs() + 1000) ==
		NativeArtifactResult::ok);
	assert(filesystem.close_attempts_.size() == opened.size());
	assert(filesystem.opens_ == opens);
	RemoveArtifactTree(root, "snes", name);
}

void TestRetainedCloseNeverTargetsAReusedForeignDescriptor(bool content_path)
{
	size_t descriptor_total = 0;
	for (size_t failed_index = 0;
		descriptor_total == 0 || failed_index < descriptor_total; ++failed_index) {
		const std::string root = TemporaryRoot();
		assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
		const char *const suffix = content_path ? ".sfc" : ".rbf";
		const std::string name = std::string(
			"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") +
			suffix;
		WriteFile(Join(Join(root, "snes"), name), "hello");
		NativePosixFileSystem posix;
		FaultFileSystem filesystem(posix);
		filesystem.foreign_kind_ = static_cast<FaultFileSystem::ForeignKind>(
			failed_index % 3);
		if (content_path) {
			NativeContentAdapter content(root.c_str(), filesystem);
			NativeArtifactAuthority authority = HelloAuthority();
			authority.extension = "sfc";
			authority.extension_length = 3;
			assert(content.Configure(authority) == MISTER_RESULT_OK);
			assert(content.RetainContent(posix.NowMs() + 1000).result ==
				MISTER_RESULT_OK);
			descriptor_total = filesystem.opened_descriptors_.size();
			filesystem.reuse_close_at_ = failed_index + 1;
			assert(content.CloseContent(posix.NowMs() + 1000) ==
				MISTER_RESULT_CLEANUP_INCOMPLETE);
			assert(content.owns_descriptors());
			assert(!content.active());
			const size_t attempts = filesystem.close_attempts_.size();
			filesystem.AssertForeignUsable();
			assert(content.CloseContent(posix.NowMs() + 1000) ==
				MISTER_RESULT_CLEANUP_INCOMPLETE);
			content.CloseContentForProcessExit();
			assert(filesystem.close_attempts_.size() == attempts);
			filesystem.AssertForeignUsable();
			assert(content.Configure(authority) == MISTER_RESULT_INVALID_ARGUMENT);
			assert(content.RetainContent(posix.NowMs() + 1000).result ==
				MISTER_RESULT_INVALID_STATE);
		} else {
			NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
			NativeCoreArtifactHandle *artifact = new NativeCoreArtifactHandle;
			assert(ResolveSnesFixtureForTest(adapter, posix.NowMs() + 1000,
				artifact) == NativeArtifactResult::ok);
			descriptor_total = filesystem.opened_descriptors_.size();
			filesystem.reuse_close_at_ = failed_index + 1;
			assert(artifact->CloseRetainedBefore(posix.NowMs() + 1000) ==
				NativeArtifactResult::cleanup_incomplete);
			assert(artifact->closure_unknown());
			assert(artifact->owns_descriptors());
			assert(!artifact->valid());
			assert(artifact->size() == 0);
			const size_t attempts = filesystem.close_attempts_.size();
			filesystem.AssertForeignUsable();
			assert(artifact->CloseRetainedBefore(posix.NowMs() + 1000) ==
				NativeArtifactResult::cleanup_incomplete);
			assert(artifact->Close() == NativeArtifactResult::cleanup_incomplete);
			assert(adapter.Resolve(HelloAuthority(), posix.NowMs() + 1000,
				artifact) == NativeArtifactResult::invalid_argument);
			delete artifact;
			assert(filesystem.close_attempts_.size() == attempts);
			filesystem.AssertForeignUsable();
		}
		assert(filesystem.close_attempts_.size() == descriptor_total);
		filesystem.CloseForeign();
		RemoveArtifactTree(root, "snes", name);
	}
}

void TestRetainedCoreCloseHonorsEqualityAndLateDeadline()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem posix;
	FaultFileSystem filesystem(posix);
	filesystem.now_override_ = 10;
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeCoreArtifactHandle artifact;
	assert(ResolveSnesFixtureForTest(adapter, 20, &artifact) ==
		NativeArtifactResult::ok);
	const unsigned opens = filesystem.opens_;
	assert(artifact.CloseRetainedBefore(10) == NativeArtifactResult::deadline);
	assert(artifact.owns_descriptors());
	assert(filesystem.close_attempts_.empty());
	filesystem.now_override_ = 11;
	assert(artifact.CloseRetainedBefore(10) == NativeArtifactResult::deadline);
	assert(artifact.owns_descriptors());
	assert(filesystem.close_attempts_.empty());
	filesystem.now_override_ = 10;
	filesystem.advance_after_close_to_ = 20;
	assert(artifact.CloseRetainedBefore(20) == NativeArtifactResult::deadline);
	assert(artifact.owns_descriptors());
	assert(filesystem.close_attempts_.size() == 1);
	filesystem.advance_after_close_to_ = 0;
	filesystem.now_override_ = 21;
	assert(artifact.CloseRetainedBefore(30) == NativeArtifactResult::ok);
	assert(!artifact.owns_descriptors());
	assert(filesystem.opens_ == opens);
	RemoveArtifactTree(root, "snes", name);
}

void TestRetainedCoreCloseHonorsEveryDeadlineBoundary()
{
	unsigned final_boundary = 0;
	for (unsigned boundary = 1;
		final_boundary == 0 || boundary <= final_boundary; ++boundary) {
		const std::string root = TemporaryRoot();
		assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
		const std::string name =
			"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
		WriteFile(Join(Join(root, "snes"), name), "hello");
		NativePosixFileSystem posix;
		FaultFileSystem filesystem(posix);
		filesystem.now_override_ = 10;
		NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
		NativeCoreArtifactHandle artifact;
		assert(ResolveSnesFixtureForTest(adapter, 20, &artifact) ==
			NativeArtifactResult::ok);
		const size_t descriptor_count = filesystem.opened_descriptors_.size();
		assert(descriptor_count >= 4);
		if (final_boundary == 0)
			final_boundary = static_cast<unsigned>(descriptor_count * 2);
		else assert(final_boundary == descriptor_count * 2);
		const unsigned opens = filesystem.opens_;
		filesystem.now_calls_ = 0;
		filesystem.expire_now_at_ = boundary;
		filesystem.expire_now_to_ = 20;
		assert(artifact.CloseRetainedBefore(20) ==
			NativeArtifactResult::deadline);
		assert(filesystem.close_attempts_.size() == boundary / 2);
		assert(artifact.owns_descriptors() == (boundary < descriptor_count * 2));
		filesystem.expire_now_at_ = 0;
		filesystem.now_calls_ = 0;
		assert(artifact.CloseRetainedBefore(20) == NativeArtifactResult::ok);
		assert(filesystem.close_attempts_.size() == descriptor_count);
		assert(filesystem.opens_ == opens);
		assert(!artifact.owns_descriptors());
		RemoveArtifactTree(root, "snes", name);
	}
}

void TestRetainedCoreCloseFailureIsStickyAndNeverRetried()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem posix;
	FaultFileSystem filesystem(posix);
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeCoreArtifactHandle artifact;
	assert(ResolveSnesFixtureForTest(adapter, posix.NowMs() + 1000, &artifact) ==
		NativeArtifactResult::ok);
	const int leaked = filesystem.opened_descriptors_.back();
	filesystem.fail_close_at_ = 1;
	assert(artifact.CloseRetainedBefore(posix.NowMs() + 1000) ==
		NativeArtifactResult::cleanup_incomplete);
	assert(artifact.closure_unknown());
	const size_t attempts = filesystem.close_attempts_.size();
	assert(artifact.CloseRetainedBefore(posix.NowMs() + 1000) ==
		NativeArtifactResult::cleanup_incomplete);
	assert(artifact.Close() == NativeArtifactResult::cleanup_incomplete);
	assert(filesystem.close_attempts_.size() == attempts);
	assert(close(leaked) == 0);
	RemoveArtifactTree(root, "snes", name);
}

void TestRetainedCoreCloseFailurePrecedesDeadlineAndIsReentrant()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem posix;
	FaultFileSystem filesystem(posix);
	filesystem.now_override_ = 10;
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeCoreArtifactHandle artifact;
	assert(ResolveSnesFixtureForTest(adapter, 20, &artifact) ==
		NativeArtifactResult::ok);
	filesystem.reuse_close_at_ = 1;
	filesystem.advance_after_close_to_ = 20;
	assert(artifact.CloseRetainedBefore(20) ==
		NativeArtifactResult::cleanup_incomplete);
	assert(artifact.owns_descriptors());
	assert(artifact.closure_unknown());
	assert(filesystem.close_attempts_.size() == 1);
	filesystem.AssertForeignUsable();
	filesystem.advance_after_close_to_ = 0;
	filesystem.now_override_ = 10;
	filesystem.during_close_ = [&]() {
		assert(artifact.CloseRetainedBefore(30) ==
			NativeArtifactResult::cleanup_incomplete);
	};
	assert(artifact.CloseRetainedBefore(30) ==
		NativeArtifactResult::cleanup_incomplete);
	assert(filesystem.close_attempts_.size() ==
		filesystem.opened_descriptors_.size());
	filesystem.AssertForeignUsable();
	filesystem.CloseForeign();
	RemoveArtifactTree(root, "snes", name);
}

void TestContentCloseRetainsOnlyDeadlineSkippedDescriptors()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.sfc";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem posix;
	FaultFileSystem filesystem(posix);
	filesystem.now_override_ = 10;
	NativeContentAdapter content(root.c_str(), filesystem);
	NativeArtifactAuthority authority = HelloAuthority();
	authority.extension = "sfc";
	authority.extension_length = 3;
	assert(content.Configure(authority) == MISTER_RESULT_OK);
	assert(content.RetainContent(20).result == MISTER_RESULT_OK);
	assert(content.CloseContent(10) == MISTER_RESULT_DEADLINE);
	assert(filesystem.close_attempts_.empty());
	assert(content.owns_descriptors());
	filesystem.advance_after_close_to_ = 20;
	assert(content.CloseContent(20) == MISTER_RESULT_DEADLINE);
	assert(filesystem.close_attempts_.size() == 1);
	assert(content.owns_descriptors());
	filesystem.advance_after_close_to_ = 0;
	filesystem.now_override_ = 21;
	assert(content.CloseContent(30) == MISTER_RESULT_OK);
	assert(!content.owns_descriptors());
	RemoveArtifactTree(root, "snes", name);
}

void TestContentReadAtFailureAndDeadlineBoundaries()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.sfc";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem posix;
	FaultFileSystem filesystem(posix);
	filesystem.now_override_ = 10;
	NativeContentAdapter content(root.c_str(), filesystem);
	NativeArtifactAuthority authority = HelloAuthority();
	authority.extension = "sfc";
	authority.extension_length = 3;
	assert(content.Configure(authority) == MISTER_RESULT_OK);
	assert(content.RetainContent(20).result == MISTER_RESULT_OK);
	char bytes[5] = {};
	filesystem.read_at_fail_at_ = filesystem.read_at_calls_ + 1;
	assert(content.ReadAt(0, bytes, sizeof(bytes), 20) ==
		MISTER_RESULT_PLATFORM);
	filesystem.read_at_fail_at_ = 0;
	filesystem.read_at_short_at_ = filesystem.read_at_calls_ + 1;
	assert(content.ReadAt(0, bytes, sizeof(bytes), 20) == MISTER_RESULT_OK);
	assert(memcmp(bytes, "hello", sizeof(bytes)) == 0);
	filesystem.read_at_short_at_ = 0;
	filesystem.read_at_overlong_at_ = filesystem.read_at_calls_ + 1;
	assert(content.ReadAt(0, bytes, sizeof(bytes), 20) ==
		MISTER_RESULT_PLATFORM);
	filesystem.read_at_overlong_at_ = 0;
	filesystem.advance_after_read_at_to_ = 20;
	assert(content.ReadAt(0, bytes, sizeof(bytes), 20) ==
		MISTER_RESULT_DEADLINE);
	assert(content.owns_descriptors());
	filesystem.advance_after_read_at_to_ = 0;
	assert(content.CloseContent(30) == MISTER_RESULT_OK);
	RemoveArtifactTree(root, "snes", name);
}

void TestRejectedContentConfigurationIsAtomic()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.sfc";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem filesystem;
	NativeContentAdapter content(root.c_str(), filesystem);
	NativeArtifactAuthority valid = HelloAuthority();
	valid.extension = "sfc";
	valid.extension_length = 3;
	assert(content.Configure(valid) == MISTER_RESULT_OK);
	NativeArtifactAuthority rejected = valid;
	rejected.system = "megadrive";
	rejected.system_length = 9;
	rejected.sha256 = nullptr;
	assert(content.Configure(rejected) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(content.RetainContent(filesystem.NowMs() + 1000).result ==
		MISTER_RESULT_OK);
	assert(content.CloseContent(filesystem.NowMs() + 1000) == MISTER_RESULT_OK);
	RemoveArtifactTree(root, "snes", name);
}

class CollectingSink final : public NativeFpgaByteSink {
public:
	CollectingSink() : fail_(false), fail_after_accept_(false), zero_(false),
		over_accept_(false), max_accept_(SIZE_MAX), last_deadline_(0),
		writes_(0), clock_(nullptr), advance_to_(0), advance_at_write_(0),
		over_accept_at_write_(0), reported_accept_(0), callback_at_write_(0),
		after_write_(), begin_result_(MISTER_RESULT_OK),
		begin_provide_session_(true), start_acquired_(false),
		start_attempted_(false), start_applied_(false),
		finish_result_(MISTER_RESULT_OK), finish_configuration_(true),
		finish_initialization_(true), finish_user_(true), finish_released_(true),
		finish_attempted_(false), finish_applied_(false) {}

private:
	class Session final : public NativeFpgaProgramSession {
	public:
		explicit Session(CollectingSink &owner) : owner_(owner) {}

	private:
		NativeFpgaSinkWriteOutcome Write(const unsigned char *bytes, size_t count,
			uint64_t absolute_deadline_ms) override
		{
			return owner_.WriteBytes(bytes, count, absolute_deadline_ms);
		}
		NativeFpgaSinkFinishOutcome Finish(uint64_t) override
		{
			const NativeFpgaSinkFinishOutcome outcome = {
				owner_.finish_result_, owner_.finish_attempted_,
				owner_.finish_applied_, owner_.finish_configuration_,
				owner_.finish_initialization_, owner_.finish_user_,
				owner_.finish_released_};
			return outcome;
		}
		CollectingSink &owner_;
	};

	NativeFpgaSinkStartOutcome Begin(uint64_t,
		uint64_t absolute_deadline_ms,
		std::unique_ptr<NativeFpgaProgramSession> *session) override
	{
		last_deadline_ = absolute_deadline_ms;
		if (begin_provide_session_) session->reset(new Session(*this));
		const NativeFpgaSinkStartOutcome outcome = {
			begin_result_, start_acquired_, start_attempted_, start_applied_};
		return outcome;
	}
	NativeFpgaSinkWriteOutcome WriteBytes(const unsigned char *bytes,
		size_t count, uint64_t absolute_deadline_ms)
	{
		++writes_;
		last_deadline_ = absolute_deadline_ms;
		if (fail_) {
			const NativeFpgaSinkWriteOutcome outcome = {
				MISTER_RESULT_PLATFORM, 0, false, false};
			return outcome;
		}
		size_t accepted = zero_ ? 0 : (count < max_accept_ ? count : max_accept_);
		const size_t copied = accepted > count ? count : accepted;
		bytes_.insert(bytes_.end(), bytes, bytes + copied);
		if (over_accept_ &&
			(over_accept_at_write_ == 0 || writes_ == over_accept_at_write_))
			accepted = reported_accept_ == 0 ? count + 1 : reported_accept_;
		if (clock_ != nullptr &&
			(advance_at_write_ == 0 || writes_ == advance_at_write_))
			clock_->SetNow(advance_to_);
		if (after_write_ && writes_ == callback_at_write_) after_write_();
		const NativeFpgaSinkWriteOutcome outcome = {
			fail_after_accept_ ? MISTER_RESULT_PLATFORM : MISTER_RESULT_OK,
			accepted, accepted != 0, accepted != 0};
		return outcome;
	}

public:
	bool fail_;
	bool fail_after_accept_;
	bool zero_;
	bool over_accept_;
	size_t max_accept_;
	uint64_t last_deadline_;
	unsigned writes_;
	std::vector<unsigned char> bytes_;
	TestClock *clock_;
	uint64_t advance_to_;
	unsigned advance_at_write_;
	unsigned over_accept_at_write_;
	size_t reported_accept_;
	unsigned callback_at_write_;
	std::function<void()> after_write_;
	Result begin_result_;
	bool begin_provide_session_;
	bool start_acquired_;
	bool start_attempted_;
	bool start_applied_;
	Result finish_result_;
	bool finish_configuration_;
	bool finish_initialization_;
	bool finish_user_;
	bool finish_released_;
	bool finish_attempted_;
	bool finish_applied_;
};

void TestProgrammingRequiresExactActiveProgramLeaseAndDeadline()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	const std::string path = Join(Join(root, "snes"), name);
	WriteFile(path, "hello");
	NativePosixFileSystem filesystem;
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeCoreArtifactHandle artifact;
	NativeCoreArtifactHandle unbound;
	assert(adapter.Resolve(HelloAuthority(), filesystem.NowMs() + 1000,
		&unbound) == NativeArtifactResult::ok);
	assert(ResolveSnesFixtureForTest(adapter, filesystem.NowMs() + 1000,
		&artifact) == NativeArtifactResult::ok);
	const uint64_t start = filesystem.NowMs();
	TestClock clock(start);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	CollectingSink sink;
	sink.max_accept_ = 2;
	NativeFpgaProgrammer programmer(broker, clock, sink);
	std::unique_ptr<OperationLease> unbound_program;
	assert(broker.Begin(generation, OperationKind::program_fpga, start + 100,
		&unbound_program) == MISTER_RESULT_OK);
	const NativeFpgaProgrammingReceipt unbound_denied =
		programmer.Program(*unbound_program, unbound);
	assert(unbound_denied.result == MISTER_RESULT_INVALID_ARGUMENT);
	assert(unbound_denied.accepted_bytes == 0);
	assert(unbound_denied.mutation_sequence == 0);
	assert(broker.mutation_sequence_for_test() == 0);
	unbound_program.reset();
	assert(unbound.Close() == NativeArtifactResult::ok);
	std::unique_ptr<OperationLease> program;
	assert(broker.Begin(generation, OperationKind::program_fpga, start + 100,
		&program) == MISTER_RESULT_OK);
	const NativeFpgaProgrammingReceipt programmed =
		programmer.Program(*program, artifact);
	assert(programmed.result == MISTER_RESULT_OK);
	assert(programmed.accepted_bytes == 5);
	assert(programmed.mutation_sequence == 3);
	assert(broker.mutation_sequence_for_test() == 3);
	assert(std::string(sink.bytes_.begin(), sink.bytes_.end()) == "hello");
	assert(sink.writes_ == 3);
	assert(sink.last_deadline_ == start + 100);
	program.reset();

	std::unique_ptr<OperationLease> audio;
	assert(broker.Begin(generation, OperationKind::audio, start + 100, &audio) ==
		MISTER_RESULT_OK);
	const size_t before = sink.bytes_.size();
	const NativeFpgaProgrammingReceipt wrong_kind =
		programmer.Program(*audio, artifact);
	assert(wrong_kind.result == MISTER_RESULT_INVALID_STATE);
	assert(wrong_kind.accepted_bytes == 0);
	assert(wrong_kind.mutation_sequence == 0);
	assert(broker.mutation_sequence_for_test() == 3);
	assert(sink.bytes_.size() == before);
	audio.reset();

	std::unique_ptr<OperationLease> expired;
	assert(broker.Begin(generation, OperationKind::program_fpga, start + 100,
		&expired) == MISTER_RESULT_OK);
	clock.SetNow(start + 100);
	const NativeFpgaProgrammingReceipt expired_receipt =
		programmer.Program(*expired, artifact);
	assert(expired_receipt.result == MISTER_RESULT_DEADLINE);
	assert(expired_receipt.accepted_bytes == 0);
	assert(expired_receipt.mutation_sequence == 0);
	assert(broker.mutation_sequence_for_test() == 3);
	expired.reset();
	assert(artifact.valid());
	assert(artifact.Close() == NativeArtifactResult::ok);
	RemoveArtifactTree(root, "snes", name);
}

void TestProgrammingAuthorityMatrix()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem filesystem;
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeCoreArtifactHandle artifact;
	assert(ResolveSnesFixtureForTest(adapter, filesystem.NowMs() + 1000,
		&artifact) == NativeArtifactResult::ok);
	const uint64_t start = filesystem.NowMs();
	TestClock clock(start);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	CollectingSink sink;
	NativeFpgaProgrammer programmer(broker, clock, sink);
	TestClock mismatched_clock(start);
	HardwareBroker mismatched_broker(mismatched_clock);
	PlatformGenerationId mismatched_generation = 0;
	assert(mismatched_broker.EnterFixtureForTest(
		*FixtureNativeCoreProfile("megadrive"), &mismatched_generation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> mismatched_lease;
	assert(mismatched_broker.Begin(mismatched_generation,
		OperationKind::program_fpga, start + 1000, &mismatched_lease) ==
		MISTER_RESULT_OK);
	CollectingSink mismatched_sink;
	NativeFpgaProgrammer mismatched_programmer(mismatched_broker,
		mismatched_clock, mismatched_sink);
	const NativeFpgaProgrammingReceipt mismatch =
		mismatched_programmer.Program(*mismatched_lease, artifact);
	assert(mismatch.result == MISTER_RESULT_INVALID_STATE);
	assert(mismatch.accepted_bytes == 0);
	assert(mismatch.mutation_sequence == 0);
	assert(mismatched_broker.mutation_sequence_for_test() == 0);
	assert(mismatched_sink.bytes_.empty());
	mismatched_lease.reset();
	const OperationKind denied_kinds[] = {
		OperationKind::core_protocol, OperationKind::input,
		OperationKind::scheduler, OperationKind::offload, OperationKind::save,
		OperationKind::audio, OperationKind::video, OperationKind::content,
		OperationKind::input_descriptors
	};
	for (size_t index = 0; index < sizeof(denied_kinds) /
		sizeof(denied_kinds[0]); ++index) {
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, denied_kinds[index], start + 1000,
			&lease) == MISTER_RESULT_OK);
		const NativeFpgaProgrammingReceipt denied =
			programmer.Program(*lease, artifact);
		assert(denied.result == MISTER_RESULT_INVALID_STATE);
		assert(denied.accepted_bytes == 0);
		assert(denied.mutation_sequence == 0);
		assert(sink.bytes_.empty());
	}
	assert(broker.mutation_sequence_for_test() == 0);
	assert(broker.Quiesce(generation, start + 1000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, start + 2000, start + 5000,
		&cleanup) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup_program;
	assert(broker.BeginCleanupOperation(*cleanup, OperationKind::program_fpga,
		&cleanup_program) == MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<OperationLease> cleanup_terminal;
	assert(broker.BeginCleanupOperation(*cleanup,
		OperationKind::terminal_fpga_cleanup, &cleanup_terminal) ==
		MISTER_RESULT_OK);
	const NativeFpgaProgrammingReceipt cleanup_denied =
		programmer.Program(*cleanup_terminal, artifact);
	assert(cleanup_denied.result == MISTER_RESULT_INVALID_STATE);
	assert(cleanup_denied.accepted_bytes == 0);
	assert(cleanup_denied.mutation_sequence == 0);
	assert(broker.mutation_sequence_for_test() == 0);

	TestClock recovery_clock(start);
	HardwareBroker recovery_broker(recovery_clock);
	std::unique_ptr<RecoveryEpoch> recovery;
	assert(recovery_broker.BeginRecovery(MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL,
		start + 2000, start + 5000, &recovery) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> recovery_program;
	assert(recovery_broker.BeginRecoveryOperation(*recovery,
		OperationKind::program_fpga, &recovery_program) ==
		MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<OperationLease> recovery_terminal;
	assert(recovery_broker.BeginRecoveryOperation(*recovery,
		OperationKind::terminal_fpga_cleanup, &recovery_terminal) ==
		MISTER_RESULT_OK);
	CollectingSink recovery_sink;
	NativeFpgaProgrammer recovery_programmer(recovery_broker, recovery_clock,
		recovery_sink);
	const NativeFpgaProgrammingReceipt recovery_denied =
		recovery_programmer.Program(*recovery_terminal, artifact);
	assert(recovery_denied.result == MISTER_RESULT_INVALID_STATE);
	assert(recovery_denied.accepted_bytes == 0);
	assert(recovery_denied.mutation_sequence == 0);
	assert(recovery_broker.mutation_sequence_for_test() == 0);
	assert(artifact.Close() == NativeArtifactResult::ok);
	RemoveArtifactTree(root, "snes", name);
}

void TestResolutionFailureInjectionAndEntrySubstitution()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	const std::string path = Join(Join(root, "snes"), name);
	WriteFile(path, "hello");
	NativePosixFileSystem posix;
	FaultFileSystem measuring(posix);
	NativeCoreArtifactAdapter measuring_adapter(root.c_str(), measuring);
	NativeCoreArtifactHandle measured;
	assert(measuring_adapter.Resolve(HelloAuthority(), posix.NowMs() + 1000,
		&measured) == NativeArtifactResult::ok);
	const unsigned resolution_call_count = measuring.call_;
	assert(resolution_call_count != 0);
	assert(measured.Close() == NativeArtifactResult::ok);
	for (unsigned fail_at = 1; fail_at <= resolution_call_count; ++fail_at) {
		FaultFileSystem filesystem(posix);
		filesystem.fail_at_ = fail_at;
		NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
		NativeCoreArtifactHandle artifact;
		assert(adapter.Resolve(HelloAuthority(), posix.NowMs() + 1000,
			&artifact) != NativeArtifactResult::ok);
		filesystem.fail_at_ = 0;
		while (artifact.owns_descriptors())
			assert(artifact.Close() == NativeArtifactResult::ok);
	}

	FaultFileSystem expiring(posix);
	expiring.now_override_ = 10;
	expiring.advance_after_read_to_ = 20;
	NativeCoreArtifactAdapter expiring_adapter(root.c_str(), expiring);
	NativeCoreArtifactHandle expired;
	assert(expiring_adapter.Resolve(HelloAuthority(), 20, &expired) ==
		NativeArtifactResult::deadline);
	assert(expiring.read_calls_ == 1);
	assert(expired.owns_descriptors());
	assert(expired.Close() == NativeArtifactResult::ok);

	FaultFileSystem substituting(posix);
	const std::string held_path = path + ".held";
	substituting.after_hash_ = [&]() {
		assert(rename(path.c_str(), held_path.c_str()) == 0);
		WriteFile(path, "hello");
	};
	NativeCoreArtifactAdapter adapter(root.c_str(), substituting);
	NativeCoreArtifactHandle changed;
	assert(adapter.Resolve(HelloAuthority(), posix.NowMs() + 1000, &changed) ==
		NativeArtifactResult::changed);
	assert(!changed.valid());
	assert(changed.owns_descriptors());
	assert(changed.Close() == NativeArtifactResult::ok);
	assert(unlink(path.c_str()) == 0);
	assert(rename(held_path.c_str(), path.c_str()) == 0);

	FaultFileSystem directory_substitution(posix);
	const std::string directory = Join(root, "snes");
	const std::string held_directory = directory + ".held";
	directory_substitution.after_hash_ = [&]() {
		assert(rename(directory.c_str(), held_directory.c_str()) == 0);
		assert(mkdir(directory.c_str(), 0700) == 0);
		WriteFile(path, "hello");
	};
	NativeCoreArtifactAdapter directory_adapter(root.c_str(),
		directory_substitution);
	NativeCoreArtifactHandle changed_directory;
	assert(directory_adapter.Resolve(HelloAuthority(), posix.NowMs() + 1000,
		&changed_directory) == NativeArtifactResult::changed);
	assert(changed_directory.Close() == NativeArtifactResult::ok);
	assert(unlink(path.c_str()) == 0);
	assert(rmdir(directory.c_str()) == 0);
	assert(rename(held_directory.c_str(), directory.c_str()) == 0);

	FaultFileSystem root_substitution(posix);
	const std::string held_root = root + ".held";
	root_substitution.after_hash_ = [&]() {
		assert(rename(root.c_str(), held_root.c_str()) == 0);
		assert(mkdir(root.c_str(), 0700) == 0);
		assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
		WriteFile(path, "hello");
	};
	NativeCoreArtifactAdapter root_adapter(root.c_str(), root_substitution);
	NativeCoreArtifactHandle changed_root;
	assert(root_adapter.Resolve(HelloAuthority(), posix.NowMs() + 1000,
		&changed_root) == NativeArtifactResult::changed);
	assert(changed_root.Close() == NativeArtifactResult::ok);
	RemoveArtifactTree(root, "snes", name);
	assert(rename(held_root.c_str(), root.c_str()) == 0);

	FaultFileSystem close_failure(posix);
	NativeCoreArtifactAdapter close_adapter(root.c_str(), close_failure);
	NativeCoreArtifactHandle retained;
	assert(close_adapter.Resolve(HelloAuthority(), posix.NowMs() + 1000,
		&retained) == NativeArtifactResult::ok);
	const int unresolved_descriptor = close_failure.opened_descriptors_.back();
	close_failure.close_failures_ = 1;
	assert(retained.Close() == NativeArtifactResult::cleanup_incomplete);
	assert(retained.owns_descriptors());
	assert(close(unresolved_descriptor) == 0);
	assert(retained.Close() == NativeArtifactResult::cleanup_incomplete);
	assert(retained.owns_descriptors());
	RemoveArtifactTree(root, "snes", name);
}

void TestProgrammingFailureBoundariesRetainAuthority()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	const std::string path = Join(Join(root, "snes"), name);
	WriteFile(path, "hello");
	NativePosixFileSystem posix;
	FaultFileSystem filesystem(posix);
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeCoreArtifactHandle artifact;
	assert(ResolveSnesFixtureForTest(adapter, posix.NowMs() + 1000, &artifact) ==
		NativeArtifactResult::ok);
	const unsigned resolution_opens = filesystem.opens_;
	const uint64_t start = posix.NowMs();
	TestClock clock(start);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	CollectingSink sink;
	NativeFpgaProgrammer programmer(broker, clock, sink);

	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.fail_ = true;
	NativeFpgaProgrammingReceipt receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.accepted_bytes == 0);
	assert(receipt.mutation_sequence == 0);
	assert(broker.mutation_sequence_for_test() == 0);
	assert(artifact.valid());
	assert(filesystem.opens_ == resolution_opens);
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.fail_ = false;
	sink.zero_ = true;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.accepted_bytes == 0);
	assert(receipt.mutation_sequence == 0);
	assert(broker.mutation_sequence_for_test() == 0);
	assert(artifact.valid());
	assert(filesystem.opens_ == resolution_opens);
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.zero_ = false;
	sink.clock_ = &clock;
	sink.advance_to_ = start + 1000;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_DEADLINE);
	assert(receipt.accepted_bytes == 5);
	assert(receipt.mutation_sequence == 1);
	assert(broker.mutation_sequence_for_test() == 1);
	assert(artifact.valid());
	assert(filesystem.opens_ == resolution_opens);
	lease.reset();
	sink.clock_ = nullptr;
	clock.SetNow(start);

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	filesystem.read_fail_at_ = filesystem.read_calls_ + 1;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.accepted_bytes == 0);
	assert(receipt.mutation_sequence == 0);
	assert(artifact.valid());
	assert(filesystem.opens_ == resolution_opens);
	filesystem.read_fail_at_ = 0;
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	filesystem.read_short_at_ = filesystem.read_calls_ + 1;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.accepted_bytes == 0);
	assert(receipt.mutation_sequence == 0);
	filesystem.read_short_at_ = 0;
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	filesystem.read_overlong_at_ = filesystem.read_calls_ + 1;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.accepted_bytes == 0);
	assert(receipt.mutation_sequence == 0);
	filesystem.read_overlong_at_ = 0;
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.zero_ = false;
	sink.over_accept_ = true;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.accepted_bytes == 6);
	assert(receipt.mutation_sequence == 2);
	assert(broker.mutation_sequence_for_test() == 2);
	sink.over_accept_ = false;
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.max_accept_ = 2;
	sink.over_accept_ = true;
	sink.over_accept_at_write_ = sink.writes_ + 2;
	sink.reported_accept_ = SIZE_MAX;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.accepted_bytes == UINT64_MAX);
	assert(receipt.mutation_sequence == 4);
	assert(broker.mutation_sequence_for_test() == 4);
	sink.over_accept_ = false;
	sink.over_accept_at_write_ = 0;
	sink.reported_accept_ = 0;
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.fail_after_accept_ = true;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.accepted_bytes == 2);
	assert(receipt.mutation_sequence == 5);
	assert(broker.mutation_sequence_for_test() == 5);
	sink.fail_after_accept_ = false;
	sink.max_accept_ = SIZE_MAX;
	lease.reset();

	assert(unlink(path.c_str()) == 0);
	WriteFile(path, "world");
	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.accepted_bytes == 0);
	assert(receipt.mutation_sequence == 0);
	assert(broker.mutation_sequence_for_test() == 5);
	assert(artifact.valid());
	assert(filesystem.opens_ == resolution_opens);
	lease.reset();
	assert(artifact.Close() == NativeArtifactResult::ok);
	RemoveArtifactTree(root, "snes", name);
}

void TestProgrammingRejectsMalformedSessionAndIncompleteFinishEvidence()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem filesystem;
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeCoreArtifactHandle artifact;
	assert(ResolveSnesFixtureForTest(adapter, filesystem.NowMs() + 1000,
		&artifact) == NativeArtifactResult::ok);
	const uint64_t start = filesystem.NowMs();
	TestClock clock(start);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	CollectingSink sink;
	NativeFpgaProgrammer programmer(broker, clock, sink);
	std::unique_ptr<OperationLease> lease;
	NativeFpgaProgrammingReceipt receipt = {};

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.begin_provide_session_ = false;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(!receipt.acquired);
	assert(sink.writes_ == 0);
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.begin_provide_session_ = true;
	sink.begin_result_ = MISTER_RESULT_UNSUPPORTED;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(!receipt.acquired);
	assert(sink.writes_ == 0);
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.begin_provide_session_ = false;
	sink.begin_result_ = MISTER_RESULT_UNSUPPORTED;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_UNSUPPORTED);
	assert(!receipt.acquired);
	assert(sink.writes_ == 0);
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.start_attempted_ = true;
	sink.start_applied_ = true;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_UNSUPPORTED);
	assert(receipt.acquired);
	assert(receipt.mutation_sequence != 0);
	assert(sink.writes_ == 0);
	lease.reset();

	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&lease) == MISTER_RESULT_OK);
	sink.begin_provide_session_ = true;
	sink.begin_result_ = MISTER_RESULT_OK;
	sink.start_attempted_ = false;
	sink.start_applied_ = false;
	sink.finish_configuration_ = false;
	receipt = programmer.Program(*lease, artifact);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.acquired);
	assert(receipt.accepted_bytes == artifact.size());
	assert(!receipt.configuration_done_observed);
	assert(receipt.initialization_observed);
	assert(receipt.user_mode_observed);
	assert(receipt.manager_drive_released);
	assert(receipt.mutation_sequence != 0);
	lease.reset();

	assert(artifact.Close() == NativeArtifactResult::ok);
	RemoveArtifactTree(root, "snes", name);
}

void TestProgrammingReceiptSurvivesLateDeadlineAndBrokerDestruction()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem filesystem;
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeCoreArtifactHandle artifact;
	assert(ResolveSnesFixtureForTest(adapter, filesystem.NowMs() + 1000,
		&artifact) == NativeArtifactResult::ok);
	const uint64_t start = filesystem.NowMs();

	TestClock deadline_clock(start);
	HardwareBroker deadline_broker(deadline_clock);
	PlatformGenerationId deadline_generation = 0;
	assert(deadline_broker.EnterFixtureForTest(
		*FixtureNativeCoreProfile("snes"), &deadline_generation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> deadline_lease;
	assert(deadline_broker.Begin(deadline_generation,
		OperationKind::program_fpga, start + 1000, &deadline_lease) ==
		MISTER_RESULT_OK);
	CollectingSink deadline_sink;
	deadline_sink.max_accept_ = 2;
	deadline_sink.clock_ = &deadline_clock;
	deadline_sink.advance_to_ = start + 1000;
	deadline_sink.advance_at_write_ = 2;
	NativeFpgaProgrammer deadline_programmer(deadline_broker, deadline_clock,
		deadline_sink);
	const NativeFpgaProgrammingReceipt deadline_receipt =
		deadline_programmer.Program(*deadline_lease, artifact);
	assert(deadline_receipt.result == MISTER_RESULT_DEADLINE);
	assert(deadline_receipt.accepted_bytes == 4);
	assert(deadline_receipt.mutation_sequence == 2);
	assert(deadline_broker.mutation_sequence_for_test() == 2);
	deadline_lease.reset();

	TestClock destroyed_clock(start);
	std::unique_ptr<HardwareBroker> destroyed_broker(
		new HardwareBroker(destroyed_clock));
	PlatformGenerationId destroyed_generation = 0;
	assert(destroyed_broker->EnterFixtureForTest(
		*FixtureNativeCoreProfile("snes"), &destroyed_generation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> destroyed_lease;
	assert(destroyed_broker->Begin(destroyed_generation,
		OperationKind::program_fpga, start + 1000, &destroyed_lease) ==
		MISTER_RESULT_OK);
	CollectingSink destroyed_sink;
	destroyed_sink.max_accept_ = 2;
	destroyed_sink.callback_at_write_ = 2;
	destroyed_sink.after_write_ = [&destroyed_broker]() {
		destroyed_broker.reset();
	};
	NativeFpgaProgrammer destroyed_programmer(*destroyed_broker,
		destroyed_clock, destroyed_sink);
	const NativeFpgaProgrammingReceipt destroyed_receipt =
		destroyed_programmer.Program(*destroyed_lease, artifact);
	assert(destroyed_receipt.result == MISTER_RESULT_PLATFORM);
	assert(destroyed_receipt.accepted_bytes == 4);
	assert(destroyed_receipt.mutation_sequence == 1);
	assert(destroyed_broker.get() == nullptr);
	destroyed_lease.reset();

	assert(artifact.Close() == NativeArtifactResult::ok);
	RemoveArtifactTree(root, "snes", name);
}

void TestTwoHundredArtifactProgramCycles()
{
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	WriteFile(Join(Join(root, "snes"), name), "hello");
	NativePosixFileSystem filesystem;
	for (unsigned cycle = 0; cycle < 200; ++cycle) {
		NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
		NativeCoreArtifactHandle artifact;
		assert(ResolveSnesFixtureForTest(adapter, filesystem.NowMs() + 1000,
			&artifact) == NativeArtifactResult::ok);
		const uint64_t start = filesystem.NowMs();
		TestClock clock(start);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
			&generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::program_fpga,
			start + 1000, &lease) == MISTER_RESULT_OK);
		CollectingSink sink;
		NativeFpgaProgrammer programmer(broker, clock, sink);
		const NativeFpgaProgrammingReceipt receipt =
			programmer.Program(*lease, artifact);
		assert(receipt.result == MISTER_RESULT_OK);
		assert(receipt.accepted_bytes == 5);
		assert(receipt.mutation_sequence == 1);
		assert(broker.mutation_sequence_for_test() == 1);
		assert(std::string(sink.bytes_.begin(), sink.bytes_.end()) == "hello");
		lease.reset();
		assert(artifact.Close() == NativeArtifactResult::ok);
	}
	RemoveArtifactTree(root, "snes", name);
}

void TestInsecureEntrySizeAndDigestFailures()
{
	NativePosixFileSystem filesystem;
	const std::string root = TemporaryRoot();
	assert(mkdir(Join(root, "snes").c_str(), 0700) == 0);
	const std::string name =
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	const std::string path = Join(Join(root, "snes"), name);
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);

	assert(symlink("/dev/null", path.c_str()) == 0);
	NativeCoreArtifactHandle symlinked;
	assert(adapter.Resolve(HelloAuthority(), filesystem.NowMs() + 1000,
		&symlinked) == NativeArtifactResult::insecure);
	assert(symlinked.Close() == NativeArtifactResult::ok);
	assert(unlink(path.c_str()) == 0);

	WriteFile(path, "hello");
	const std::string hardlink = path + ".hardlink-sentinel";
	assert(link(path.c_str(), hardlink.c_str()) == 0);
	NativeCoreArtifactHandle linked;
	assert(adapter.Resolve(HelloAuthority(), filesystem.NowMs() + 1000,
		&linked) == NativeArtifactResult::insecure);
	assert(linked.Close() == NativeArtifactResult::ok);
	assert(unlink(hardlink.c_str()) == 0);
	assert(unlink(path.c_str()) == 0);

	WriteFile(path, "hell");
	NativeCoreArtifactHandle short_file;
	assert(adapter.Resolve(HelloAuthority(), filesystem.NowMs() + 1000,
		&short_file) == NativeArtifactResult::changed);
	assert(short_file.Close() == NativeArtifactResult::ok);
	assert(unlink(path.c_str()) == 0);

	WriteFile(path, "world");
	NativeCoreArtifactHandle wrong_digest;
	assert(adapter.Resolve(HelloAuthority(), filesystem.NowMs() + 1000,
		&wrong_digest) == NativeArtifactResult::digest_mismatch);
	assert(!wrong_digest.valid());
	assert(wrong_digest.owns_descriptors());
	const uint64_t start = filesystem.NowMs();
	TestClock clock(start);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> program;
	assert(broker.Begin(generation, OperationKind::program_fpga, start + 1000,
		&program) == MISTER_RESULT_OK);
	CollectingSink sink;
	NativeFpgaProgrammer programmer(broker, clock, sink);
	const NativeFpgaProgrammingReceipt denied =
		programmer.Program(*program, wrong_digest);
	assert(denied.result == MISTER_RESULT_INVALID_ARGUMENT);
	assert(denied.accepted_bytes == 0);
	assert(denied.mutation_sequence == 0);
	assert(broker.mutation_sequence_for_test() == 0);
	assert(sink.bytes_.empty());
	NativeContentAdapter content(root.c_str(), filesystem);
	assert(content.Configure(HelloAuthority()) == MISTER_RESULT_OK);
	const NativeAcquisitionOutcome content_result =
		content.RetainContent(filesystem.NowMs() + 1000);
	assert(content_result.result == MISTER_RESULT_PLATFORM);
	assert(content_result.acquired);
	assert(!content.active());
	char denied_byte = 0;
	assert(content.ReadAt(0, &denied_byte, 1, filesystem.NowMs() + 1000) ==
		MISTER_RESULT_INVALID_STATE);
	assert(content.CloseContent(filesystem.NowMs() + 1000) == MISTER_RESULT_OK);
	assert(wrong_digest.Close() == NativeArtifactResult::ok);
	RemoveArtifactTree(root, "snes", name);

	const std::string backing = TemporaryRoot();
	const std::string root_link = backing + ".root-symlink-sentinel";
	assert(symlink(backing.c_str(), root_link.c_str()) == 0);
	NativeCoreArtifactAdapter root_adapter(root_link.c_str(), filesystem);
	NativeCoreArtifactHandle insecure_root;
	assert(root_adapter.Resolve(HelloAuthority(), filesystem.NowMs() + 1000,
		&insecure_root) == NativeArtifactResult::insecure);
	assert(unlink(root_link.c_str()) == 0);
	const std::string trailing_root_link = root_link + "/";
	assert(symlink(backing.c_str(), root_link.c_str()) == 0);
	NativeCoreArtifactAdapter trailing_root_adapter(trailing_root_link.c_str(),
		filesystem);
	NativeCoreArtifactHandle trailing_insecure_root;
	assert(trailing_root_adapter.Resolve(HelloAuthority(),
		filesystem.NowMs() + 1000, &trailing_insecure_root) ==
		NativeArtifactResult::insecure);
	assert(unlink(root_link.c_str()) == 0);

	const std::string intermediate_parent = TemporaryRoot();
	const std::string real_parent = Join(intermediate_parent, "real");
	assert(mkdir(real_parent.c_str(), 0700) == 0);
	const std::string intermediate_link = Join(intermediate_parent,
		"intermediate-symlink-sentinel");
	assert(symlink(real_parent.c_str(), intermediate_link.c_str()) == 0);
	const std::string nested_root = Join(intermediate_link, "cache");
	assert(mkdir(Join(real_parent, "cache").c_str(), 0700) == 0);
	NativeCoreArtifactAdapter intermediate_adapter(nested_root.c_str(),
		filesystem);
	NativeCoreArtifactHandle intermediate_insecure;
	assert(intermediate_adapter.Resolve(HelloAuthority(),
		filesystem.NowMs() + 1000, &intermediate_insecure) ==
		NativeArtifactResult::insecure);
	assert(unlink(intermediate_link.c_str()) == 0);
	assert(rmdir(Join(real_parent, "cache").c_str()) == 0);
	assert(rmdir(real_parent.c_str()) == 0);
	assert(rmdir(intermediate_parent.c_str()) == 0);
	assert(rmdir(backing.c_str()) == 0);
}

void TestPrivateIdentitySentinelsAreNotEmitted()
{
	const std::string root = TemporaryRoot();
	NativePosixFileSystem filesystem;
	NativeCoreArtifactAdapter adapter(root.c_str(), filesystem);
	NativeArtifactAuthority core = HelloAuthority(
		"core_identity_sentinel_unique", 29);
	NativeCoreArtifactHandle core_handle;
	assert(adapter.Resolve(core, filesystem.NowMs() + 1000, &core_handle) ==
		NativeArtifactResult::not_found);
	assert(core_handle.Close() == NativeArtifactResult::ok);
	NativeContentAdapter content(root.c_str(), filesystem);
	NativeArtifactAuthority content_authority = HelloAuthority();
	content_authority.extension = "content_identity_sentinel_unique";
	content_authority.extension_length = 32;
	assert(content.Configure(content_authority) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(rmdir(root.c_str()) == 0);
}

std::string ReadPipe(int descriptor)
{
	std::string output;
	char bytes[256];
	for (;;) {
		const ssize_t count = read(descriptor, bytes, sizeof(bytes));
		assert(count >= 0);
		if (count == 0) return output;
		output.append(bytes, static_cast<size_t>(count));
	}
}

void RunAllTests()
{
	TestSha256KnownAnswersAndExactLength();
	TestSecureArtifactResolutionAndComponents();
	TestContentRetainsExactDescriptorAndRechecksEntry();
	TestRetainedContentSystemMustMatchTheAdmittedSaveProfile();
	TestRetainedCoreCloseIsOrderedAndIdempotent();
	TestRetainedCloseNeverTargetsAReusedForeignDescriptor(false);
	TestRetainedCloseNeverTargetsAReusedForeignDescriptor(true);
	TestRetainedCoreCloseHonorsEqualityAndLateDeadline();
	TestRetainedCoreCloseHonorsEveryDeadlineBoundary();
	TestRetainedCoreCloseFailureIsStickyAndNeverRetried();
	TestRetainedCoreCloseFailurePrecedesDeadlineAndIsReentrant();
	TestContentCloseRetainsOnlyDeadlineSkippedDescriptors();
	TestContentReadAtFailureAndDeadlineBoundaries();
	TestRejectedContentConfigurationIsAtomic();
	TestProgrammingRequiresExactActiveProgramLeaseAndDeadline();
	TestProgrammingAuthorityMatrix();
	TestResolutionFailureInjectionAndEntrySubstitution();
	TestProgrammingFailureBoundariesRetainAuthority();
	TestProgrammingRejectsMalformedSessionAndIncompleteFinishEvidence();
	TestProgrammingReceiptSurvivesLateDeadlineAndBrokerDestruction();
	TestInsecureEntrySizeAndDigestFailures();
	TestPrivateIdentitySentinelsAreNotEmitted();
	TestTwoHundredArtifactProgramCycles();
}

void TestAllAdapterPathsArePrivacySilent()
{
	int output_pipe[2] = {-1, -1};
	int error_pipe[2] = {-1, -1};
	assert(pipe(output_pipe) == 0);
	assert(pipe(error_pipe) == 0);
	fflush(stdout);
	fflush(stderr);
	const int saved_output = dup(STDOUT_FILENO);
	const int saved_error = dup(STDERR_FILENO);
	assert(saved_output >= 0 && saved_error >= 0);
	assert(dup2(output_pipe[1], STDOUT_FILENO) == STDOUT_FILENO);
	assert(dup2(error_pipe[1], STDERR_FILENO) == STDERR_FILENO);
	assert(close(output_pipe[1]) == 0);
	assert(close(error_pipe[1]) == 0);
	RunAllTests();
	fflush(stdout);
	fflush(stderr);
	assert(dup2(saved_output, STDOUT_FILENO) == STDOUT_FILENO);
	assert(dup2(saved_error, STDERR_FILENO) == STDERR_FILENO);
	assert(close(saved_output) == 0);
	assert(close(saved_error) == 0);
	const std::string output = ReadPipe(output_pipe[0]);
	const std::string error = ReadPipe(error_pipe[0]);
	assert(close(output_pipe[0]) == 0);
	assert(close(error_pipe[0]) == 0);
	assert(output.empty());
	assert(error.empty());
	const char *const sentinels[] = {
		"fogcast-private-root-sentinel",
		"hardlink-sentinel",
		"root-symlink-sentinel",
		"intermediate-symlink-sentinel",
		"core_identity_sentinel_unique",
		"content_identity_sentinel_unique",
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
		"snes", "hello", "world"
	};
	for (size_t index = 0; index < sizeof(sentinels) / sizeof(sentinels[0]);
		++index) {
		assert(output.find(sentinels[index]) == std::string::npos);
		assert(error.find(sentinels[index]) == std::string::npos);
	}
}

} // namespace

int main()
{
	TestAllAdapterPathsArePrivacySilent();
	return 0;
}
