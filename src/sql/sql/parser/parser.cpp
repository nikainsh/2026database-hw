/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/parser/parser.h"

#include <cctype>

#include "common/log.h"
#include "common/string_util.h"
#include "sql/expr/expression.h"
#include "sql/stmt/stmt.h"

ParseResult::~ParseResult() = default;

static bool token_is_keyword(const Token &token, const char *keyword)
{
  return token.kind == TokenKind::IDENT && to_lower(token.text) == keyword;
}

// ---------------------------------------------------------------------------
// token 流
// ---------------------------------------------------------------------------

RC Parser::ensure_lookahead()
{
  if (has_lookahead_) {
    return RC::SUCCESS;
  }
  RC rc = lexer_.next_token(lookahead_);
  if (OB_FAIL(rc)) {
    return rc;
  }
  has_lookahead_ = true;
  return RC::SUCCESS;
}

const Token &Parser::peek() { return lookahead_; }

void Parser::consume() { has_lookahead_ = false; }

bool Parser::at_end()
{
  if (OB_FAIL(ensure_lookahead())) {
    return true;
  }
  return lookahead_.kind == TokenKind::END;
}

bool Parser::peek_keyword(const char *keyword)
{
  if (OB_FAIL(ensure_lookahead())) {
    return false;
  }
  return token_is_keyword(lookahead_, keyword);
}

bool Parser::accept_keyword(const char *keyword)
{
  if (!peek_keyword(keyword)) {
    return false;
  }
  consume();
  return true;
}

bool Parser::peek_punct(const char *text)
{
  if (OB_FAIL(ensure_lookahead())) {
    return false;
  }
  return lookahead_.kind == TokenKind::PUNCT && lookahead_.text == text;
}

bool Parser::accept_punct(const char *text)
{
  if (!peek_punct(text)) {
    return false;
  }
  consume();
  return true;
}

RC Parser::expect_identifier(std::string &name)
{
  RC rc = ensure_lookahead();
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (lookahead_.kind != TokenKind::IDENT) {
    LOG_WARN("expect an identifier but got '%s'", lookahead_.text.c_str());
    return RC::SQL_SYNTAX;
  }
  name = lookahead_.text;
  consume();
  return RC::SUCCESS;
}

RC Parser::expect_literal_value(Value &value)
{
  RC rc = ensure_lookahead();
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (lookahead_.kind == TokenKind::NUMBER) {
    value = lookahead_.number;
    consume();
    return RC::SUCCESS;
  }
  if (lookahead_.kind == TokenKind::STRING) {
    value = Value(lookahead_.text);
    consume();
    return RC::SUCCESS;
  }
  LOG_WARN("expect a literal value but got '%s'", lookahead_.text.c_str());
  return RC::SQL_SYNTAX;
}

/// 把无法识别的语句尾部（例如 engine=heap; charset=utf8）吃掉，保持兼容性
RC Parser::parse_ignore_tokens()
{
  while (!at_end()) {
    consume();
    if (peek_punct(";")) {
      consume();
    }
  }
  return RC::SUCCESS;
}

// ---------------------------------------------------------------------------
// 表达式
// ---------------------------------------------------------------------------

RC Parser::parse_condition(std::unique_ptr<Expression> &expr)
{
  return parse_or_expr(expr);
}

RC Parser::parse_or_expr(std::unique_ptr<Expression> &expr)
{
  std::vector<std::unique_ptr<Expression>> parts;
  std::unique_ptr<Expression>              first;
  RC                                       rc = parse_and_expr(first);
  if (OB_FAIL(rc)) {
    return rc;
  }
  parts.push_back(std::move(first));

  while (peek_keyword("or")) {
    consume();
    std::unique_ptr<Expression> next;
    rc = parse_and_expr(next);
    if (OB_FAIL(rc)) {
      return rc;
    }
    parts.push_back(std::move(next));
  }

  if (parts.size() == 1) {
    expr = std::move(parts.front());
  } else {
    expr = std::make_unique<ConjunctionExpression>(std::move(parts), false);
  }
  return RC::SUCCESS;
}

