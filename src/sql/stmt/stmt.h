/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/rc.h"
#include "common/value.h"
#include "sql/expr/bind_context.h"
#include "sql/expr/expression.h"
#include "sql/parser/parse_defs.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"

class Db;

/**
 * @brief 语义检查后的语句，对应 miniob 的 Stmt
 * @details 解析器只关心理法结构；Stmt 阶段负责把名字换成真实的表/列，
 *          并把类型不匹配、表不存在等语义错误尽早报出来（prepare）。
 */
class Stmt
{
public:
  enum class Kind
  {
    CREATE_TABLE,
    DROP_TABLE,
    CREATE_INDEX,
    SHOW_TABLES,
    DESC_TABLE,
    INSERT,
    DELETE_STMT,
    UPDATE,
    SELECT,
    EXPLAIN,
    EXIT,
    HELP,
  };

  virtual ~Stmt() = default;

  Kind        kind() const { return kind_; }
  const char *name() const;

  /// 由解析结果构造并 prepare 出一条语句
  /// SqlNode 里带着表达式树的所有权，所以这里用非 const 引用并把内容移动进 Stmt
  static RC create_stmt(ParseResult &parse_result, Db *db, std::unique_ptr<Stmt> &stmt);

  /// 语义检查
  virtual RC prepare() { return RC::SUCCESS; }

  /// SELECT 语句才有内容；其它语句返回 nullptr
  virtual class SelectStmt *select_stmt() { return nullptr; }

protected:
  explicit Stmt(Kind kind) : kind_(kind) {}

  Kind kind_;
};

/// create table
class CreateTableStmt : public Stmt
{
public:
  CreateTableStmt(CreateTableSqlNode node, Db *db) : Stmt(Kind::CREATE_TABLE), node_(std::move(node)), db_(db) {}

  RC prepare() override;

  const TableMeta  &table_meta() const { return table_meta_; }
  const std::vector<IndexMeta> &primary_index() const { return primary_index_; }
  bool               if_not_exists() const { return node_.if_not_exists; }

private:
  CreateTableSqlNode      node_;
  Db                     *db_           = nullptr;
  TableMeta               table_meta_;
  std::vector<IndexMeta>  primary_index_;  /// 单列主键会自动建一个唯一索引
};

/// drop table
class DropTableStmt : public Stmt
{
public:
  DropTableStmt(DropTableSqlNode node, Db *db) : Stmt(Kind::DROP_TABLE), node_(std::move(node)), db_(db) {}

  RC prepare() override;

  const std::string &table_name() const { return node_.relation_name; }
  bool               ignore_missing() const { return node_.if_exists; }

private:
  DropTableSqlNode node_;
  Db              *db_ = nullptr;
};

/// create index
class CreateIndexStmt : public Stmt
{
public:
  CreateIndexStmt(CreateIndexSqlNode node, Db *db) : Stmt(Kind::CREATE_INDEX), node_(std::move(node)), db_(db) {}

  RC prepare() override;

  Table            *table() const { return table_; }
  const IndexMeta  &index_meta() const { return index_meta_; }

private:
  CreateIndexSqlNode node_;
  Db                *db_   = nullptr;
  Table             *table_ = nullptr;
  IndexMeta          index_meta_;
};

/// show tables
class ShowTablesStmt : public Stmt
{
public:
  explicit ShowTablesStmt(Db *db) : Stmt(Kind::SHOW_TABLES), db_(db) {}
  const std::vector<std::string> &table_names() const { return names_; }
  RC prepare() override;

private:
  Db                *db_ = nullptr;
  std::vector<std::string> names_;
};

/// desc table
class DescTableStmt : public Stmt
{
public:
  DescTableStmt(const std::string &table_name, Db *db) : Stmt(Kind::DESC_TABLE), table_name_(table_name), db_(db) {}

  RC prepare() override;

  const TableMeta *table_meta() const { return meta_; }

private:
  std::string        table_name_;
  Db                *db_   = nullptr;
  const TableMeta   *meta_ = nullptr;
};

