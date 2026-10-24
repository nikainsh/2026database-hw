/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/rc.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"

/**
 * @brief 一个数据库（一组表 + 一个目录文件 + 一个缓冲池管理器）
 * @details 对应 miniob 的 Db。目录文件 catalog.bin 用二进制保存所有表的元数据，
 *          表数据是 <table>.heap，索引是 <table>.<index>.index，全部平铺在同一个目录下。
 */
class Db
{
public:
  ~Db();

  /// 建库：创建目录并写出空目录文件；如果库已存在返回 RC::SCHEMA_DB_EXIST
  static RC create(const std::string &data_dir, const std::string &db_name, Db **out);
  /// 打开一个已存在的库（表元数据全部加载，数据文件按需打开）
  static RC open(const std::string &data_dir, const std::string &db_name, Db **out);

  RC flush();  /// 把所有脏页写回磁盘
  RC close();  /// flush 后关闭全部文件

  RC create_table(const TableMeta &meta);
  RC drop_table(const std::string &table_name);
  /// 表必须已存在；找不到返回 RC::SCHEMA_TABLE_NOT_EXIST
  RC find_table(const std::string &table_name, Table **table);
  RC create_index(const std::string &table_name, const IndexMeta &meta);

  Table *find_table_or_null(const std::string &table_name);
  const std::vector<std::unique_ptr<Table>> &tables() const { return tables_; }

  const std::string &name() const { return db_name_; }
  const std::string &path() const { return db_path_; }
  DiskBufferPoolManager &buffer_pool_manager() { return *buffer_pool_manager_; }

private:
  Db() = default;

  RC init(const std::string &data_dir, const std::string &db_name);
  RC load_catalog();
  RC write_catalog();

  std::string                       db_name_;
  std::string                       db_path_;
  std::unique_ptr<DiskBufferPoolManager> buffer_pool_manager_;
  std::vector<std::unique_ptr<Table>>    tables_;
};
