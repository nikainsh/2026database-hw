/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/parser/lexer.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

#include "common/log.h"
#include "common/string_util.h"

std::string strip_sql_comments(const std::string &sql)
{
  std::string out;
  out.reserve(sql.size());

  bool in_string  = false;
  bool in_line    = false;
  bool in_block   = false;
  for (size_t i = 0; i < sql.size();) {
    const char c = sql[i];
    if (in_line) {
      if (c == '\n') {
        in_line = false;
        out.push_back('\n');
      }
      ++i;
      continue;
    }
    if (in_block) {
      if (c == '*' && i + 1 < sql.size() && sql[i + 1] == '/') {
        in_block = false;
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    if (in_string) {
      out.push_back(c);
      if (c == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {  /// '' 是转义的单引号
        out.push_back('\'');
        i += 2;
        continue;
      }
      if (c == '\'') {
        in_string = false;
      }
      ++i;
      continue;
    }

    if (c == '\'' && !in_string) {
      in_string = true;
      out.push_back(c);
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      in_line = true;
      i += 2;
      continue;
    }
    if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      in_block = true;
      i += 2;
      continue;
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

void Lexer::skip_space()
{
  while (!eof()) {
    const char c = peek();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++pos_;
    } else {
      break;
    }
  }
}

RC Lexer::next_token(Token &token)
{
  token = Token();
  skip_space();
  if (eof()) {
    token.kind = TokenKind::END;
    return RC::SUCCESS;
  }

  const char c = peek();

  /// 字符串常量：单引号包裹，'' 表示一个引号
  if (c == '\'') {
    std::string value;
    ++pos_;
    while (true) {
      if (eof()) {
        LOG_WARN("unterminated string literal. pos=%d", (int)pos_);
        error_ = true;
        return RC::UNEXPECTED_END_OF_STRING;
      }
      const char ch = sql_[pos_++];
      if (ch == '\'') {
        if (!eof() && sql_[pos_] == '\'') {
          value.push_back('\'');
          ++pos_;
          continue;
        }
        break;
      }
      if (ch == '\\' && !eof() && sql_[pos_] == '\'') {  /// 兼容 \' 写法
        value.push_back('\'');
        ++pos_;
        continue;
      }
      value.push_back(ch);
    }
    token.kind = TokenKind::STRING;
    token.text = value;
    return RC::SUCCESS;
  }

  /// 数字：整数或小数，允许前面的负号（本实现不支持算术表达式，所以不会与减号混淆）
  if (std::isdigit(static_cast<unsigned char>(c))
      || (c == '-' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
    size_t      start = pos_;
    bool        real  = false;
    if (sql_[pos_] == '-') {
      ++pos_;
    }
    while (!eof()) {
      const char ch = sql_[pos_];
      if (std::isdigit(static_cast<unsigned char>(ch))) {
        ++pos_;
      } else if (ch == '.' && !real) {
        real  = true;
        ++pos_;
      } else {
        break;
      }
    }
    token.kind = TokenKind::NUMBER;
    token.text = sql_.substr(start, pos_ - start);

    /// 超出 int32 范围的整数字面量按 bigint 处理，避免存进记录时被截断
    AttrType type = real ? AttrType::FLOAT : AttrType::INT;
    if (!real) {
      const long long wide = strtoll(token.text.c_str(), nullptr, 10);
      if (wide > 2147483647LL || wide < -2147483648LL) {
        type = AttrType::BIGINT;
      }
    }

    RC rc = RC::SUCCESS;
    token.number = Value::make(type, token.text, rc);
    if (OB_FAIL(rc)) {
      error_ = true;
      return RC::INVALID_TOKEN;
    }
    return RC::SUCCESS;
  }

  /// 标识符 / 关键字
  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    const size_t start = pos_;
    while (!eof()) {
      const char ch = sql_[pos_];
      if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
        ++pos_;
      } else {
        break;
      }
    }
    token.kind = TokenKind::IDENT;
    token.text = sql_.substr(start, pos_ - start);
    return RC::SUCCESS;
  }

  /// 运算符与标点
  static const char *multi[] = {"!=", "<>", "<=", ">="};
  for (const char *op : multi) {
    if (sql_.compare(pos_, 2, op, 0, 2) == 0) {
      token.kind = TokenKind::PUNCT;
      token.text = op;
      pos_ += 2;
      return RC::SUCCESS;
    }
  }

  if (c == '(' || c == ')' || c == ',' || c == ';' || c == '*' || c == '.' || c == '=' || c == '<' || c == '>') {
    token.kind = TokenKind::PUNCT;
    token.text = std::string(1, c);
    ++pos_;
    return RC::SUCCESS;
  }

  LOG_WARN("unexpected character '%c' at pos=%d", c, (int)pos_);
  error_ = true;
  return RC::INVALID_TOKEN;
}
