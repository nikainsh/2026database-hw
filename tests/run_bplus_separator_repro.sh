#!/usr/bin/env bash
# Build and run the deterministic B+Tree separator reproducer.
# While the bug exists this script intentionally exits 1 after printing the
# separator mismatch. It is not part of the default build.sh test suite.
set -e

export PATH="/usr/bin:${PATH}"

CXX=${CXX:-g++}
CXXFLAGS="-std=c++17 -O2 -Isrc -pthread"
LDFLAGS=""

CXX_MACHINE=$($CXX -dumpmachine 2>/dev/null || echo unknown)
if echo "${CXX_MACHINE}" | grep -qi mingw; then
  CXXFLAGS="$CXXFLAGS -D__USE_MINGW_ANSI_STDIO=1"
  LDFLAGS="-static -static-libgcc -static-libstdc++"
fi

# Ensure the library objects exist and are current.
bash build.sh >/dev/null
OBJS=$(find build/obj -path '*/src/*.o' ! -path '*/shell/main.o' | sort)
if [ -z "$OBJS" ]; then
  echo "no compiled objects found under build/obj" >&2
  exit 1
fi

mkdir -p build/bin
overall=0
for src in tests/repro_bplus_separator.cpp tests/repro_bplus_separator_int.cpp; do
  name=$(basename "$src" .cpp)
  $CXX $CXXFLAGS "$src" $OBJS -o "build/bin/$name" $LDFLAGS
  if ! "./build/bin/$name"; then
    overall=1
  fi
done
exit $overall
