/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/db/db.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "common/binary.h"
#include "common/file_util.h"
#include "common/log.h"

static constexpr int32_t CATALOG_MAGIC   = 0x4D444243;  /// 'MDBC'
static constexpr int32_t CATALOG_VERSION = 1;

std::string catalog_file_path(const std::string &db_path) { return path_join(db_path, "catalog.bin"); }

Db::~Db() { close(); }

RC Db::init(const std::string &data_dir, const std::string &db_name)
{
  db_name_ = db_name;
  db_path_ = path_join(data_dir, db_name);

  RC rc = path_mkdir_recursive(db_path_);
  if (OB_FAIL(rc)) {
    LOG_ERROR("failed to create db directory. path=%s, rc=%s", db_path_.c_str(), strrc(rc));
    return rc;
  }

  /// 每个表同时打开数据文件和索引文件，帧数给得比 miniob 小一些也够用
  buffer_pool_manager_ = std::make_unique<DiskBufferPoolManager>(32);
  return RC::SUCCESS;
}

RC Db::create(const std::string &data_dir, const std::string &db_name, Db **out)
{
  if (db_name.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  const std::string path = path_join(data_dir, db_name);
  if (file_exist(catalog_file_path(path))) {
    return RC::SCHEMA_DB_EXIST;
  }

  std::unique_ptr<Db> db(new Db());
  RC                  rc = db->init(data_dir, db_name);
  if (OB_FAIL(rc)) {
    return rc;
  }

  rc = db->write_catalog();
  if (OB_FAIL(rc)) {
    return rc;
  }

  *out = db.release();
  return RC::SUCCESS;
}

RC Db::open(const std::string &data_dir, const std::string &db_name, Db **out)
{
  std::unique_ptr<Db> db(new Db());
  RC                  rc = db->init(data_dir, db_name);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (!file_exist(catalog_file_path(db->db_path_))) {
    LOG_WARN("db not found. db=%s, path=%s", db_name.c_str(), db->db_path_.c_str());
    return RC::SCHEMA_DB_NOT_EXIST;
  }

  rc = db->load_catalog();
  if (OB_FAIL(rc)) {
    return rc;
  }

  *out = db.release();
  return RC::SUCCESS;
}

RC Db::load_catalog()
{
  const std::string path = catalog_file_path(db_path_);
  std::ifstream     in(path, std::ios::binary);
  if (!in) {
    return RC::FILE_NOT_EXIST;
  }

  std::vector<char> buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (buffer.size() < sizeof(int32_t) * 3) {
    return RC::IOERR_READ;
  }

  BinaryReader reader(reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size());
  if (reader.get_i32() != CATALOG_MAGIC || reader.get_i32() != CATALOG_VERSION) {
    LOG_ERROR("invalid catalog file. path=%s", path.c_str());
    return RC::IOERR_READ;
  }

  const int32_t count = reader.get_i32();
  if (!reader.ok() || count < 0 || count > 65536) {
    return RC::IOERR_READ;
  }

  tables_.clear();
  for (int32_t i = 0; i < count; ++i) {
    TableMeta meta;
    RC        rc = TableMeta::deserialize(reader, meta);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to parse table meta in catalog. table_index=%d, rc=%s", i, strrc(rc));
      return rc;
    }

    auto table = std::make_unique<Table>();
    rc         = table->open(buffer_pool_manager_.get(), db_path_, meta, false);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to open table from catalog. table=%s, rc=%s", meta.table_name().c_str(), strrc(rc));
      return rc;
    }
    tables_.push_back(std::move(table));
  }

  return reader.ok() ? RC::SUCCESS : RC::IOERR_READ;
}

