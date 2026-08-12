// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/mister_runtime_linux_v2.hpp"

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace linux_v2 = mister::linux_v2;

class MutateAfterHash : public linux_v2::StorageValidationObserver {
public:
	explicit MutateAfterHash(const std::string& path) : path_(path), called_(false) {}
	void AfterHash() override {
		const int descriptor = open(path_.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
		assert(descriptor >= 0);
		assert(write(descriptor, "!", 1) == 1);
		assert(close(descriptor) == 0);
		called_ = true;
	}
	bool called() const { return called_; }
private:
	std::string path_;
	bool called_;
};

class ReplaceParentAfterHash : public linux_v2::StorageValidationObserver {
public:
	ReplaceParentAfterHash(const std::string& root, bool replace_root)
		: root_(root), replace_root_(replace_root), replaced_(false) {}
	void AfterHash() override {
		assert(!replaced_);
		if (replace_root_) {
			replaced_path_ = root_ + ".posthash";
			assert(rename(root_.c_str(), replaced_path_.c_str()) == 0);
			assert(mkdir(root_.c_str(), 0700) == 0);
			assert(mkdir((root_ + "/megadrive").c_str(), 0700) == 0);
			assert(mkdir((root_ + "/snes").c_str(), 0700) == 0);
		} else {
			const std::string system = root_ + "/snes";
			replaced_path_ = system + ".posthash";
			assert(rename(system.c_str(), replaced_path_.c_str()) == 0);
			assert(mkdir(system.c_str(), 0700) == 0);
		}
		replaced_ = true;
	}
	void Restore() {
		assert(replaced_);
		if (replace_root_) {
			assert(rmdir((root_ + "/megadrive").c_str()) == 0);
			assert(rmdir((root_ + "/snes").c_str()) == 0);
			assert(rmdir(root_.c_str()) == 0);
			assert(rename(replaced_path_.c_str(), root_.c_str()) == 0);
		} else {
			const std::string system = root_ + "/snes";
			assert(rmdir(system.c_str()) == 0);
			assert(rename(replaced_path_.c_str(), system.c_str()) == 0);
		}
		replaced_ = false;
	}
private:
	std::string root_;
	std::string replaced_path_;
	bool replace_root_;
	bool replaced_;
};

static std::string Join(const std::string& left, const char* right) {
	return left + "/" + right;
}

static void MakeDirectory(const std::string& path) {
	assert(mkdir(path.c_str(), 0700) == 0);
}

static void WriteFile(const std::string& path, const char* bytes) {
	const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	assert(descriptor >= 0);
	const size_t size = strlen(bytes);
	assert(write(descriptor, bytes, size) == static_cast<ssize_t>(size));
	assert(close(descriptor) == 0);
}

static MisterLaunchV2 Launch(const char* system, const char* core, const char* digest,
	uint64_t size, const char* extension) {
	MisterLaunchV2 launch = {};
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.struct_size = sizeof(launch);
	launch.game_id = {"game", 4};
	launch.system = {system, static_cast<uint32_t>(strlen(system))};
	launch.expected_core = {core, static_cast<uint32_t>(strlen(core))};
	launch.content.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.content.struct_size = sizeof(launch.content);
	launch.content.sha256 = {digest, static_cast<uint32_t>(strlen(digest))};
	launch.content.size = size;
	launch.content.extension = {extension, static_cast<uint32_t>(strlen(extension))};
	return launch;
}

static void RemoveTree(const std::string& root) {
	const std::string mega = Join(root, "megadrive");
	const std::string snes = Join(root, "snes");
	const std::string systems[] = {mega, snes};
	for (size_t i = 0; i < 2; ++i) {
		DIR* directory = opendir(systems[i].c_str());
		if (directory == nullptr) continue;
		for (;;) {
			errno = 0;
			struct dirent* entry = readdir(directory);
			if (entry == nullptr) { assert(errno == 0); break; }
			if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
				assert(unlink(Join(systems[i], entry->d_name).c_str()) == 0);
		}
		assert(closedir(directory) == 0);
	}
	(void)rmdir(mega.c_str());
	(void)rmdir(snes.c_str());
	(void)rmdir(root.c_str());
}

static std::string NewCacheRoot() {
	char root_template[] = "/tmp/fogcast-privacy-sentinel.XXXXXX";
	char* created = mkdtemp(root_template);
	assert(created != nullptr);
	const std::string root(created);
	MakeDirectory(Join(root, "megadrive"));
	MakeDirectory(Join(root, "snes"));
	return root;
}

struct ProbeResult {
	int exit_code;
	std::string output;
	std::string error;
};

static std::string ReadAll(int descriptor) {
	std::string result;
	char bytes[256];
	for (;;) {
		const ssize_t count = read(descriptor, bytes, sizeof(bytes));
		if (count < 0 && errno == EINTR) continue;
		assert(count >= 0);
		if (count == 0) break;
		result.append(bytes, static_cast<size_t>(count));
	}
	return result;
}

static ProbeResult RunProbe(const char* probe, const std::vector<std::string>& arguments) {
	int output_pipe[2];
	int error_pipe[2];
	assert(pipe(output_pipe) == 0);
	assert(pipe(error_pipe) == 0);
	const pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		assert(dup2(output_pipe[1], STDOUT_FILENO) == STDOUT_FILENO);
		assert(dup2(error_pipe[1], STDERR_FILENO) == STDERR_FILENO);
		close(output_pipe[0]);
		close(output_pipe[1]);
		close(error_pipe[0]);
		close(error_pipe[1]);
		std::vector<char*> argv;
		argv.push_back(const_cast<char*>(probe));
		for (size_t i = 0; i < arguments.size(); ++i)
			argv.push_back(const_cast<char*>(arguments[i].c_str()));
		argv.push_back(nullptr);
		execv(probe, argv.data());
		_exit(127);
	}
	close(output_pipe[1]);
	close(error_pipe[1]);
	ProbeResult result = {};
	result.output = ReadAll(output_pipe[0]);
	result.error = ReadAll(error_pipe[0]);
	close(output_pipe[0]);
	close(error_pipe[0]);
	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	result.exit_code = WEXITSTATUS(status);
	return result;
}

