/* minidb - 索引统计（CBO 用）自测：增量维护、失效重采、重启复用 */
/*
 * IndexStats 是优化器做代价估算的输入，它必须满足：
 *   1) 空表时合法但条目数为 0（min/max 为空），不会让优化器算错。
 *   2) 插入后 min/max 由 O(1) 的增量维护更新（不需要每次 DML 都扫叶子链）。
 *   3) 删除非极值不影响 min/max；删掉极值时统计标记失效，下次规划重采后给出新极值。
 *   4) 唯一索引的 NDV 能随插入/删除精确变化；非唯一索引的 NDV 由重采样修正。
 *   5) 统计随 catalog 在 close() 时落盘，重开数据库后直接复用，不必重新扫索引。
 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_util.h"
#include "common/log.h"
#include "storage/db/db.h"
#include "storage/index/index.h"
#include "storage/table/table.h"

static const std::string kRoot   = "./_test_index_stats";
static const std::string kDbName = "statsby";
static const std::string kDbDir  = path_join(kRoot, kDbName);
static const std::string kTable  = "stats_t";

static TableMeta make_meta()
{
  std::vector<ColumnMeta> columns;
  ColumnMeta              id, cat;
  assert(OB_SUCC(id.init("id", AttrType::INT, 0, 0)));
  assert(OB_SUCC(cat.init("cat", AttrType::INT, 0, 0)));
  columns.push_back(id);
  columns.push_back(cat);

  TableMeta meta;
  assert(OB_SUCC(meta.init(kTable, columns)));
  return meta;
}

static void insert_row(Table &table, int id, int cat)
{
  std::vector<Value> values;
  values.push_back(Value(id));
  values.push_back(Value(cat));

  Record record;
  assert(OB_SUCC(table.table_meta().make_record(values, record)));
  assert(OB_SUCC(table.insert_record(record)));
}

static RID rid_by_id(Table &table, int id)
{
  Index *index = table.index_by_name("idx_id");
  assert(index != nullptr);

  Value            key(id);
  BplusTreeScanner scanner;
  assert(OB_SUCC(index->make_scanner(scanner, &key, true, &key, true)));

  std::string scanned_key;
  RID         rid;
  assert(OB_SUCC(scanner.next(scanned_key, rid)));
  assert(OB_SUCC(scanner.close()));
  return rid;
}

static void delete_row(Table &table, int id)
{
  const RID rid = rid_by_id(table, id);
  assert(OB_SUCC(table.delete_record(rid)));
}

static IndexStats stats_of(Table &table, const char *index_name)
{
  const IndexStats *stats = table.table_meta().index_stats(index_name);
  assert(stats != nullptr);
  return *stats;
}

static int decoded_key(Table &table, const char *index_name, const std::string &key)
{
  Index *index = table.index_by_name(index_name);
  assert(index != nullptr);

  Value value;
  assert(OB_SUCC(index->decode_key(key, value)));
  return value.get_int32();
}

int main()
{
  std::error_code ec;
  std::filesystem::remove_all(kRoot, ec);
  assert(OB_SUCC(path_mkdir_recursive(kRoot)));

  // ---- 建库建表建索引 ----
  Db *db = nullptr;
  assert(OB_SUCC(Db::create(kRoot, kDbName, &db)));
  assert(OB_SUCC(db->create_table(make_meta())));

  Table *table = nullptr;
  assert(OB_SUCC(db->find_table(kTable, &table)));

  IndexMeta id_index;
  id_index.index_name = "idx_id";
  id_index.field_name = "id";
  id_index.unique     = true;
  assert(OB_SUCC(db->create_index(kTable, id_index)));

  IndexMeta cat_index;
  cat_index.index_name = "idx_cat";
  cat_index.field_name = "cat";
  cat_index.unique     = false;
  assert(OB_SUCC(db->create_index(kTable, cat_index)));

  // ---- 1) 空表：统计合法但为 0 ----
  assert(OB_SUCC(table->ensure_index_stats()));
  {
    const IndexStats stats = stats_of(*table, "idx_id");
    assert(stats.valid);
    assert(stats.entries == 0);
    assert(stats.distinct_keys == 0);
    assert(stats.min_key.empty() && stats.max_key.empty());
  }

  // ---- 2) 插入 500 行（id 1..500，cat 0..9）：collect 出正确的 min/max/NDV ----
  for (int i = 1; i <= 500; ++i) {
    insert_row(*table, i, i % 10);
  }
  assert(OB_SUCC(table->ensure_index_stats()));
  {
    const IndexStats stats = stats_of(*table, "idx_id");
    assert(stats.valid);
    assert(stats.entries == 500);
    assert(stats.distinct_keys == 500);  /// 唯一索引，键互不相同
    assert(decoded_key(*table, "idx_id", stats.min_key) == 1);
    assert(decoded_key(*table, "idx_id", stats.max_key) == 500);

    const IndexStats cat = stats_of(*table, "idx_cat");
    assert(cat.valid);
    assert(cat.entries == 500);
    assert(cat.distinct_keys == 10);  /// cat 只有 0..9 十个值
    assert(decoded_key(*table, "idx_cat", cat.min_key) == 0);
    assert(decoded_key(*table, "idx_cat", cat.max_key) == 9);
  }

  // ---- 3) 插入新极值：min/max/NDV 靠 O(1) 增量维护跟上（不重采也必须对） ----
  insert_row(*table, 0, 5);
  insert_row(*table, 1000, 5);
  assert(OB_SUCC(table->ensure_index_stats()));
  {
    const IndexStats stats = stats_of(*table, "idx_id");
    assert(stats.entries == 502);
    assert(stats.distinct_keys == 502);
    assert(decoded_key(*table, "idx_id", stats.min_key) == 0);
    assert(decoded_key(*table, "idx_id", stats.max_key) == 1000);
  }

  // ---- 4) 删除非极值：min/max 不变，条目数与 NDV 各减 1 ----
  delete_row(*table, 250);
  assert(OB_SUCC(table->ensure_index_stats()));
  {
    const IndexStats stats = stats_of(*table, "idx_id");
    assert(stats.valid);
    assert(stats.entries == 501);
    assert(stats.distinct_keys == 501);
    assert(decoded_key(*table, "idx_id", stats.min_key) == 0);
    assert(decoded_key(*table, "idx_id", stats.max_key) == 1000);
  }

  // ---- 5) 删掉最小键：统计必须失效，重采后给出新最小键 ----
  delete_row(*table, 0);
  {
    const IndexStats stats = stats_of(*table, "idx_id");
    assert(!stats.valid);  /// 极值被删，旧 min_key 已不可信
  }
  assert(OB_SUCC(table->ensure_index_stats()));
  {
    const IndexStats stats = stats_of(*table, "idx_id");
    assert(stats.valid);
    assert(stats.entries == 500);
    assert(stats.distinct_keys == 500);
    assert(decoded_key(*table, "idx_id", stats.min_key) == 1);
    assert(decoded_key(*table, "idx_id", stats.max_key) == 1000);
  }

  // ---- 6) 非唯一索引的新极值：插入后 max 立刻更新，NDV 不因重复值乱涨 ----
  insert_row(*table, 5000, 99);
  assert(OB_SUCC(table->ensure_index_stats()));
  {
    const IndexStats cat = stats_of(*table, "idx_cat");
    assert(cat.valid);
    assert(decoded_key(*table, "idx_cat", cat.min_key) == 0);
    assert(decoded_key(*table, "idx_cat", cat.max_key) == 99);
    assert(cat.distinct_keys == 10);  /// 99 是新键，但非唯一索引不做精确 NDV，重采前保持旧值
  }

  // ---- 7) 删掉非唯一索引的最大键：失效 + 重采后 max 回到 9 ----
  delete_row(*table, 5000);
  {
    const IndexStats cat = stats_of(*table, "idx_cat");
    assert(!cat.valid);
  }
  assert(OB_SUCC(table->ensure_index_stats()));
  {
    const IndexStats cat = stats_of(*table, "idx_cat");
    assert(cat.valid);
    assert(cat.entries == 500);
    assert(cat.distinct_keys == 10);
    assert(decoded_key(*table, "idx_cat", cat.min_key) == 0);
    assert(decoded_key(*table, "idx_cat", cat.max_key) == 9);
  }

  // ---- 8) 关闭时随 catalog 落盘：重开后统计直接可用，不需要重新扫索引 ----
  const IndexStats before_close = stats_of(*table, "idx_id");
  assert(OB_SUCC(db->close()));
  delete db;

  assert(OB_SUCC(Db::open(kRoot, kDbName, &db)));
  Table *reopened = nullptr;
  assert(OB_SUCC(db->find_table(kTable, &reopened)));

  {
    const IndexStats stats = stats_of(*reopened, "idx_id");
    assert(stats.valid);  /// 若 close 没写 catalog，这里会是 create_index 时的 invalid
    assert(stats.entries == before_close.entries);
    assert(stats.distinct_keys == before_close.distinct_keys);
    assert(decoded_key(*reopened, "idx_id", stats.min_key) == 1);
    assert(decoded_key(*reopened, "idx_id", stats.max_key) == 1000);

    const IndexStats cat = stats_of(*reopened, "idx_cat");
    assert(cat.valid);
    assert(decoded_key(*reopened, "idx_cat", cat.max_key) == 9);
  }

  assert(OB_SUCC(db->close()));
  delete db;

  printf("test_index_stats: OK\n");
  return 0;
}
