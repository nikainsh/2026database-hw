/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "common/value.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <strings.h>  /// strcasecmp：MinGW 与 POSIX 都提供

const char *attr_type_to_string(AttrType type)
{
  switch (type) {
    case AttrType::INT: return "int";
    case AttrType::BIGINT: return "bigint";
    case AttrType::FLOAT: return "float";
    case AttrType::CHARS: return "char";
    case AttrType::BOOLEAN: return "bool";
    default: return "undefined";
  }
}

AttrType attr_type_from_string(const char *s)
{
  static const struct {
    const char *name;
    AttrType    type;
  } table[] = {
      {"int", AttrType::INT},
      {"integer", AttrType::INT},
      {"bigint", AttrType::BIGINT},
      {"float", AttrType::FLOAT},
      {"real", AttrType::FLOAT},
      {"double", AttrType::FLOAT},
      {"char", AttrType::CHARS},
      {"varchar", AttrType::CHARS},
      {"text", AttrType::CHARS},
      {"bool", AttrType::BOOLEAN},
      {"boolean", AttrType::BOOLEAN},
  };

  for (const auto &item : table) {
    if (strcasecmp(s, item.name) == 0) {
      return item.type;
    }
  }
  return AttrType::UNDEFINED;
}

bool is_numerical_type(AttrType type)
{
  return type == AttrType::INT || type == AttrType::BIGINT || type == AttrType::FLOAT;
}

bool is_string_type(AttrType type) { return type == AttrType::CHARS; }

int attr_type_fixed_len(AttrType type)
{
  switch (type) {
    case AttrType::INT: return 4;
    case AttrType::BIGINT: return 8;
    case AttrType::FLOAT: return 4;
    case AttrType::BOOLEAN: return 1;
    default: return 0;
  }
}

Value Value::make(AttrType type, const std::string &text, RC &rc)
{
  Value v;
  switch (type) {
    case AttrType::INT: {
      char *end = nullptr;
      long  n   = strtol(text.c_str(), &end, 10);
      if (end == text.c_str()) {
        rc = RC::SCHEMA_FIELD_TYPE_MISMATCH;
        return v;
      }
      v = Value(static_cast<int32_t>(n));
      break;
    }
    case AttrType::BIGINT: {
      char       *end = nullptr;
      long long   n   = strtoll(text.c_str(), &end, 10);
      if (end == text.c_str()) {
        rc = RC::SCHEMA_FIELD_TYPE_MISMATCH;
        return v;
      }
      v = Value(static_cast<int64_t>(n));
      break;
    }
    case AttrType::FLOAT: {
      char  *end = nullptr;
      double n   = strtod(text.c_str(), &end);
      if (end == text.c_str()) {
        rc = RC::SCHEMA_FIELD_TYPE_MISMATCH;
        return v;
      }
      v = Value(static_cast<float>(n));
      break;
    }
    case AttrType::CHARS: v = Value(text); break;
    case AttrType::BOOLEAN: v = Value(text == "1" || strcasecmp(text.c_str(), "true") == 0); break;
    default:
      rc = RC::UNSUPPORTED;
      return v;
  }
  rc = RC::SUCCESS;
  return v;
}

