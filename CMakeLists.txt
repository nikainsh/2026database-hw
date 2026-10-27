# database 的 CMake 工程（等价于 Makefile / build.sh，供习惯 cmake 的环境使用）
cmake_minimum_required(VERSION 3.10)
project(database CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

file(GLOB_RECURSE MINIDB_LIB_SRC CONFIGURE_DEPENDS src/*.cpp)
list(FILTER MINIDB_LIB_SRC EXCLUDE REGEX "main\\.cpp$")

add_executable(database src/shell/main.cpp ${MINIDB_LIB_SRC})

if(MINGW OR WIN32)
  add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()

# 单元测试：tests/test_*.cpp 每个都是独立的可执行文件
file(GLOB MINIDB_TESTS CONFIGURE_DEPENDS tests/test_*.cpp)
enable_testing()
foreach(test_src ${MINIDB_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src} ${MINIDB_LIB_SRC})
  # Keep assertions active even in Release builds.
  target_compile_options(${test_name} PRIVATE -UNDEBUG)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
