/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include "common/rc.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/record/record.h"
#include "storage/record/record_manager.h"

/**
 * @brief 全表扫描游标：按 (页号, 槽位号) 顺序遍历堆文件里的有效记录
 * @details 每次只 pin 住当前页，对应 miniob 的 HeapRecordScanner。
 *          同一页内连续取行不会再取一次页（一页只 pin 一次，性能上很重要）。
 *
 * @warning 因此 next() 返回后**当前页的读锁仍然被这个游标持有**：扫描过程中不要在
 *          同一个线程里对同一张表做写操作（delete/update 会去取同一页的写锁，非递归锁
 *          会自死锁）。SQL 层的 DELETE/UPDATE 是「先收齐 RID、关掉子扫描再改」；
 *          自己写代码时照此办理，或者让 scanner 析构 / restart() 之后再写。
 */
class RecordScanner
{
public:
  RC init(HeapRecordManager *manager);

  /// 取下一条记录，返回的记录自带副本；遍历结束返回 RC::RECORD_EOF
  RC next(Record &record);
  RC restart();

  int64_t scanned_count() const { return scanned_; }

private:
  HeapRecordManager *manager_      = nullptr;
  DiskBufferPool    *buffer_pool_  = nullptr;
  int                record_size_  = 0;
  PageNum            current_page_ = RECORD_FIRST_DATA_PAGE;
  int32_t            next_slot_    = 0;
  int64_t            scanned_      = 0;
  PageHandle         handle_;
};
