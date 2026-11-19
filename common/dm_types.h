/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_types.h - the fixed-size types of the code every driver shares
 *                 (common/): the NT and WDM driver, the VxDs and the
 *                 tests on the build machine.  Freestanding C99: no C
 *                 library but what the kernels export (memcpy, memset).
 *
 */
#ifndef DM_TYPES_H
#define DM_TYPES_H

#include <stddef.h>

typedef unsigned char dm_u8;
typedef unsigned short dm_u16;
typedef unsigned int dm_u32;
typedef signed int dm_s32;
typedef unsigned long long dm_u64;

#define DM_STATIC_ASSERT(e, name) \
	typedef char dm_static_assert_##name[(e) ? 1 : -1]

DM_STATIC_ASSERT(sizeof(dm_u16) == 2, u16);
DM_STATIC_ASSERT(sizeof(dm_u32) == 4, u32);
DM_STATIC_ASSERT(sizeof(dm_u64) == 8, u64);

/* memcpy and memset: the kernels' own (ntoskrnl exports them, and
   clang-retro's libvxd.a has them for the VxDs) */
#include <string.h>

#endif