RC Parser::parse_and_expr(std::unique_ptr<Expression> &expr)
{
  std::vector<std::unique_ptr<Expression>> parts;
  std::unique_ptr<Expression>              first;
  RC                                       rc = parse_comparison_expr(first);
  if (OB_FAIL(rc)) {
    return rc;
  }
  parts.push_back(std::move(first));

  while (peek_keyword("and")) {
    consume();
    std::unique_ptr<Expression> next;
    rc = parse_comparison_expr(next);
    if (OB_FAIL(rc)) {
      return rc;
    }
    parts.push_back(std::move(next));
  }

  if (parts.size() == 1) {
    expr = std::move(parts.front());
  } else {
    expr = std::make_unique<ConjunctionExpression>(std::move(parts), true);
  }
  return RC::SUCCESS;
}

static bool parse_comp_op(const Token &token, CompOp &op)
{
  if (token.kind != TokenKind::PUNCT) {
    return false;
  }
  if (token.text == "=") { op = CompOp::EQUAL_TO; return true; }
  if (token.text == "!=" || token.text == "<>") { op = CompOp::NOT_EQUAL; return true; }
  if (token.text == "<") { op = CompOp::LESS_THAN; return true; }
  if (token.text == "<=") { op = CompOp::LESS_EQUAL; return true; }
  if (token.text == ">") { op = CompOp::GREAT_THAN; return true; }
  if (token.text == ">=") { op = CompOp::GREAT_EQUAL; return true; }
  return false;
}

RC Parser::parse_comparison_expr(std::unique_ptr<Expression> &expr)
{
  if (peek_punct("(")) {
    consume();
    RC rc = parse_or_expr(expr);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (!accept_punct(")")) {
      LOG_WARN("expect ')' to close a condition");
      return RC::SQL_SYNTAX;
    }
    return RC::SUCCESS;
  }

  std::unique_ptr<Expression> left;
  RC                          rc = parse_primary_expr(left);
  if (OB_FAIL(rc)) {
    return rc;
  }

  RC    lookahead_rc = ensure_lookahead();
  CompOp op          = CompOp::NO_OP;
  if (lookahead_rc == RC::SUCCESS && parse_comp_op(lookahead_, op)) {
    consume();
    std::unique_ptr<Expression> right;
    rc = parse_primary_expr(right);
    if (OB_FAIL(rc)) {
      return rc;
    }
    expr = std::make_unique<ComparisonExpression>(std::move(left), op, std::move(right));
  } else {
    expr = std::move(left);
  }
  return RC::SUCCESS;
}

