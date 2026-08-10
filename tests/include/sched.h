/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Darwin-only host-test compatibility shim. Production builds use the system
 * Linux sched.h through their normal include paths.
 */

#ifndef FOGCAST_TEST_SCHED_H
#define FOGCAST_TEST_SCHED_H

#if defined(__APPLE__)
#include <stddef.h>
#include <sys/types.h>

typedef struct {
	unsigned long bits[4];
} cpu_set_t;

#define CPU_ZERO(set) do { *(set) = {}; } while (0)
#define CPU_SET(cpu, set) ((set)->bits[(cpu) / (8 * sizeof(unsigned long))] |= \
	(1ul << ((cpu) % (8 * sizeof(unsigned long)))))

static inline int sched_setaffinity(pid_t, size_t, const cpu_set_t *)
{
	return 0;
}
#else
#include_next <sched.h>
#endif

#endif