static void AssertProbe(const char* probe, const std::vector<std::string>& arguments,
	const char* expected, int exit_code) {
	const ProbeResult result = RunProbe(probe, arguments);
	if (result.exit_code != exit_code || result.output != std::string(expected) + "\n" ||
		!result.error.empty())
		fprintf(stderr, "probe expectation %s failed: exit=%d\n", expected, result.exit_code);
	assert(result.exit_code == exit_code);
	assert(result.output == std::string(expected) + "\n");
	assert(result.error.empty());
	for (size_t i = 0; i < arguments.size(); ++i) {
		assert(result.output.find(arguments[i]) == std::string::npos);
		assert(result.error.find(arguments[i]) == std::string::npos);
	}
}

static void AssertCoreResolverSilent(const linux_v2::CoreArtifactAuthority& authority,
	linux_v2::CoreArtifactResult expected, bool provide_handle = true,
	bool emit_buffered_sentinel = false) {
	const char sentinel[] = "BUFFERED_CORE_PRIVACY_SENTINEL_UNIQUE";
	int output_pipe[2];
	int error_pipe[2];
	assert(pipe(output_pipe) == 0);
	assert(pipe(error_pipe) == 0);
	const pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		assert(dup2(output_pipe[1], STDOUT_FILENO) == STDOUT_FILENO);
		assert(dup2(error_pipe[1], STDERR_FILENO) == STDERR_FILENO);
		close(output_pipe[0]);
		close(output_pipe[1]);
		close(error_pipe[0]);
		close(error_pipe[1]);
		linux_v2::UnavailableCoreArtifactResolver resolver;
		linux_v2::CoreArtifactHandle artifact;
		if (emit_buffered_sentinel) assert(fputs(sentinel, stdout) >= 0);
		linux_v2::CoreArtifactHandle* handle = provide_handle ? &artifact : nullptr;
		const int child_result = resolver.Resolve(authority, handle) == expected &&
			!artifact.valid() ? 0 : 1;
		assert(fflush(nullptr) == 0);
		_exit(child_result);
	}
	close(output_pipe[1]);
	close(error_pipe[1]);
	const std::string output = ReadAll(output_pipe[0]);
	const std::string error = ReadAll(error_pipe[0]);
	close(output_pipe[0]);
	close(error_pipe[0]);
	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
	if (emit_buffered_sentinel)
		assert(output == sentinel);
	else
		assert(output.empty());
	assert(error.empty());
	const std::string digest(authority.sha256.data, authority.sha256.length);
	assert(output.find(digest) == std::string::npos);
	assert(error.find(digest) == std::string::npos);
}

