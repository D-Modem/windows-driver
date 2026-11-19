/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    test.h - the host tests' few checks: CHECK(e), CHECK_EQ(a, b) (as
 *             unsigned long long), and TEST(name) functions run by
 *             RUN(name).
 *
 */
#ifndef DM_TEST_H
#define DM_TEST_H

#include <stdio.h>

static int t_fail, t_checks;

#define CHECK(e) do { t_checks++; if (!(e)) { t_fail++; \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long a_ = (a), b_ = (b); \
	t_checks++; if (a_ != b_) { t_fail++; \
	fprintf(stderr, "%s:%d: %s == %s: %llu != %llu (0x%llx, 0x%llx)\n", \
		__FILE__, __LINE__, #a, #b, a_, b_, a_, b_); } } while (0)
#define TEST(name) static void name(void)
#define RUN(name) do { int f_ = t_fail; name(); \
	printf("%s %s\n", t_fail == f_ ? "ok  " : "FAIL", #name); } while (0)
#define DONE() (printf("%d checks, %d failed\n", t_checks, t_fail), \
		t_fail ? 1 : 0)

#endif
