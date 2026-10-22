#!/usr/bin/env bash
# minidb 构建脚本：不依赖 cmake，Git Bash / Linux / macOS 都能直接用
#
#   ./build.sh            编译命令行程序 -> build/bin/minidb
#   ./build.sh test       编译主程序 + 全部自测并运行
#   ./build.sh clean      清理 build/ 与测试数据
#   ./build.sh rebuild    重新编译全部
#
# 增量编译靠 -MMD 生成的 .d 依赖文件，所以改头文件只会重编真正依赖它的 .cpp。
set -e

CXX=${CXX:-g++}

# 把编译器身份放进对象目录名：两套不同的 MinGW/ GCC 永远不会共用同一堆 .o，
# 避免 "dangerous relocation / ld 段错误" 这种混合产物导致的链接崩溃。
CXX_MACHINE=$($CXX -dumpmachine 2>/dev/null || echo unknown)
CXX_VERSION=$($CXX -dumpversion 2>/dev/null || echo 0)
OBJ_TAG=$(echo "${CXX_MACHINE}-${CXX_VERSION}" | tr -c 'A-Za-z0-9._-' '_')

OBJDIR="build/obj/${OBJ_TAG}"
BINDIR="build/bin"

CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc -MMD -MP"
LDFLAGS=""

# MinGW：msvcrt 的 printf 不认 %lld，需要打开 ANSI stdio 兼容；静态链接免去 libstdc++ DLL
if echo "${CXX_MACHINE}" | grep -qi mingw; then
  CXXFLAGS="$CXXFLAGS -D__USE_MINGW_ANSI_STDIO=1"
  LDFLAGS="-static -static-libgcc -static-libstdc++"
  EXE=".exe"
else
  EXE=""
fi

mkdir -p "$BINDIR" "$OBJDIR"

LIB_SRCS=$(find src -name '*.cpp' ! -name 'main.cpp' | sort)
LIB_OBJS=()

# 源文件、它包含过的每个头文件，只要有一个比 .o 新，就要重编
needs_rebuild() {
  local src="$1" obj="$2" dep="$3"
  [ -f "$obj" ] || return 0
  [ "$src" -nt "$obj" ] && return 0
  if [ -f "$dep" ]; then
    # .d 的格式是 "目标: 依赖1 依赖2 ... \"（可续行），取冒号后面的全部依赖项
    local prereqs prereq
    prereqs=$(sed -e 's/\\$//' -e 's/^[^:]*://' "$dep" | tr -s ' \t\n' ' ')
    for prereq in $prereqs; do
      if [ -f "$prereq" ] && [ "$prereq" -nt "$obj" ]; then
        return 0
      fi
    done
  fi
  return 1
}

compile_one() {
  local src="$1"
  local obj="${OBJDIR}/${src%.cpp}.o"
  local dep="${OBJDIR}/${src%.cpp}.d"
  mkdir -p "$(dirname "$obj")"
  if needs_rebuild "$src" "$obj" "$dep"; then
    echo "  CXX $src"
    $CXX $CXXFLAGS -c "$src" -o "$obj"
  fi
}

build_lib() {
  LIB_OBJS=()
  for src in $LIB_SRCS; do
    compile_one "$src"
    LIB_OBJS+=("${OBJDIR}/${src%.cpp}.o")
  done
}

link_main() {
  if [ -f src/shell/main.cpp ]; then
    echo "==> linking ${BINDIR}/minidb$EXE"
    $CXX $CXXFLAGS src/shell/main.cpp "${LIB_OBJS[@]}" -o "${BINDIR}/minidb$EXE" $LDFLAGS \
      || { echo "!! 链接失败。若报 dangerous relocation / ld 崩溃，请先执行：bash build.sh clean"; return 1; }
  else
    echo "==> no src/shell/main.cpp yet, only compiled library objects"
  fi
}

build_main() {
  build_lib
  link_main
}

build_tests() {
  build_lib
  link_main
  for t in $(ls tests/test_*.cpp 2>/dev/null); do
    name=$(basename "$t" .cpp)
    echo "==> building ${BINDIR}/$name$EXE"
    $CXX $CXXFLAGS "$t" "${LIB_OBJS[@]}" -o "${BINDIR}/$name$EXE" $LDFLAGS
  done

  echo "==> running tests"
  rm -rf _test_tmp _test_buf _test_record _test_index _test_db
  for t in $(ls tests/test_*.cpp 2>/dev/null); do
    name=$(basename "$t" .cpp)
    "${BINDIR}/$name$EXE"
  done
}

case "${1:-all}" in
  all) build_main ;;
  test | tests) build_tests ;;
  clean) rm -rf build _test_tmp _test_buf _test_record _test_index _test_db e2e_data smoke repl_data data demo_data ;;
  rebuild) rm -rf build; mkdir -p "$BINDIR" "$OBJDIR"; build_main ;;
  *) echo "usage: $0 [all|test|clean|rebuild]"; exit 1 ;;
esac