int main(int argc, char** argv) {
	assert(argc == 2);
	const char* probe = argv[1];
	assert(strcmp(linux_v2::ProductionCacheRoot(), "/media/fat/fogcast/cache") == 0);
	char root_template[] = "/tmp/fogcast-linux-v2.XXXXXX";
	char* created = mkdtemp(root_template);
	assert(created != nullptr);
	const std::string root(created);
	MakeDirectory(Join(root, "megadrive"));
	MakeDirectory(Join(root, "snes"));

	const char hello_digest[] = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
	const std::string hello_path = Join(Join(root, "megadrive"),
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.md");
	WriteFile(hello_path, "hello");

	linux_v2::StorageAdapter storage(root.c_str());
	assert(storage.status() == linux_v2::StorageResult::ok);
	linux_v2::ContentHandle content;
	MisterLaunchV2 launch = Launch("megadrive", "MegaDrive", hello_digest, 5, "md");
	assert(storage.Resolve(launch, &content) == linux_v2::StorageResult::ok);
	assert(content.valid());
	assert(content.size() == 5);
	assert(unlink(hello_path.c_str()) == 0);
	char retained[5] = {};
	assert(content.ReadAt(0, retained, sizeof(retained)));
	assert(memcmp(retained, "hello", sizeof(retained)) == 0);
	WriteFile(hello_path, "world");
	assert(storage.Resolve(launch, &content) == linux_v2::StorageResult::invalid_argument);
	content.Close();
	assert(!content.valid());
	assert(storage.Resolve(launch, &content) == linux_v2::StorageResult::digest_mismatch);

	MisterLaunchV2 wrong_core = Launch("megadrive", "SNES", hello_digest, 5, "md");
	assert(storage.Resolve(wrong_core, &content) == linux_v2::StorageResult::unsupported_identity);
	MisterLaunchV2 wrong_extension = Launch("megadrive", "MegaDrive", hello_digest, 5, "sfc");
	assert(storage.Resolve(wrong_extension, &content) == linux_v2::StorageResult::unsupported_identity);
	MisterLaunchV2 path_extension = Launch("megadrive", "MegaDrive", hello_digest, 5, "../md");
	assert(storage.Resolve(path_extension, &content) == linux_v2::StorageResult::invalid_identity);
	MisterLaunchV2 wrong_size = Launch("megadrive", "MegaDrive", hello_digest, 4, "md");
	assert(storage.Resolve(wrong_size, &content) == linux_v2::StorageResult::changed);
	MisterLaunchV2 wrong_digest = Launch("megadrive", "MegaDrive",
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 5, "md");
	const std::string wrong_digest_path = Join(Join(root, "megadrive"),
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.md");
	WriteFile(wrong_digest_path, "hello");
	assert(storage.Resolve(wrong_digest, &content) == linux_v2::StorageResult::digest_mismatch);

	const std::string linked = Join(Join(root, "megadrive"), "outside.md");
	assert(link(hello_path.c_str(), linked.c_str()) == 0);
	assert(storage.Resolve(launch, &content) == linux_v2::StorageResult::insecure_entry);
	assert(unlink(linked.c_str()) == 0);

	assert(unlink(hello_path.c_str()) == 0);
	assert(symlink("/dev/null", hello_path.c_str()) == 0);
	assert(storage.Resolve(launch, &content) == linux_v2::StorageResult::insecure_entry);
	assert(unlink(hello_path.c_str()) == 0);
	assert(mkfifo(hello_path.c_str(), 0600) == 0);
	assert(storage.Resolve(launch, &content) == linux_v2::StorageResult::insecure_entry);
	assert(unlink(hello_path.c_str()) == 0);

	const char world_digest[] = "486ea46224d1bb4fb680f34f7c9ad96a8f24ec88be73ea8e5a6c65260e9cb8a7";
	const std::string world_path = Join(Join(root, "snes"),
		"486ea46224d1bb4fb680f34f7c9ad96a8f24ec88be73ea8e5a6c65260e9cb8a7.sfc");
	WriteFile(world_path, "world");
	MisterLaunchV2 world = Launch("snes", "SNES", world_digest, 5, "sfc");
	assert(storage.Resolve(world, &content) == linux_v2::StorageResult::ok);
	assert(content.valid());
	content.Close();
	assert(chmod(root.c_str(), 0777) == 0);
	assert(chmod(Join(root, "megadrive").c_str(), 0777) == 0);
	assert(chmod(Join(root, "snes").c_str(), 0777) == 0);
	assert(chmod(world_path.c_str(), 0666) == 0);
	linux_v2::StorageAdapter permissive_storage(root.c_str());
	assert(permissive_storage.status() == linux_v2::StorageResult::ok);
	assert(permissive_storage.Resolve(world, &content) == linux_v2::StorageResult::ok);
	content.Close();
	assert(chmod(root.c_str(), 0700) == 0);
	assert(chmod(Join(root, "megadrive").c_str(), 0700) == 0);
	assert(chmod(Join(root, "snes").c_str(), 0700) == 0);
	assert(chmod(world_path.c_str(), 0600) == 0);

	ReplaceParentAfterHash replace_system(root, false);
	linux_v2::StorageAdapter system_replacement_storage(root.c_str(), &replace_system);
	assert(system_replacement_storage.Resolve(world, &content) ==
		linux_v2::StorageResult::changed);
	assert(!content.valid());
	replace_system.Restore();
	ReplaceParentAfterHash replace_root_after_hash(root, true);
	linux_v2::StorageAdapter root_replacement_storage(root.c_str(), &replace_root_after_hash);
	assert(root_replacement_storage.Resolve(world, &content) ==
		linux_v2::StorageResult::changed);
	assert(!content.valid());
	replace_root_after_hash.Restore();

	linux_v2::LinuxV2Context context(root.c_str());
	assert(context.storage_status() == linux_v2::StorageResult::ok);
	assert(context.acquired_mask() == 0);
	assert(context.neutral_mask() == 0);
	assert(context.PrepareContent(world) == linux_v2::StorageResult::ok);
	assert(context.content_active());
	assert(context.acquired_mask() == MISTER_RESOURCE_CONTENT);
	char context_bytes[5] = {};
	assert(context.ReadContentAt(0, context_bytes, sizeof(context_bytes)));
	assert(memcmp(context_bytes, "world", sizeof(context_bytes)) == 0);
	assert(context.PrepareContent(world) == linux_v2::StorageResult::invalid_argument);

	linux_v2::CoreArtifactAuthority core_authority = {};
	core_authority.system = {"snes", 4};
	core_authority.expected_core = {"SNES", 4};
	core_authority.sha256 = {
		"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 64};
	core_authority.size = 1024;
	linux_v2::UnavailableCoreArtifactResolver core_resolver;
	linux_v2::CoreArtifactHandle core_artifact;
	assert(core_resolver.Resolve(core_authority, &core_artifact) ==
		linux_v2::CoreArtifactResult::unavailable);
	AssertCoreResolverSilent(core_authority, linux_v2::CoreArtifactResult::unavailable);
	linux_v2::CoreArtifactAuthority invalid_argument_authority = core_authority;
	invalid_argument_authority.sha256 = {
		"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", 64};
	AssertCoreResolverSilent(invalid_argument_authority,
		linux_v2::CoreArtifactResult::invalid_argument, false);
	linux_v2::CoreArtifactAuthority capture_self_check = core_authority;
	capture_self_check.sha256 = {
		"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", 64};
	AssertCoreResolverSilent(capture_self_check, linux_v2::CoreArtifactResult::unavailable,
		true, true);
	assert(!core_artifact.valid());
	linux_v2::CoreArtifactAuthority invalid_identity_authority = core_authority;
	invalid_identity_authority.sha256 = {
		"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", 64};
	invalid_identity_authority.expected_core = {"MegaDrive", 9};
	assert(core_resolver.Resolve(invalid_identity_authority, &core_artifact) ==
		linux_v2::CoreArtifactResult::invalid_identity);
	AssertCoreResolverSilent(invalid_identity_authority,
		linux_v2::CoreArtifactResult::invalid_identity);
	assert(strcmp(linux_v2::CoreArtifactResultName(linux_v2::CoreArtifactResult::unavailable),
		"unavailable") == 0);

	const std::string privacy_ok = NewCacheRoot();
	WriteFile(Join(Join(privacy_ok, "snes"),
		"486ea46224d1bb4fb680f34f7c9ad96a8f24ec88be73ea8e5a6c65260e9cb8a7.sfc"), "world");
	assert(chmod(privacy_ok.c_str(), 0777) == 0);
	assert(chmod(Join(privacy_ok, "snes").c_str(), 0777) == 0);
	assert(chmod(Join(Join(privacy_ok, "snes"),
		"486ea46224d1bb4fb680f34f7c9ad96a8f24ec88be73ea8e5a6c65260e9cb8a7.sfc").c_str(),
		0666) == 0);
	AssertProbe(probe, {privacy_ok, "snes", "SNES", world_digest, "5", "sfc"}, "ok", 0);
	RemoveTree(privacy_ok);

	const std::string privacy_invalid_identity = NewCacheRoot();
	AssertProbe(probe, {privacy_invalid_identity, "snes", "SNES",
		"BAD_DIGEST_SENTINEL_UNIQUE", "5", "sfc"},
		"invalid_identity", 1);
	RemoveTree(privacy_invalid_identity);

	const std::string privacy_unsupported = NewCacheRoot();
	const std::string unsupported_digest(64, '1');
	AssertProbe(probe, {privacy_unsupported, "snes", "MegaDrive", unsupported_digest,
		"5", "sfc"},
		"unsupported_identity", 1);
	RemoveTree(privacy_unsupported);

	const std::string privacy_missing = NewCacheRoot();
	const std::string missing_digest(64, '2');
	AssertProbe(probe, {privacy_missing, "snes", "SNES", missing_digest, "5", "sfc"},
		"not_found", 1);
	RemoveTree(privacy_missing);

	const std::string privacy_changed = NewCacheRoot();
	const std::string changed_digest(64, '3');
	WriteFile(Join(Join(privacy_changed, "snes"), (changed_digest + ".sfc").c_str()), "world");
	AssertProbe(probe, {privacy_changed, "snes", "SNES", changed_digest, "4", "sfc"},
		"changed", 1);
	RemoveTree(privacy_changed);

	const std::string privacy_digest = NewCacheRoot();
	const std::string mismatch_digest(64, '4');
	WriteFile(Join(Join(privacy_digest, "megadrive"), (mismatch_digest + ".md").c_str()), "hello");
	AssertProbe(probe, {privacy_digest, "megadrive", "MegaDrive", mismatch_digest, "5", "md"},
		"digest_mismatch", 1);
	RemoveTree(privacy_digest);

	WriteFile(hello_path, "hello");
	MutateAfterHash mutate(hello_path);
	linux_v2::StorageAdapter mutation_storage(root.c_str(), &mutate);
	linux_v2::ContentHandle mutation_content;
	assert(mutation_storage.Resolve(launch, &mutation_content) == linux_v2::StorageResult::changed);
	assert(mutate.called());
	assert(!mutation_content.valid());
	assert(unlink(hello_path.c_str()) == 0);
	const std::string privacy_entry = NewCacheRoot();
	const std::string entry_digest(64, '5');
	const std::string entry_path = Join(Join(privacy_entry, "megadrive"),
		(entry_digest + ".md").c_str());
	assert(symlink("/dev/null", entry_path.c_str()) == 0);
	AssertProbe(probe, {privacy_entry, "megadrive", "MegaDrive", entry_digest, "5", "md"},
		"insecure_entry", 1);
	RemoveTree(privacy_entry);

	const std::string privacy_root_backing = NewCacheRoot();
	const std::string root_symlink = privacy_root_backing + ".root-link-sentinel";
	const std::string root_digest(64, '6');
	assert(symlink(privacy_root_backing.c_str(), root_symlink.c_str()) == 0);
	AssertProbe(probe, {root_symlink, "snes", "SNES", root_digest, "5", "sfc"},
		"insecure_root", 1);
	assert(unlink(root_symlink.c_str()) == 0);
	RemoveTree(privacy_root_backing);

	const std::string invalid_argument_digest(64, '7');
	AssertProbe(probe, {"relative-root-sentinel-unique", "snes", "SNES",
		invalid_argument_digest, "5", "sfc"},
		"invalid_argument", 1);
	const std::string privacy_overflow = NewCacheRoot();
	const std::string overflow_digest(64, '8');
	AssertProbe(probe, {privacy_overflow, "snes", "SNES", overflow_digest,
		"184467440737095516160000", "sfc"}, "invalid_arguments", 2);
	RemoveTree(privacy_overflow);

	char io_parent_template[] = "/tmp/fogcast-io-sentinel.XXXXXX";
	char* io_parent_created = mkdtemp(io_parent_template);
	assert(io_parent_created != nullptr);
	const std::string io_parent(io_parent_created);
	const std::string io_root = Join(io_parent, "cache");
	MakeDirectory(io_root);
	MakeDirectory(Join(io_root, "megadrive"));
	MakeDirectory(Join(io_root, "snes"));
	assert(chmod(io_parent.c_str(), 0000) == 0);
	const std::string io_digest(64, '9');
	AssertProbe(probe, {io_root, "snes", "SNES", io_digest, "5", "sfc"}, "io", 1);
	assert(chmod(io_parent.c_str(), 0700) == 0);
	RemoveTree(io_root);
	assert(rmdir(io_parent.c_str()) == 0);

	const std::string replaced_root = root + ".replaced";
	assert(rename(root.c_str(), replaced_root.c_str()) == 0);
	MakeDirectory(root);
	MakeDirectory(Join(root, "megadrive"));
	MakeDirectory(Join(root, "snes"));
	assert(storage.Resolve(world, &content) == linux_v2::StorageResult::changed);
	RemoveTree(root);
	assert(rename(replaced_root.c_str(), root.c_str()) == 0);

	assert(strcmp(linux_v2::StorageResultName(linux_v2::StorageResult::ok), "ok") == 0);
	assert(strcmp(linux_v2::StorageResultName(linux_v2::StorageResult::changed), "changed") == 0);
	RemoveTree(root);
	return 0;
}
