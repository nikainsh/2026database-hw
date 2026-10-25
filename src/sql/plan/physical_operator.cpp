/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/plan/physical_operator.h"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace {
using Clock = std::chrono::steady_clock;
int64_t nanoseconds_since(Clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}
}

RC PhysicalOperator::open_tracked()
{
  if (!statistics_enabled_) return open();
  ++statistics_.open_calls;
  const auto start = Clock::now();
  RC rc = open();
  statistics_.open_ns += nanoseconds_since(start);
  if (OB_FAIL(rc)) ++statistics_.errors;
  return rc;
}

RC PhysicalOperator::next_tracked(Tuple *&tuple)
{
  if (!statistics_enabled_) return next(tuple);
  ++statistics_.next_calls;
  const auto start = Clock::now();
  RC rc = next(tuple);
  statistics_.next_ns += nanoseconds_since(start);
  if (OB_SUCC(rc)) ++statistics_.output_rows;
  else if (rc != RC::RECORD_EOF) ++statistics_.errors;
  return rc;
}

RC PhysicalOperator::close_tracked()
{
  if (!statistics_enabled_) return close();
  ++statistics_.close_calls;
  const auto start = Clock::now();
  RC rc = close();
  statistics_.close_ns += nanoseconds_since(start);
  if (OB_FAIL(rc)) ++statistics_.errors;
  return rc;
}

void PhysicalOperator::reset_statistics(bool enabled)
{
  statistics_enabled_ = enabled;
  statistics_ = OperatorStatistics{};
  for (auto &child : children_) child->reset_statistics(enabled);
}

int64_t PhysicalOperator::input_rows() const
{
  int64_t rows = 0;
  for (const auto &child : children_) rows += child->statistics().output_rows;
  return rows;
}

void PhysicalOperator::explain_statistics(std::ostream &os, int depth) const
{
  const auto &s = statistics_;
  os << std::string(static_cast<size_t>(depth * 2), ' ') << describe()
     << " in_rows=" << input_rows() << " out_rows=" << s.output_rows
     << " opens=" << s.open_calls << " nexts=" << s.next_calls << " closes=" << s.close_calls
     << " errors=" << s.errors << " candidates=" << s.candidate_pairs;
  if (kind() == PhysicalOperatorKind::HASH_JOIN) {
    os << " build_rows=" << s.build_rows << " probe_rows=" << s.probe_rows
       << " peak_build_bytes_est=" << s.peak_build_bytes;
  }
  // 独立流格式化，避免改变调用者的精度设置。
  std::ostringstream time;
  time << std::fixed << std::setprecision(3)
       << " open_us=" << s.open_ns / 1000.0 << " next_us=" << s.next_ns / 1000.0
       << " close_us=" << s.close_ns / 1000.0
       << " inclusive_us=" << (s.open_ns + s.next_ns + s.close_ns) / 1000.0;
  os << time.str() << '\n';
  for (const auto &child : children_) child->explain_statistics(os, depth + 1);
}

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
    case PhysicalOperatorKind::HASH_JOIN: return "hash_join";
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
