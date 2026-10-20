/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <string>
#include <vector>

#include "common/rc.h"
#include "common/value.h"

/// 词法单元类型
enum class TokenKind
{
  END,     /// 输入结束
  IDENT,   /// 标识符或关键字（不区分大小写，text 保留原始大小写）
  NUMBER,  /// 数值常量
  STRING,  /// 字符串常量
  PUNCT,   /// 运算符与标点
};

struct Token
{
  TokenKind   kind  = TokenKind::END;
  std::string text;
  Value       number;  /// kind == NUMBER 时有值
};

/**
 * @brief 手写词法分析器
 * @details miniob 用 flex 生成词法分析器；这里为了让项目在任何只装了 g++ 的环境里
 *          都能编译，直接手写。规则与 lex_sql.l 等价。
 */
class Lexer
{
public:
  void feed(const std::string &sql)
  {
    sql_   = sql;
    pos_   = 0;
    error_ = false;
  }

  const std::string &sql() const { return sql_; }
  bool                has_error() const { return error_; }
  size_t              position() const { return pos_; }

  /// 取下一个词法单元；到结尾返回 TokenKind::END
  RC next_token(Token &token);

private:
  void skip_space();
  bool eof() const { return pos_ >= sql_.size(); }
  char peek(size_t offset = 0) const
  {
    return pos_ + offset < sql_.size() ? sql_[pos_ + offset] : '\0';
  }

  std::string sql_;
  size_t      pos_   = 0;
  bool        error_ = false;
};

/// 去掉字符串里的注释（-- 和 /* */），CLI 与解析器都用得到
std::string strip_sql_comments(const std::string &sql);
