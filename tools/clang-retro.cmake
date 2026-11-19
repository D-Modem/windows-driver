# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# clang-retro.cmake - a CMake toolchain file for D-Modem's libdmodem on
# Windows 95, 98, ME, NT 4.0 and later: clang-retro's win32-picolibc
# config (picolibc for the C library, libc++, no C runtime DLL),
# i386, subsystem 4.0.  CLANG_DOS: the toolchain's folder (default
# /opt/clang-dos).  The Makefile's engine target uses it.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
if(NOT CLANG_DOS)
	if(DEFINED ENV{CLANG_DOS})
		set(CLANG_DOS $ENV{CLANG_DOS})
	else()
		set(CLANG_DOS /opt/clang-dos)
	endif()
endif()
set(CLANG_DOS ${CLANG_DOS} CACHE PATH "clang-retro's toolchain")
set(CMAKE_C_COMPILER ${CLANG_DOS}/bin/clang)
set(CMAKE_CXX_COMPILER ${CLANG_DOS}/bin/clang++)
set(CMAKE_RC_COMPILER ${CLANG_DOS}/bin/llvm-rc)
set(CMAKE_AR ${CLANG_DOS}/bin/llvm-ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${CLANG_DOS}/bin/llvm-ranlib CACHE FILEPATH "")
# (_GNU_SOURCE: picolibc's CLOCK_MONOTONIC and the rest of POSIX's)
set(_f "--config=${CLANG_DOS}/bin/win32-picolibc.cfg -D_GNU_SOURCE")
set(CMAKE_C_FLAGS_INIT "${_f}")
set(CMAKE_CXX_FLAGS_INIT "${_f}")
set(CMAKE_FIND_ROOT_PATH ${CLANG_DOS}/sysroots/win32-picolibc)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
