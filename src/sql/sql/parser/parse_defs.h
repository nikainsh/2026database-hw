/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/rc.h"
#include "common/value.h"

/// @defgroup SQLParser
/// @brief 语法树的节点定义（命名与 miniob 的 parse_defs.h 保持一致）

class Expression;
struct ParseResult;

/// 比较运算符
enum class CompOp
{
  EQUAL_TO,
  NOT_EQUAL,
  LESS_THAN,
  LESS_EQUAL,
  GREAT_THAN,
  GREAT_EQUAL,
  NO_OP,
};

inline const char *comp_op_to_string(CompOp op)
{
  switch (op) {
    case CompOp::EQUAL_TO: return "=";
    case CompOp::NOT_EQUAL: return "!=";
    case CompOp::LESS_THAN: return "<";
    case CompOp::LESS_EQUAL: return "<=";
    case CompOp::GREAT_THAN: return ">";
    case CompOp::GREAT_EQUAL: return ">=";
    default: return "?";
  }
}

/// 带表名的列引用：select t.a 里的 t 和 a
struct RelAttrSqlNode
{
  std::string relation_name;  /// 可以为空
  std::string attribute_name;
};

/// 一列的类型定义
struct AttrInfoSqlNode
{
  AttrType    type = AttrType::UNDEFINED;
  std::string name;
  size_t      length = 0;
};

/// 查询里的一个目标：列、常量、聚合函数等，解析阶段就是表达式树
struct SelectSqlNode
{
  std::vector<std::unique_ptr<Expression>> expressions;  /// select 列表
  std::vector<std::string>                 relations;    /// from 的表名
  std::vector<std::string>                 aliases;      /// 与 relations 平行，可为空
  std::unique_ptr<Expression>               filter;        /// where 条件
  std::vector<std::unique_ptr<Expression>>  group_by;      /// group by 列表
  bool                                       is_star = false;  /// select *
};

struct InsertSqlNode
{
  std::string                             relation_name;
  std::vector<std::string>                columns;    /// 指定要插入的列
  std::vector<std::vector<Value>>         values_list; /// 多组值
};

struct DeleteSqlNode
{
  std::string                  relation_name;
  std::unique_ptr<Expression>  filter;
};

struct UpdateSqlNode
{
  std::string                             relation_name;
  std::vector<std::pair<std::string, Value>> sets;  /// 列 = 值
  std::unique_ptr<Expression>              filter;
};

struct CreateTableSqlNode
{
  std::string                  relation_name;
  std::vector<AttrInfoSqlNode> attr_infos;
  std::vector<std::string>     primary_keys;
  bool                         if_not_exists = false;
};

struct DropTableSqlNode
{
  std::string relation_name;
  bool        if_exists = false;
};

struct CreateIndexSqlNode
{
  std::string index_name;
  std::string relation_name;
  std::string attribute_name;
  bool        unique = false;
};

struct DescTableSqlNode
{
  std::string relation_name;
};

struct ExplainSqlNode
{
  std::unique_ptr<ParseResult> child_parse;  /// 被解释的语句（本实现支持 explain select）
  std::string                  original_sql;
};

/// 一条语句解析后的结果
struct ParseResult
{
  enum class StmtType
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

  explicit ParseResult(StmtType type) : stmt_type(type) {}
  /// unique_ptr<Stmt> 需要 Stmt 的完整定义，因此析构函数在 parser.cpp 里实现
  ~ParseResult();

  StmtType    stmt_type = StmtType::SELECT;
  std::string original_sql;

  CreateTableSqlNode   create_table;
  DropTableSqlNode     drop_table;
  CreateIndexSqlNode   create_index;
  DescTableSqlNode     desc_table;
  InsertSqlNode        insert;
  DeleteSqlNode        delete_stmt;
  UpdateSqlNode        update;
  SelectSqlNode        select;
  ExplainSqlNode       explain;
};
