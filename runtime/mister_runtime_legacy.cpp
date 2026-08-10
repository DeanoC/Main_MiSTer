/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime_internal.hpp"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

#include "file_io.h"
#include "fpga_io.h"
#include "offload.h"
#include "scheduler.h"
#include "user_io.h"

extern const char *version;

static bool legacy_started;
static bool legacy_loaded;

static bool mister_runtime_legacy_start(void *)
{
	if (legacy_started) return false;

	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(1, &set);
	sched_setaffinity(0, sizeof(set), &set);

	offload_start();
	fpga_io_init();
	DISKLED_OFF;

	printf("\nMinimig by Dennis van Weeren");
	printf("\nARM Controller by Jakub Bednarski");
	printf("\nMiSTer code by Sorgelig\n\n");
	printf("Version %s\n\n", version + 5);

	legacy_started = true;
	return true;
}

static bool mister_runtime_legacy_load(void *, const MisterLaunch *launch)
{
	if (!legacy_started || legacy_loaded) return false;

	const char *core_path = launch->core_path ? launch->core_path : "";
	const char *xml_path = launch->xml_path;
	if (core_path[0]) printf("Core path: %s\n", core_path);
	if (xml_path) printf("XML path: %s\n", xml_path);

	if (!is_fpga_ready(1)) {
		printf("\nGPI[31]==1. FPGA is uninitialized or incompatible core loaded.\n");
		printf("Quitting. Bye bye...\n");
		exit(0);
	}

	FindStorage();
	user_io_init(core_path, xml_path);
	if (!scheduler_init()) return false;

	legacy_loaded = true;
	return true;
}

static bool mister_runtime_legacy_tick(void *)
{
	return legacy_loaded && scheduler_step();
}

static MisterPlatformStopResult mister_runtime_legacy_stop(void *)
{
	return MISTER_PLATFORM_EXIT_REQUIRED;
}

const MisterPlatform *MisterRuntime_LegacyPlatform(void)
{
	static const MisterPlatform platform = {
		MISTER_RUNTIME_ABI_VERSION,
		sizeof(MisterPlatform),
		MISTER_PLATFORM_CAP_PROCESS_CONTROL_ESCAPE,
		nullptr,
		mister_runtime_legacy_start,
		mister_runtime_legacy_load,
		mister_runtime_legacy_tick,
		mister_runtime_legacy_stop
	};
	return &platform;
}
