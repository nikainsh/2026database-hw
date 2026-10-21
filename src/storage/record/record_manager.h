/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include "common/rc.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/record/record.h"
#include "storage/record/slotted_page.h"

/// 第 0 页归缓冲池（文件头），第 1 页归记录管理器（表头），数据页从第 2 页开始
static constexpr PageNum RECORD_META_PAGE = 1;

static constexpr int32_t RECORD_FILE_MAGIC = 0x4D444231;  /// 'MDB1'

/// 表头页（第 1 页）里保存的信息
struct RecordFileHeader
{
  int32_t magic = RECORD_FILE_MAGIC;
  int32_t record_size = 0;
  int64_t record_count = 0;      /// 有效记录数
  PageNum last_insert_page = RECORD_FIRST_DATA_PAGE;
};

/**
 * @brief 存储管理器中负责“表数据”的部件：定长记录的堆文件
 * @details 对应 miniob 的 HeapRecordManager。记录都是定长的（由 TableMeta 算出 record_size），
 *          因此更新可以直接原地改写，删除留下的槽位会被后续插入复用。
 */
class HeapRecordManager
{
public:
  HeapRecordManager() = default;
  ~HeapRecordManager();

  /// create=true 时把表头页初始化成空文件，否则要求表头页的 record_size 与传入值一致
  RC open(DiskBufferPool *buffer_pool, int record_size, bool create);
  RC close();

  /// 插入记录，成功后 record.rid() 被回填
  RC insert_record(Record &record);
  RC delete_record(const RID &rid);
  RC get_record(const RID &rid, Record &record);
  /// 定长记录，原地更新；record.data() 长度必须等于 record_size
  RC update_record(const RID &rid, const Record &record);

  RC get_record_count(int64_t &count);
  int  record_size() const { return record_size_; }
  DiskBufferPool *buffer_pool() const { return buffer_pool_; }
  const std::string &file_name() const { return file_name_; }

  /// 供扫描器使用：页号是否合法、页头读取
  RC read_page_header(PageNum page_num, SlottedPageHeader &header);

private:
  RC read_header(RecordFileHeader &header);
  RC write_header(const RecordFileHeader &header);

  /// 在第 page_num 页里找一个可用槽位；found 为 false 说明这页放不下了
  RC allocate_slot(PageNum page_num, SlotNum *slot, int32_t *offset, bool *found);

private:
  DiskBufferPool *buffer_pool_ = nullptr;
  std::string     file_name_;
  int             record_size_ = 0;
  bool            opened_      = false;
};
