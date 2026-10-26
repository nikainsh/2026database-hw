/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/index/index.h"

#include "common/file_util.h"
#include "common/log.h"

std::string index_file_path(const std::string &dir, const std::string &table_name, const std::string &index_name)
{
  return path_join(dir, table_name + "." + index_name + ".index");
}

std::string heap_file_path(const std::string &dir, const std::string &table_name)
{
  return path_join(dir, table_name + ".heap");
}

RC Index::init(const IndexMeta &meta, const TableMeta &table_meta)
{
  meta_ = meta;
  column_ = table_meta.column(meta.field_name);
  if (column_ == nullptr) {
    LOG_WARN("index field not found. index=%s, field=%s", meta.index_name.c_str(), meta.field_name.c_str());
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }
  if (column_->type() == AttrType::UNDEFINED || column_->type() == AttrType::BOOLEAN) {
    return RC::UNSUPPORTED;
  }
  return RC::SUCCESS;
}

std::string Index::key_of(const char *record_data) const
{
  return std::string(record_data + column_->offset(), static_cast<size_t>(column_->data_len()));
}

RC BplusTreeIndex::open(DiskBufferPoolManager *buffer_pool_manager, const std::string &path,
                        const IndexMeta &meta, const TableMeta &table_meta, bool create)
{
  RC rc = Index::init(meta, table_meta);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (create && !file_exist(path)) {
    rc = buffer_pool_manager->create_file(path);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to create index file. path=%s, rc=%s", path.c_str(), strrc(rc));
      return rc;
    }
  }

  DiskBufferPool *pool = nullptr;
  rc = buffer_pool_manager->open_file(path, ReadWriteMode::READ_WRITE, &pool);
  if (OB_FAIL(rc)) {
    return rc;
  }
  buffer_pool_ = pool;

  return tree_.open(pool, column_->type(), column_->data_len(), meta.unique, create);
}

RC BplusTreeIndex::insert_entry(const char *record_data, const RID &rid)
{
  if (record_data == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  return tree_.insert_entry(key_of(record_data).c_str(), rid);
}

RC BplusTreeIndex::delete_entry(const char *record_data, const RID &rid)
{
  if (record_data == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  return tree_.delete_entry(key_of(record_data).c_str(), rid);
}

RC BplusTreeIndex::make_scanner(BplusTreeScanner &scanner,
                               const Value *low, bool low_inclusive,
                               const Value *high, bool high_inclusive) const
{
  std::string low_key, high_key;
  if (low != nullptr) {
    low_key = tree_.value_to_key(*low);
  }
  if (high != nullptr) {
    high_key = tree_.value_to_key(*high);
  }

  return tree_.make_scanner(scanner,
                             low != nullptr ? low_key.c_str() : nullptr, low_inclusive,
                             high != nullptr ? high_key.c_str() : nullptr, high_inclusive);
}

RC BplusTreeIndex::entry_count(int64_t &count) const
{
  count = tree_.entry_count();
  return RC::SUCCESS;
}

int BplusTreeIndex::compare_key(const char *left, const char *right) const
{
  return tree_.compare_key_bytes(left, right);
}

RC BplusTreeIndex::collect_stats(IndexStats &stats) const
{
  int64_t entries       = 0;
  int64_t distinct_keys = 0;
  std::string min_key, max_key;

  RC rc = tree_.collect_stats(entries, distinct_keys, min_key, max_key);
  if (OB_FAIL(rc)) {
    return rc;
  }

  stats.valid           = true;
  stats.entries         = entries;
  stats.distinct_keys   = distinct_keys;
  stats.min_key         = min_key;
  stats.max_key         = max_key;
  stats.sampled_entries = entries;  /// 漂移基线：下次条目数变化超过阈值才会重采
  return RC::SUCCESS;
}

RC Index::decode_key(const std::string &key, Value &value) const
{
  if (column_ == nullptr || static_cast<int>(key.size()) != column_->data_len()) {
    return RC::INVALID_ARGUMENT;
  }
  value = Value();
  value.set_type(column_->type());
  return value.unpack_from(key.data(), column_->data_len());
}
