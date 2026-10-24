/* minidb - 存储层集成自测：Table/Db/目录文件/索引维护/重启持久化 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/file_util.h"
#include "common/log.h"
#include "storage/db/db.h"
#include "storage/record/record_scanner.h"
#include "storage/table/table.h"

static const std::string kDir  = "./_test_db";
static const std::string kDbName = "testdb";

static TableMeta make_student_meta()
{
  std::vector<ColumnMeta> columns;
  ColumnMeta              id, name, age, score;
  assert(OB_SUCC(id.init("id", AttrType::INT, 0, 0)));
  assert(OB_SUCC(name.init("name", AttrType::CHARS, 16, 0)));
  assert(OB_SUCC(age.init("age", AttrType::INT, 0, 0)));
  assert(OB_SUCC(score.init("score", AttrType::FLOAT, 0, 0)));
  columns = {id, name, age, score};

  TableMeta meta;
  assert(OB_SUCC(meta.init("student", columns)));
  return meta;
}

static Record make_row(Table &table, int id, const char *name, int age, float score)
{
  const TableMeta &meta = table.table_meta();
  std::vector<Value> values;
  values.push_back(Value(id));
  values.push_back(Value(std::string(name)));
  values.push_back(Value(age));
  values.push_back(Value(score));

  Record record;
  RC     rc = meta.make_record(values, record);
  assert(OB_SUCC(rc));
  return record;
}

static int count_rows(Table &table)
{
  RecordScanner scanner;
  assert(OB_SUCC(table.start_scan(scanner)));
  int      n = 0;
  Record   record;
  while (true) {
    RC rc = scanner.next(record);
    if (rc == RC::RECORD_EOF) break;
    assert(OB_SUCC(rc));
    ++n;
  }
  return n;
}

static Value row_field(Table &table, const RID &rid, const char *field)
{
  Record record;
  assert(OB_SUCC(table.get_record(rid, record)));
  Value  value;
  assert(OB_SUCC(table.table_meta().get_field_value(record, *table.table_meta().column(field), value)));
  return value;
}

static void cleanup_dir()
{
  for (const std::string &file : path_list_files(kDir)) {
    path_remove_file(file);
  }
}

int main()
{
  path_mkdir_recursive(kDir);
  cleanup_dir();

  // ---- 建库建表 ----
  Db *db = nullptr;
  assert(OB_SUCC(Db::create(kDir, kDbName, &db)));
  assert(db->create_table(make_student_meta()) == RC::SUCCESS);
  assert(db->create_table(make_student_meta()) == RC::SCHEMA_TABLE_EXIST);

  Table *table = nullptr;
  assert(OB_SUCC(db->find_table("student", &table)));
  assert(table != nullptr);
  /// id(4) + name(16) + age(4) + score(4)
  assert(table->table_meta().record_size() == 28);
  assert(table->table_meta().column("nope") == nullptr);

  // ---- 插入 + 扫描 ----
  std::vector<RID> rids;
  for (int i = 0; i < 500; ++i) {
    Record record = make_row(*table, i, ("n" + std::to_string(i)).c_str(), 18 + i % 30, static_cast<float>(i) / 2);
    assert(OB_SUCC(table->insert_record(record)));
    rids.push_back(record.rid());
  }
  assert(count_rows(*table) == 500);
  int64_t count = 0;
  assert(OB_SUCC(table->record_count(count)));
  assert(count == 500);

  assert(row_field(*table, rids[7], "name").get_string() == "n7");
  assert(row_field(*table, rids[7], "age").get_int32() == 25);

  // ---- 更新：被索引列改动时索引同步 ----
  {
    Record record = make_row(*table, 7, "changed", 99, 1.5f);
    assert(OB_SUCC(table->update_record(rids[7], record)));
    assert(row_field(*table, rids[7], "name").get_string() == "changed");
    assert(row_field(*table, rids[7], "age").get_int32() == 99);
    assert(count_rows(*table) == 500);
  }

  // ---- 建索引（要对已有 500 行重建索引内容）----
  IndexMeta id_index;
  id_index.index_name = "idx_id";
  id_index.field_name = "id";
  id_index.unique     = true;
  assert(OB_SUCC(db->create_index("student", id_index)));
  assert(table->index_of_field("id") != nullptr);
  assert(table->index_by_name("idx_id") != nullptr);

  IndexMeta age_index;
  age_index.index_name = "idx_age";
  age_index.field_name = "age";
  age_index.unique     = false;
  assert(OB_SUCC(db->create_index("student", age_index)));

  IndexMeta dup;
  dup.index_name = "idx_id";
  dup.field_name = "id";
  assert(db->create_index("student", dup) == RC::SCHEMA_INDEX_NAME_REPEAT);

  int64_t entries = 0;
  assert(OB_SUCC(table->index_of_field("id")->entry_count(entries)));
  assert(entries == 500);

  // ---- 唯一索引冲突必须回滚记录 ----
  {
    Record record = make_row(*table, 7, "dup_id", 20, 0.0f);  /// id=7 已存在
    assert(table->insert_record(record) == RC::RECORD_DUPLICATE_KEY);
    assert(count_rows(*table) == 500);
    assert(OB_SUCC(table->index_of_field("id")->entry_count(entries)));
    assert(entries == 500);
  }

  // ---- 索引上的范围扫描 ----
  {
    Value  low(20), high(30);
    BplusTreeScanner scanner;
    assert(OB_SUCC(table->index_of_field("id")->make_scanner(scanner, &low, true, &high, true)));
    int hits = 0;
    std::string key;
    RID     rid;
    while (true) {
      RC rc = scanner.next(key, rid);
      if (rc == RC::RECORD_EOF) break;
      assert(OB_SUCC(rc));
      ++hits;
    }
    assert(hits == 11);  /// id 20..30，其中 7 被改成过但不在区间内
  }

  // ---- 删除：索引项同步消失 ----
  for (int i = 0; i < 100; ++i) {
    assert(OB_SUCC(table->delete_record(rids[i])));
  }
  assert(count_rows(*table) == 400);
  assert(OB_SUCC(table->index_of_field("id")->entry_count(entries)));
  assert(entries == 400);
  assert(table->delete_record(rids[0]) == RC::RECORD_NOT_EXIST);

  assert(OB_SUCC(db->flush()));

  // ---- 关掉重开：表结构、数据、索引都要在 ----
  assert(OB_SUCC(db->close()));
  delete db;
  db = nullptr;

  assert(OB_SUCC(Db::open(kDir, kDbName, &db)));
  assert(db->tables().size() == 1);
  Table *reopened = nullptr;
  assert(OB_SUCC(db->find_table("student", &reopened)));
  assert(reopened != nullptr);
  assert(count_rows(*reopened) == 400);
  assert(reopened->table_meta().indexes().size() == 2);
  assert(OB_SUCC(reopened->index_of_field("id")->entry_count(entries)));
  assert(entries == 400);
  assert(row_field(*reopened, rids[150], "name").get_string() == "n150");

  // ---- 索引仍然能用来精确定位 ----
  {
    Value  target(150);
    BplusTreeScanner scanner;
    assert(OB_SUCC(reopened->index_of_field("id")->make_scanner(scanner, &target, true, &target, true)));
    std::string key;
    RID         rid;
    assert(OB_SUCC(scanner.next(key, rid)));
    assert(rid == rids[150]);
    assert(scanner.next(key, rid) == RC::RECORD_EOF);
  }

  // ---- drop table 后文件与目录都要干净 ----
  assert(OB_SUCC(db->drop_table("student")));
  assert(db->tables().empty());
  assert(db->find_table("student", &reopened) == RC::SCHEMA_TABLE_NOT_EXIST);
  assert(!file_exist(heap_file_path(kDir, "student")));

  assert(OB_SUCC(db->close()));
  delete db;

  assert(OB_SUCC(Db::open(kDir, kDbName, &db)));
  assert(db->tables().empty());
  assert(OB_SUCC(db->close()));
  delete db;

  printf("test_db: OK\n");
  return 0;
}
