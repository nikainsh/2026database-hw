#!/usr/bin/env bash
# Build and run executor-owned checks without invoking unrelated storage tests.
# bash tests/run_execution_tests.sh
# bash tests/run_execution_tests.sh --bench --rows 1000 --repeat 7
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/usr/bin:${PATH}"
CXX=${CXX:-g++}
MODE=${1:-check}
if [ "$MODE" = "--bench" ]; then
  shift
elif [ "$#" -gt 0 ]; then
  echo "usage: bash tests/run_execution_tests.sh [--bench --rows N --repeat N --csv path]"
  exit 2
fi

# Reuse the project's object layout and compiler identity; do not mix toolchains.
CXX="$CXX" bash build.sh
MACHINE=$($CXX -dumpmachine)
VERSION=$($CXX -dumpversion)
TAG=$(printf '%s\n' "${MACHINE}-${VERSION}" | tr -c 'A-Za-z0-9._-' '_')
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc)
LINK=()
EXE=""
if printf '%s' "$MACHINE" | grep -qi mingw; then
  FLAGS+=(-D__USE_MINGW_ANSI_STDIO=1)
  LINK+=(-static -static-libgcc -static-libstdc++)
  EXE=".exe"
fi
OBJECTS=()
while IFS= read -r src; do
  OBJECTS+=("build/obj/${TAG}/${src%.cpp}.o")
done < <(find src -name '*.cpp' ! -name main.cpp | sort)
mkdir -p build/execution_checks
TESTS=(test_execution_lifecycle test_execution_operators test_nested_loop_join
       test_hash_join test_hash_join_sql test_hash_join_composite test_hash_join_workloads)
FAIL=0
for name in "${TESTS[@]}"; do
  "$CXX" "${FLAGS[@]}" "tests/${name}.cpp" "${OBJECTS[@]}" -o "build/bin/${name}${EXE}" "${LINK[@]}"
  if "build/bin/${name}${EXE}" >"build/execution_checks/${name}.log" 2>&1; then
    printf 'PASS %s\n' "$name"
  else
    printf 'FAIL %s\n' "$name"
    cat "build/execution_checks/${name}.log"
    FAIL=$((FAIL + 1))
  fi
done
printf 'Executor checks: PASS=%d FAIL=%d\n' "$((${#TESTS[@]} - FAIL))" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
if [ "$MODE" = "--bench" ]; then
  "build/bin/test_hash_join_workloads${EXE}" --bench "$@"
fi
