/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FOGCAST_TEST_UNISTD_H
#define FOGCAST_TEST_UNISTD_H

#include <sys/types.h>

typedef long long __off64_t;

#ifdef __cplusplus
extern "C" {
#endif

pid_t fork(void);
void _Exit(int status) __attribute__((__noreturn__));

#ifdef __cplusplus
}
#endif

#endif
