/* minidb - 存储层一致性与健壮性自测：索引回滚、建索引收尾、并发计数、损坏页、记录大小上限 */
/*
 * 这个测试盯的是存储层的几条硬约定：
 *   1) INSERT 时第 2 个唯一索引冲突，第 1 个索引里已经写进去的项必须被撤回，
 *      否则会留下指向不存在记录的悬挂索引项（之后索引扫描取记录会直接报错）。
 *   2) UPDATE 改了多个被索引列，后一个索引冲突时，前面索引改动也必须撤回。
 *   3) 在有重复值的列上建 UNIQUE 索引失败后，索引文件/元数据/缓冲池都要干净，
 *      同名索引还能重新建成功。
 *   4) record_size 超过一页容量时必须在 open() 就报错（否则 insert 会无限扩文件）。
 *   5) 多线程同时插入同一张表，表头行数不能丢更新（扫描行数 == record_count == 总数）。
 *   6) 页头被读坏时（崩溃后的半写页）必须返回错误码，而不是按坏偏移越界写/越界读。
 */

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/file_util.h"
#include "common/log.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/db/db.h"
#include "storage/index/index.h"
#include "storage/record/record_manager.h"
#include "storage/table/table.h"

/// 一页能放下的最大记录：页头 + 至少一个槽位之后剩下的空间（测试自己算，不依赖生产常量名）
static constexpr int32_t kMaxRecordSize =
    BP_PAGE_SIZE - static_cast<int32_t>(sizeof(SlottedPageHeader)) - static_cast<int32_t>(sizeof(int32_t));

static const std::string kRoot   = "./_test_consistency";
static const std::string kDbName = "consistency";
static const std::string kDbDir  = path_join(kRoot, kDbName);

static std::vector<ColumnMeta> make_columns()
{
  std::vector<ColumnMeta> columns;
  ColumnMeta              id, name, score;
  assert(OB_SUCC(id.init("id", AttrType::INT, 0, 0)));
  assert(OB_SUCC(name.init("name", AttrType::CHARS, 16, 0)));
  assert(OB_SUCC(score.init("score", AttrType::INT, 0, 0)));
  columns.push_back(id);
  columns.push_back(name);
  columns.push_back(score);
  return columns;
}

static TableMeta make_user_meta()
{
  TableMeta meta;
  assert(OB_SUCC(meta.init("usr", make_columns())));
  return meta;
}

static TableMeta make_dup_meta()
{
  std::vector<ColumnMeta> columns;
  ColumnMeta              v;
  assert(OB_SUCC(v.init("v", AttrType::INT, 0, 0)));
  columns.push_back(v);

  TableMeta meta;
  assert(OB_SUCC(meta.init("dup", columns)));
  return meta;
}

static Record make_row(Table &table, int id, const char *name, int score)
{
  std::vector<Value> values;
  values.push_back(Value(id));
  values.push_back(Value(std::string(name)));
  values.push_back(Value(score));

  Record record;
  assert(OB_SUCC(table.table_meta().make_record(values, record)));
  return record;
}

static Record make_dup_row(Table &table, int v)
{
  std::vector<Value> values;
  values.push_back(Value(v));

  Record record;
  assert(OB_SUCC(table.table_meta().make_record(values, record)));
  return record;
}

static int count_rows(Table &table)
{
  RecordScanner scanner;
  assert(OB_SUCC(table.start_scan(scanner)));
  int    n = 0;
  Record record;
  while (true) {
    RC rc = scanner.next(record);
    if (rc == RC::RECORD_EOF) {
      break;
    }
    assert(OB_SUCC(rc));
    ++n;
  }
  return n;
}

static int64_t index_entries(Table &table, const std::string &index_name)
{
  Index *index = table.index_by_name(index_name);
  assert(index != nullptr);
  int64_t count = -1;
  assert(OB_SUCC(index->entry_count(count)));
  return count;
}

/// 用索引做一次范围扫描；必须 close() 释放树级共享锁，否则后续写操作会一直等锁
static std::vector<RID> scan_index(Table       &table,
                                   const std::string &index_name,
                                   const Value *low, bool low_inclusive,
                                   const Value *high, bool high_inclusive)
{
  Index *index = table.index_by_name(index_name);
  assert(index != nullptr);

  BplusTreeScanner scanner;
  assert(OB_SUCC(index->make_scanner(scanner, low, low_inclusive, high, high_inclusive)));

  std::vector<RID> rids;
  std::string      key;
  RID              rid;
  while (true) {
    RC rc = scanner.next(key, rid);
    if (rc == RC::RECORD_EOF) {
      break;
    }
    assert(OB_SUCC(rc));
    rids.push_back(rid);
  }
  assert(OB_SUCC(scanner.close()));
  return rids;
}

