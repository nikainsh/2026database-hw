/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include "sql/expr/expression.h"
#include "sql/plan/physical_operator.h"
#include "storage/index/index.h"
#include "storage/record/record_scanner.h"
#include "storage/table/table.h"

/// 索引扫描的键范围；两个方向都可以不设界
struct IndexScanRange
{
  bool  has_low        = false;
  Value low;
  bool  low_inclusive  = true;
  bool  has_high       = false;
  Value high;
  bool  high_inclusive = true;
};

/**
 * @brief 全表扫描
 * @details 对应 miniob 的 TableScanPhysicalOperator；下推到表上的条件（AND 关系）在这里过滤。
 */
class TableScanOperator : public PhysicalOperator
{
public:
  TableScanOperator(Table *table, std::vector<Expression *> filters, int table_index)
      : table_(table), filters_(std::move(filters)), table_index_(table_index)
  {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::TABLE_SCAN; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  RC                   close() override;
  std::string          describe() const override
  {
    std::string text = "table_scan(" + table_->table_name() + ")";
    for (Expression *filter : filters_) {
      text += " filter=" + filter->describe();
    }
    return text;
  }

private:
  RC fill_tuple(const Record &record);
  RC matches(const Tuple &tuple, bool &ok);

  Table                     *table_       = nullptr;
  std::vector<Expression *>  filters_;
  int                        table_index_ = 0;

  RecordScanner scanner_;
  Record        record_;
  Tuple         tuple_;
};

/**
 * @brief 索引扫描：按 B+Tree 的范围游标取 RID，再回表拿完整记录
 * @details 对应 miniob 的 IndexScanPhysicalOperator。回表是随机 I/O，
 *          所以优化器只在「等值或窄范围」时才选它。
 */
class IndexScanOperator : public PhysicalOperator
{
public:
  IndexScanOperator(Table *table, Index *index, std::vector<Expression *> filters,
                    const IndexScanRange &range, int table_index)
      : table_(table), index_(index), filters_(std::move(filters)), range_(range), table_index_(table_index)
  {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::INDEX_SCAN; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  RC                   close() override;
  std::string          describe() const override
  {
    std::string text = "index_scan(" + table_->table_name() + "." + index_->index_meta().index_name + ")";
    if (range_.has_low) {
      text += " low=" + range_.low.to_string() + (range_.low_inclusive ? "(closed)" : "(open)");
    }
    if (range_.has_high) {
      text += " high=" + range_.high.to_string() + (range_.high_inclusive ? "(closed)" : "(open)");
    }
    for (Expression *filter : filters_) {
      text += " filter=" + filter->describe();
    }
    return text;
  }

private:
  RC fill_tuple(const Record &record);
  RC matches(const Tuple &tuple, bool &ok);

  Table                     *table_       = nullptr;
  Index                     *index_       = nullptr;
  std::vector<Expression *>  filters_;
  IndexScanRange              range_;
  int                         table_index_ = 0;

  BplusTreeScanner scanner_;
  Tuple            tuple_;
  std::string      key_;
};
