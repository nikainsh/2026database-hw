/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <map>
#include <string>
#include <vector>

#include "sql/expr/expression.h"
#include "sql/plan/physical_operator.h"

/**
 * @brief 没有 group by 的聚合（整表一个组）
 * @details 对应 miniob 的 ScalarGroupByPhysicalOperator。open 时把子算子的数据全部消费掉，
 *          next 只输出一行。
 */
class ScalarGroupByOperator : public PhysicalOperator
{
public:
  explicit ScalarGroupByOperator(std::vector<Expression *> select_list)
      : select_list_(std::move(select_list))
  {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::SCALAR_GROUP_BY; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  RC                   close() override;
  std::string          describe() const override;

private:
  std::vector<Expression *> select_list_;
  std::vector<AggregateState> states_;
  Tuple                     tuple_;
  bool                      emitted_ = false;
  bool                      child_opened_ = false;
};

/**
 * @brief group by 聚合：按分组键把数据分桶，每组算一次聚合
 * @details 当前使用有序表查找分组，输出按分组首次遇到顺序，不保证 SQL 排序。
 */
class HashGroupByOperator : public PhysicalOperator
{
public:
  HashGroupByOperator(std::vector<Expression *> select_list, std::vector<Expression *> group_by)
      : select_list_(std::move(select_list)), group_by_(std::move(group_by))
  {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::HASH_GROUP_BY; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  RC                   close() override;
  std::string          describe() const override;

private:
  struct Group
  {
    Tuple                       sample;  /// 组内第一行，用来算非聚合的 select 项
    std::vector<AggregateState> states;
  };

  /// 分组键：类型、长度及无损值编码，避免显示精度与分隔符碰撞。
  RC make_group_key(const Tuple &tuple, std::string &key);

  std::vector<Expression *> select_list_;
  std::vector<Expression *> group_by_;

  std::map<std::string, size_t> index_;
  std::vector<Group>            groups_;
  Tuple                         tuple_;
  size_t                        cursor_ = 0;
  bool                          child_opened_ = false;
};
