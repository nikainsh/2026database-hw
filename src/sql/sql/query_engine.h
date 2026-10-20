/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/rc.h"
#include "sql/expr/tuple.h"
#include "sql/plan/physical_operator.h"
#include "storage/db/db.h"

/// 一条语句的执行结果概要，对应 miniob 的 SqlResult + CmdResult
struct SqlResult
{
  RC    code = RC::SUCCESS;
  std::string message;            /// 给用户的补充说明（错误详情、影响行数等）

  bool  is_query = false;         /// 是否有逐行输出
  std::vector<std::string> column_names;
  int64_t rows = 0;               /// 返回的行数
  int64_t affected_rows = 0;      /// DML 影响的行数

  int64_t execution_time_us = 0;

  bool        want_exit = false;  /// exit / quit
  const char *hint = nullptr;     /// help 等由 CLI 直接处理的提示
};

/**
 * @brief 结果输出的抽象，UI 层实现它
 * @details 把「执行引擎产出元组」和「界面怎么展示」解耦，
 *          将来换 GUI 只需要换一个 ResultWriter。
 */
class ResultWriter
{
public:
  virtual ~ResultWriter() = default;

  virtual RC write_header(const SqlResult &result) = 0;
  virtual RC write_row(const SqlResult &result, const Tuple &tuple) = 0;
  virtual RC write_result(const SqlResult &result) = 0;
};

/**
 * @brief 查询处理器 + 执行引擎的入口
 * @details 完整流水线：解析 → 语义检查 → 逻辑计划 → 优化 → 物理计划 → 逐行执行。
 *          对应 miniob 的 SQLStage / ResolveStage / OptimizeStage / ExecuteStage，
 *          这里合并成一个类，因为没有网络和并发会话。
 */
class QueryEngine
{
public:
  explicit QueryEngine(Db *db) : db_(db) {}

  RC execute(const std::string &sql, ResultWriter &writer, SqlResult &result);

  /// 只做解析和优化，返回计划文本（explain 用）
  RC explain(const std::string &sql, std::string &plan_text);

  Db *db() const { return db_; }

private:
  RC run_plan(PhysicalOperator &plan, const SqlResult &result, ResultWriter &writer);

  Db *db_ = nullptr;
};
