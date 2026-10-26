/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/record/record_manager.h"

#include <cstring>
#include <mutex>

#include "common/log.h"

namespace
{
/// 页头自检失败统一返回「页损坏」：宁可报错，也不要按被读坏的偏移去写页
RC check_page(const char *page, const char *file_name, PageNum page_num)
{
  if (check_slotted_page(page)) {
    return RC::SUCCESS;
  }
  LOG_ERROR("invalid slotted page header. file=%s, page=%d", file_name, page_num);
  return RC::BUFFERPOOL_CORRUPTED;
}
}  // namespace

HeapRecordManager::~HeapRecordManager() { close(); }

RC HeapRecordManager::open(DiskBufferPool *buffer_pool, int record_size, bool create)
{
  if (buffer_pool == nullptr || record_size <= 0) {
    return RC::INVALID_ARGUMENT;
  }
  /// 一页放不下一条记录时必须在打开阶段就拒绝：否则 insert 会一直找页、一直分配新页
  if (record_size > RECORD_MAX_SIZE) {
    LOG_ERROR("record size too large to fit in a page. record_size=%d, max=%d", record_size, RECORD_MAX_SIZE);
    return RC::INVALID_ARGUMENT;
  }

  buffer_pool_  = buffer_pool;
  file_name_    = buffer_pool->file_name();
  record_size_  = record_size;
  header_dirty_ = false;

  if (create) {
    PageNum page_num = INVALID_PAGE_NUM;
    RC      rc       = buffer_pool_->allocate_page(&page_num);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (page_num != RECORD_META_PAGE) {
      LOG_ERROR("unexpected meta page. file=%s, page=%d", file_name_.c_str(), page_num);
      return RC::INTERNAL;
    }

    header_                  = RecordFileHeader();
    header_.record_size      = record_size_;
    header_.record_count     = 0;
    header_.last_insert_page = RECORD_FIRST_DATA_PAGE;
    header_dirty_            = true;
    rc                       = flush_header();
    if (OB_FAIL(rc)) {
      return rc;
    }
  } else {
    RC rc = load_header();
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (header_.magic != RECORD_FILE_MAGIC) {
      LOG_ERROR("invalid record file magic. file=%s", file_name_.c_str());
      return RC::IOERR_READ;
    }
    if (header_.record_size != record_size_) {
      LOG_ERROR("record size mismatch. file=%s, file=%d, expect=%d",
                file_name_.c_str(), header_.record_size, record_size_);
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }

  opened_ = true;
  return RC::SUCCESS;
}

RC HeapRecordManager::close()
{
  std::lock_guard<std::mutex> guard(latch_);

  RC rc = RC::SUCCESS;
  if (opened_ && buffer_pool_ != nullptr) {
    rc = flush_header();
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to flush record header on close. file=%s, rc=%s", file_name_.c_str(), strrc(rc));
    }
  }

  opened_      = false;
  buffer_pool_ = nullptr;  /// 文件由 DiskBufferPoolManager 统一关闭
  return rc;
}