RC Db::write_catalog()
{
  BinaryWriter writer;
  writer.put_i32(CATALOG_MAGIC);
  writer.put_i32(CATALOG_VERSION);
  writer.put_i32(static_cast<int32_t>(tables_.size()));

  for (const auto &table : tables_) {
    table->table_meta().serialize(writer);
  }

  const std::string path = catalog_file_path(db_path_);
  const std::string temp = path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      LOG_ERROR("failed to open catalog for write. path=%s", temp.c_str());
      return RC::FILE_OPEN;
    }
    if (!writer.data().empty()) {
      out.write(reinterpret_cast<const char *>(writer.data().data()),
                static_cast<std::streamsize>(writer.data().size()));
    }
    out.flush();
    if (!out) {
      return RC::IOERR_WRITE;
    }
  }

  /// 先写临时文件再改名，避免目录文件被写坏后整个库打不开
  if (file_exist(path) && remove(path.c_str()) != 0) {
    return RC::FILE_REMOVE;
  }
  if (rename(temp.c_str(), path.c_str()) != 0) {
    return RC::IOERR_WRITE;
  }
  return RC::SUCCESS;
}

RC Db::create_table(const TableMeta &meta)
{
  if (find_table_or_null(meta.table_name()) != nullptr) {
    return RC::SCHEMA_TABLE_EXIST;
  }

  auto table = std::make_unique<Table>();
  RC   rc    = table->open(buffer_pool_manager_.get(), db_path_, meta, true);
  if (OB_FAIL(rc)) {
    return rc;
  }

  tables_.push_back(std::move(table));

  rc = write_catalog();
  if (OB_FAIL(rc)) {
    LOG_ERROR("created table but failed to persist catalog. table=%s", meta.table_name().c_str());
    return rc;
  }
  return RC::SUCCESS;
}

RC Db::drop_table(const std::string &table_name)
{
  auto iter = std::find_if(tables_.begin(), tables_.end(),
                           [&table_name](const std::unique_ptr<Table> &t) { return t->table_name() == table_name; });
  if (iter == tables_.end()) {
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  const TableMeta meta = (*iter)->table_meta();
  std::vector<std::string> files{heap_file_path(db_path_, table_name)};
  for (const IndexMeta &index : meta.indexes()) {
    files.push_back(index_file_path(db_path_, table_name, index.index_name));
  }

  /// 先丢掉表对象（会关掉记录管理器），再关闭缓冲池里的文件，最后删除磁盘文件
  tables_.erase(iter);
  for (const std::string &file : files) {
    buffer_pool_manager_->close_file(file);
    path_remove_file(file);
    /// v2 buffer manager keeps a page-level redo WAL next to every file.
    path_remove_file(file + ".wal");
  }

  return write_catalog();
}

RC Db::create_index(const std::string &table_name, const IndexMeta &meta)
{
  Table *table = nullptr;
  RC     rc    = find_table(table_name, &table);
  if (OB_FAIL(rc)) {
    return rc;
  }

  rc = table->create_index(meta, true);
  if (OB_FAIL(rc)) {
    return rc;
  }

  rc = write_catalog();
  if (OB_FAIL(rc)) {
    return rc;
  }
  return buffer_pool_manager_->flush_all();
}

RC Db::find_table(const std::string &table_name, Table **table)
{
  Table *found = find_table_or_null(table_name);
  if (found == nullptr) {
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }
  *table = found;
  return RC::SUCCESS;
}

Table *Db::find_table_or_null(const std::string &table_name)
{
  for (const auto &table : tables_) {
    if (table->table_name() == table_name) {
      return table.get();
    }
  }
  return nullptr;
}

RC Db::flush() { return buffer_pool_manager_->flush_all(); }

RC Db::close()
{
  RC rc = flush();
  if (OB_FAIL(rc)) {
    LOG_ERROR("failed to flush db on close. db=%s, rc=%s", db_name_.c_str(), strrc(rc));
  }

  tables_.clear();
  if (buffer_pool_manager_) {
    RC one = buffer_pool_manager_->close();
    if (OB_FAIL(one)) {
      rc = one;
    }
  }
  return rc;
}