static int field_int(Table &table, const Record &record, const char *field)
{
  Value value;
  const ColumnMeta *column = table.table_meta().column(field);
  assert(column != nullptr);
  assert(OB_SUCC(table.table_meta().get_field_value(record, *column, value)));
  return value.get_int32();
}

static std::string field_str(Table &table, const Record &record, const char *field)
{
  Value value;
  const ColumnMeta *column = table.table_meta().column(field);
  assert(column != nullptr);
  assert(OB_SUCC(table.table_meta().get_field_value(record, *column, value)));
  return value.get_string();
}

// ---------------------------------------------------------------------------
// 1) 多索引 INSERT 失败必须完整回滚
// ---------------------------------------------------------------------------
static void test_insert_rollback(Db &db, Table &table)
{
  Record alice = make_row(table, 1, "alice", 10);
  assert(OB_SUCC(table.insert_record(alice)));

  /// id=2 是新主键（idx_id 会写成功），name='alice' 与已有行冲突（idx_name 失败）
  Record conflict = make_row(table, 2, "alice", 20);
  assert(table.insert_record(conflict) == RC::RECORD_DUPLICATE_KEY);

  assert(count_rows(table) == 1);
  assert(index_entries(table, "idx_id") == 1);    /// idx_id 里不能留下 id=2 的悬挂项
  assert(index_entries(table, "idx_name") == 1);

  const Value two(2);
  assert(scan_index(table, "idx_id", &two, true, &two, true).empty());

  const Value name("alice");
  std::vector<RID> hits = scan_index(table, "idx_name", &name, true, &name, true);
  assert(hits.size() == 1);

  Record fetched;
  assert(OB_SUCC(table.get_record(hits[0], fetched)));  /// 索引项必须还能取到记录
  assert(field_int(table, fetched, "id") == 1);
}

// ---------------------------------------------------------------------------
// 2) 多索引 UPDATE 失败必须把前面索引的改动也撤回
// ---------------------------------------------------------------------------
static void test_update_rollback(Db &db, Table &table)
{
  Record bob = make_row(table, 2, "bob", 30);
  assert(OB_SUCC(table.insert_record(bob)));

  /// 现在有 (1,alice) 和 (2,bob)。把 (1,alice) 改成 (5,bob)：
  /// idx_id 先改（1 -> 5）并成功，idx_name 冲突（bob 已存在）后失败。
  Table   *found = nullptr;
  assert(OB_SUCC(db.find_table("usr", &found)));
  assert(found == &table);

  Record old_row;
  const Value one(1);
  std::vector<RID> old_hits = scan_index(table, "idx_id", &one, true, &one, true);
  assert(old_hits.size() == 1);
  const RID rid = old_hits[0];
  assert(OB_SUCC(table.get_record(rid, old_row)));
  assert(field_str(table, old_row, "name") == "alice");

  Record updated = make_row(table, 5, "bob", 11);
  assert(table.update_record(rid, updated) == RC::RECORD_DUPLICATE_KEY);

  assert(count_rows(table) == 2);
  assert(index_entries(table, "idx_id") == 2);    /// idx_id 不能多出 id=5，也不能少掉 id=1
  assert(index_entries(table, "idx_name") == 2);

  const Value five(5);
  assert(scan_index(table, "idx_id", &five, true, &five, true).empty());
  assert(scan_index(table, "idx_id", &one, true, &one, true).size() == 1);

  Record after;
  assert(OB_SUCC(table.get_record(rid, after)));
  assert(field_int(table, after, "id") == 1);
  assert(field_str(table, after, "name") == "alice");
  assert(field_int(table, after, "score") == 10);
}

