/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <mutex>

#include "common/rc.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/record/record.h"
#include "storage/record/slotted_page.h"

/// 第 0 页归缓冲池（文件头），第 1 页归记录管理器（表头），数据页从第 2 页开始
static constexpr PageNum RECORD_META_PAGE = 1;

/// 一条记录最多能有多大：页头 + 至少要给它留一个槽位，剩下的空间才放得下记录本身。
/// 超过这个值的 record_size 在 open() 阶段直接拒绝 —— 否则空页也装不下，
/// insert_record 会不断分配新页、无限扩文件（每轮 8KB）而永远插不进去。
static constexpr int32_t RECORD_MAX_SIZE =
    BP_PAGE_SIZE - static_cast<int32_t>(sizeof(SlottedPageHeader)) - static_cast<int32_t>(sizeof(int32_t));

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

private:
  /// 把页 1 的表头读进内存缓存（open 时调用一次）
  RC load_header();
  /// 缓存脏了才写回页 1；返回失败时缓存保持脏标记，调用方可重试
  RC flush_header();
  /// 缓存访问：不再每次 DML 都真读一次页 1（调用方需持 latch_）
  RC read_header(RecordFileHeader &header);
  /// 更新缓存并落盘（调用方需持 latch_）
  RC write_header(const RecordFileHeader &header);

private:
  DiskBufferPool *buffer_pool_ = nullptr;
  std::string     file_name_;
  int             record_size_ = 0;
  bool            opened_      = false;

  /// 表头页（页 1）的内存缓存：open 时读一次，之后每次写操作只写回一次，
  /// 省掉原先「每次 insert/delete 读一次页头 + 写一次页头」的两趟页访问。
  RecordFileHeader header_;
  bool             header_dirty_ = false;

  /// 表级互斥：表头是「读-改-写」的共享状态。并发插入/删除若不串行化，
  /// 两个线程会各自读到 count=N 再写回 N+1，行数统计永久漂移（丢更新）。
  mutable std::mutex latch_;
};
