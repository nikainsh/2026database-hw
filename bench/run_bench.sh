#!/usr/bin/env bash
set -e

# Git Bash on Windows may pick Windows find.exe; prefer GNU find.
export PATH="/usr/bin:$PATH"

CXX=${CXX:-g++}
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Isrc -pthread"
LDFLAGS=""

CXX_MACHINE=$($CXX -dumpmachine 2>/dev/null || echo unknown)
if echo "${CXX_MACHINE}" | grep -qi mingw; then
  CXXFLAGS="$CXXFLAGS -D__USE_MINGW_ANSI_STDIO=1"
  LDFLAGS="-static -static-libgcc -static-libstdc++"
fi

SRCS=$(find src/common src/storage/buffer -name '*.cpp' | sort)
$CXX $CXXFLAGS bench/bench_buffer_pool.cpp $SRCS -o bench/bench_buffer_pool $LDFLAGS
./bench/bench_buffer_pool
