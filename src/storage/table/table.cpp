/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/table/table.h"

#include <cstring>

#include "common/file_util.h"
#include "common/log.h"

Table::~Table()
{
  if (record_manager_) {
    record_manager_->close();
  }
  indexes_.clear();
}

RC Table::open(DiskBufferPoolManager *buffer_pool_manager, const std::string &dir, const TableMeta &meta, bool create)
{
  if (buffer_pool_manager == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  buffer_pool_manager_ = buffer_pool_manager;
  dir_                 = dir;
  meta_                = meta;

  const std::string heap_path = heap_file_path(dir_, meta_.table_name());
  if (create) {
    RC rc = buffer_pool_manager_->create_file(heap_path);
    if (OB_FAIL(rc) && rc != RC::FILE_EXIST) {
      LOG_ERROR("failed to create heap file. path=%s, rc=%s", heap_path.c_str(), strrc(rc));
      return rc;
    }
  }

  DiskBufferPool *pool = nullptr;
  RC              rc   = buffer_pool_manager_->open_file(heap_path, ReadWriteMode::READ_WRITE, &pool);
  if (OB_FAIL(rc)) {
    return rc;
  }

  record_manager_ = std::make_unique<HeapRecordManager>();
  rc              = record_manager_->open(pool, meta_.record_size(), create);
  if (OB_FAIL(rc)) {
    record_manager_.reset();
    LOG_ERROR("failed to open record manager. table=%s, rc=%s", meta_.table_name().c_str(), strrc(rc));
    return rc;
  }

  return open_indexes(create);
}

RC Table::open_indexes(bool create)
{
  indexes_.clear();
  for (const IndexMeta &meta : meta_.indexes()) {
    auto index = std::make_unique<BplusTreeIndex>();
    RC   rc    = index->open(buffer_pool_manager_, index_file_path(dir_, meta_.table_name(), meta.index_name), meta,
                             meta_, create);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to open index. table=%s, index=%s, rc=%s",
                meta_.table_name().c_str(), meta.index_name.c_str(), strrc(rc));
      return rc;
    }
    indexes_.push_back(std::move(index));
  }
  return RC::SUCCESS;
}

RC Table::insert_record(Record &record)
{
  RC rc = record_manager_->insert_record(record);
  if (OB_FAIL(rc)) {
    return rc;
  }

  for (auto &index : indexes_) {
    rc = index->insert_entry(record.data(), record.rid());
    if (OB_FAIL(rc)) {
      /// 索引写不进去（例如唯一键冲突）就把刚插入的记录撤掉，保证表与索引一致
      record_manager_->delete_record(record.rid());
      return rc;
    }
  }
  return RC::SUCCESS;
}

RC Table::delete_record(const RID &rid)
{
  Record old;
  RC     rc = record_manager_->get_record(rid, old);
  if (OB_FAIL(rc)) {
    return rc;
  }

  for (auto &index : indexes_) {
    rc = index->delete_entry(old.data(), rid);
    if (OB_FAIL(rc) && rc != RC::RECORD_NOT_EXIST) {
      LOG_ERROR("failed to delete index entry. table=%s, index=%s, rc=%s",
                meta_.table_name().c_str(), index->index_meta().index_name.c_str(), strrc(rc));
      return rc;
    }
  }

  return record_manager_->delete_record(rid);
}

RC Table::update_record(const RID &rid, const Record &record)
{
  Record old;
  RC     rc = record_manager_->get_record(rid, old);
  if (OB_FAIL(rc)) {
    return rc;
  }

  for (auto &index : indexes_) {
    const ColumnMeta *column = index->index_column();
    if (memcmp(old.data() + column->offset(), record.data() + column->offset(), column->data_len()) == 0) {
      continue;  /// 被索引的列没变
    }

    rc = index->delete_entry(old.data(), rid);
    if (OB_FAIL(rc) && rc != RC::RECORD_NOT_EXIST) {
      return rc;
    }

    rc = index->insert_entry(record.data(), rid);
    if (OB_FAIL(rc)) {
      /// 新值违反唯一约束：把旧值和旧索引项还原回去
      index->insert_entry(old.data(), rid);
      record_manager_->update_record(rid, old);
      return rc;
    }
  }

  return record_manager_->update_record(rid, record);
}

RC Table::get_record(const RID &rid, Record &record) { return record_manager_->get_record(rid, record); }

RC Table::start_scan(RecordScanner &scanner) { return scanner.init(record_manager_.get()); }

RC Table::record_count(int64_t &count) { return record_manager_->get_record_count(count); }

RC Table::create_index(const IndexMeta &meta, bool replay_data)
{
  RC rc = meta_.add_index(meta);
  if (OB_FAIL(rc)) {
    return rc;
  }

  auto index = std::make_unique<BplusTreeIndex>();
  rc         = index->open(buffer_pool_manager_, index_file_path(dir_, meta_.table_name(), meta.index_name),
                           meta, meta_, true);
  if (OB_FAIL(rc)) {
    meta_.remove_index(meta.index_name);
    return rc;
  }

  if (replay_data) {
    /// 已有数据要重建索引内容；失败就把索引文件删掉，保持元数据一致
    RecordScanner scanner;
    rc = scanner.init(record_manager_.get());
    if (OB_FAIL(rc)) {
      meta_.remove_index(meta.index_name);
      return rc;
    }

    Record record;
    while (true) {
      rc = scanner.next(record);
      if (rc == RC::RECORD_EOF) {
        break;
      }
      if (OB_FAIL(rc)) {
        meta_.remove_index(meta.index_name);
        path_remove_file(index_file_path(dir_, meta_.table_name(), meta.index_name));
        return rc;
      }
      rc = index->insert_entry(record.data(), record.rid());
      if (OB_FAIL(rc)) {
        meta_.remove_index(meta.index_name);
        path_remove_file(index_file_path(dir_, meta_.table_name(), meta.index_name));
        return rc;
      }
    }
  }

  indexes_.push_back(std::move(index));
  return RC::SUCCESS;
}

Index *Table::index_by_name(const std::string &index_name) const
{
  for (const auto &index : indexes_) {
    if (index->index_meta().index_name == index_name) {
      return index.get();
    }
  }
  return nullptr;
}

Index *Table::index_of_field(const std::string &field_name) const
{
  for (const auto &index : indexes_) {
    if (index->index_meta().field_name == field_name) {
      return index.get();
    }
  }
  return nullptr;
}
