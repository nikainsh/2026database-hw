/* database26 - test-only crash injection hook */
#include "common/fault_inject.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
const char *g_crash_point = std::getenv("MINIDB_CRASH_POINT");
}  // namespace

void fault_inject_point(const char *point)
{
  if (g_crash_point != nullptr && point != nullptr && std::strcmp(g_crash_point, point) == 0) {
    std::fprintf(stderr, "[fault-inject] crash at %s\n", point);
    std::fflush(stderr);
    std::_Exit(99);
  }
}