// ---------------------------------------------------------------------------
// 3) 建 UNIQUE 索引失败后：元数据、文件、WAL、缓冲池都要干净
// ---------------------------------------------------------------------------
static void test_create_index_rollback(Db &db)
{
  assert(OB_SUCC(db.create_table(make_dup_meta())));

  Table *dup = nullptr;
  assert(OB_SUCC(db.find_table("dup", &dup)));

  Record r1 = make_dup_row(*dup, 1);
  assert(OB_SUCC(dup->insert_record(r1)));
  Record r2 = make_dup_row(*dup, 1);
  assert(OB_SUCC(dup->insert_record(r2)));
  Record r3 = make_dup_row(*dup, 2);
  assert(OB_SUCC(dup->insert_record(r3)));

  /// 列 v 有重复值：重建唯一索引会在回放阶段冲突
  IndexMeta unique_index;
  unique_index.index_name = "idx_v";
  unique_index.field_name = "v";
  unique_index.unique     = true;
  assert(db.create_index("dup", unique_index) == RC::RECORD_DUPLICATE_KEY);

  assert(dup->index_by_name("idx_v") == nullptr);
  assert(dup->table_meta().indexes().empty());

  const std::string index_path = index_file_path(kDbDir, "dup", "idx_v");
  assert(!file_exist(index_path));              /// 失败的索引文件必须删掉
  assert(!file_exist(index_path + ".wal"));     /// 页级 WAL 也要清掉

  /// 同名索引必须还能建成功（说明文件/池/元数据都被清干净了）
  IndexMeta plain_index = unique_index;
  plain_index.unique    = false;
  assert(OB_SUCC(db.create_index("dup", plain_index)));
  assert(dup->index_by_name("idx_v") != nullptr);
  assert(index_entries(*dup, "idx_v") == 3);
}

// ---------------------------------------------------------------------------
// 4) record_size 上限：一页放不下就拒绝，不能死循环扩文件
// ---------------------------------------------------------------------------
static void test_record_size_limit()
{
  DiskBufferPoolManager bpm(32);
  const std::string     heap_path = path_join(kDbDir, "big.heap");
  assert(OB_SUCC(bpm.create_file(heap_path)));

  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(heap_path, ReadWriteMode::READ_WRITE, &pool)));

  HeapRecordManager too_big;
  assert(too_big.open(pool, kMaxRecordSize + 1, true) == RC::INVALID_ARGUMENT);

  /// 正好等于上限：一页只能放一条，第二条必须落新页
  HeapRecordManager manager;
  assert(OB_SUCC(manager.open(pool, kMaxRecordSize, true)));

  std::vector<char> bytes(static_cast<size_t>(kMaxRecordSize), 'x');

  Record first;
  assert(OB_SUCC(first.copy_data(bytes.data(), kMaxRecordSize)));
  assert(OB_SUCC(manager.insert_record(first)));

  const PageNum pages_after_first = pool->page_count();

  Record second;
  assert(OB_SUCC(second.copy_data(bytes.data(), kMaxRecordSize)));
  assert(OB_SUCC(manager.insert_record(second)));
  assert(pool->page_count() == pages_after_first + 1);

  int64_t count = 0;
  assert(OB_SUCC(manager.get_record_count(count)));
  assert(count == 2);

  assert(OB_SUCC(bpm.close()));
}

