/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/rc.h"
#include "storage/index/index.h"
#include "storage/record/record_manager.h"
#include "storage/record/record_scanner.h"
#include "storage/table/table_meta.h"

/**
 * @brief 一张表：元数据 + 堆文件 + 该表上的所有索引
 * @details 对应 miniob 的 Table。写操作会同步维护索引，唯一索引冲突会返回
 *          RC::RECORD_DUPLICATE_KEY。
 */
class Table
{
public:
  Table() = default;
  ~Table();

  /// create=true 时新建数据文件与索引文件
  RC open(DiskBufferPoolManager *buffer_pool_manager, const std::string &dir, const TableMeta &meta, bool create);

  const TableMeta &table_meta() const { return meta_; }
  TableMeta       &table_meta() { return meta_; }
  const std::string &table_name() const { return meta_.table_name(); }

  HeapRecordManager &record_manager() { return *record_manager_; }

  /// 插入一条记录（回填 rid，并维护所有索引）
  RC insert_record(Record &record);
  RC delete_record(const RID &rid);
  /// 定长记录原地更新；如果被索引列变了，索引里先删后插
  RC update_record(const RID &rid, const Record &record);
  RC get_record(const RID &rid, Record &record);

  RC create_index(const IndexMeta &meta, bool replay_data);
  Index *index_by_name(const std::string &index_name) const;
  Index *index_of_field(const std::string &field_name) const;
  const std::vector<std::unique_ptr<Index>> &indexes() const { return indexes_; }

  RC start_scan(RecordScanner &scanner);
  RC record_count(int64_t &count);

  const std::string &dir() const { return dir_; }

private:
  RC open_indexes(bool create);

private:
  TableMeta                            meta_;
  std::string                          dir_;
  DiskBufferPoolManager               *buffer_pool_manager_ = nullptr;
  std::unique_ptr<HeapRecordManager>   record_manager_;
  std::vector<std::unique_ptr<Index>>  indexes_;
};