RC HeapRecordManager::load_header()
{
  if (buffer_pool_ == nullptr) {
    return RC::RECORD_OPENNED;
  }

  PageHandle handle;
  RC         rc = buffer_pool_->get_page_read(RECORD_META_PAGE, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  memcpy(&header_, handle.const_data(), sizeof(header_));
  header_dirty_ = false;
  return RC::SUCCESS;
}

RC HeapRecordManager::flush_header()
{
  if (buffer_pool_ == nullptr) {
    return RC::RECORD_OPENNED;
  }
  if (!header_dirty_) {
    return RC::SUCCESS;
  }

  PageHandle handle;
  RC         rc = buffer_pool_->get_page_write(RECORD_META_PAGE, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  memcpy(handle.value(), &header_, sizeof(header_));
  rc = handle.mark_dirty();
  if (OB_FAIL(rc)) {
    return rc;
  }
  header_dirty_ = false;
  return RC::SUCCESS;
}

RC HeapRecordManager::read_header(RecordFileHeader &header)
{
  if (buffer_pool_ == nullptr) {
    return RC::RECORD_OPENNED;
  }
  header = header_;
  return RC::SUCCESS;
}

RC HeapRecordManager::write_header(const RecordFileHeader &header)
{
  header_       = header;
  header_dirty_ = true;
  return flush_header();
}

RC HeapRecordManager::insert_record(Record &record)
{
  std::lock_guard<std::mutex> guard(latch_);

  if (!opened_) {
    return RC::RECORD_OPENNED;
  }
  if (record.len() != record_size_) {
    LOG_ERROR("invalid record size. file=%s, len=%d, expect=%d", file_name_.c_str(), record.len(), record_size_);
    return RC::INVALID_ARGUMENT;
  }
  /// 防御：open 已拦截，但如果 record_size_ 被改坏，这里必须直接失败而不是无限找页
  if (record_size_ > RECORD_MAX_SIZE) {
    LOG_ERROR("record size too large to fit in a page. file=%s, record_size=%d, max=%d",
              file_name_.c_str(), record_size_, RECORD_MAX_SIZE);
    return RC::INVALID_ARGUMENT;
  }

  RecordFileHeader file_header;
  RC               rc = read_header(file_header);
  if (OB_FAIL(rc)) {
    return rc;
  }

  PageNum page_num = file_header.last_insert_page;
  if (page_num < RECORD_FIRST_DATA_PAGE) {
    page_num = RECORD_FIRST_DATA_PAGE;
  }

  const PageNum start   = page_num;
  bool          wrapped = false;
  while (true) {
    if (page_num >= buffer_pool_->page_count()) {
      /// 还没有这一页，向缓冲管理器要一个（新页一定是空的）
      rc = buffer_pool_->allocate_page(&page_num);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }

    PageHandle handle;
    rc = buffer_pool_->get_page_write(page_num, handle);
    if (OB_FAIL(rc)) {
      return rc;
    }

    char              *page   = handle.value();
    SlottedPageHeader *header = page_header_of(page);

    /// 缓冲池新建的页是全零的，第一次使用前要初始化成空页
    if (header->free_end <= 0 || header->free_start < static_cast<int32_t>(sizeof(SlottedPageHeader))) {
      init_slotted_page(page);
    }

    /// 崩溃后可能读到半写页，先校验页头再按里面的偏移写
    rc = check_page(page, file_name_.c_str(), page_num);
    if (OB_FAIL(rc)) {
      return rc;
    }

    SlotNum reuse = find_free_slot(page);
    /// 复用已删槽位分两种：
    ///   * 新格式存的是 -offset：记录体原地覆写，不占新空间（记录定长、从不搬移）
    ///   * 旧格式只有 -1：偏移丢了，记录体要重新分配，但槽位目录那 4 字节仍然复用
    const bool    can_reuse_body = (reuse >= 0) && slot_has_reusable_offset(slot_value_at(page, reuse));
    const int32_t need          = can_reuse_body
                                      ? 0
                                      : record_size_ + (reuse >= 0 ? 0 : static_cast<int32_t>(sizeof(int32_t)));

    /// 先减后比：free_start + need 在被读坏的页上可能溢出成负数而绕过检查
    if (need <= header->free_end - header->free_start) {
      SlotNum slot   = reuse;
      int32_t offset = 0;

      if (can_reuse_body) {
        offset = slot_deleted_offset(slot_value_at(page, reuse));
        /// 槽位指向的位置必须落在本页记录区内（半写页/坏页时不要照着写）
        if (offset < static_cast<int32_t>(sizeof(SlottedPageHeader)) || offset + record_size_ > BP_PAGE_SIZE) {
          LOG_ERROR("invalid slot offset. file=%s, page=%d, slot=%d, offset=%d",
                    file_name_.c_str(), page_num, reuse, offset);
          return RC::BUFFERPOOL_CORRUPTED;
        }
        header->deleted_count -= 1;
      } else {
        offset = header->free_start;
        header->free_start += record_size_;
        if (reuse >= 0) {
          header->deleted_count -= 1;  /// 旧格式删除标记：只能复用槽位，不能复用记录体
        } else {
          slot = header->record_count;
          header->record_count += 1;
          header->free_end -= static_cast<int32_t>(sizeof(int32_t));
        }
      }

      *slot_at(page, slot) = offset;
      memcpy(page + offset, record.data(), record_size_);

      if (OB_FAIL(handle.mark_dirty())) {
        return rc;
      }

      record.set_rid(page_num, slot);

      file_header.record_count += 1;
      file_header.last_insert_page = page_num;
      handle.reset();
      return write_header(file_header);
    }

    /// 这页放不下：如果整页都是已删除的槽位，就把它重置成空页
    if (header->record_count > 0 && header->deleted_count == header->record_count) {
      init_slotted_page(page);
      handle.mark_dirty();
    }
    handle.reset();

    if (!wrapped && page_num + 1 < buffer_pool_->page_count()) {
      page_num += 1;
    } else if (!wrapped && start != RECORD_FIRST_DATA_PAGE) {
      /// 从 last_insert_page 往后没找到空间，回到最前面再扫一遍
      page_num = RECORD_FIRST_DATA_PAGE;
      wrapped  = true;
    } else {
      wrapped  = true;
      page_num = buffer_pool_->page_count();  /// 越界，下一轮会新建一页
    }
  }
}

RC HeapRecordManager::delete_record(const RID &rid)
{
  std::lock_guard<std::mutex> guard(latch_);

  if (!opened_) {
    return RC::RECORD_OPENNED;
  }
  if (rid.page_num < RECORD_FIRST_DATA_PAGE || rid.page_num >= buffer_pool_->page_count()) {
    return RC::RECORD_INVALID_RID;
  }

  PageHandle handle;
  RC         rc = buffer_pool_->get_page_write(rid.page_num, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char *page = handle.value();
  rc         = check_page(page, file_name_.c_str(), rid.page_num);
  if (OB_FAIL(rc)) {
    return rc;
  }

  SlottedPageHeader *header = page_header_of(page);
  if (rid.slot_num < 0 || rid.slot_num >= header->record_count) {
    return RC::RECORD_INVALID_RID;
  }

  int32_t *slot = slot_at(page, rid.slot_num);
  if (slot_is_deleted(*slot)) {
    return RC::RECORD_NOT_EXIST;
  }
  if (*slot < static_cast<int32_t>(sizeof(SlottedPageHeader)) || *slot + record_size_ > BP_PAGE_SIZE) {
    LOG_ERROR("invalid slot offset. file=%s, page=%d, slot=%d, offset=%d",
              file_name_.c_str(), rid.page_num, rid.slot_num, *slot);
    return RC::BUFFERPOOL_CORRUPTED;
  }

  /// 用「负的偏移」表示已删除：绝对值保留记录体位置，后续插入可以原地复用这块空间
  *slot = -(*slot);
  header->deleted_count += 1;
  rc = handle.mark_dirty();
  if (OB_FAIL(rc)) {
    /// 只读池上 mark_dirty 会返回 IOERR_ACCESS：这里必须报错，不能返回成功却不落盘
    return rc;
  }
  handle.reset();

  RecordFileHeader file_header;
  rc = read_header(file_header);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (file_header.record_count > 0) {
    file_header.record_count -= 1;
  }
  return write_header(file_header);
}

RC HeapRecordManager::get_record(const RID &rid, Record &record)
{
  if (!opened_) {
    return RC::RECORD_OPENNED;
  }
  if (rid.page_num < RECORD_FIRST_DATA_PAGE || rid.page_num >= buffer_pool_->page_count()) {
    return RC::RECORD_INVALID_RID;
  }

  PageHandle handle;
  RC         rc = buffer_pool_->get_page_read(rid.page_num, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  const char *page = handle.const_data();
  rc               = check_page(page, file_name_.c_str(), rid.page_num);
  if (OB_FAIL(rc)) {
    return rc;
  }

  const SlottedPageHeader *header = page_header_of(page);
  if (rid.slot_num < 0 || rid.slot_num >= header->record_count) {
    return RC::RECORD_INVALID_RID;
  }

  const int32_t offset = slot_value_at(page, rid.slot_num);
  if (slot_is_deleted(offset)) {
    return RC::RECORD_NOT_EXIST;
  }
  if (offset < static_cast<int32_t>(sizeof(SlottedPageHeader)) || offset + record_size_ > BP_PAGE_SIZE) {
    LOG_ERROR("invalid slot offset. file=%s, page=%d, slot=%d, offset=%d",
              file_name_.c_str(), rid.page_num, rid.slot_num, offset);
    return RC::BUFFERPOOL_CORRUPTED;
  }

  record.set_rid(rid);
  return record.copy_data(page + offset, record_size_);
}

RC HeapRecordManager::update_record(const RID &rid, const Record &record)
{
  std::lock_guard<std::mutex> guard(latch_);

  if (!opened_) {
    return RC::RECORD_OPENNED;
  }
  if (record.len() != record_size_) {
    return RC::INVALID_ARGUMENT;
  }
  if (rid.page_num < RECORD_FIRST_DATA_PAGE || rid.page_num >= buffer_pool_->page_count()) {
    return RC::RECORD_INVALID_RID;
  }

  PageHandle handle;
  RC         rc = buffer_pool_->get_page_write(rid.page_num, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char *page = handle.value();
  rc         = check_page(page, file_name_.c_str(), rid.page_num);
  if (OB_FAIL(rc)) {
    return rc;
  }

  SlottedPageHeader *header = page_header_of(page);
  if (rid.slot_num < 0 || rid.slot_num >= header->record_count) {
    return RC::RECORD_INVALID_RID;
  }

  const int32_t offset = slot_value_at(page, rid.slot_num);
  if (slot_is_deleted(offset)) {
    return RC::RECORD_NOT_EXIST;
  }
  if (offset < static_cast<int32_t>(sizeof(SlottedPageHeader)) || offset + record_size_ > BP_PAGE_SIZE) {
    LOG_ERROR("invalid slot offset. file=%s, page=%d, slot=%d, offset=%d",
              file_name_.c_str(), rid.page_num, rid.slot_num, offset);
    return RC::BUFFERPOOL_CORRUPTED;
  }

  memcpy(page + offset, record.data(), record_size_);
  return handle.mark_dirty();
}

RC HeapRecordManager::get_record_count(int64_t &count)
{
  std::lock_guard<std::mutex> guard(latch_);

  if (!opened_) {
    return RC::RECORD_OPENNED;
  }
  count = header_.record_count;  /// 直接读缓存，不再每次都读表头页
  return RC::SUCCESS;
}
