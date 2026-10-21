/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include "common/rc.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/record/record.h"
#include "storage/record/record_manager.h"

/**
 * @brief 全表扫描游标：按 (页号, 槽位号) 顺序遍历堆文件里的有效记录
 * @details 每次只 pin 住当前页，对应 miniob 的 HeapRecordScanner。
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
