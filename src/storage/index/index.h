/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <string>

#include "common/rc.h"
#include "common/value.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/index/bplus_tree.h"
#include "storage/record/record.h"
#include "storage/table/table_meta.h"

/**
 * @brief 索引的通用接口
 * @details 与 miniob 一样，索引接口按「整条记录 + RID」收键：由索引自己从记录里取出被索引的列。
 */
class Index
{
public:
  virtual ~Index() = default;

  /// 从表元数据里解析出被索引列的偏移、长度与类型
  RC init(const IndexMeta &meta, const TableMeta &table_meta);

  const IndexMeta  &index_meta() const { return meta_; }
  const ColumnMeta *index_column() const { return column_; }

  virtual RC insert_entry(const char *record_data, const RID &rid) = 0;
  virtual RC delete_entry(const char *record_data, const RID &rid) = 0;
  /// low/high 为 nullptr 表示该方向不设界；值会先按列类型编码成键
  virtual RC make_scanner(BplusTreeScanner &scanner,
                          const Value *low, bool low_inclusive,
                          const Value *high, bool high_inclusive) const = 0;
  virtual RC entry_count(int64_t &count) const = 0;

protected:
  /// 从记录数据里取出定长的索引键
  std::string key_of(const char *record_data) const;

  IndexMeta          meta_;
  const ColumnMeta  *column_ = nullptr;
};

/// 用 B+Tree 实现的单列索引
class BplusTreeIndex : public Index
{
public:
  /// create=true 时新建索引文件与树；否则要求文件里已有匹配的树头
  RC open(DiskBufferPoolManager *buffer_pool_manager, const std::string &path,
          const IndexMeta &meta, const TableMeta &table_meta, bool create);

  RC insert_entry(const char *record_data, const RID &rid) override;
  RC delete_entry(const char *record_data, const RID &rid) override;
  RC make_scanner(BplusTreeScanner &scanner,
                  const Value *low, bool low_inclusive,
                  const Value *high, bool high_inclusive) const override;
  RC entry_count(int64_t &count) const override;

  BplusTree       &tree() { return tree_; }
  const BplusTree &tree() const { return tree_; }

private:
  BplusTree      tree_;
  DiskBufferPool *buffer_pool_ = nullptr;
};

/// 索引文件路径：<dir>/<table>.<index>.index
std::string index_file_path(const std::string &dir, const std::string &table_name, const std::string &index_name);
/// 数据文件路径：<dir>/<table>.heap
std::string heap_file_path(const std::string &dir, const std::string &table_name);
