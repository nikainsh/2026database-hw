/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/table/table.h"

#include <cstring>

#include "common/file_util.h"
#include "common/log.h"

namespace
{
/**
 * @brief 一次表级写操作的「索引撤销日志」
 * @details 一条记录可能落在多个索引里（主键 + 若干唯一/普通索引），写操作中途失败时，
 *          只回滚「当前这个索引」是不够的：前面的索引已经被改过了。这里按发生顺序记录
 *          <索引, RID, 记录快照>，失败时逆序撤销，保证堆文件与索引一致。
 *          记录是定长的，快照只有 record_size 字节，代价可以忽略。
 */
class IndexChangeLog
{
public:
  /// 已经向 index 插入了一条索引项；撤销 = 删掉它
  void on_insert(Index *index, const char *record_data, int record_size, const RID &rid)
  {
    append(index, record_data, record_size, rid, false);
  }

  /// 已经从 index 删掉了一条索引项；撤销 = 用旧记录把旧键重新插回去
  void on_delete(Index *index, const char *record_data, int record_size, const RID &rid)
  {
    append(index, record_data, record_size, rid, true);
  }

  /// 逆序撤销；单条失败只记日志，尽量把能撤的都撤掉，不掩盖最初的错误
  void rollback()
  {
    for (auto it = actions_.rbegin(); it != actions_.rend(); ++it) {
      const RC rc = it->undo_is_insert ? it->index->insert_entry(it->record.data(), it->rid)
                                       : it->index->delete_entry(it->record.data(), it->rid);
      if (OB_FAIL(rc) && rc != RC::RECORD_NOT_EXIST) {
        LOG_ERROR("failed to rollback index entry. index=%s, rid=%s, rc=%s",
                  it->index->index_meta().index_name.c_str(), it->rid.to_string().c_str(), strrc(rc));
      }
    }
    actions_.clear();
  }

private:
  void append(Index *index, const char *record_data, int record_size, const RID &rid, bool undo_is_insert)
  {
    Action action;
    action.index          = index;
    action.rid            = rid;
    action.record.assign(record_data, record_data + record_size);
    action.undo_is_insert = undo_is_insert;
    actions_.push_back(std::move(action));
  }

  struct Action
  {
    Index      *index          = nullptr;
    RID         rid;
    std::string record;                  /// 记录快照，撤销时用它重新算出索引键
    bool        undo_is_insert = false;  /// true 表示撤销动作是「重新插入旧条目」
  };

  std::vector<Action> actions_;
};
}  // namespace

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

  IndexChangeLog changes;
  for (auto &index : indexes_) {
    rc = index->insert_entry(record.data(), record.rid());
    if (OB_FAIL(rc)) {
      /// 索引写不进去（例如唯一键冲突）：先把已经写成功的索引项逆序撤掉，再撤堆记录，
      /// 否则前面的索引会留下指向不存在记录的悬挂项（之后索引扫描取记录会直接报错）
      changes.rollback();
      const RC undo = record_manager_->delete_record(record.rid());
      if (OB_FAIL(undo)) {
        LOG_ERROR("failed to rollback record after index failure. table=%s, rid=%s, rc=%s",
                  meta_.table_name().c_str(), record.rid().to_string().c_str(), strrc(undo));
      }
      return rc;
    }
    changes.on_insert(index.get(), record.data(), record.len(), record.rid());
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

  IndexChangeLog changes;
  for (auto &index : indexes_) {
    rc = index->delete_entry(old.data(), rid);
    if (OB_FAIL(rc) && rc != RC::RECORD_NOT_EXIST) {
      LOG_ERROR("failed to delete index entry. table=%s, index=%s, rc=%s",
                meta_.table_name().c_str(), index->index_meta().index_name.c_str(), strrc(rc));
      /// 已经删掉的索引项要加回去，否则记录还在、索引项却没了
      changes.rollback();
      return rc;
    }
    if (OB_SUCC(rc)) {
      changes.on_delete(index.get(), old.data(), old.len(), rid);
    }
  }

  rc = record_manager_->delete_record(rid);
  if (OB_FAIL(rc)) {
    changes.rollback();
    return rc;
  }
  return RC::SUCCESS;
}

