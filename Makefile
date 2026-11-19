# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# make test       the shared code's tests, on this machine (cc)
# make driver     dmodem.sys for Windows 2000 and XP (and 98 SE/ME), with
#                 clang-retro (CLANG_DOS: its unpacked package)
# make check      the driver's imports against 2000's kernel and 98 SE's
#                 and ME's WDM
# make inf        inf/dmodem.inf from D-Modem's INF (DMODEM: a D-Modem
#                 checkout); make inf-check: is it up to date

CLANG_DOS ?= /opt/clang-dos
DMODEM ?= ../D-Modem
CC ?= cc
PYTHON ?= python3
OUT ?= build

COMMON = common/dm_port.c common/dm_wire.c common/dm_link.c
HOST_CFLAGS = -std=c99 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
	-Icommon -fsanitize=address,undefined -fno-sanitize-recover=all

DRV_CFLAGS = --config=$(CLANG_DOS)/bin/nt4-ddk.cfg -march=i586 -O2 -g -gcodeview \
	-std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
	-Wno-missing-field-initializers -Icommon -Iinclude -DDM_KERNEL

.PHONY: all test driver check inf inf-check clean

all: test

$(OUT):
	mkdir -p $@

$(OUT)/port_test: tests/port_test.c $(COMMON) common/*.h tests/test.h | $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/port_test.c $(COMMON)
$(OUT)/link_test: tests/link_test.c $(COMMON) common/*.h tests/test.h | $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/link_test.c $(COMMON)

test: $(OUT)/port_test $(OUT)/link_test
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/port_test
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/link_test

$(OUT)/dmodem.sys: nt/dmodem.c $(COMMON) common/*.h include/*.h | $(OUT)
	$(CLANG_DOS)/bin/clang $(DRV_CFLAGS) -o $@ nt/dmodem.c $(COMMON) \
		-Wl,--pdb=$(OUT)/dmodem.pdb -Wl,--Xlink=-pdbaltpath:%_PDB%

driver: $(OUT)/dmodem.sys

check: $(OUT)/dmodem.sys
	$(PYTHON) $(CLANG_DOS)/share/clang-dos/check-imports.py --nt 2000 $<
	$(PYTHON) $(CLANG_DOS)/share/clang-dos/check-imports.py --wdm 98se $<
	$(PYTHON) $(CLANG_DOS)/share/clang-dos/check-imports.py --wdm me $<

DMODEM_INF = $(DMODEM)/platforms/windows/winmodem/dmodem.inf

inf:
	$(PYTHON) tools/make_inf.py $(DMODEM_INF) inf/dmodem.inf
inf-check:
	$(PYTHON) tools/make_inf.py $(DMODEM_INF) inf/dmodem.inf --check

clean:
	rm -rf $(OUT)
