/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "common/rc.h"
#include "sql/expr/tuple.h"

/// 物理算子种类
enum class PhysicalOperatorKind
{
  TABLE_SCAN,
  INDEX_SCAN,
  PREDICATE,
  NESTED_LOOP_JOIN,
  PROJECT,
  SCALAR_GROUP_BY,
  HASH_GROUP_BY,
  INSERT,
  DELETE_OP,
  UPDATE,
  STRING_LIST,
  EXPLAIN,
};

const char *physical_operator_kind_to_string(PhysicalOperatorKind kind);

/**
 * @brief 物理算子基类：火山模型（open / next / close）
 * @details 对应 miniob 的 PhysicalOperator。next() 把结果写进算子自己的 tuple_
 *          并通过引用参数返回指针；返回 RC::RECORD_EOF 表示没有更多数据。
 *          DML 算子用 affected_rows() 汇报影响了多少行。
 */
class PhysicalOperator
{
public:
  virtual ~PhysicalOperator() = default;

  virtual PhysicalOperatorKind kind() const = 0;

  virtual RC open() { return RC::SUCCESS; }
  virtual RC next(Tuple *&tuple) = 0;
  /// 默认实现递归关闭所有子算子；需要额外清理的算子再自己覆盖
  virtual RC close()
  {
    RC rc = RC::SUCCESS;
    for (auto &child : children_) {
      RC one = child->close();
      if (OB_FAIL(one)) {
        rc = one;
      }
    }
    return rc;
  }

  const std::vector<std::unique_ptr<PhysicalOperator>> &children() const { return children_; }
  void add_child(std::unique_ptr<PhysicalOperator> child) { children_.push_back(std::move(child)); }

  virtual std::string describe() const = 0;

  void explain(std::ostream &os, int depth = 0) const;

  int64_t affected_rows() const { return affected_rows_; }
  void    increase_affected_rows(int64_t count = 1) { affected_rows_ += count; }

protected:
  std::vector<std::unique_ptr<PhysicalOperator>> children_;
  int64_t                                        affected_rows_ = 0;
};
