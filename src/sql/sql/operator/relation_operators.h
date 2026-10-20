/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <string>
#include <vector>

#include "sql/expr/expression.h"
#include "sql/plan/physical_operator.h"

/// 过滤算子：child 逐行输出，本算子只放行所有条件都为真的行
class FilterPhysicalOperator : public PhysicalOperator
{
public:
  explicit FilterPhysicalOperator(std::vector<Expression *> conjuncts) : conjuncts_(std::move(conjuncts)) {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::PREDICATE; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  std::string          describe() const override
  {
    if (conjuncts_.empty()) {
      return "predicate(true)";
    }
    std::string text = "predicate(";
    for (size_t i = 0; i < conjuncts_.size(); ++i) {
      if (i > 0) {
        text += " and ";
      }
      text += conjuncts_[i]->describe();
    }
    return text + ")";
  }

private:
  std::vector<Expression *> conjuncts_;
};

/**
 * @brief 嵌套循环连接：对左边的每一行，把右边完整扫一遍
 * @details 对应 miniob 的 NestedLoopJoinPhysicalOperator。join 条件为空时就是笛卡尔积。
 *          元组用「每列自带来源」的方式拼接，所以两边的列都能被上层表达式按 (table,col) 找到。
 */
class NestedLoopJoinOperator : public PhysicalOperator
{
public:
  explicit NestedLoopJoinOperator(Expression *on_condition) : on_condition_(on_condition) {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::NESTED_LOOP_JOIN; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  RC                   close() override;
  std::string          describe() const override
  {
    return "nested_loop_join" + (on_condition_ != nullptr ? " on=" + on_condition_->describe() : std::string(" (cross)"));
  }

private:
  Expression *on_condition_ = nullptr;

  Tuple left_tuple_;
  Tuple joined_;
  bool  left_ready_ = false;
  bool  left_eof_   = false;
};

/// 投影算子：按 select 列表求值，输出一行计算结果
class ProjectPhysicalOperator : public PhysicalOperator
{
public:
  explicit ProjectPhysicalOperator(std::vector<Expression *> project_list)
      : project_list_(std::move(project_list))
  {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::PROJECT; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  std::string          describe() const override
  {
    std::string text = "project(";
    for (size_t i = 0; i < project_list_.size(); ++i) {
      if (i > 0) {
        text += ", ";
      }
      text += project_list_[i]->describe();
    }
    return text + ")";
  }

private:
  std::vector<Expression *> project_list_;
  Tuple                     tuple_;
};

/// 直接吐出一批现成的行（show tables / desc table 用）
class StringListOperator : public PhysicalOperator
{
public:
  void add_row(const std::vector<Value> &row) { rows_.push_back(row); }

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::STRING_LIST; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  std::string          describe() const override { return "string_list(" + std::to_string(rows_.size()) + " rows)"; }

private:
  std::vector<std::vector<Value>> rows_;
  size_t                          index_ = 0;
  Tuple                           tuple_;
};

/// explain 算子：把计划树按行输出
class ExplainOperator : public PhysicalOperator
{
public:
  explicit ExplainOperator(std::string plan_text) : plan_text_(std::move(plan_text)) {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::EXPLAIN; }
  RC                   open() override;
  RC                   next(Tuple *&tuple) override;
  std::string          describe() const override { return "explain"; }

private:
  std::string       plan_text_;
  std::vector<std::string> lines_;
  Tuple              tuple_;
  size_t             index_ = 0;
};
