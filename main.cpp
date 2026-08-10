/*
Copyright 2005, 2006, 2007 Dennis van Weeren
Copyright 2008, 2009 Jakub Bednarski
Copyright 2012 Till Harbaum

This file is part of Minimig

Minimig is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

Minimig is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "runtime/mister_runtime_internal.hpp"

#include <stdio.h>

const char *version = "$VER:" VDATE;

static int mister_runtime_fail(MisterRuntime *runtime)
{
	MisterStatus status = MisterRuntime_Status(runtime);
	fprintf(stderr,
		"MiSTer runtime failed: state=%u primary_error=%u cleanup_error=%u\n",
		status.state, status.primary_error, status.cleanup_error);
	MisterRuntime_Stop(runtime);
	MisterStatus stopped_status = MisterRuntime_Status(runtime);
	if (stopped_status.state == MISTER_RUNTIME_EXIT_REQUIRED) return 1;
	return 1;
}

int main(int argc, char *argv[])
{
	MisterLaunch launch = {};
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION;
	launch.struct_size = sizeof(launch);
	launch.core_path = argc > 1 ? argv[1] : "";
	launch.xml_path = argc > 2 ? argv[2] : nullptr;

	MisterRuntime *runtime = MisterRuntime_Create(MisterRuntime_LegacyPlatform());
	if (!runtime) {
		fputs("MiSTer runtime create failed\n", stderr);
		return 1;
	}
	if (!MisterRuntime_Start(runtime)) {
		return mister_runtime_fail(runtime);
	}
	if (argc > 1 && argv[1][0] == '\0') {
		printf("Core path: %s\n", argv[1]);
	}
	if (!MisterRuntime_Load(runtime, &launch)) {
		return mister_runtime_fail(runtime);
	}

	while (MisterRuntime_Status(runtime).state == MISTER_RUNTIME_RUNNING) {
		MisterRuntime_Tick(runtime);
	}
	return mister_runtime_fail(runtime);
}
