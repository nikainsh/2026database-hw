/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/table/table_meta.h"

#include <cstring>
#include <sstream>

#include "common/log.h"
#include "storage/buffer/page.h"
#include "storage/record/slotted_page.h"

// ---------------------------------------------------------------------------
// IndexMeta
// ---------------------------------------------------------------------------

void IndexMeta::serialize(BinaryWriter &w) const
{
  w.put_string(index_name);
  w.put_string(field_name);
  w.put_bool(unique);
}

RC IndexMeta::deserialize(BinaryReader &r, IndexMeta &meta)
{
  meta.index_name = r.get_string();
  meta.field_name = r.get_string();
  meta.unique     = r.get_bool();
  return r.ok() ? RC::SUCCESS : RC::IOERR_READ;
}

// ---------------------------------------------------------------------------
// ColumnMeta
// ---------------------------------------------------------------------------

RC ColumnMeta::init(const std::string &name, AttrType type, int len, int offset)
{
  if (name.empty()) {
    return RC::INVALID_ARGUMENT;
  }
  if (type == AttrType::UNDEFINED || type == AttrType::MAXTYPE) {
    return RC::UNSUPPORTED;
  }
  if (type == AttrType::CHARS && len <= 0) {
    return RC::INVALID_ARGUMENT;
  }
  if (type != AttrType::CHARS && attr_type_fixed_len(type) <= 0) {
    return RC::UNSUPPORTED;
  }

  name_   = name;
  type_   = type;
  len_    = type == AttrType::CHARS ? len : attr_type_fixed_len(type);
  offset_ = offset;
  return RC::SUCCESS;
}

void ColumnMeta::serialize(BinaryWriter &w) const
{
  w.put_string(name_);
  w.put_i32(static_cast<int32_t>(type_));
  w.put_i32(len_);
  w.put_i32(offset_);
}

RC ColumnMeta::deserialize(BinaryReader &r, ColumnMeta &column)
{
  const std::string name = r.get_string();
  const AttrType    type = static_cast<AttrType>(r.get_i32());
  const int         len  = r.get_i32();
  const int         offset = r.get_i32();
  if (!r.ok()) {
    return RC::IOERR_READ;
  }
  return column.init(name, type, len, offset);
}

// ---------------------------------------------------------------------------
// TableMeta
// ---------------------------------------------------------------------------

