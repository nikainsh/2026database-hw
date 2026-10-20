/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/plan/physical_operator.h"

const char *physical_operator_kind_to_string(PhysicalOperatorKind kind)
{
  switch (kind) {
    case PhysicalOperatorKind::TABLE_SCAN: return "table_scan";
    case PhysicalOperatorKind::INDEX_SCAN: return "index_scan";
    case PhysicalOperatorKind::PREDICATE: return "predicate";
    case PhysicalOperatorKind::NESTED_LOOP_JOIN: return "nested_loop_join";
    case PhysicalOperatorKind::PROJECT: return "project";
    case PhysicalOperatorKind::SCALAR_GROUP_BY: return "scalar_group_by";
    case PhysicalOperatorKind::HASH_GROUP_BY: return "hash_group_by";
    case PhysicalOperatorKind::INSERT: return "insert";
    case PhysicalOperatorKind::DELETE_OP: return "delete";
    case PhysicalOperatorKind::UPDATE: return "update";
    case PhysicalOperatorKind::STRING_LIST: return "string_list";
    case PhysicalOperatorKind::EXPLAIN: return "explain";
  }
  return "unknown";
}

void PhysicalOperator::explain(std::ostream &os, int depth) const
{
  for (int i = 0; i < depth; ++i) {
    os << "  ";
  }
  os << describe() << "\n";
  for (const auto &child : children_) {
    child->explain(os, depth + 1);
  }
}