/// insert into ... values ...
class InsertStmt : public Stmt
{
public:
  InsertStmt(InsertSqlNode node, Db *db) : Stmt(Kind::INSERT), node_(std::move(node)), db_(db) {}

  RC prepare() override;

  Table *table() const { return table_; }
  /// 已经按表的列顺序补齐好的值，一组对应一行
  const std::vector<std::vector<Value>> &values_list() const { return rows_; }

private:
  InsertSqlNode                 node_;
  Db                           *db_ = nullptr;
  Table                        *table_ = nullptr;
  std::vector<std::vector<Value>> rows_;
};

/// delete from ... where ...
class DeleteStmt : public Stmt
{
public:
  DeleteStmt(DeleteSqlNode node, Db *db) : Stmt(Kind::DELETE_STMT), node_(std::move(node)), db_(db) {}

  RC prepare() override;

  Table      *table() const { return table_; }
  Expression *filter() const { return filter_.get(); }
  const BindContext &bind_context() const { return context_; }

private:
  DeleteSqlNode node_;
  Db           *db_      = nullptr;
  Table        *table_   = nullptr;
  std::unique_ptr<Expression> filter_;
  BindContext   context_;
};

/// update ... set ... where ...
class UpdateStmt : public Stmt
{
public:
  UpdateStmt(UpdateSqlNode node, Db *db) : Stmt(Kind::UPDATE), node_(std::move(node)), db_(db) {}

  RC prepare() override;

  Table *table() const { return table_; }
  /// 要更新的列（下标）与新值
  const std::vector<std::pair<int, Value>> &assignments() const { return assignments_; }
  Expression *filter() const { return filter_.get(); }

private:
  UpdateSqlNode node_;
  Db           *db_ = nullptr;
  Table        *table_ = nullptr;
  std::vector<std::pair<int, Value>> assignments_;
  std::unique_ptr<Expression>        filter_;
};

/**
 * @brief select 语句：本实现里最复杂的一类
 * @details 记录 from 的表（含别名）、已经绑定好的表达式、where 与 group by。
 */
class SelectStmt : public Stmt
{
public:
  SelectStmt(SelectSqlNode node, Db *db) : Stmt(Kind::SELECT), node_(std::move(node)), db_(db) {}

  RC prepare() override;

  class SelectStmt *select_stmt() override { return this; }

  const std::vector<Table *>   &tables() const { return tables_; }
  const std::vector<std::string> &table_names() const { return display_names_; }
  const BindContext              &bind_context() const { return context_; }

  /// select 列表（'select *' 已经展开成所有列）
  const std::vector<std::unique_ptr<Expression>> &select_list() const { return select_list_; }
  Expression                                    *filter() const { return filter_.get(); }
  const std::vector<std::unique_ptr<Expression>> &group_by() const { return group_by_; }
  bool                             has_aggregate() const { return has_aggregate_; }

private:
  RC bind_expression(Expression *expr);

  SelectSqlNode node_;
  Db           *db_ = nullptr;

  std::vector<Table *>                  tables_;
  std::vector<std::string>              display_names_;
  BindContext                           context_;
  std::vector<std::unique_ptr<Expression>> select_list_;
  std::unique_ptr<Expression>              filter_;
  std::vector<std::unique_ptr<Expression>> group_by_;
  bool                                     has_aggregate_ = false;
};

/// explain select ...
class ExplainStmt : public Stmt
{
public:
  ExplainStmt(std::unique_ptr<Stmt> child_stmt) : Stmt(Kind::EXPLAIN), child_stmt_(std::move(child_stmt)) {}

  RC prepare() override;

  Stmt *child_stmt() const { return child_stmt_.get(); }

private:
  std::unique_ptr<Stmt> child_stmt_;
};

/// exit / help：由执行层直接处理，不产生计划
class ExitStmt : public Stmt
{
public:
  ExitStmt() : Stmt(Kind::EXIT) {}
};

class HelpStmt : public Stmt
{
public:
  HelpStmt() : Stmt(Kind::HELP) {}
};
