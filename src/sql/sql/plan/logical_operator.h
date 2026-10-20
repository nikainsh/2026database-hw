/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "common/rc.h"
#include "common/value.h"
#include "sql/expr/expression.h"
#include "storage/table/table.h"

/// 逻辑算子种类，对应 miniob 的 LogicalOperator::Type
enum class LogicalOperatorKind
{
  TABLE_GET,
  PREDICATE,
  JOIN,
  PROJECT,
  AGGREGATE,
  INSERT,
  DELETE_OP,
  UPDATE,
  EXPLAIN,
};

const char *logical_operator_kind_to_string(LogicalOperatorKind kind);

/**
 * @brief 逻辑算子：只描述“要算什么”，不涉及怎么算
 * @details 所有权约定：所有表达式都由 Stmt（进而由 QueryEngine 持有的 ParseResult/Stmt）持有，
 *          计划树与物理算子只借用指针。Stmt 的生命周期覆盖整个执行过程，因此指针始终有效。
 */
class LogicalOperator
{
public:
  explicit LogicalOperator(LogicalOperatorKind kind) : kind_(kind) {}
  virtual ~LogicalOperator() = default;

  LogicalOperatorKind kind() const { return kind_; }

  const std::vector<std::unique_ptr<LogicalOperator>> &children() const { return children_; }
  void                                                add_child(std::unique_ptr<LogicalOperator> child)
  {
    children_.push_back(std::move(child));
  }

  virtual std::string describe() const = 0;

  /// 打印一棵计划树（explain 用）
  void explain(std::ostream &os, int depth = 0) const;

protected:
  LogicalOperatorKind                           kind_;
  std::vector<std::unique_ptr<LogicalOperator>> children_;
};

/// 扫一张表；filters 是谓词下推之后落到表上的条件（彼此是 AND 关系）
class TableGetOperator : public LogicalOperator
{
public:
  TableGetOperator(Table *table, int table_index)
      : LogicalOperator(LogicalOperatorKind::TABLE_GET), table_(table), table_index_(table_index)
  {}

  Table *table() const { return table_; }
  int    table_index() const { return table_index_; }

  const std::vector<Expression *> &filters() const { return filters_; }
  void                              add_filter(Expression *filter) { filters_.push_back(filter); }
  void                              set_filters(std::vector<Expression *> filters) { filters_ = std::move(filters); }

  /// 常量折叠后可以直接断定没有数据满足条件，物理计划就换成空扫描
  bool        always_false() const { return always_false_; }
  void        mark_always_false() { always_false_ = true; }

  std::string describe() const override
  {
    std::string text = "table_get(" + table_->table_name() + ")";
    for (Expression *filter : filters_) {
      text += " filter=" + filter->describe();
    }
    if (always_false_) {
      text += " always_false";
    }
    return text;
  }

private:
  Table                    *table_        = nullptr;
  int                       table_index_  = 0;
  std::vector<Expression *> filters_;
  bool                      always_false_ = false;
};

/// where 条件：按 conjuncts（AND 关系）过滤 child 的输出
class PredicateOperator : public LogicalOperator
{
public:
  explicit PredicateOperator(std::vector<Expression *> conjuncts)
      : LogicalOperator(LogicalOperatorKind::PREDICATE), conjuncts_(std::move(conjuncts))
  {}

  const std::vector<Expression *> &conjuncts() const { return conjuncts_; }
  void                              set_conjuncts(std::vector<Expression *> conjuncts)
  {
    conjuncts_ = std::move(conjuncts);
  }

  bool        always_false() const { return always_false_; }
  void        mark_always_false() { always_false_ = true; }

  std::string describe() const override
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
    if (always_false_) {
      text += " always_false";
    }
    return text + ")";
  }

private:
  std::vector<Expression *> conjuncts_;
  bool                      always_false_ = false;
};

/// 多表连接：children 是各张表的扫描；连接条件留在上层 predicate 里过滤
class JoinOperator : public LogicalOperator
{
public:
  JoinOperator() : LogicalOperator(LogicalOperatorKind::JOIN) {}

  std::string describe() const override
  {
    return "join(" + std::to_string(children_.size()) + " tables, nested loop)";
  }
};

/// 投影：select 列表
class ProjectOperator : public LogicalOperator
{
public:
  explicit ProjectOperator(std::vector<Expression *> project_list)
      : LogicalOperator(LogicalOperatorKind::PROJECT), project_list_(std::move(project_list))
  {}

  const std::vector<Expression *> &project_list() const { return project_list_; }

  std::string describe() const override
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
};

/// 聚合：select 里的聚合项 + group by 键
class AggregateOperator : public LogicalOperator
{
public:
  AggregateOperator(std::vector<Expression *> select_list, std::vector<Expression *> group_by)
      : LogicalOperator(LogicalOperatorKind::AGGREGATE),
        select_list_(std::move(select_list)),
        group_by_(std::move(group_by))
  {}

  const std::vector<Expression *> &select_list() const { return select_list_; }
  const std::vector<Expression *> &group_by() const { return group_by_; }

  std::string describe() const override
  {
    std::string text = group_by_.empty() ? "scalar_aggregate(" : "group_by(";
    for (size_t i = 0; i < select_list_.size(); ++i) {
      if (i > 0) {
        text += ", ";
      }
      text += select_list_[i]->describe();
    }
    if (!group_by_.empty()) {
      text += " by ";
      for (size_t i = 0; i < group_by_.size(); ++i) {
        if (i > 0) {
          text += ", ";
        }
        text += group_by_[i]->describe();
      }
    }
    return text + ")";
  }

private:
  std::vector<Expression *> select_list_;
  std::vector<Expression *> group_by_;
};

/// insert
class InsertLogicalOperator : public LogicalOperator
{
public:
  InsertLogicalOperator(Table *table, std::vector<std::vector<Value>> rows)
      : LogicalOperator(LogicalOperatorKind::INSERT), table_(table), rows_(std::move(rows))
  {}

  Table *table() const { return table_; }
  const std::vector<std::vector<Value>> &rows() const { return rows_; }

  std::string describe() const override
  {
    return "insert(" + table_->table_name() + ", " + std::to_string(rows_.size()) + " rows)";
  }

private:
  Table                        *table_ = nullptr;
  std::vector<std::vector<Value>> rows_;
};

/// delete
class DeleteLogicalOperator : public LogicalOperator
{
public:
  explicit DeleteLogicalOperator(Table *table)
      : LogicalOperator(LogicalOperatorKind::DELETE_OP), table_(table)
  {}

  Table *table() const { return table_; }

  std::string describe() const override { return "delete(" + table_->table_name() + ")"; }

private:
  Table *table_ = nullptr;
};

/// update
class UpdateLogicalOperator : public LogicalOperator
{
public:
  UpdateLogicalOperator(Table *table, std::vector<std::pair<int, Value>> assignments)
      : LogicalOperator(LogicalOperatorKind::UPDATE), table_(table), assignments_(std::move(assignments))
  {}

  Table                                    *table() const { return table_; }
  const std::vector<std::pair<int, Value>> &assignments() const { return assignments_; }

  std::string describe() const override
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
  Table                                    *table_ = nullptr;
  std::vector<std::pair<int, Value>>        assignments_;
};

/// explain
class ExplainLogicalOperator : public LogicalOperator
{
public:
  ExplainLogicalOperator() : LogicalOperator(LogicalOperatorKind::EXPLAIN) {}

  std::string describe() const override { return "explain"; }
};
