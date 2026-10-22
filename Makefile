# minidb 的 Makefile：Linux / macOS / Windows(mingw32-make) 通用
#
#   make            构建 build/bin/minidb
#   make test       构建并运行全部自测（单元测试 + 端到端）
#   make clean      清理构建产物
#
# 没有 make 也可以用 ./build.sh，效果相同。
# 注意：两个脚本把 .o 放在按编译器签名区分的目录 build/obj/<machine>-<version>/ 下，
# 所以不同的工具链不会共用同一堆目标文件（混用会让 ld 报 dangerous relocation 甚至崩溃）。

CXX      ?= g++
CXXSTD   := -std=c++17
WARNINGS := -Wall -Wextra -Wno-unused-parameter
OPT      := -O2
INCLUDES := -Isrc

CXX_MACHINE := $(shell $(CXX) -dumpmachine 2>/dev/null || echo unknown)
CXX_VERSION := $(shell $(CXX) -dumpversion 2>/dev/null || echo 0)
# 非字母数字换成下划线，保证能当目录名
OBJ_TAG := $(shell echo "$(CXX_MACHINE)-$(CXX_VERSION)" | tr -c 'A-Za-z0-9._-' '_')

OBJDIR := build/obj/$(OBJ_TAG)
BINDIR := build/bin

UNAME_S  := $(shell uname -s 2>/dev/null)
ifneq (,$(findstring MINGW,$(UNAME_S)))
  CXXFLAGS_EXTRA := -D__USE_MINGW_ANSI_STDIO=1
  LDFLAGS        := -static -static-libgcc -static-libstdc++
  EXE            := .exe
else ifneq (,$(findstring MSYS,$(UNAME_S)))
  CXXFLAGS_EXTRA := -D__USE_MINGW_ANSI_STDIO=1
  LDFLAGS        := -static
  EXE            := .exe
else
  CXXFLAGS_EXTRA :=
  LDFLAGS        :=
  EXE            :=
endif

# -MMD -MP：随编译生成 .d 依赖文件，改头文件只重编真正依赖它的 .cpp
CXXFLAGS := $(CXXSTD) $(OPT) $(WARNINGS) $(INCLUDES) $(CXXFLAGS_EXTRA) -MMD -MP

LIB_SRCS  := $(shell find src -name '*.cpp' ! -name 'main.cpp' | sort)
LIB_OBJS  := $(patsubst %.cpp,$(OBJDIR)/%.o,$(LIB_SRCS))
MAIN_OBJ  := $(OBJDIR)/src/shell/main.o
TEST_SRCS := $(wildcard tests/test_*.cpp)
TEST_BINS := $(patsubst tests/%.cpp,$(BINDIR)/%$(EXE),$(TEST_SRCS))

MAIN_BIN  := $(BINDIR)/minidb$(EXE)

.PHONY: all test unit e2e clean rebuild

all: $(MAIN_BIN)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(MAIN_BIN): $(LIB_OBJS) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BINDIR)/test_%$(EXE): tests/test_%.cpp $(LIB_OBJS)
	@mkdir -p $(dir $@) $(OBJDIR)/tests
	$(CXX) $(CXXFLAGS) -MF $(OBJDIR)/tests/test_$*.d $< $(LIB_OBJS) -o $@ $(LDFLAGS)

# 把编译期生成的依赖文件读进来（前面的 - 表示文件不存在也不报错）
TEST_DEPS := $(patsubst tests/%.cpp,$(OBJDIR)/tests/%.d,$(TEST_SRCS))
DEPS := $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_DEPS)
-include $(DEPS)

unit: $(TEST_BINS)
	@rm -rf _test_tmp _test_buf _test_record _test_index _test_db
	@for t in $(TEST_BINS); do $$t || exit 1; done
	@echo "unit tests: OK"

e2e: $(MAIN_BIN)
	@bash tests/run_e2e.sh

test: unit e2e

clean:
	rm -rf build _test_tmp _test_buf _test_record _test_index _test_db e2e_data smoke repl_data data demo_data

rebuild: clean all