// ---------------------------------------------------------------------------
// 5) 并发插入：表头行数不能丢更新
// ---------------------------------------------------------------------------
static void test_concurrent_insert()
{
  const int         kRecordSize = 16;
  const std::string heap_path   = path_join(kDbDir, "concurrent.heap");

  DiskBufferPoolManager bpm(64);
  assert(OB_SUCC(bpm.create_file(heap_path)));

  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(heap_path, ReadWriteMode::READ_WRITE, &pool)));

  HeapRecordManager manager;
  assert(OB_SUCC(manager.open(pool, kRecordSize, true)));

  constexpr int kThreads   = 8;
  constexpr int kPerThread = 500;

  std::atomic<int>         failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&manager, &failures, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        char    data[kRecordSize] = {0};
        int32_t id                = t * kPerThread + i;
        memcpy(data, &id, sizeof(id));

        Record record;
        assert(OB_SUCC(record.copy_data(data, kRecordSize)));
        if (OB_FAIL(manager.insert_record(record))) {
          ++failures;
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  assert(failures.load() == 0);

  int64_t count = 0;
  assert(OB_SUCC(manager.get_record_count(count)));
  assert(count == kThreads * kPerThread);  /// 旧实现里并发读改写表头会把这数字写小

  /// 扫描行数必须与表头一致，且 RID 不能重复（槽位没有被两条记录共用）
  RecordScanner                        scanner;
  std::set<std::pair<PageNum, SlotNum>> rids;
  assert(OB_SUCC(scanner.init(&manager)));
  Record record;
  while (true) {
    RC rc = scanner.next(record);
    if (rc == RC::RECORD_EOF) {
      break;
    }
    assert(OB_SUCC(rc));
    rids.insert(std::make_pair(record.rid().page_num, record.rid().slot_num));
  }
  assert(rids.size() == static_cast<size_t>(kThreads * kPerThread));

  assert(OB_SUCC(bpm.close()));

  /// 重开：行数已经落盘，不能因为缓存没刷而丢
  DiskBufferPoolManager bpm2(64);
  DiskBufferPool       *pool2 = nullptr;
  assert(OB_SUCC(bpm2.open_file(heap_path, ReadWriteMode::READ_WRITE, &pool2)));

  HeapRecordManager reopened;
  assert(OB_SUCC(reopened.open(pool2, kRecordSize, false)));
  int64_t reopened_count = 0;
  assert(OB_SUCC(reopened.get_record_count(reopened_count)));
  assert(reopened_count == kThreads * kPerThread);

  assert(OB_SUCC(bpm2.close()));
}

// ---------------------------------------------------------------------------
// 6) 页头损坏：必须报错，而不是按坏偏移越界写
// ---------------------------------------------------------------------------
static void test_corrupted_page_header()
{
  const int         kRecordSize = 16;
  const std::string heap_path   = path_join(kDbDir, "corrupted.heap");

  DiskBufferPoolManager bpm(32);
  assert(OB_SUCC(bpm.create_file(heap_path)));

  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(heap_path, ReadWriteMode::READ_WRITE, &pool)));

  HeapRecordManager manager;
  assert(OB_SUCC(manager.open(pool, kRecordSize, true)));

  char   data[kRecordSize] = {0};
  Record first;
  assert(OB_SUCC(first.copy_data(data, kRecordSize)));
  assert(OB_SUCC(manager.insert_record(first)));

  /// 把第 2 页的页头改成离谱值：模拟崩溃/位翻转留下的半写页
  {
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_write(RECORD_FIRST_DATA_PAGE, handle)));
    SlottedPageHeader *header = page_header_of(handle.value());
    header->free_start        = 0x7FFFFFF0;
    header->record_count      = 1 << 30;
    assert(OB_SUCC(handle.mark_dirty()));
  }

  Record next;
  assert(OB_SUCC(next.copy_data(data, kRecordSize)));
  assert(manager.insert_record(next) == RC::BUFFERPOOL_CORRUPTED);

  Record probe;
  assert(manager.get_record(RID(RECORD_FIRST_DATA_PAGE, 0), probe) == RC::BUFFERPOOL_CORRUPTED);

  RecordScanner scanner;
  assert(OB_SUCC(scanner.init(&manager)));
  Record scanned;
  assert(scanner.next(scanned) == RC::BUFFERPOOL_CORRUPTED);

  assert(OB_SUCC(bpm.close()));
}

