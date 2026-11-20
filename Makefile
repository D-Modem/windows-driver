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
# make engine     dmodem-svc.exe, the engine (D-Modem's libdmodem, built
#                 with clang-retro for 95/98/ME/NT 4.0 and later), its
#                 imports checked for 98 and 2000
# make engine-test  the engine's loopback line on this machine (libdmodem
#                 built for it with cmake)

CLANG_DOS ?= /opt/clang-dos
DMODEM ?= ../D-Modem
CC ?= cc
PYTHON ?= python3
OUT ?= build

COMMON = common/dm_port.c common/dm_wire.c common/dm_link.c common/dm_usb.c
HOST_CFLAGS = -std=c99 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
	-Icommon -Iinclude -fsanitize=address,undefined -fno-sanitize-recover=all

DRV_CFLAGS = --config=$(CLANG_DOS)/bin/nt4-ddk.cfg -march=i586 -O2 -g -gcodeview \
	-std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
	-Wno-missing-field-initializers -Icommon -Iinclude -DDM_KERNEL

.PHONY: all test driver check inf inf-check engine engine-test clean

all: test

$(OUT):
	mkdir -p $@

$(OUT)/port_test: tests/port_test.c $(COMMON) common/*.h tests/test.h | $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/port_test.c $(COMMON)
$(OUT)/link_test: tests/link_test.c $(COMMON) common/*.h tests/test.h | $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/link_test.c $(COMMON)

$(OUT)/usb_test: tests/usb_test.c $(COMMON) common/*.h include/*.h tests/test.h | $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/usb_test.c $(COMMON)

test: $(OUT)/port_test $(OUT)/link_test $(OUT)/usb_test
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/port_test
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/link_test
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/usb_test

$(OUT)/dmodem.sys: nt/dmodem.c nt/usb.c nt/*.h $(COMMON) common/*.h include/*.h | $(OUT)
	$(CLANG_DOS)/bin/clang $(DRV_CFLAGS) -o $@ nt/dmodem.c nt/usb.c $(COMMON) \
		-Wl,--pdb=$(OUT)/dmodem.pdb -Wl,--Xlink=-pdbaltpath:%_PDB%

driver: $(OUT)/dmodem.sys

check: $(OUT)/dmodem.sys
	$(PYTHON) $(CLANG_DOS)/share/clang-dos/check-imports.py --nt 2000 $<
	$(PYTHON) $(CLANG_DOS)/share/clang-dos/check-imports.py --wdm 98se $<
	$(PYTHON) $(CLANG_DOS)/share/clang-dos/check-imports.py --wdm me $<

DMODEM_INF = $(DMODEM)/platforms/windows/winmodem/dmodem.inf

inf:
	$(PYTHON) tools/make_inf.py $(DMODEM_INF) inf
inf-check:
	$(PYTHON) tools/make_inf.py $(DMODEM_INF) inf --check

# ---- the engine

CMAKE ?= cmake
# (Ninja where it is installed, else make: CI needs no package for it)
CMAKE_GENERATOR ?= $(if $(shell command -v ninja),Ninja,Unix Makefiles)
LIBDMODEM_OPTS = -G "$(CMAKE_GENERATOR)" -DCMAKE_BUILD_TYPE=Release -DDMODEM_DMODEMD=OFF \
	-DDMODEM_TESTS=OFF -DDMODEM_USB=OFF -DDMODEM_ASTERISK=OFF \
	-DDMODEM_FREESWITCH=OFF -DDMODEM_ARI=OFF
ENGINE_SRC = engine/dmodem_svc.cpp engine/loopback.cpp common/dm_wire.c
ENGINE_H = engine/*.h common/*.h include/*.h
ENGINE_FLAGS = --config=$(CLANG_DOS)/bin/win32-picolibc.cfg -D_GNU_SOURCE \
	-O2 -Wall -Wextra -Werror -Iengine -Icommon -Iinclude \
	-I$(DMODEM)/frontend/dmodemd

# libdmodem for Windows (clang-retro: D-Modem's toolchain file with the
# win32-picolibc config, for 95/98/ME/NT 4.0 and later; _GNU_SOURCE for
# picolibc's CLOCK_MONOTONIC) and for this machine
$(OUT)/w32/dmodemd/libdmodem.a: FORCE | $(OUT)
	CFLAGS=-D_GNU_SOURCE CXXFLAGS=-D_GNU_SOURCE \
	$(CMAKE) -S $(DMODEM) -B $(OUT)/w32 $(LIBDMODEM_OPTS) \
		-DCMAKE_TOOLCHAIN_FILE=$(abspath $(DMODEM))/platforms/windows/clang-retro.cmake \
		-DCLANG_DOS=$(CLANG_DOS) -DCLANG_RETRO_CONFIG=win32-picolibc \
		>/dev/null
	$(CMAKE) --build $(OUT)/w32 --target dmodem --parallel
$(OUT)/host/dmodemd/libdmodem.a: FORCE | $(OUT)
	$(CMAKE) -S $(DMODEM) -B $(OUT)/host $(LIBDMODEM_OPTS) >/dev/null
	$(CMAKE) --build $(OUT)/host --target dmodem --parallel
FORCE:

$(OUT)/dmodem-svc.exe: $(ENGINE_SRC) $(ENGINE_H) $(OUT)/w32/dmodemd/libdmodem.a
	$(CLANG_DOS)/bin/clang $(ENGINE_FLAGS) -std=gnu99 -c common/dm_wire.c \
		-o $(OUT)/engine_wire.o
	$(CLANG_DOS)/bin/clang++ $(ENGINE_FLAGS) -std=c++17 -o $@ \
		engine/dmodem_svc.cpp engine/loopback.cpp $(OUT)/engine_wire.o \
		$(OUT)/w32/dmodemd/libdmodem.a -lws2_32 -lwinmm \
		-Wl,--strip-debug

engine: $(OUT)/dmodem-svc.exe
	$(PYTHON) $(CLANG_DOS)/share/clang-dos/check-imports.py $<
	$(PYTHON) $(CLANG_DOS)/share/clang-dos/check-imports.py --nt 5.0 $<

$(OUT)/loopback_test: tests/loopback_test.cpp engine/loopback.cpp engine/*.h \
		$(OUT)/host/dmodemd/libdmodem.a
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Iengine -Itests \
		-I$(DMODEM)/frontend/dmodemd -o $@ tests/loopback_test.cpp \
		engine/loopback.cpp $(OUT)/host/dmodemd/libdmodem.a -lpthread -lm

engine-test: $(OUT)/loopback_test
	$(OUT)/loopback_test

clean:
	rm -rf $(OUT)
