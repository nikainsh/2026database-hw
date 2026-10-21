/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

/// 与 miniob 一致的返回码风格：所有内部接口返回 RC，不抛异常
#define DEFINE_RCS                      \
  DEFINE_RC(SUCCESS)                    \
  DEFINE_RC(INVALID_ARGUMENT)           \
  DEFINE_RC(UNIMPLEMENTED)              \
  DEFINE_RC(SQL_SYNTAX)                 \
  DEFINE_RC(SYNTAX_ERROR)               \
  DEFINE_RC(INVALID_TOKEN)              \
  DEFINE_RC(UNEXPECTED_END_OF_STRING)   \
  DEFINE_RC(INTERNAL)                   \
  DEFINE_RC(NOMEM)                      \
  DEFINE_RC(NOTFOUND)                   \
  DEFINE_RC(EMPTY)                      \
  DEFINE_RC(FULL)                       \
  DEFINE_RC(EXIST)                      \
  DEFINE_RC(NOT_EXIST)                  \
  DEFINE_RC(BUFFERPOOL_OPEN)            \
  DEFINE_RC(BUFFERPOOL_NOBUF)           \
  DEFINE_RC(BUFFERPOOL_INVALID_PAGE_NUM) \
  DEFINE_RC(BUFFERPOOL_PAGE_PINNED)     \
  DEFINE_RC(RECORD_OPENNED)             \
  DEFINE_RC(RECORD_INVALID_RID)         \
  DEFINE_RC(RECORD_INVALID_KEY)         \
  DEFINE_RC(RECORD_DUPLICATE_KEY)       \
  DEFINE_RC(RECORD_EOF)                 \
  DEFINE_RC(RECORD_NOT_EXIST)           \
  DEFINE_RC(SCHEMA_DB_EXIST)            \
  DEFINE_RC(SCHEMA_DB_NOT_EXIST)        \
  DEFINE_RC(SCHEMA_TABLE_NOT_EXIST)     \
  DEFINE_RC(SCHEMA_TABLE_EXIST)         \
  DEFINE_RC(SCHEMA_FIELD_NOT_EXIST)     \
  DEFINE_RC(SCHEMA_FIELD_MISSING)       \
  DEFINE_RC(SCHEMA_FIELD_TYPE_MISMATCH) \
  DEFINE_RC(SCHEMA_INDEX_NAME_REPEAT)   \
  DEFINE_RC(SCHEMA_INDEX_NOT_EXIST)     \
  DEFINE_RC(IOERR_READ)                 \
  DEFINE_RC(IOERR_WRITE)                \
  DEFINE_RC(IOERR_ACCESS)               \
  DEFINE_RC(IOERR_OPEN)                 \
  DEFINE_RC(IOERR_CLOSE)                \
  DEFINE_RC(IOERR_SEEK)                 \
  DEFINE_RC(IOERR_TOO_LONG)             \
  DEFINE_RC(IOERR_SYNC)                 \
  DEFINE_RC(FILE_EXIST)                 \
  DEFINE_RC(FILE_NOT_EXIST)             \
  DEFINE_RC(FILE_NOT_OPENED)            \
  DEFINE_RC(FILE_CREATE)                \
  DEFINE_RC(FILE_OPEN)                  \
  DEFINE_RC(FILE_REMOVE)                \
  DEFINE_RC(UNSUPPORTED)

enum class RC
{
#define DEFINE_RC(name) name,
  DEFINE_RCS
#undef DEFINE_RC
};

const char *strrc(RC rc);

inline bool OB_SUCC(RC rc) { return rc == RC::SUCCESS; }
inline bool OB_FAIL(RC rc) { return rc != RC::SUCCESS; }