RC TableMeta::init(const std::string &table_name, const std::vector<ColumnMeta> &columns)
{
  if (table_name.empty() || columns.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  table_name_ = table_name;
  columns_    = columns;
  indexes_.clear();

  int offset = 0;
  for (size_t i = 0; i < columns_.size(); ++i) {
    ColumnMeta &column = columns_[i];
    for (size_t j = 0; j < i; ++j) {
      if (columns_[j].name() == column.name()) {
        LOG_WARN("duplicated column name. table=%s, column=%s", table_name.c_str(), column.name().c_str());
        return RC::SCHEMA_FIELD_MISSING;
      }
    }
    column.set_offset(offset);
    offset += column.data_len();
  }

  /// 一条记录必须能放进一个空页：页头 + 一个槽位 + 记录本身
  const int limit = BP_PAGE_SIZE - static_cast<int>(sizeof(SlottedPageHeader)) - static_cast<int>(sizeof(int32_t));
  if (offset <= 0 || offset > limit) {
    LOG_WARN("record size too large. table=%s, size=%d, limit=%d", table_name.c_str(), offset, limit);
    return RC::INVALID_ARGUMENT;
  }

  record_size_ = offset;
  return RC::SUCCESS;
}

const ColumnMeta *TableMeta::column(const std::string &name) const
{
  for (const ColumnMeta &column : columns_) {
    if (column.name() == name) {
      return &column;
    }
  }
  return nullptr;
}

int TableMeta::column_index(const std::string &name) const
{
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].name() == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

const IndexMeta *TableMeta::index(const std::string &index_name) const
{
  for (const IndexMeta &meta : indexes_) {
    if (meta.index_name == index_name) {
      return &meta;
    }
  }
  return nullptr;
}

const IndexMeta *TableMeta::index_of_field(const std::string &field_name) const
{
  for (const IndexMeta &meta : indexes_) {
    if (meta.field_name == field_name) {
      return &meta;
    }
  }
  return nullptr;
}

RC TableMeta::add_index(const IndexMeta &meta)
{
  if (meta.index_name.empty() || meta.field_name.empty()) {
    return RC::INVALID_ARGUMENT;
  }
  if (index(meta.index_name) != nullptr) {
    return RC::SCHEMA_INDEX_NAME_REPEAT;
  }
  if (column(meta.field_name) == nullptr) {
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }
  indexes_.push_back(meta);
  return RC::SUCCESS;
}

RC TableMeta::remove_index(const std::string &index_name)
{
  for (auto it = indexes_.begin(); it != indexes_.end(); ++it) {
    if (it->index_name == index_name) {
      indexes_.erase(it);
      return RC::SUCCESS;
    }
  }
  return RC::SCHEMA_INDEX_NOT_EXIST;
}

RC TableMeta::make_record(const std::vector<Value> &values, Record &record) const
{
  if (values.size() != columns_.size()) {
    LOG_WARN("value count mismatch. table=%s, values=%d, columns=%d",
             table_name_.c_str(), (int)values.size(), (int)columns_.size());
    return RC::SCHEMA_FIELD_MISSING;
  }

  std::vector<char> buffer(static_cast<size_t>(record_size_), '\0');
  for (size_t i = 0; i < columns_.size(); ++i) {
    const ColumnMeta &column = columns_[i];
    Value             staged;
    RC                rc = values[i].cast_to(column.type(), staged);
    if (OB_FAIL(rc)) {
      LOG_WARN("cannot convert value to column type. table=%s, column=%s", table_name_.c_str(), column.name().c_str());
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
    rc = staged.pack_into(buffer.data() + column.offset(), column.data_len());
    if (OB_FAIL(rc)) {
      if (rc == RC::IOERR_TOO_LONG) {
        LOG_WARN("value too long. table=%s, column=%s, len=%d, max=%d",
                 table_name_.c_str(), column.name().c_str(), (int)staged.get_string().size(), column.data_len());
        return RC::IOERR_TOO_LONG;
      }
      return rc;
    }
  }
  return record.copy_data(buffer.data(), record_size_);
}

RC TableMeta::get_field_value(const Record &record, const ColumnMeta &column, Value &value) const
{
  value = Value();
  value.set_type(column.type());
  return value.unpack_from(record.data() + column.offset(), column.data_len());
}

const char *TableMeta::field_data(const Record &record, const ColumnMeta &column) const
{
  return record.data() + column.offset();
}

void TableMeta::serialize(BinaryWriter &w) const
{
  w.put_string(table_name_);
  w.put_i32(static_cast<int32_t>(columns_.size()));
  for (const ColumnMeta &column : columns_) {
    column.serialize(w);
  }
  w.put_i32(static_cast<int32_t>(indexes_.size()));
  for (const IndexMeta &meta : indexes_) {
    meta.serialize(w);
  }
}

RC TableMeta::deserialize(BinaryReader &r, TableMeta &meta)
{
  const std::string name = r.get_string();
  const int32_t     count = r.get_i32();
  if (!r.ok() || count <= 0 || count > 1024) {
    return RC::IOERR_READ;
  }

  std::vector<ColumnMeta> columns;
  for (int32_t i = 0; i < count; ++i) {
    ColumnMeta column;
    RC         rc = ColumnMeta::deserialize(r, column);
    if (OB_FAIL(rc)) {
      return rc;
    }
    columns.push_back(column);
  }

  RC rc = meta.init(name, columns);
  if (OB_FAIL(rc)) {
    return rc;
  }

  const int32_t index_count = r.get_i32();
  if (!r.ok() || index_count < 0 || index_count > 1024) {
    return RC::IOERR_READ;
  }
  for (int32_t i = 0; i < index_count; ++i) {
    IndexMeta index_meta;
    rc        = IndexMeta::deserialize(r, index_meta);
    if (OB_FAIL(rc)) {
      return rc;
    }
    rc = meta.add_index(index_meta);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  return r.ok() ? RC::SUCCESS : RC::IOERR_READ;
}

std::string TableMeta::to_string() const
{
  std::ostringstream oss;
  oss << "table=" << table_name_ << "(size=" << record_size_ << ")";
  for (const ColumnMeta &column : columns_) {
    oss << " " << column.name() << ":" << attr_type_to_string(column.type());
    if (column.type() == AttrType::CHARS) {
      oss << "(" << column.len() << ")";
    }
  }
  for (const IndexMeta &meta : indexes_) {
    oss << " index[" << meta.index_name << " on " << meta.field_name << (meta.unique ? ",unique" : "") << "]";
  }
  return oss.str();
}
