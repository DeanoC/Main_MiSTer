// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/mister_runtime_linux_v2.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
	using namespace mister::linux_v2;
	if (argc != 7) {
		puts("invalid_arguments");
		return 2;
	}
	char* end = nullptr;
	errno = 0;
	const unsigned long long size = strtoull(argv[5], &end, 10);
	if (errno == ERANGE || !end || *end || size == 0 || argv[5][0] == '0') {
		puts("invalid_arguments");
		return 2;
	}
	MisterLaunchV2 launch = {};
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.struct_size = sizeof(launch);
	launch.game_id = {"probe", 5};
	launch.system = {argv[2], static_cast<uint32_t>(strlen(argv[2]))};
	launch.expected_core = {argv[3], static_cast<uint32_t>(strlen(argv[3]))};
	launch.content.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.content.struct_size = sizeof(launch.content);
	launch.content.sha256 = {argv[4], static_cast<uint32_t>(strlen(argv[4]))};
	launch.content.size = size;
	launch.content.extension = {argv[6], static_cast<uint32_t>(strlen(argv[6]))};
	StorageAdapter storage(argv[1]);
	ContentHandle content;
	StorageResult result = storage.status();
	if (result == StorageResult::ok) result = storage.Resolve(launch, &content);
	puts(StorageResultName(result));
	return result == StorageResult::ok ? 0 : 1;
}
