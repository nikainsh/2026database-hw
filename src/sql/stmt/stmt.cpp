/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/stmt/stmt.h"

#include "common/log.h"
#include "common/string_util.h"
#include <algorithm>

const char *Stmt::name() const
{
  switch (kind_) {
    case Kind::CREATE_TABLE: return "create table";
    case Kind::DROP_TABLE: return "drop table";
    case Kind::CREATE_INDEX: return "create index";
    case Kind::SHOW_TABLES: return "show tables";
    case Kind::DESC_TABLE: return "desc table";
    case Kind::INSERT: return "insert";
    case Kind::DELETE_STMT: return "delete";
    case Kind::UPDATE: return "update";
    case Kind::SELECT: return "select";
    case Kind::EXPLAIN: return "explain";
    case Kind::EXIT: return "exit";
    case Kind::HELP: return "help";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// DDL
// ---------------------------------------------------------------------------

RC CreateTableStmt::prepare()
{
  std::vector<ColumnMeta> columns;
  for (const AttrInfoSqlNode &attr : node_.attr_infos) {
    ColumnMeta column;
    RC         rc = column.init(attr.name, attr.type, static_cast<int>(attr.length), 0);
    if (OB_FAIL(rc)) {
      LOG_WARN("invalid column definition. column=%s, rc=%s", attr.name.c_str(), strrc(rc));
      return RC::SQL_SYNTAX;
    }
    columns.push_back(column);
  }

  RC rc = table_meta_.init(node_.relation_name, columns);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (db_->find_table_or_null(table_meta_.table_name()) != nullptr) {
    if (node_.if_not_exists) {
      return RC::SUCCESS;  /// 已存在且允许忽略：当作成功，但不建表
    }
    LOG_WARN("table already exists. table=%s", table_meta_.table_name().c_str());
    return RC::SCHEMA_TABLE_EXIST;
  }

  /// 单列主键自动建唯一索引（多列主键本实现不支持）
  if (node_.primary_keys.size() == 1) {
    const std::string &key = node_.primary_keys[0];
    if (table_meta_.column(key) == nullptr) {
      LOG_WARN("primary key column not found. table=%s, column=%s", table_meta_.table_name().c_str(), key.c_str());
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }
    IndexMeta meta;
    meta.index_name = table_meta_.table_name() + "_" + key + "_pk";
    meta.field_name = key;
    meta.unique     = true;
    primary_index_.push_back(meta);
  } else if (node_.primary_keys.size() > 1) {
    LOG_WARN("multi-column primary key is not supported, ignored");
  }
  return RC::SUCCESS;
}

RC DropTableStmt::prepare()
{
  if (node_.relation_name.empty()) {
    return RC::SQL_SYNTAX;
  }
  if (db_->find_table_or_null(node_.relation_name) == nullptr && !node_.if_exists) {
    LOG_WARN("table not found. table=%s", node_.relation_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }
  return RC::SUCCESS;
}

RC CreateIndexStmt::prepare()
{
  RC rc = db_->find_table(node_.relation_name, &table_);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (table_->table_meta().column(node_.attribute_name) == nullptr) {
    LOG_WARN("index column not found. table=%s, column=%s",
             node_.relation_name.c_str(), node_.attribute_name.c_str());
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }
  if (table_->index_by_name(node_.index_name) != nullptr) {
    LOG_WARN("index name already used. index=%s", node_.index_name.c_str());
    return RC::SCHEMA_INDEX_NAME_REPEAT;
  }
  if (table_->index_of_field(node_.attribute_name) != nullptr) {
    LOG_WARN("the column already has an index. table=%s, column=%s",
             node_.relation_name.c_str(), node_.attribute_name.c_str());
    return RC::SCHEMA_INDEX_NAME_REPEAT;
  }

  const AttrType type = table_->table_meta().column(node_.attribute_name)->type();
  if (type != AttrType::INT && type != AttrType::BIGINT && type != AttrType::FLOAT && type != AttrType::CHARS) {
    LOG_WARN("cannot index a column of type %s", attr_type_to_string(type));
    return RC::UNSUPPORTED;
  }

  index_meta_.index_name = node_.index_name;
  index_meta_.field_name = node_.attribute_name;
  index_meta_.unique     = node_.unique;
  return RC::SUCCESS;
}

RC ShowTablesStmt::prepare()
{
  names_.clear();
  for (const auto &table : db_->tables()) {
    names_.push_back(table->table_name());
  }
  return RC::SUCCESS;
}

RC DescTableStmt::prepare()
{
  Table *table = nullptr;
  RC     rc    = db_->find_table(table_name_, &table);
  if (OB_FAIL(rc)) {
    return rc;
  }
  meta_ = &table->table_meta();
  return RC::SUCCESS;
}

// ---------------------------------------------------------------------------
// DML
// ---------------------------------------------------------------------------

/// 某一列的默认值（本实现没有 NULL，也没有列默认值语法）
static Value default_value_of(const ColumnMeta &column)
{
  switch (column.type()) {
    case AttrType::INT: return Value(static_cast<int32_t>(0));
    case AttrType::BIGINT: return Value(static_cast<int64_t>(0));
    case AttrType::FLOAT: return Value(0.0f);
    default: return Value(std::string());
  }
}

RC InsertStmt::prepare()
{
  RC rc = db_->find_table(node_.relation_name, &table_);
  if (OB_FAIL(rc)) {
    return rc;
  }

  const TableMeta &meta = table_->table_meta();
  rows_.clear();

  /// 没写列名时按表的列顺序给值；写了列名则只给这些列，其余列用默认值
  std::vector<int> column_index_of_value;  /// values[i] 对应第几列
  if (node_.columns.empty()) {
    for (size_t i = 0; i < meta.columns().size(); ++i) {
      column_index_of_value.push_back(static_cast<int>(i));
    }
  } else {
    for (const std::string &name : node_.columns) {
      const int index = meta.column_index(name);
      if (index < 0) {
        LOG_WARN("unknown column in insert. table=%s, column=%s", meta.table_name().c_str(), name.c_str());
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }
      column_index_of_value.push_back(index);
    }
  }

  for (const std::vector<Value> &row : node_.values_list) {
    if (row.size() != column_index_of_value.size()) {
      LOG_WARN("value count mismatch. table=%s, values=%d, columns=%d",
               meta.table_name().c_str(), (int)row.size(), (int)column_index_of_value.size());
      return RC::SCHEMA_FIELD_MISSING;
    }

    std::vector<Value> full;
    for (size_t i = 0; i < meta.columns().size(); ++i) {
      full.push_back(default_value_of(meta.columns()[i]));
    }
    for (size_t i = 0; i < row.size(); ++i) {
      full[column_index_of_value[i]] = row[i];
    }
    rows_.push_back(std::move(full));
  }

  if (rows_.empty()) {
    return RC::INVALID_ARGUMENT;
  }
  return RC::SUCCESS;
}

RC DeleteStmt::prepare()
{
  RC rc = db_->find_table(node_.relation_name, &table_);
  if (OB_FAIL(rc)) {
    return rc;
  }

  context_.add_table(table_->table_name(), table_->table_name(), &table_->table_meta());
  filter_ = std::move(node_.filter);
  if (filter_ != nullptr) {
    rc = filter_->bind(context_);
    if (OB_FAIL(rc)) {
      filter_.reset();
      return rc;
    }
    if (filter_->result_type() != AttrType::BOOLEAN) {
      LOG_WARN("the where condition must be a boolean comparison");
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }
  return RC::SUCCESS;
}

RC UpdateStmt::prepare()
{
  RC rc = db_->find_table(node_.relation_name, &table_);
  if (OB_FAIL(rc)) {
    return rc;
  }

  const TableMeta &meta = table_->table_meta();
  assignments_.clear();
  for (const auto &item : node_.sets) {
    const ColumnMeta *column = meta.column(item.first);
    if (column == nullptr) {
      LOG_WARN("unknown column in update. table=%s, column=%s", meta.table_name().c_str(), item.first.c_str());
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }
    Value casted;
    rc = item.second.cast_to(column->type(), casted);
    if (OB_FAIL(rc)) {
      LOG_WARN("cannot assign '%s' to column %s", item.second.to_string().c_str(), column->name().c_str());
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
    if (column->type() == AttrType::CHARS
        && static_cast<int>(casted.get_string().size()) > column->data_len()) {
      LOG_WARN("value too long. column=%s, max=%d", column->name().c_str(), column->data_len());
      return RC::IOERR_TOO_LONG;
    }
    assignments_.emplace_back(meta.column_index(column->name()), casted);
  }

  BindContext context;
  context.add_table(meta.table_name(), meta.table_name(), &meta);
  filter_ = std::move(node_.filter);
  if (filter_ != nullptr) {
    rc = filter_->bind(context);
    if (OB_FAIL(rc)) {
      filter_.reset();
      return rc;
    }
  }
  return RC::SUCCESS;
}

RC SelectStmt::bind_expression(Expression *expr)
{
  return expr->bind(context_);
}

RC SelectStmt::prepare()
{
  tables_.clear();
  display_names_.clear();

  for (size_t i = 0; i < node_.relations.size(); ++i) {
    Table *table = nullptr;
    RC     rc    = db_->find_table(node_.relations[i], &table);
    if (OB_FAIL(rc)) {
      LOG_WARN("unknown table in from. table=%s", node_.relations[i].c_str());
      return rc;
    }
    const std::string &alias = node_.aliases[i];
    std::string        display = alias.empty() ? table->table_name() : alias;
    if (std::find(display_names_.begin(), display_names_.end(), display) != display_names_.end()) {
      LOG_WARN("duplicated table name or alias in from. name=%s", display.c_str());
      return RC::INVALID_ARGUMENT;
    }
    tables_.push_back(table);
    display_names_.push_back(display);
    context_.add_table(display, table->table_name(), &table->table_meta());
  }

  if (tables_.empty()) {
    LOG_WARN("select needs at least one table in from");
    return RC::SQL_SYNTAX;
  }

  /// select *：展开成所有表的所有列，后面的算子就不用再关心 star 了
  if (node_.is_star) {
    for (const std::string &display : display_names_) {
      const BindContext::TableRef *ref = context_.find_table(display);
      for (const ColumnMeta &column : ref->meta->columns()) {
        auto expr = make_column_expr(display, column.name());
        RC   rc   = bind_expression(expr.get());
        if (OB_FAIL(rc)) {
          return rc;
        }
        select_list_.push_back(std::move(expr));
      }
    }
  } else {
    select_list_ = std::move(node_.expressions);
    for (auto &expr : select_list_) {
      RC rc = bind_expression(expr.get());
      if (OB_FAIL(rc)) {
        return rc;
      }
      has_aggregate_ = has_aggregate_ || expr->has_aggregate();
    }
  }

  group_by_ = std::move(node_.group_by);
  for (auto &expr : group_by_) {
    RC rc = bind_expression(expr.get());
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (expr->expr_type() != ExprType::COLUMN) {
      LOG_WARN("group by only supports plain columns");
      return RC::UNSUPPORTED;
    }
  }

  /// 有 group by 时，select 里的非聚合项必须是分组列；没有 group by 时不允许混合聚合与普通列
  if (!group_by_.empty()) {
    for (auto &expr : select_list_) {
      if (expr->has_aggregate()) {
        continue;
      }
      if (expr->expr_type() != ExprType::COLUMN) {
        LOG_WARN("select item must be an aggregate or a group by column");
        return RC::UNSUPPORTED;
      }
      const ColumnExpression *column = static_cast<const ColumnExpression *>(expr.get());
      bool                    grouped = false;
      for (const auto &group : group_by_) {
        const ColumnExpression *key = static_cast<const ColumnExpression *>(group.get());
        if (key->table_index() == column->table_index() && key->column_index() == column->column_index()) {
          grouped = true;
          break;
        }
      }
      if (!grouped) {
        LOG_WARN("column '%s' must appear in group by", column->name().c_str());
        return RC::UNSUPPORTED;
      }
    }
  } else if (has_aggregate_) {
    for (const auto &expr : select_list_) {
      if (!expr->has_aggregate()) {
        LOG_WARN("cannot mix bare columns with aggregates without group by: '%s'", expr->name().c_str());
        return RC::UNSUPPORTED;
      }
    }
  }

  filter_ = std::move(node_.filter);
  if (filter_ != nullptr) {
    RC rc = bind_expression(filter_.get());
    if (OB_FAIL(rc)) {
      filter_.reset();
      return rc;
    }
    if (filter_->result_type() != AttrType::BOOLEAN) {
      LOG_WARN("the where condition must be a boolean comparison");
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }
  return RC::SUCCESS;
}

RC ExplainStmt::prepare() { return child_stmt_->prepare(); }

// ---------------------------------------------------------------------------
// 工厂
// ---------------------------------------------------------------------------

RC Stmt::create_stmt(ParseResult &parse_result, Db *db, std::unique_ptr<Stmt> &stmt)
{
  if (db == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  switch (parse_result.stmt_type) {
    case ParseResult::StmtType::CREATE_TABLE:
      stmt = std::make_unique<CreateTableStmt>(std::move(parse_result.create_table), db);
      break;
    case ParseResult::StmtType::DROP_TABLE:
      stmt = std::make_unique<DropTableStmt>(std::move(parse_result.drop_table), db);
      break;
    case ParseResult::StmtType::CREATE_INDEX:
      stmt = std::make_unique<CreateIndexStmt>(std::move(parse_result.create_index), db);
      break;
    case ParseResult::StmtType::SHOW_TABLES:
      stmt = std::make_unique<ShowTablesStmt>(db);
      break;
    case ParseResult::StmtType::DESC_TABLE:
      stmt = std::make_unique<DescTableStmt>(parse_result.desc_table.relation_name, db);
      break;
    case ParseResult::StmtType::INSERT:
      stmt = std::make_unique<InsertStmt>(std::move(parse_result.insert), db);
      break;
    case ParseResult::StmtType::DELETE_STMT:
      stmt = std::make_unique<DeleteStmt>(std::move(parse_result.delete_stmt), db);
      break;
    case ParseResult::StmtType::UPDATE:
      stmt = std::make_unique<UpdateStmt>(std::move(parse_result.update), db);
      break;
    case ParseResult::StmtType::SELECT:
      stmt = std::make_unique<SelectStmt>(std::move(parse_result.select), db);
      break;
    case ParseResult::StmtType::EXIT:
      stmt = std::make_unique<ExitStmt>();
      break;
    case ParseResult::StmtType::HELP:
      stmt = std::make_unique<HelpStmt>();
      break;
    case ParseResult::StmtType::EXPLAIN: {
      if (parse_result.explain.child_parse == nullptr) {
        return RC::SQL_SYNTAX;
      }
      std::unique_ptr<Stmt> child;
      RC                    rc = create_stmt(*parse_result.explain.child_parse, db, child);
      if (OB_FAIL(rc)) {
        return rc;
      }
      stmt = std::make_unique<ExplainStmt>(std::move(child));
      break;
    }
    default: {
      LOG_WARN("unsupported statement type");
      return RC::UNSUPPORTED;
    }
  }

  RC rc = stmt->prepare();
  if (OB_FAIL(rc)) {
    stmt.reset();
    return rc;
  }
  return RC::SUCCESS;
}
