/* minidb - 记录管理器自测：slotted page 插入/删除/更新/扫描/复开校验 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "common/file_util.h"
#include "common/log.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/record/record_manager.h"
#include "storage/record/record_scanner.h"

static const std::string kFile = "./_test_record/t1.heap";
static const int         kRecordSize = 16;

static void make_record(Record &record, int id)
{
  char data[kRecordSize];
  memset(data, 0, sizeof(data));
  int32_t fields[4] = {id, id * 2, id * 3, id * 4};
  memcpy(data, fields, sizeof(fields));
  record.copy_data(data, kRecordSize);
}

static int record_id(const Record &record)
{
  int32_t fields[4] = {0};
  memcpy(fields, record.data(), sizeof(fields));
  return fields[0];
}

static void check_roundtrip(DiskBufferPool *pool, int total)
{
  HeapRecordManager manager;
  assert(OB_SUCC(manager.open(pool, kRecordSize, false)));

  int64_t count = 0;
  assert(OB_SUCC(manager.get_record_count(count)));
  assert(count == total);

  /// 扫描条数必须与表头记录的条数一致，且每条记录都能再次按 rid 读出来
  RecordScanner scanner;
  assert(OB_SUCC(scanner.init(&manager)));
  std::set<int> ids;
  Record        record;
  while (true) {
    RC rc = scanner.next(record);
    if (rc == RC::RECORD_EOF) break;
    assert(OB_SUCC(rc));
    assert(record.len() == kRecordSize);

    Record again;
    assert(OB_SUCC(manager.get_record(record.rid(), again)));
    assert(record_id(again) == record_id(record));

    assert(ids.count(record_id(record)) == 0);  /// 同一条记录不会被扫描两次
    ids.insert(record_id(record));
  }
  assert(static_cast<int>(ids.size()) == total);
}

int main()
{
  path_mkdir_recursive("./_test_record");
  if (file_exist(kFile)) {
    assert(OB_SUCC(path_remove_file(kFile)));
  }

  DiskBufferPoolManager bpm(32);
  assert(OB_SUCC(bpm.create_file(kFile)));

  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));

  HeapRecordManager manager;
  assert(OB_SUCC(manager.open(pool, kRecordSize, true)));

  /// 插入 1000 条记录，必然跨多个数据页
  const int kTotal = 1000;
  std::vector<RID> rids(kTotal);
  for (int i = 0; i < kTotal; ++i) {
    Record record;
    make_record(record, i);
    assert(OB_SUCC(manager.insert_record(record)));
    rids[i] = record.rid();
  }
  assert(pool->page_count() > 3);

  int64_t count = 0;
  assert(OB_SUCC(manager.get_record_count(count)));
  assert(count == kTotal);

  for (int i = 0; i < kTotal; ++i) {
    Record record;
    assert(OB_SUCC(manager.get_record(rids[i], record)));
    assert(record_id(record) == i);
    assert(record.rid() == rids[i]);
  }

  /// 更新：定长记录原地改写
  for (int i = 0; i < kTotal; i += 7) {
    Record record;
    make_record(record, kTotal + i);
    record.set_rid(rids[i]);
    assert(OB_SUCC(manager.update_record(rids[i], record)));
  }
  for (int i = 0; i < kTotal; i += 7) {
    Record record;
    assert(OB_SUCC(manager.get_record(rids[i], record)));
    assert(record_id(record) == kTotal + i);
  }

  /// 删除三分之一，并确认重复删除会报错
  std::set<int> deleted;
  for (int i = 0; i < kTotal; i += 3) {
    assert(OB_SUCC(manager.delete_record(rids[i])));
    deleted.insert(i);
    assert(manager.delete_record(rids[i]) == RC::RECORD_NOT_EXIST);
  }
  assert(OB_SUCC(manager.get_record_count(count)));
  assert(count == kTotal - static_cast<int64_t>(deleted.size()));

  /// 扫描出来的记录必须与删除结果一致
  {
    RecordScanner scanner;
    assert(OB_SUCC(scanner.init(&manager)));
    std::set<int> ids;
    Record        record;
    while (true) {
      RC rc = scanner.next(record);
      if (rc == RC::RECORD_EOF) break;
      assert(OB_SUCC(rc));
      ids.insert(record_id(record));
    }
    assert(ids.size() == static_cast<size_t>(kTotal - static_cast<int>(deleted.size())));
    for (const int id : deleted) {
      assert(ids.count(id) == 0);
    }
  }

  /// 复用被删除的槽位：再插入同样多的记录，文件不应无限增长
  const PageNum pages_before = pool->page_count();
  std::vector<RID> more;
  for (int i = 0; i < static_cast<int>(deleted.size()); ++i) {
    Record record;
    make_record(record, 500000 + i);
    assert(OB_SUCC(manager.insert_record(record)));
    more.push_back(record.rid());
  }
  assert(pool->page_count() <= pages_before + 1);

  /// 非法 rid 要有明确错误码
  Record probe;
  assert(manager.get_record(RID(0, 0), probe) == RC::RECORD_INVALID_RID);
  assert(manager.get_record(RID(1, 0), probe) == RC::RECORD_INVALID_RID);
  assert(manager.get_record(RID(9999, 0), probe) == RC::RECORD_INVALID_RID);

  assert(OB_SUCC(bpm.close()));

  /// 重新打开，内容必须完整落盘
  DiskBufferPoolManager bpm2(32);
  DiskBufferPool       *pool2 = nullptr;
  assert(OB_SUCC(bpm2.open_file(kFile, ReadWriteMode::READ_WRITE, &pool2)));

  const int expect_total = kTotal - static_cast<int>(deleted.size()) + static_cast<int>(more.size());
  check_roundtrip(pool2, expect_total);

  /// 以 record_size 不一致的方式打开必须失败
  HeapRecordManager wrong;
  assert(wrong.open(pool2, kRecordSize + 4, false) == RC::SCHEMA_FIELD_TYPE_MISMATCH);

  assert(OB_SUCC(bpm2.close()));
  printf("test_record_manager: OK\n");
  return 0;
}
