/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/record/record_manager.h"

#include <cstring>

#include "common/log.h"

HeapRecordManager::~HeapRecordManager() { close(); }

RC HeapRecordManager::open(DiskBufferPool *buffer_pool, int record_size, bool create)
{
  if (buffer_pool == nullptr || record_size <= 0) {
    return RC::INVALID_ARGUMENT;
  }

  buffer_pool_ = buffer_pool;
  file_name_   = buffer_pool->file_name();
  record_size_ = record_size;

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

    RecordFileHeader header;
    header.record_size     = record_size_;
    header.record_count    = 0;
    header.last_insert_page = RECORD_FIRST_DATA_PAGE;
    rc                     = write_header(header);
    if (OB_FAIL(rc)) {
      return rc;
    }
  } else {
    RecordFileHeader header;
    RC               rc = read_header(header);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (header.magic != RECORD_FILE_MAGIC) {
      LOG_ERROR("invalid record file magic. file=%s", file_name_.c_str());
      return RC::IOERR_READ;
    }
    if (header.record_size != record_size_) {
      LOG_ERROR("record size mismatch. file=%s, file=%d, expect=%d",
                file_name_.c_str(), header.record_size, record_size_);
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }

  opened_ = true;
  return RC::SUCCESS;
}

RC HeapRecordManager::close()
{
  opened_      = false;
  buffer_pool_ = nullptr;  /// 文件由 DiskBufferPoolManager 统一关闭
  return RC::SUCCESS;
}

RC HeapRecordManager::read_header(RecordFileHeader &header)
{
  if (buffer_pool_ == nullptr) {
    return RC::RECORD_OPENNED;
  }

  PageHandle handle;
  RC         rc = buffer_pool_->get_page(RECORD_META_PAGE, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  memcpy(&header, handle.const_data(), sizeof(header));
  return RC::SUCCESS;
}

RC HeapRecordManager::write_header(const RecordFileHeader &header)
{
  PageHandle handle;
  RC         rc = buffer_pool_->get_page(RECORD_META_PAGE, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  memcpy(handle.value(), &header, sizeof(header));
  return handle.mark_dirty();
}

RC HeapRecordManager::insert_record(Record &record)
{
  if (!opened_) {
    return RC::RECORD_OPENNED;
  }
  if (record.len() != record_size_) {
    LOG_ERROR("invalid record size. file=%s, len=%d, expect=%d", file_name_.c_str(), record.len(), record_size_);
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

  const PageNum start  = page_num;
  bool          wrapped = false;
  while (true) {
    if (page_num >= buffer_pool_->page_count()) {
      /// 还没有这一页，向缓冲管理器要一个（新页一定是空的）
      rc = buffer_pool_->allocate_page(&page_num);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }

    PageHandle          handle;
    rc = buffer_pool_->get_page(page_num, handle);
    if (OB_FAIL(rc)) {
      return rc;
    }

    char              *page   = handle.value();
    SlottedPageHeader *header = page_header_of(page);

    /// 缓冲池新建的页是全零的，第一次使用前要初始化成空页
    if (header->free_end <= 0 || header->free_start < static_cast<int32_t>(sizeof(SlottedPageHeader))) {
      init_slotted_page(page);
    }

    SlotNum reuse = find_free_slot(page);
    /// 复用旧槽位不需要新的槽位目录空间
    const int32_t need = record_size_ + (reuse >= 0 ? 0 : static_cast<int32_t>(sizeof(int32_t)));

    if (header->free_start + need <= header->free_end) {
      SlotNum   slot   = reuse;
      int32_t   offset = header->free_start;
      header->free_start += record_size_;

      if (reuse >= 0) {
        header->deleted_count -= 1;
      } else {
        slot = header->record_count;
        header->record_count += 1;
        header->free_end     -= static_cast<int32_t>(sizeof(int32_t));
      }
      *slot_at(page, slot) = offset;
      memcpy(page + offset, record.data(), record_size_);

      if (OB_FAIL(handle.mark_dirty())) {
        return rc;
      }

      record.set_rid(page_num, slot);

      file_header.record_count += 1;
      file_header.last_insert_page = page_num;
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
      wrapped = true;
      page_num = buffer_pool_->page_count();  /// 越界，下一轮会新建一页
    }
  }
}

RC HeapRecordManager::delete_record(const RID &rid)
{
  if (!opened_) {
    return RC::RECORD_OPENNED;
  }
  if (rid.page_num < RECORD_FIRST_DATA_PAGE || rid.page_num >= buffer_pool_->page_count()) {
    return RC::RECORD_INVALID_RID;
  }

  PageHandle handle;
  RC         rc = buffer_pool_->get_page(rid.page_num, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char              *page   = handle.value();
  SlottedPageHeader *header = page_header_of(page);
  if (rid.slot_num < 0 || rid.slot_num >= header->record_count) {
    return RC::RECORD_INVALID_RID;
  }

  int32_t *slot = slot_at(page, rid.slot_num);
  if (*slot == -1) {
    return RC::RECORD_NOT_EXIST;
  }

  *slot = -1;
  header->deleted_count += 1;
  handle.mark_dirty();

  RecordFileHeader file_header;
  rc                = read_header(file_header);
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
  RC         rc = buffer_pool_->get_page(rid.page_num, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  const char          *page   = handle.const_data();
  const SlottedPageHeader *header = page_header_of(page);
  if (rid.slot_num < 0 || rid.slot_num >= header->record_count) {
    return RC::RECORD_INVALID_RID;
  }

  const int32_t offset = slot_value_at(page, rid.slot_num);
  if (offset == -1) {
    return RC::RECORD_NOT_EXIST;
  }

  record.set_rid(rid);
  return record.copy_data(page + offset, record_size_);
}

RC HeapRecordManager::update_record(const RID &rid, const Record &record)
{
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
  RC         rc = buffer_pool_->get_page(rid.page_num, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char              *page   = handle.value();
  SlottedPageHeader *header = page_header_of(page);
  if (rid.slot_num < 0 || rid.slot_num >= header->record_count) {
    return RC::RECORD_INVALID_RID;
  }

  const int32_t offset = slot_value_at(page, rid.slot_num);
  if (offset == -1) {
    return RC::RECORD_NOT_EXIST;
  }

  memcpy(page + offset, record.data(), record_size_);
  return handle.mark_dirty();
}

RC HeapRecordManager::get_record_count(int64_t &count)
{
  RecordFileHeader header;
  RC               rc = read_header(header);
  if (OB_FAIL(rc)) {
    return rc;
  }
  count = header.record_count;
  return RC::SUCCESS;
}
