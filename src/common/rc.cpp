/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "common/rc.h"

#include <cstddef>

const char *strrc(RC rc)
{
  static const char *names[] = {
#define DEFINE_RC(name) #name,
      DEFINE_RCS
#undef DEFINE_RC
  };
  const size_t index = static_cast<size_t>(rc);
  if (index < sizeof(names) / sizeof(names[0])) {
    return names[index];
  }
  return "UNKNOWN";
}
