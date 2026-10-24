/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <string>

#include "common/rc.h"
#include "common/types.h"

/// 属性的数据类型，对应 miniob 的 AttrType
enum class AttrType
{
  UNDEFINED = 0,
  INT,      /// 4 字节整数
  BIGINT,   /// 8 字节整数
  FLOAT,    /// 4 字节浮点数
  CHARS,    /// 定长字符串 char(n)
  BOOLEAN,  /// 程序内部使用，parser 不会直接产生
  MAXTYPE,
};

const char *attr_type_to_string(AttrType type);
AttrType    attr_type_from_string(const char *s);
bool        is_numerical_type(AttrType type);
bool        is_string_type(AttrType type);
/// 在磁盘记录中占用的字节数，CHARS 由调用方给出长度
int attr_type_fixed_len(AttrType type);

/**
 * @brief 一个带类型的值
 * @details 与 miniob 一样，值同时携带类型，比较、类型转换、记录打包都由 Value 完成。
 */
class Value
{
public:
  Value() = default;
  explicit Value(int32_t v) : type_(AttrType::INT), num_(v) {}
  explicit Value(int64_t v) : type_(AttrType::BIGINT), num_(v) {}
  explicit Value(float v) : type_(AttrType::FLOAT), float_(v) {}
  explicit Value(bool v) : type_(AttrType::BOOLEAN), num_(v ? 1 : 0) {}
  explicit Value(const char *s) : type_(AttrType::CHARS), str_(s == nullptr ? "" : s) {}
  explicit Value(const std::string &s) : type_(AttrType::CHARS), str_(s) {}

  static Value make(AttrType type, const std::string &text, RC &rc);

public:
  AttrType type() const { return type_; }
  void     set_type(AttrType type) { type_ = type; }

  int64_t          get_int64() const { return num_; }
  int32_t          get_int32() const { return static_cast<int32_t>(num_); }
  float            get_float() const { return float_; }
  bool             get_bool() const { return num_ != 0; }
  const std::string &get_string() const { return str_; }

  void set_int64(int64_t v)
  {
    type_ = AttrType::BIGINT;
    num_  = v;
  }
  void set_float(float v)
  {
    type_ = AttrType::FLOAT;
    float_ = v;
  }
  void set_string(const std::string &s)
  {
    type_ = AttrType::CHARS;
    str_  = s;
  }

  /// 打包到磁盘记录缓冲区，CHARS 会用 '\0' 补齐到 dst_len
  RC pack_into(char *dst, int dst_len) const;
  /// 从磁盘记录缓冲区还原，会去掉字符串尾部的 '\0'
  RC unpack_from(const char *src, int len);

  RC cast_to(AttrType to, Value &result) const;
  /// 比较：result 为 -1/0/1，类型不兼容时返回错误码
  RC compare_to(const Value &other, int &result) const;

  bool is_boolean_true() const { return type_ == AttrType::BOOLEAN && num_ != 0; }

  /// 展示给用户的文本
  std::string to_string() const;
  std::string type_name() const { return attr_type_to_string(type_); }

  bool same_type_as(const Value &other) const { return type_ == other.type_; }

private:
  AttrType    type_  = AttrType::UNDEFINED;
  int64_t     num_   = 0;
  float       float_ = 0.0f;
  std::string str_;
};