// ---------------------------------------------------------------------------
// 7) 槽位复用要真正回收记录体空间：挖空的页还能原位插回去，文件不该变长
// ---------------------------------------------------------------------------
static void test_slot_reuse_reclaims_space()
{
  const int         kRecordSize = 16;
  const std::string heap_path   = path_join(kDbDir, "reuse.heap");

  DiskBufferPoolManager bpm(32);
  assert(OB_SUCC(bpm.create_file(heap_path)));

  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(heap_path, ReadWriteMode::READ_WRITE, &pool)));

  HeapRecordManager manager;
  assert(OB_SUCC(manager.open(pool, kRecordSize, true)));

  /// 一页能放多少条：页头 16 字节 + 每条 (16 字节记录 + 4 字节槽位)
  const int kPerPage = (BP_PAGE_SIZE - static_cast<int>(sizeof(SlottedPageHeader))) /
                       (kRecordSize + static_cast<int>(sizeof(int32_t)));
  assert(kPerPage == 408);

  std::vector<RID> rids;
  for (int i = 0; i < kPerPage * 3; ++i) {
    char    data[kRecordSize] = {0};
    int32_t id                = i;
    memcpy(data, &id, sizeof(id));

    Record record;
    assert(OB_SUCC(record.copy_data(data, kRecordSize)));
    assert(OB_SUCC(manager.insert_record(record)));
    rids.push_back(record.rid());
  }

  const PageNum pages_before = pool->page_count();
  assert(pages_before == 5);  /// 页 0（缓冲池）+ 页 1（表头）+ 3 个数据页

  /// 把第 2 页挖出 100 个空槽（记录体空间还占着）
  int deleted = 0;
  for (const RID &rid : rids) {
    if (rid.page_num != RECORD_FIRST_DATA_PAGE || deleted >= 100) {
      continue;
    }
    assert(OB_SUCC(manager.delete_record(rid)));
    ++deleted;
  }
  assert(deleted == 100);

  /// 再插 100 条：必须原地复用第 2 页的空槽，而不是去扩新页
  for (int i = 0; i < 100; ++i) {
    char    data[kRecordSize] = {0};
    int32_t id                = 100000 + i;
    memcpy(data, &id, sizeof(id));

    Record record;
    assert(OB_SUCC(record.copy_data(data, kRecordSize)));
    assert(OB_SUCC(manager.insert_record(record)));
    assert(record.rid().page_num == RECORD_FIRST_DATA_PAGE);
  }

  assert(pool->page_count() == pages_before);

  int64_t count = 0;
  assert(OB_SUCC(manager.get_record_count(count)));
  assert(count == static_cast<int64_t>(kPerPage) * 3);

  assert(OB_SUCC(bpm.close()));
}

// ---------------------------------------------------------------------------
// 8) 取字段时越界要报错：记录缓冲区比列范围还短时不能照读
// ---------------------------------------------------------------------------
static void test_field_value_bounds()
{
  const TableMeta meta = make_user_meta();

  char   short_data[8] = {0};
  Record short_record;
  assert(OB_SUCC(short_record.copy_data(short_data, 8)));

  Value value;
  /// score 列在 offset 20 之后，8 字节的缓冲区装不下
  assert(meta.get_field_value(short_record, *meta.column("score"), value) == RC::INVALID_ARGUMENT);
  assert(meta.field_data(short_record, *meta.column("score")) == nullptr);

  /// id 列落在前 8 个字节里，仍然可以正常读
  assert(OB_SUCC(meta.get_field_value(short_record, *meta.column("id"), value)));
  assert(value.get_int32() == 0);
}

static void setup_database(Db **db)
{
  std::error_code ec;
  std::filesystem::remove_all(kRoot, ec);  /// 上一轮残留（索引文件/目录）全部清掉
  assert(OB_SUCC(path_mkdir_recursive(kRoot)));

  assert(OB_SUCC(Db::create(kRoot, kDbName, db)));
}

int main()
{
  Db *db = nullptr;
  setup_database(&db);
  assert(OB_SUCC(db->create_table(make_user_meta())));

  Table *table = nullptr;
  assert(OB_SUCC(db->find_table("usr", &table)));

  IndexMeta id_index;
  id_index.index_name = "idx_id";
  id_index.field_name = "id";
  id_index.unique     = true;
  assert(OB_SUCC(db->create_index("usr", id_index)));

  IndexMeta name_index;
  name_index.index_name = "idx_name";
  name_index.field_name = "name";
  name_index.unique     = true;
  assert(OB_SUCC(db->create_index("usr", name_index)));

  test_insert_rollback(*db, *table);
  test_update_rollback(*db, *table);
  test_create_index_rollback(*db);
  test_record_size_limit();
  test_concurrent_insert();
  test_corrupted_page_header();
  test_slot_reuse_reclaims_space();
  test_field_value_bounds();

  assert(OB_SUCC(db->close()));
  delete db;

  /// 关掉再打开：失败过的写不能被持久化成脏数据
  assert(OB_SUCC(Db::open(kRoot, kDbName, &db)));
  Table *reopened = nullptr;
  assert(OB_SUCC(db->find_table("usr", &reopened)));
  assert(count_rows(*reopened) == 2);
  assert(index_entries(*reopened, "idx_id") == 2);

  const Value five(5);
  assert(scan_index(*reopened, "idx_id", &five, true, &five, true).empty());

  assert(OB_SUCC(db->close()));
  delete db;

  printf("test_storage_consistency: OK\n");
  return 0;
}
