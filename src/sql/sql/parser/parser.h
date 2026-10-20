/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <string>

#include "common/rc.h"
#include "sql/parser/lexer.h"
#include "sql/parser/parse_defs.h"

class Expression;

/**
 * @brief 手写递归下降语法分析器
 * @details miniob 用 flex + bison（lex_sql.l / yacc_sql.y）生成解析器。为了让项目在
 *          只有 g++ 的环境下也能构建，这里用递归下降实现同一套语法，产出同样的
 *          ParseResult/SqlNode 结构。
 *
 *          支持的语句（关键字不区分大小写）：
 *            create table t (col type [not null] [primary key], ...)
 *            drop table [if exists] t
 *            create [unique] index i on t (col)
 *            show tables | desc|describe t
 *            insert into t [(c1,c2)] values (v1,v2) [, (v3,v4)]
 *            delete from t [where cond]
 *            update t set c1=v1 [, c2=v2] [where cond]
 *            select expr [, expr] from t1 [alias], t2 [alias] [where cond] [group by exprs]
 *            explain select ...
 *            help | exit | quit
 *          条件支持 = != <> < <= > >= 以及 and / or 与括号。
 */
class Parser
{
public:
  /// 解析一条完整语句（不含结尾分号以外的多条语句）
  RC parse_statement(const std::string &sql, std::unique_ptr<ParseResult> &result);

private:
  // ---- token 流 ----
  RC                ensure_lookahead();
  const Token      &peek();
  void              consume();
  bool              at_end();
  bool              peek_keyword(const char *keyword);
  bool              accept_keyword(const char *keyword);
  bool              peek_punct(const char *text);
  bool              accept_punct(const char *text);
  RC                expect_identifier(std::string &name);
  RC                expect_literal_value(Value &value);

  // ---- 各语句 ----
  RC parse_create_table(ParseResult &result);
  RC parse_drop_table(ParseResult &result);
  RC parse_create_index(ParseResult &result);
  RC parse_desc_table(ParseResult &result);
  RC parse_insert(ParseResult &result);
  RC parse_delete(ParseResult &result);
  RC parse_update(ParseResult &result);
  RC parse_select(ParseResult &result);
  RC parse_explain(ParseResult &result);
  RC parse_ignore_tokens();

  // ---- 表达式 ----
  RC parse_condition(std::unique_ptr<Expression> &expr);
  RC parse_or_expr(std::unique_ptr<Expression> &expr);
  RC parse_and_expr(std::unique_ptr<Expression> &expr);
  RC parse_comparison_expr(std::unique_ptr<Expression> &expr);
  RC parse_primary_expr(std::unique_ptr<Expression> &expr);
  RC parse_select_list(ParseResult &result);
  RC parse_relations(ParseResult &result);
  RC parse_group_by(ParseResult &result);

  Lexer lexer_;
  Token lookahead_;
  bool  has_lookahead_ = false;
};
