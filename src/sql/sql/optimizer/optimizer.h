/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/rc.h"
#include "common/value.h"
#include "sql/operator/scan_operators.h"
#include "sql/plan/logical_operator.h"
#include "sql/plan/physical_operator.h"
#include "sql/stmt/stmt.h"

/// 一条语句的逻辑计划
struct LogicalPlan
{
  std::unique_ptr<LogicalOperator> root;

  std::string to_string() const;
};

/**
 * @brief Stmt -> 逻辑计划
 * @details 对应 miniob 的 LogicalPlanGenerator。
 */
class LogicalPlanGenerator
{
public:
  RC create(Stmt &stmt, LogicalPlan &plan);

private:
  RC create_select(SelectStmt &stmt, LogicalPlan &plan, std::unique_ptr<LogicalOperator> &root);
};

/// 规则集合：把逻辑计划改写得更快（语义不变）
class Optimizer
{
public:
  explicit Optimizer(LogicalPlan &plan) : plan_(plan) {}

  RC optimize();

private:
  /// 谓词下推：只涉及单表的条件搬到 table_get 上，尽早减少数据量
  RC predicate_pushdown();
  /// 比较式化简：常量比较折叠成 TRUE/FALSE，恒真条件删除，恒假标记空扫描
  RC comparison_simplification();

  LogicalPlan &plan_;
};

/**
 * @brief 逻辑计划 -> 物理计划，同时做访问路径选择（索引 or 全表）
 * @details 对应 miniob 的 PhysicalPlanGenerator + 索引选择。
 */
class PhysicalPlanGenerator
{
public:
  RC create(LogicalOperator &logical, std::unique_ptr<PhysicalOperator> &plan);

private:
  /// 找出可用的索引扫描范围；返回 true 表示应该走索引
  bool choose_index_access(TableGetOperator &get, Index *&index, IndexScanRange &range);
};
