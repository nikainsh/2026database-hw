/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/query_engine.h"

#include <chrono>
#include <sstream>

#include "common/log.h"
#include "sql/operator/relation_operators.h"
#include "sql/optimizer/optimizer.h"
#include "sql/parser/parser.h"
#include "sql/stmt/stmt.h"

static int64_t elapsed_us(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

RC QueryEngine::run_plan(PhysicalOperator &plan, const SqlResult &input, ResultWriter &writer)
{
  SqlResult local  = input;
  RC        rc     = plan.open();
  if (OB_FAIL(rc)) {
    local.code = rc;
    writer.write_result(local);
    return rc;
  }

  if (local.is_query) {
    RC header_rc = writer.write_header(local);
    if (OB_FAIL(header_rc)) {
      return header_rc;
    }
  }

  int64_t rows = 0;
  while (true) {
    Tuple *tuple = nullptr;
    rc         = plan.next(tuple);
    if (rc == RC::RECORD_EOF) {
      rc = RC::SUCCESS;
      break;
    }
    if (OB_FAIL(rc)) {
      break;
    }
    ++rows;
    local.rows = rows;
    rc         = writer.write_row(local, *tuple);
    if (OB_FAIL(rc)) {
      break;
    }
  }

  RC close_rc = plan.close();
  if (OB_FAIL(close_rc) && rc == RC::SUCCESS) {
    rc = close_rc;
  }

  local.code           = rc;
  local.rows           = rows;
  local.affected_rows  = plan.affected_rows();
  writer.write_result(local);
  return rc;
}

/// desc table 的输出：字段、类型、长度、是否有索引
static void build_desc_rows(const TableMeta &meta, StringListOperator &list)
{
  for (const ColumnMeta &column : meta.columns()) {
    std::vector<Value> row;
    row.push_back(Value(column.name()));
    row.push_back(Value(std::string(attr_type_to_string(column.type()))));
    row.push_back(Value(std::to_string(column.data_len())));
    const IndexMeta *index = meta.index_of_field(column.name());
    row.push_back(Value(index == nullptr ? std::string("-")
                                         : std::string(index->index_name + (index->unique ? "(unique)" : ""))));
    list.add_row(row);
  }
}

RC QueryEngine::execute(const std::string &sql, ResultWriter &writer, SqlResult &result)
{
  const auto start = std::chrono::steady_clock::now();

  std::unique_ptr<ParseResult> parse_result;
  Parser                       parser;
  RC                           rc = parser.parse_statement(sql, parse_result);
  if (OB_FAIL(rc)) {
    result.code           = rc;
    result.message        = "syntax error, please check the statement (try: help)";
    result.execution_time_us = elapsed_us(start);
    return rc;
  }

  std::unique_ptr<Stmt> stmt;
  rc = Stmt::create_stmt(*parse_result, db_, stmt);
  if (OB_FAIL(rc)) {
    result.code           = rc;
    result.message        = std::string("semantic error: ") + strrc(rc);
    result.execution_time_us = elapsed_us(start);
    return rc;
  }

  switch (stmt->kind()) {
    case Stmt::Kind::EXIT:
      result.want_exit = true;
      result.code      = RC::SUCCESS;
      return RC::SUCCESS;

    case Stmt::Kind::HELP:
      result.hint = "help";
      result.code = RC::SUCCESS;
      return RC::SUCCESS;

    case Stmt::Kind::CREATE_TABLE: {
      CreateTableStmt &create = *static_cast<CreateTableStmt *>(stmt.get());
      if (db_->find_table_or_null(create.table_meta().table_name()) != nullptr && create.if_not_exists()) {
        result.message = "table exists, ignored by 'if not exists'";
        result.code    = RC::SUCCESS;
        break;
      }
      rc = db_->create_table(create.table_meta());
      if (OB_FAIL(rc)) {
        result.code = rc;
        result.message = std::string("failed to create table: ") + strrc(rc);
        break;
      }
      /// 主键自动建的唯一索引
      for (const IndexMeta &index : create.primary_index()) {
        RC index_rc = db_->create_index(create.table_meta().table_name(), index);
        if (OB_FAIL(index_rc)) {
          LOG_WARN("table created but failed to create primary key index. rc=%s", strrc(index_rc));
        }
      }
      result.message = "created table " + create.table_meta().table_name();
      break;
    }

    case Stmt::Kind::DROP_TABLE: {
      DropTableStmt &drop = *static_cast<DropTableStmt *>(stmt.get());
      if (db_->find_table_or_null(drop.table_name()) == nullptr) {
        if (drop.ignore_missing()) {
          result.message = "table not found, ignored by 'if exists'";
          break;
        }
        result.code    = RC::SCHEMA_TABLE_NOT_EXIST;
        result.message = "table not found: " + drop.table_name();
        break;
      }
      rc = db_->drop_table(drop.table_name());
      result.code    = rc;
      result.message = OB_SUCC(rc) ? "dropped table " + drop.table_name() : std::string("failed to drop table: ") + strrc(rc);
      break;
    }

    case Stmt::Kind::CREATE_INDEX: {
      CreateIndexStmt &create = *static_cast<CreateIndexStmt *>(stmt.get());
      rc                      = db_->create_index(create.table()->table_name(), create.index_meta());
      result.code             = rc;
      result.message = OB_SUCC(rc) ? "created index " + create.index_meta().index_name
                                   : std::string("failed to create index: ") + strrc(rc);
      break;
    }

    case Stmt::Kind::SHOW_TABLES: {
      ShowTablesStmt &show = *static_cast<ShowTablesStmt *>(stmt.get());
      auto list = std::make_unique<StringListOperator>();
      for (const std::string &name : show.table_names()) {
        list->add_row(std::vector<Value>{Value(name)});
      }
      result.is_query       = true;
      result.column_names   = {"table_name"};
      result.execution_time_us = elapsed_us(start);
      return run_plan(*list, result, writer);
    }

    case Stmt::Kind::DESC_TABLE: {
      DescTableStmt &desc = *static_cast<DescTableStmt *>(stmt.get());
      auto           list = std::make_unique<StringListOperator>();
      build_desc_rows(*desc.table_meta(), *list);
      result.is_query       = true;
      result.column_names   = {"field", "type", "length", "index"};
      result.execution_time_us = elapsed_us(start);
      return run_plan(*list, result, writer);
    }

    case Stmt::Kind::EXPLAIN: {
      std::string plan_text;
      rc = explain(parse_result->explain.original_sql, plan_text);
      if (OB_FAIL(rc)) {
        result.code    = rc;
        result.message = "failed to explain the statement";
        break;
      }
      auto list = std::make_unique<StringListOperator>();
      std::istringstream in(plan_text);
      std::string        line;
      while (std::getline(in, line)) {
        if (!line.empty()) {
          list->add_row(std::vector<Value>{Value(line)});
        }
      }
      result.is_query       = true;
      result.column_names   = {"plan"};
      result.execution_time_us = elapsed_us(start);
      return run_plan(*list, result, writer);
    }

    default: {
      /// select / insert / delete / update：走完整的计划生成与执行流程
      LogicalPlan plan;
      rc = LogicalPlanGenerator().create(*stmt, plan);
      if (OB_FAIL(rc)) {
        result.code    = rc;
        result.message = "failed to build the logical plan";
        break;
      }

      Optimizer optimizer(plan);
      rc = optimizer.optimize();
      if (OB_FAIL(rc)) {
        result.code    = rc;
        result.message = "failed to optimize the plan";
        break;
      }

      std::unique_ptr<PhysicalOperator> physical;
      PhysicalPlanGenerator             physical_generator;
      rc = physical_generator.create(*plan.root, physical);
      if (OB_FAIL(rc)) {
        result.code    = rc;
        result.message = "failed to build the physical plan";
        break;
      }

      if (stmt->kind() == Stmt::Kind::SELECT) {
        SelectStmt           &select  = *stmt->select_stmt();
        result.is_query               = true;
        for (const auto &expr : select.select_list()) {
          result.column_names.push_back(expr->name());
        }
      }

      rc = run_plan(*physical, result, writer);
      if (OB_SUCC(rc) && stmt->kind() != Stmt::Kind::SELECT) {
        /// DML 之后把脏页写回磁盘，本实现没有 WAL，落盘就是它的“提交”
        RC flush_rc = db_->flush();
        if (OB_FAIL(flush_rc)) {
          LOG_WARN("failed to flush after DML. rc=%s", strrc(flush_rc));
        }
      }
      result.code = rc;
      break;
    }
  }

  result.execution_time_us = elapsed_us(start);
  return result.code;
}

RC QueryEngine::explain(const std::string &sql, std::string &plan_text)
{
  std::unique_ptr<ParseResult> parse_result;
  Parser                       parser;
  RC                           rc = parser.parse_statement(sql, parse_result);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (parse_result->stmt_type != ParseResult::StmtType::SELECT) {
    return RC::UNSUPPORTED;
  }

  std::unique_ptr<Stmt> stmt;
  rc = Stmt::create_stmt(*parse_result, db_, stmt);
  if (OB_FAIL(rc)) {
    return rc;
  }

  LogicalPlan plan;
  rc = LogicalPlanGenerator().create(*stmt, plan);
  if (OB_FAIL(rc)) {
    return rc;
  }

  Optimizer optimizer(plan);
  rc = optimizer.optimize();
  if (OB_FAIL(rc)) {
    return rc;
  }

  std::unique_ptr<PhysicalOperator> physical;
  PhysicalPlanGenerator             physical_generator;
  rc = physical_generator.create(*plan.root, physical);
  if (OB_FAIL(rc)) {
    return rc;
  }

  std::ostringstream oss;
  oss << "== logical plan (after optimization) ==\n";
  oss << plan.to_string();
  oss << "== physical plan ==\n";
  physical->explain(oss);
  plan_text = oss.str();
  return RC::SUCCESS;
}
