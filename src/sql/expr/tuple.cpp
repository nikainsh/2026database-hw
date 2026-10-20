/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/expr/tuple.h"

#include <sstream>

std::string Tuple::to_string() const
{
  std::ostringstream oss;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      oss << " | ";
    }
    oss << values[i].to_string();
  }
  if (has_rid) {
    oss << " @rid=" << rid.to_string();
  }
  return oss.str();
}
