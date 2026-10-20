/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <utility>
#include <vector>

#include "sql/plan/physical_operator.h"
#include "storage/table/table.h"

/// 插入算子：open 时把值打包成记录写进表（含索引维护），不输出行
class InsertOperator : public PhysicalOperator
{
public:
  InsertOperator(Table *table, std::vector<std::vector<Value>> rows)
      : table_(table), rows_(std::move(rows))
  {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::INSERT; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  std::string          describe() const override
  {
    return "insert(" + table_->table_name() + ", " + std::to_string(rows_.size()) + " rows)";
  }

private:
  Table                       *table_ = nullptr;
  std::vector<std::vector<Value>> rows_;
};

/// 删除算子：子算子每输出一行就删掉它的 rid
class DeleteOperator : public PhysicalOperator
{
public:
  explicit DeleteOperator(Table *table) : table_(table) {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::DELETE_OP; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  std::string          describe() const override { return "delete(" + table_->table_name() + ")"; }

private:
  Table *table_    = nullptr;
  bool  finished_ = false;
};

/// 更新算子：定长记录原地改写，assignments 是「列下标 -> 新值」
class UpdateOperator : public PhysicalOperator
{
public:
  UpdateOperator(Table *table, std::vector<std::pair<int, Value>> assignments)
      : table_(table), assignments_(std::move(assignments))
  {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::UPDATE; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  std::string          describe() const override
  {
    std::string text = "update(" + table_->table_name() + ", set ";
    for (size_t i = 0; i < assignments_.size(); ++i) {
      if (i > 0) {
        text += ", ";
      }
      text += table_->table_meta().columns()[assignments_[i].first].name() + "=" + assignments_[i].second.to_string();
    }
    return text + ")";
  }

private:
  Table                              *table_ = nullptr;
  std::vector<std::pair<int, Value>> assignments_;
  bool                               finished_ = false;
};
