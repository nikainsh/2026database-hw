/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <string>
#include <vector>

#include "common/binary.h"
#include "common/rc.h"
#include "common/value.h"
#include "storage/record/record.h"

/// 索引元数据：本实现一个索引只覆盖一个字段（与 miniob 的简化一致）
struct IndexMeta
{
  std::string index_name;
  std::string field_name;
  bool        unique = false;

  void serialize(BinaryWriter &w) const;
  static RC deserialize(BinaryReader &r, IndexMeta &meta);
};

/**
 * @brief 一个字段（列）的元数据
 * @details offset_ 是这一列在记录缓冲区里的字节偏移，记录是定长且紧凑存放的。
 */
class ColumnMeta
{
public:
  ColumnMeta() = default;
  RC init(const std::string &name, AttrType type, int len, int offset);

  const std::string &name() const { return name_; }
  AttrType           type() const { return type_; }
  int                len() const { return len_; }     /// CHARS 的字符长度
  int                offset() const { return offset_; }
  void               set_offset(int offset) { offset_ = offset; }

  /// 这一列在记录里占用的字节数
  int data_len() const { return type_ == AttrType::CHARS ? len_ : attr_type_fixed_len(type_); }

  void serialize(BinaryWriter &w) const;
  static RC deserialize(BinaryReader &r, ColumnMeta &column);

private:
  std::string name_;
  AttrType    type_   = AttrType::UNDEFINED;
  int         len_    = 0;
  int         offset_ = 0;
};

/**
 * @brief 一张表的元数据
 * @details 对应 miniob 的 TableMeta：字段列表 + 索引列表 + 记录长度。
 *          这里不再用 JSON，而是用小端二进制写进 catalog 文件。
 */
class TableMeta
{
public:
  TableMeta() = default;

  /// 根据字段定义算出每一列的偏移和记录长度，并做基本校验
  RC init(const std::string &table_name, const std::vector<ColumnMeta> &columns);

  const std::string             &table_name() const { return table_name_; }
  const std::vector<ColumnMeta> &columns() const { return columns_; }
  const std::vector<IndexMeta>  &indexes() const { return indexes_; }
  int                            record_size() const { return record_size_; }

  const ColumnMeta *column(const std::string &name) const;
  int               column_index(const std::string &name) const;
  const IndexMeta  *index(const std::string &index_name) const;
  const IndexMeta  *index_of_field(const std::string &field_name) const;
  RC                add_index(const IndexMeta &meta);
  RC                remove_index(const std::string &index_name);

  /// 把一组值按列顺序打包成一条记录（会做类型转换与长度检查）
  RC make_record(const std::vector<Value> &values, Record &record) const;
  /// 从记录里取出某一列的值
  RC get_field_value(const Record &record, const ColumnMeta &column, Value &value) const;
  /// 从记录里取出某一列的原始字节
  const char *field_data(const Record &record, const ColumnMeta &column) const;

  void serialize(BinaryWriter &w) const;
  static RC deserialize(BinaryReader &r, TableMeta &meta);

  std::string to_string() const;

private:
  std::string             table_name_;
  std::vector<ColumnMeta> columns_;
  std::vector<IndexMeta>  indexes_;
  int                     record_size_ = 0;
};