RC Table::update_record(const RID &rid, const Record &record)
{
  if (record.len() != meta_.record_size()) {
    return RC::INVALID_ARGUMENT;
  }

  Record old;
  RC     rc = record_manager_->get_record(rid, old);
  if (OB_FAIL(rc)) {
    return rc;
  }

  IndexChangeLog changes;
  for (auto &index : indexes_) {
    const ColumnMeta *column = index->index_column();
    if (column == nullptr) {
      continue;
    }
    if (memcmp(old.data() + column->offset(), record.data() + column->offset(), column->data_len()) == 0) {
      continue;  /// 被索引的列没变
    }

    rc = index->delete_entry(old.data(), rid);
    if (OB_FAIL(rc) && rc != RC::RECORD_NOT_EXIST) {
      changes.rollback();
      return rc;
    }
    if (OB_SUCC(rc)) {
      changes.on_delete(index.get(), old.data(), old.len(), rid);
    }

    rc = index->insert_entry(record.data(), rid);
    if (OB_FAIL(rc)) {
      /// 新值违反唯一约束：把本索引刚删掉的旧项、以及前面索引已经改成新值的项全部还原
      changes.rollback();
      return rc;
    }
    changes.on_insert(index.get(), record.data(), record.len(), rid);
  }

  rc = record_manager_->update_record(rid, record);
  if (OB_FAIL(rc)) {
    changes.rollback();
    return rc;
  }
  return RC::SUCCESS;
}

RC Table::get_record(const RID &rid, Record &record) { return record_manager_->get_record(rid, record); }

RC Table::start_scan(RecordScanner &scanner) { return scanner.init(record_manager_.get()); }

RC Table::record_count(int64_t &count) { return record_manager_->get_record_count(count); }

RC Table::ensure_index_stats()
{
  for (const IndexMeta &meta : meta_.indexes()) {
    Index             *index = index_by_name(meta.index_name);
    if (index == nullptr) {
      continue;
    }

    const IndexStats *existing = meta_.index_stats(meta.index_name);
    if (existing != nullptr && existing->valid) {
      /// 条目数是 O(1) 的实时值；NDV 与最小最大键沿用上次采集（本实现不做自动失效）
      IndexStats fresh  = *existing;
      int64_t    entries = 0;
      if (OB_SUCC(index->entry_count(entries))) {
        fresh.entries = entries;
        meta_.set_index_stats(meta.index_name, fresh);
      }
      continue;
    }

    IndexStats stats;
    RC         rc = index->collect_stats(stats);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to collect index stats. table=%s, index=%s, rc=%s",
               meta_.table_name().c_str(), meta.index_name.c_str(), strrc(rc));
      return rc;
    }
    meta_.set_index_stats(meta.index_name, stats);
  }
  return RC::SUCCESS;
}

/**
 * @brief 建索引失败时的收尾：元数据、缓冲池里的文件、磁盘文件（含页级 WAL）一起清掉
 * @details 只 path_remove_file 是不够的：文件还被 DiskBufferPool 打开着，Windows 上删不掉，
 *          残留下次会被当成「已存在的索引文件」打开，留下脏数据；同时池和帧也不会释放。
 */
RC Table::rollback_index_creation(const std::string &index_name)
{
  meta_.remove_index(index_name);

  const std::string path = index_file_path(dir_, meta_.table_name(), index_name);
  const RC          close_rc = buffer_pool_manager_->close_file(path);
  if (OB_FAIL(close_rc) && close_rc != RC::FILE_NOT_EXIST) {
    LOG_WARN("failed to close index file while rolling back. path=%s, rc=%s", path.c_str(), strrc(close_rc));
  }

  const RC remove_rc = path_remove_file(path);
  if (OB_FAIL(remove_rc) && remove_rc != RC::FILE_NOT_EXIST) {
    LOG_WARN("failed to remove index file while rolling back. path=%s, rc=%s", path.c_str(), strrc(remove_rc));
  }
  path_remove_file(path + ".wal");  /// 缓冲池给每个文件配一个页级 WAL
  return RC::SUCCESS;
}

RC Table::create_index(const IndexMeta &meta, bool replay_data)
{
  RC rc = meta_.add_index(meta);
  if (OB_FAIL(rc)) {
    return rc;
  }

  auto        index = std::make_unique<BplusTreeIndex>();
  const std::string index_path = index_file_path(dir_, meta_.table_name(), meta.index_name);
  rc = index->open(buffer_pool_manager_, index_path, meta, meta_, true);
  if (OB_FAIL(rc)) {
    /// 打开阶段就可能已经建了文件/占了缓冲池，同样要收尾
    index.reset();
    rollback_index_creation(meta.index_name);
    return rc;
  }

  if (replay_data) {
    /// 已有数据要重建索引内容；失败就把索引文件删掉，保持元数据一致
    RecordScanner scanner;
    rc = scanner.init(record_manager_.get());
    if (OB_FAIL(rc)) {
      index.reset();
      rollback_index_creation(meta.index_name);
      return rc;
    }

    Record record;
    while (true) {
      rc = scanner.next(record);
      if (rc == RC::RECORD_EOF) {
        break;
      }
      if (OB_FAIL(rc)) {
        index.reset();
        rollback_index_creation(meta.index_name);
        return rc;
      }
      rc = index->insert_entry(record.data(), record.rid());
      if (OB_FAIL(rc)) {
        index.reset();
        rollback_index_creation(meta.index_name);
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