RC Parser::parse_primary_expr(std::unique_ptr<Expression> &expr)
{
  RC rc = ensure_lookahead();
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (lookahead_.kind == TokenKind::NUMBER || lookahead_.kind == TokenKind::STRING) {
    Value value;
    rc = expect_literal_value(value);
    if (OB_FAIL(rc)) {
      return rc;
    }
    expr = make_const_expr(value);
    return RC::SUCCESS;
  }

  if (lookahead_.kind != TokenKind::IDENT) {
    LOG_WARN("expect a column or a literal but got '%s'", lookahead_.text.c_str());
    return RC::SQL_SYNTAX;
  }

  const std::string first = lookahead_.text;
  const std::string lower = to_lower(first);

  /// 聚合函数：count(*) / sum(col) / min(col) ...
  if (agg_func_from_string(lower) != AggFunc::NONE) {
    consume();
    if (!accept_punct("(")) {
      return RC::SQL_SYNTAX;
    }

    AggFunc func = agg_func_from_string(lower);
    std::unique_ptr<Expression> arg;
    if (accept_punct("*")) {
      if (func != AggFunc::COUNT) {
        LOG_WARN("only count(*) accepts '*', but got %s(*)", lower.c_str());
        return RC::UNSUPPORTED;
      }
    } else {
      rc = parse_primary_expr(arg);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
    if (!accept_punct(")")) {
      LOG_WARN("expect ')' to close %s()", lower.c_str());
      return RC::SQL_SYNTAX;
    }
    expr = std::make_unique<AggregateExpression>(func, std::move(arg));
    return RC::SUCCESS;
  }

  /// 列引用：col 或 table.col
  consume();
  std::string table_name;
  std::string column_name = first;
  if (peek_punct(".")) {
    consume();
    table_name   = first;
    rc = expect_identifier(column_name);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  expr = make_column_expr(table_name, column_name);
  return RC::SUCCESS;
}

// ---------------------------------------------------------------------------
// 语句
// ---------------------------------------------------------------------------

RC Parser::parse_statement(const std::string &sql, std::unique_ptr<ParseResult> &result)
{
  const std::string cleaned = strip_sql_comments(sql);
  lexer_.feed(cleaned);
  has_lookahead_ = false;
  lookahead_     = Token();

  RC rc = ensure_lookahead();
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to tokenize sql. rc=%s", strrc(rc));
    return rc;
  }
  if (lookahead_.kind == TokenKind::END) {
    return RC::EMPTY;
  }

  std::unique_ptr<ParseResult> parsed;
  if (peek_keyword("create")) {
    consume();
    const bool unique = accept_keyword("unique");
    if (accept_keyword("table")) {
      if (unique) {
        LOG_WARN("'unique' only applies to index");
        return RC::SQL_SYNTAX;
      }
      parsed = std::make_unique<ParseResult>(ParseResult::StmtType::CREATE_TABLE);
      rc     = parse_create_table(*parsed);
    } else if (accept_keyword("index")) {
      parsed = std::make_unique<ParseResult>(ParseResult::StmtType::CREATE_INDEX);
      parsed->create_index.unique = unique;
      rc                          = parse_create_index(*parsed);
    } else {
      LOG_WARN("only 'create table' and 'create index' are supported");
      return RC::SQL_SYNTAX;
    }
  } else if (peek_keyword("drop")) {
    consume();
    if (!accept_keyword("table")) {
      LOG_WARN("only 'drop table' is supported after drop");
      return RC::SQL_SYNTAX;
    }
    parsed = std::make_unique<ParseResult>(ParseResult::StmtType::DROP_TABLE);
    rc     = parse_drop_table(*parsed);
  } else if (peek_keyword("show")) {
    consume();
    if (!accept_keyword("tables")) {
      LOG_WARN("only 'show tables' is supported after show");
      return RC::SQL_SYNTAX;
    }
    parsed = std::make_unique<ParseResult>(ParseResult::StmtType::SHOW_TABLES);
  } else if (peek_keyword("desc") || peek_keyword("describe")) {
    consume();
    parsed = std::make_unique<ParseResult>(ParseResult::StmtType::DESC_TABLE);
    rc     = parse_desc_table(*parsed);
  } else if (peek_keyword("insert")) {
    consume();
    parsed = std::make_unique<ParseResult>(ParseResult::StmtType::INSERT);
    rc     = parse_insert(*parsed);
  } else if (peek_keyword("delete")) {
    consume();
    parsed = std::make_unique<ParseResult>(ParseResult::StmtType::DELETE_STMT);
    rc     = parse_delete(*parsed);
  } else if (peek_keyword("update")) {
    consume();
    parsed = std::make_unique<ParseResult>(ParseResult::StmtType::UPDATE);
    rc     = parse_update(*parsed);
  } else if (peek_keyword("select")) {
    parsed = std::make_unique<ParseResult>(ParseResult::StmtType::SELECT);
    rc     = parse_select(*parsed);
  } else if (peek_keyword("explain")) {
    consume();
    parsed = std::make_unique<ParseResult>(ParseResult::StmtType::EXPLAIN);
    rc     = parse_explain(*parsed);
  } else if (peek_keyword("exit") || peek_keyword("quit")) {
    consume();
    parsed = std::make_unique<ParseResult>(ParseResult::StmtType::EXIT);
  } else if (peek_keyword("help")) {
    consume();
    parsed = std::make_unique<ParseResult>(ParseResult::StmtType::HELP);
  } else {
    LOG_WARN("unrecognized statement starting with '%s'", lookahead_.text.c_str());
    return RC::SQL_SYNTAX;
  }

  if (OB_FAIL(rc)) {
    return rc;
  }

  /// 结尾必须没有多余内容（最多一个分号）
  accept_punct(";");
  if (!at_end()) {
    LOG_WARN("unexpected trailing token '%s' in statement", peek().text.c_str());
    return RC::SQL_SYNTAX;
  }

  parsed->original_sql = trim(cleaned);
  result               = std::move(parsed);
  return RC::SUCCESS;
}

/// create table t (a int, b char(10) not null, primary key(a))
RC Parser::parse_create_table(ParseResult &result)
{
  CreateTableSqlNode &node = result.create_table;

  if (accept_keyword("if")) {
    if (!accept_keyword("not")) {
      LOG_WARN("'if' must be followed by 'not exists'");
      return RC::SQL_SYNTAX;
    }
    if (!accept_keyword("exists")) {
      return RC::SQL_SYNTAX;
    }
    node.if_not_exists = true;
  }

  RC rc = expect_identifier(node.relation_name);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (to_lower(node.relation_name) == "tables") {
    LOG_WARN("table name is reserved");
    return RC::SQL_SYNTAX;
  }

  if (!accept_punct("(")) {
    LOG_WARN("expect '(' after table name");
    return RC::SQL_SYNTAX;
  }

  bool closed = false;
  while (!closed) {
    /// 表级 primary key(k1,k2)
    if (peek_keyword("primary")) {
      consume();
      if (!accept_keyword("key")) {
        return RC::SQL_SYNTAX;
      }
      if (!accept_punct("(")) {
        return RC::SQL_SYNTAX;
      }
      std::string column;
      rc = expect_identifier(column);
      if (OB_FAIL(rc)) {
        return rc;
      }
      node.primary_keys.push_back(column);
      while (accept_punct(",")) {
        rc = expect_identifier(column);
        if (OB_FAIL(rc)) {
          return rc;
        }
        node.primary_keys.push_back(column);
      }
      if (!accept_punct(")")) {
        return RC::SQL_SYNTAX;
      }
    } else {
      AttrInfoSqlNode attr;
      rc = expect_identifier(attr.name);
      if (OB_FAIL(rc)) {
        return rc;
      }
      std::string type_name;
      rc = expect_identifier(type_name);
      if (OB_FAIL(rc)) {
        return rc;
      }
      attr.type = attr_type_from_string(type_name.c_str());
      if (attr.type == AttrType::UNDEFINED) {
        LOG_WARN("unsupported column type '%s'", type_name.c_str());
        return RC::SQL_SYNTAX;
      }

      if (accept_punct("(")) {
        Value length;
        rc = expect_literal_value(length);
        if (OB_FAIL(rc)) {
          return rc;
        }
        attr.length = static_cast<size_t>(length.get_int64());
        if (!accept_punct(")")) {
          return RC::SQL_SYNTAX;
        }
      }
      if (attr.type == AttrType::CHARS) {
        if (attr.length == 0) {
          /// text 之类没写长度时给一个默认值，与 miniob 的 char 定义保持一致
          attr.length = to_lower(type_name) == "text" ? 128 : 0;
          if (attr.length == 0) {
            LOG_WARN("column '%s' needs a length, e.g. char(10)", attr.name.c_str());
            return RC::SQL_SYNTAX;
          }
        }
      } else if (attr.length != 0) {
        LOG_WARN("type %s does not take a length", type_name.c_str());
        return RC::SQL_SYNTAX;
      }

      /// 忽略列级约束（not null / primary key / unique / default），主键记录在 primary_keys 里
      while (peek_keyword("not") || peek_keyword("null") || peek_keyword("primary") || peek_keyword("key")
             || peek_keyword("unique") || peek_keyword("default")) {
        if (peek_keyword("primary")) {
          node.primary_keys.push_back(attr.name);
        }
        consume();
      }

      node.attr_infos.push_back(attr);
    }

    if (accept_punct(",")) {
      continue;
    }
    if (accept_punct(")")) {
      closed = true;
    } else {
      LOG_WARN("expect ',' or ')' in column list");
      return RC::SQL_SYNTAX;
    }
  }

  if (node.attr_infos.empty()) {
    LOG_WARN("table must have at least one column");
    return RC::SQL_SYNTAX;
  }
  return parse_ignore_tokens();
}

RC Parser::parse_drop_table(ParseResult &result)
{
  if (accept_keyword("if")) {
    if (!accept_keyword("exists")) {
      return RC::SQL_SYNTAX;
    }
    result.drop_table.if_exists = true;
  }
  RC rc = expect_identifier(result.drop_table.relation_name);
  return rc;
}

/// create [unique] index <name> on <table> (<col>)：unique 已在调用处消费
RC Parser::parse_create_index(ParseResult &result)
{
  CreateIndexSqlNode &node = result.create_index;
  RC                 rc    = expect_identifier(node.index_name);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (!accept_keyword("on")) {
    LOG_WARN("expect 'on' in create index");
    return RC::SQL_SYNTAX;
  }
  rc = expect_identifier(node.relation_name);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (!accept_punct("(")) {
    return RC::SQL_SYNTAX;
  }
  rc = expect_identifier(node.attribute_name);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (!accept_punct(")")) {
    return RC::SQL_SYNTAX;
  }
  return RC::SUCCESS;
}

RC Parser::parse_desc_table(ParseResult &result) { return expect_identifier(result.desc_table.relation_name); }

/// insert into t (a,b) values (1,'x'), (2,'y')
RC Parser::parse_insert(ParseResult &result)
{
  InsertSqlNode &node = result.insert;
  if (!accept_keyword("into")) {
    LOG_WARN("expect 'into' after insert");
    return RC::SQL_SYNTAX;
  }
  RC rc = expect_identifier(node.relation_name);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (accept_punct("(")) {
    std::string column;
    rc = expect_identifier(column);
    if (OB_FAIL(rc)) {
      return rc;
    }
    node.columns.push_back(column);
    while (accept_punct(",")) {
      rc = expect_identifier(column);
      if (OB_FAIL(rc)) {
        return rc;
      }
      node.columns.push_back(column);
    }
    if (!accept_punct(")")) {
      return RC::SQL_SYNTAX;
    }
  }

  if (!accept_keyword("values")) {
    LOG_WARN("expect 'values' in insert");
    return RC::SQL_SYNTAX;
  }

  do {
    if (!accept_punct("(")) {
      return RC::SQL_SYNTAX;
    }
    std::vector<Value> row;
    while (true) {
      Value value;
      rc = expect_literal_value(value);
      if (OB_FAIL(rc)) {
        return rc;
      }
      row.push_back(value);
      if (accept_punct(",")) {
        continue;
      }
      break;
    }
    if (!accept_punct(")")) {
      return RC::SQL_SYNTAX;
    }
    node.values_list.push_back(std::move(row));
  } while (accept_punct(","));

  return RC::SUCCESS;
}

RC Parser::parse_delete(ParseResult &result)
{
  DeleteSqlNode &node = result.delete_stmt;
  if (!accept_keyword("from")) {
    LOG_WARN("expect 'from' after delete");
    return RC::SQL_SYNTAX;
  }
  RC rc = expect_identifier(node.relation_name);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (accept_keyword("where")) {
    rc = parse_condition(node.filter);
  }
  return rc;
}

RC Parser::parse_update(ParseResult &result)
{
  UpdateSqlNode &node = result.update;
  RC            rc    = expect_identifier(node.relation_name);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (!accept_keyword("set")) {
    LOG_WARN("expect 'set' in update");
    return RC::SQL_SYNTAX;
  }

  do {
    std::string column;
    rc = expect_identifier(column);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (!accept_punct("=")) {
      return RC::SQL_SYNTAX;
    }
    Value value;
    rc = expect_literal_value(value);
    if (OB_FAIL(rc)) {
      return rc;
    }
    node.sets.emplace_back(column, value);
  } while (accept_punct(","));

  if (accept_keyword("where")) {
    rc = parse_condition(node.filter);
  }
  return rc;
}

/// select a, count(b) from t1 a, t2 where ... group by ...
RC Parser::parse_select(ParseResult &result)
{
  RC rc = parse_select_list(result);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (!accept_keyword("from")) {
    LOG_WARN("expect 'from' in select");
    return RC::SQL_SYNTAX;
  }
  rc = parse_relations(result);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (accept_keyword("where")) {
    rc = parse_condition(result.select.filter);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  if (accept_keyword("group")) {
    if (!accept_keyword("by")) {
      return RC::SQL_SYNTAX;
    }
    rc = parse_group_by(result);
  }
  return rc;
}

RC Parser::parse_select_list(ParseResult &result)
{
  SelectSqlNode &node = result.select;
  if (!accept_keyword("select")) {
    return RC::SQL_SYNTAX;
  }

  if (accept_punct("*")) {
    node.is_star = true;
    if (accept_punct(",")) {
      LOG_WARN("'select *' cannot be mixed with other columns");
      return RC::UNSUPPORTED;
    }
    return RC::SUCCESS;
  }

  do {
    std::unique_ptr<Expression> expr;
    RC                          rc = parse_primary_expr(expr);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (accept_keyword("as")) {
      std::string alias;
      rc = expect_identifier(alias);
      if (OB_FAIL(rc)) {
        return rc;
      }
      expr->set_name(alias);
    }
    node.expressions.push_back(std::move(expr));
  } while (accept_punct(","));

  return RC::SUCCESS;
}

/// from 子句里，遇到这些词说明表名列表结束了，不能把它当成别名
static bool is_clause_keyword(const std::string &text)
{
  static const char *stop[] = {"where", "group", "order", "limit", "having", "and", "or", "on", "values", "from"};
  const std::string         lower = to_lower(text);
  for (const char *keyword : stop) {
    if (lower == keyword) {
      return true;
    }
  }
  return false;
}

RC Parser::parse_relations(ParseResult &result)
{
  SelectSqlNode &node = result.select;
  do {
    std::string table;
    RC          rc = expect_identifier(table);
    if (OB_FAIL(rc)) {
      return rc;
    }
    node.relations.push_back(table);

    std::string alias;
    rc = ensure_lookahead();
    if (rc == RC::SUCCESS && peek().kind == TokenKind::IDENT) {
      if (to_lower(peek().text) == "as") {
        consume();
        rc = expect_identifier(alias);
        if (OB_FAIL(rc)) {
          return rc;
        }
      } else if (!is_clause_keyword(peek().text)) {
        alias = peek().text;
        consume();
      }
    }
    node.aliases.push_back(alias);
  } while (accept_punct(","));

  return RC::SUCCESS;
}

RC Parser::parse_group_by(ParseResult &result)
{
  do {
    std::unique_ptr<Expression> expr;
    RC                          rc = parse_primary_expr(expr);
    if (OB_FAIL(rc)) {
      return rc;
    }
    result.select.group_by.push_back(std::move(expr));
  } while (accept_punct(","));
  return RC::SUCCESS;
}

RC Parser::parse_explain(ParseResult &result)
{
  /// "explain" 已经被消费掉，剩下的文本交给一个新的解析器
  std::unique_ptr<Parser>      inner(new Parser());
  std::unique_ptr<ParseResult> child;
  const std::string            rest = trim(lexer_.sql().substr(lexer_.position()));
  RC                           rc   = inner->parse_statement(rest, child);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to parse the statement inside explain. rc=%s", strrc(rc));
    return rc;
  }
  if (child->stmt_type != ParseResult::StmtType::SELECT) {
    LOG_WARN("explain only supports select statements in this implementation");
    return RC::UNSUPPORTED;
  }

  result.explain.child_parse  = std::move(child);
  result.explain.original_sql = rest;  /// 不含 explain 前缀，便于重新解析出计划
  /// 子语句已经消耗了全部输入，外层解析器不再有内容
  lexer_.feed(std::string());
  return RC::SUCCESS;
}
