/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/record/record_scanner.h"

#include <algorithm>

#include "common/log.h"
#include "storage/record/slotted_page.h"

RC RecordScanner::init(HeapRecordManager *manager)
{
  if (manager == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  manager_      = manager;
  buffer_pool_  = manager->buffer_pool();
  record_size_  = manager->record_size();
  current_page_ = RECORD_FIRST_DATA_PAGE;
  next_slot_    = 0;
  scanned_      = 0;
  handle_.reset();

  if (buffer_pool_ == nullptr) {
    return RC::RECORD_OPENNED;
  }
  return RC::SUCCESS;
}

RC RecordScanner::restart()
{
  handle_.reset();
  current_page_ = RECORD_FIRST_DATA_PAGE;
  next_slot_    = 0;
  scanned_      = 0;
  return RC::SUCCESS;
}

RC RecordScanner::next(Record &record)
{
  while (true) {
    if (!handle_.valid()) {
      if (current_page_ >= buffer_pool_->page_count()) {
        return RC::RECORD_EOF;
      }
      RC rc = buffer_pool_->get_page_read(current_page_, handle_);
      if (OB_FAIL(rc)) {
        return rc;
      }
      /// 崩溃后可能读到半写页：页头不可信就不要按它的 record_count / 偏移继续走
      if (!check_slotted_page(handle_.const_data())) {
        LOG_ERROR("invalid slotted page header. file=%s, page=%d", buffer_pool_->file_name().c_str(), current_page_);
        handle_.reset();
        return RC::BUFFERPOOL_CORRUPTED;
      }
      next_slot_ = 0;
    }

    const char              *page   = handle_.const_data();
    const SlottedPageHeader *header = page_header_of(page);

    while (next_slot_ < header->record_count) {
      const int32_t offset = slot_value_at(page, next_slot_);
      const SlotNum slot   = next_slot_;
      ++next_slot_;

      if (offset != -1) {
        record.set_rid(current_page_, slot);
        ++scanned_;
        return record.copy_data(page + offset, record_size_);
      }
    }

    handle_.reset();
    current_page_ += 1;

    /// Sequential scan hint: warm the next few pages after releasing the
    /// current one. This is a synchronous prefetch in the current buffer
    /// manager, but it still removes the page-fault stall from the hot path.
    if (current_page_ % 4 == 0) {
      const PageNum end = buffer_pool_->page_count();
      if (current_page_ + 1 < end) {
        const int count = std::min<int>(4, end - current_page_ - 1);
        buffer_pool_->prefetch_range(current_page_ + 1, count);
      }
    }
  }
}