RC Value::pack_into(char *dst, int dst_len) const
{
  if (dst == nullptr || dst_len <= 0) {
    return RC::INVALID_ARGUMENT;
  }

  switch (type_) {
    case AttrType::INT: {
      if (dst_len != 4) return RC::INVALID_ARGUMENT;
      int32_t v = static_cast<int32_t>(num_);
      memcpy(dst, &v, sizeof(v));
      break;
    }
    case AttrType::BIGINT: {
      if (dst_len != 8) return RC::INVALID_ARGUMENT;
      int64_t v = num_;
      memcpy(dst, &v, sizeof(v));
      break;
    }
    case AttrType::FLOAT: {
      if (dst_len != 4) return RC::INVALID_ARGUMENT;
      float v = float_;
      memcpy(dst, &v, sizeof(v));
      break;
    }
    case AttrType::BOOLEAN: {
      if (dst_len != 1) return RC::INVALID_ARGUMENT;
      int8_t v = static_cast<int8_t>(num_ != 0 ? 1 : 0);
      memcpy(dst, &v, sizeof(v));
      break;
    }
    case AttrType::CHARS: {
      if (static_cast<int>(str_.size()) > dst_len) {
        return RC::IOERR_TOO_LONG;
      }
      memset(dst, 0, dst_len);
      memcpy(dst, str_.data(), str_.size());
      break;
    }
    default: return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  return RC::SUCCESS;
}

RC Value::unpack_from(const char *src, int len)
{
  if (src == nullptr || len <= 0) {
    return RC::INVALID_ARGUMENT;
  }

  switch (type_) {
    case AttrType::INT: {
      if (len != 4) return RC::INTERNAL;
      int32_t v = 0;
      memcpy(&v, src, sizeof(v));
      num_ = v;
      break;
    }
    case AttrType::BIGINT: {
      if (len != 8) return RC::INTERNAL;
      int64_t v = 0;
      memcpy(&v, src, sizeof(v));
      num_ = v;
      break;
    }
    case AttrType::FLOAT: {
      if (len != 4) return RC::INTERNAL;
      float v = 0.0f;
      memcpy(&v, src, sizeof(v));
      float_ = v;
      break;
    }
    case AttrType::BOOLEAN: {
      if (len != 1) return RC::INTERNAL;
      int8_t v = 0;
      memcpy(&v, src, sizeof(v));
      num_ = v;
      break;
    }
    case AttrType::CHARS: {
      int real_len = 0;
      while (real_len < len && src[real_len] != '\0') {
        ++real_len;
      }
      str_.assign(src, real_len);
      break;
    }
    default: return RC::INTERNAL;
  }
  return RC::SUCCESS;
}

RC Value::cast_to(AttrType to, Value &result) const
{
  result = Value();
  switch (to) {
    case AttrType::INT: {
      switch (type_) {
        case AttrType::INT: case AttrType::BOOLEAN:
          result = Value(static_cast<int32_t>(num_)); break;
        case AttrType::BIGINT:
          /// 超出 int32 范围就报错，而不是静默截断
          if (num_ < -2147483648LL || num_ > 2147483647LL) {
            return RC::SCHEMA_FIELD_TYPE_MISMATCH;
          }
          result = Value(static_cast<int32_t>(num_)); break;
        case AttrType::FLOAT: result = Value(static_cast<int32_t>(lrintf(float_))); break;
        case AttrType::CHARS: {
          RC rc = RC::SUCCESS;
          result = Value::make(AttrType::INT, str_, rc);
          return rc;
        }
        default: return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      break;
    }
    case AttrType::BIGINT: {
      switch (type_) {
        case AttrType::INT: case AttrType::BIGINT: case AttrType::BOOLEAN:
          result = Value(static_cast<int64_t>(num_)); break;
        case AttrType::FLOAT: result = Value(static_cast<int64_t>(llrintf(float_))); break;
        case AttrType::CHARS: {
          RC rc = RC::SUCCESS;
          result = Value::make(AttrType::BIGINT, str_, rc);
          return rc;
        }
        default: return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      break;
    }
    case AttrType::FLOAT: {
      switch (type_) {
        case AttrType::INT: case AttrType::BIGINT: case AttrType::BOOLEAN:
          result = Value(static_cast<float>(num_)); break;
        case AttrType::FLOAT: result = *this; break;
        case AttrType::CHARS: {
          RC rc = RC::SUCCESS;
          result = Value::make(AttrType::FLOAT, str_, rc);
          return rc;
        }
        default: return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      break;
    }
    case AttrType::CHARS: {
      switch (type_) {
        case AttrType::INT: case AttrType::BIGINT: result = Value(std::to_string(num_)); break;
        case AttrType::FLOAT: {
          std::ostringstream oss;
          oss << float_;
          result = Value(oss.str());
          break;
        }
        case AttrType::BOOLEAN: result = Value(num_ != 0 ? "true" : "false"); break;
        case AttrType::CHARS: result = *this; break;
        default: return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      break;
    }
    case AttrType::BOOLEAN: {
      switch (type_) {
        case AttrType::INT: case AttrType::BIGINT: case AttrType::BOOLEAN: result = Value(num_ != 0); break;
        case AttrType::FLOAT: result = Value(float_ != 0.0f); break;
        case AttrType::CHARS: result = Value(!str_.empty() && str_ != "0" && str_ != "false"); break;
        default: return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      break;
    }
    default: return RC::UNSUPPORTED;
  }
  return RC::SUCCESS;
}

RC Value::compare_to(const Value &other, int &result) const
{
  result = 0;

  if (type_ == AttrType::UNDEFINED || other.type() == AttrType::UNDEFINED) {
    return RC::INVALID_ARGUMENT;
  }

  if (type_ == AttrType::CHARS || other.type() == AttrType::CHARS) {
    /// 字符串只与字符串比较；绑定阶段已经把所有字面量转换成列的类型
    if (type_ != AttrType::CHARS || other.type() != AttrType::CHARS) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
    result = str_ < other.str_ ? -1 : (str_ == other.str_ ? 0 : 1);
    return RC::SUCCESS;
  }

  const bool lhs_real = type_ == AttrType::FLOAT;
  const bool rhs_real = other.type() == AttrType::FLOAT;
  if (lhs_real || rhs_real) {
    const double l = lhs_real ? static_cast<double>(float_) : static_cast<double>(num_);
    const double r = rhs_real ? static_cast<double>(other.float_) : static_cast<double>(other.num_);
    result         = l < r ? -1 : (l > r ? 1 : 0);
  } else {
    result = num_ < other.num_ ? -1 : (num_ > other.num_ ? 1 : 0);
  }
  return RC::SUCCESS;
}

std::string Value::to_string() const
{
  switch (type_) {
    case AttrType::INT: return std::to_string(static_cast<int32_t>(num_));
    case AttrType::BIGINT: return std::to_string(num_);
    case AttrType::FLOAT: {
      std::ostringstream oss;
      oss.precision(6);
      oss << static_cast<double>(float_);
      return oss.str();
    }
    case AttrType::BOOLEAN: return num_ != 0 ? "true" : "false";
    case AttrType::CHARS: return str_;
    default: return "<undefined>";
  }
}
