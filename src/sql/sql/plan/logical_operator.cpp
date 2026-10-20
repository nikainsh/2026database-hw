/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/plan/logical_operator.h"

const char *logical_operator_kind_to_string(LogicalOperatorKind kind)
{
  switch (kind) {
    case LogicalOperatorKind::TABLE_GET: return "table_get";
    case LogicalOperatorKind::PREDICATE: return "predicate";
    case LogicalOperatorKind::JOIN: return "join";
    case LogicalOperatorKind::PROJECT: return "project";
    case LogicalOperatorKind::AGGREGATE: return "aggregate";
    case LogicalOperatorKind::INSERT: return "insert";
    case LogicalOperatorKind::DELETE_OP: return "delete";
    case LogicalOperatorKind::UPDATE: return "update";
    case LogicalOperatorKind::EXPLAIN: return "explain";
  }
  return "unknown";
}

void LogicalOperator::explain(std::ostream &os, int depth) const
{
  for (int i = 0; i < depth; ++i) {
    os << "  ";
  }
  os << describe() << "\n";
  for (const auto &child : children_) {
    child->explain(os, depth + 1);
  }
}
