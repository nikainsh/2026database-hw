/* database26 buffer manager - thread-safe page cache with WAL */
#include "storage/buffer/disk_buffer_pool.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <system_error>

#include "common/fault_inject.h"
#include "common/log.h"
#include "storage/buffer/checksum.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
constexpr int kMaxFramesPerFile = 1 << 20;

int file_seek(FILE *fp, int64_t offset)
{
#ifdef _WIN32
  return _fseeki64(fp, offset, SEEK_SET);
#else
  return fseeko(fp, static_cast<off_t>(offset), SEEK_SET);
#endif
}

RC sync_file(FILE *fp)
{
  if (fp == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  if (fflush(fp) != 0) {
    return RC::IOERR_SYNC;
  }
#ifdef _WIN32
  if (_commit(_fileno(fp)) != 0) {
    return RC::IOERR_SYNC;
  }
#else
  if (fsync(fileno(fp)) != 0) {
    return RC::IOERR_SYNC;
  }
#endif
  return RC::SUCCESS;
}

void encode_header(const DiskFileHeader &in, char *page)
{
  memset(page, 0, BP_PAGE_SIZE);
  DiskFileHeader header = in;
  header.header_size     = sizeof(DiskFileHeader);
  header.header_crc      = 0;
  memcpy(page, &header, sizeof(header));
  header.header_crc = crc32(page, offsetof(DiskFileHeader, header_crc));
  memcpy(page, &header, sizeof(header));
}

RC decode_header(const char *page, DiskFileHeader *out)
{
  DiskFileHeader header;
  memcpy(&header, page, sizeof(header));
  if (header.header_size != sizeof(DiskFileHeader)) {
    return RC::BUFFERPOOL_CORRUPTED;
  }
  const uint32_t expect = crc32(page, offsetof(DiskFileHeader, header_crc));
  if (expect != header.header_crc) {
    return RC::BUFFERPOOL_CORRUPTED;
  }
  *out = header;
  return RC::SUCCESS;
}
}  // namespace

// ---------------------------------------------------------------------------
// PageHandle
// ---------------------------------------------------------------------------

void PageHandle::reset()
{
  if (pool_ != nullptr) {
    if (access_ == PageAccess::READ) {
      pool_->unlock_frame_shared(frame_id_);
    } else {
      pool_->unlock_frame_exclusive(frame_id_);
    }
    pool_->unpin_page(frame_id_);
    pool_->release_handle();
    pool_     = nullptr;
    frame_id_ = INVALID_FRAME_ID;
    page_id_  = INVALID_PAGE_NUM;
    access_   = PageAccess::READ;
  }
}

char *PageHandle::value() const
{
  ASSERT(pool_ != nullptr, "the page handle is not valid");
  return pool_->frame_data(frame_id_);
}

RC PageHandle::mark_dirty()
{
  ASSERT(pool_ != nullptr, "the page handle is not valid");
  if (access_ != PageAccess::WRITE) {
    return RC::BUFFERPOOL_READ_ONLY_HANDLE;
  }
  return pool_->mark_page_dirty(frame_id_);
}

// ---------------------------------------------------------------------------
// DiskBufferPool
// ---------------------------------------------------------------------------

DiskBufferPool::DiskBufferPool() = default;

DiskBufferPool::~DiskBufferPool()
{
  RC rc = close();
  if (OB_FAIL(rc)) {
    LOG_WARN("buffer pool close on destruction failed. file=%s, rc=%s", file_name_.c_str(), strrc(rc));
  }
}

RC DiskBufferPool::create_file(const std::string &path)
{
  if (std::filesystem::exists(path)) {
    return RC::FILE_EXIST;
  }

  /// A newly created data file must not inherit a stale WAL from a previous
  /// incarnation of the same path.
  std::error_code ec;
  const std::string wal_path = path + ".wal";
  if (std::filesystem::exists(wal_path, ec)) {
    std::filesystem::remove(wal_path, ec);
  }

  FILE *fp = fopen(path.c_str(), "wb");
  if (fp == nullptr) {
    LOG_ERROR("failed to create file. path=%s", path.c_str());
    return RC::FILE_CREATE;
  }

  DiskFileHeader header;
  header.header_size     = sizeof(DiskFileHeader);
  header.page_count      = 1;
  header.first_free_page = INVALID_PAGE_NUM;
  header.free_page_count = 0;
  header.checkpoint_lsn  = 0;

  char page[BP_PAGE_SIZE];
  encode_header(header, page);

  if (fwrite(page, 1, sizeof(page), fp) != sizeof(page)) {
    fclose(fp);
    return RC::IOERR_WRITE;
  }
  RC rc = sync_file(fp);
  fclose(fp);
  return rc;
}

RC DiskBufferPool::open(const std::string &path, ReadWriteMode mode, int frame_limit, bool enable_wal)
{
  if (frame_limit <= 0 || frame_limit > kMaxFramesPerFile) {
    return RC::BUFFERPOOL_INVALID_FRAME_LIMIT;
  }
  if (handle_ != nullptr) {
    return RC::BUFFERPOOL_OPEN;
  }

  owned_frame_manager_ = std::make_unique<FrameManager>(frame_limit);
  RC rc = open_with_frame_manager(path, mode, frame_limit, enable_wal, owned_frame_manager_.get(), 0);
  if (OB_FAIL(rc)) {
    owned_frame_manager_.reset();
  }
  return rc;
}

RC DiskBufferPool::open_with_frame_manager(const std::string &path, ReadWriteMode mode, int frame_limit,
                                           bool enable_wal, FrameManager *frame_manager, int pool_id)
{
  if (frame_limit <= 0 || frame_limit > kMaxFramesPerFile) {
    return RC::BUFFERPOOL_INVALID_FRAME_LIMIT;
  }
  if (frame_manager == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  if (handle_ != nullptr) {
    return RC::BUFFERPOOL_OPEN;
  }

  file_name_       = path;
  wal_path_        = path + ".wal";
  mode_            = mode;
  enable_wal_      = enable_wal && mode == ReadWriteMode::READ_WRITE;
  header_          = DiskFileHeader{};
  header_dirty_    = false;
  closed_          = false;
  closing_         = false;
  active_handles_.store(0, std::memory_order_relaxed);
  free_pages_.clear();
  frame_limit_  = frame_limit;
  pool_id_      = pool_id;
  frame_manager_ = frame_manager;

  const char *fmode = mode == ReadWriteMode::READ_WRITE ? "r+b" : "rb";
  handle_           = fopen(path.c_str(), fmode);
  if (handle_ == nullptr) {
    closed_ = true;
    frame_manager_ = nullptr;
    LOG_ERROR("failed to open file. path=%s, mode=%s", path.c_str(), fmode);
    return RC::FILE_OPEN;
  }

  if (mode_ == ReadWriteMode::READ_ONLY) {
    std::error_code ec;
    const auto      size = std::filesystem::file_size(wal_path_, ec);
    if (!ec && size > sizeof(uint32_t) * 4) {
      LOG_ERROR("read-only open requires WAL recovery first. file=%s", path.c_str());
      fclose(handle_);
      handle_ = nullptr;
      closed_ = true;
      frame_manager_ = nullptr;
      return RC::BUFFERPOOL_WAL_ERROR;
    }
  }

  if (enable_wal_) {
    wal_ = std::make_unique<PageWAL>(wal_path_);
    RC rc = wal_->open(true);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to open WAL. file=%s, rc=%s", wal_path_.c_str(), strrc(rc));
      fclose(handle_);
      handle_ = nullptr;
      closed_ = true;
      frame_manager_ = nullptr;
      wal_.reset();
      return rc;
    }

    rc = wal_->replay([this](PageNum page_num, const char *data) {
      return write_page_raw(page_num, data);
    });
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to replay WAL. file=%s, rc=%s", wal_path_.c_str(), strrc(rc));
      fclose(handle_);
      handle_ = nullptr;
      closed_ = true;
      frame_manager_ = nullptr;
      wal_.reset();
      return rc;
    }

    rc = sync_data_file();
    if (OB_FAIL(rc)) {
      fclose(handle_);
      handle_ = nullptr;
      closed_ = true;
      frame_manager_ = nullptr;
      wal_.reset();
      return rc;
    }
    rc = wal_->checkpoint();
    if (OB_FAIL(rc)) {
      fclose(handle_);
      handle_ = nullptr;
      closed_ = true;
      frame_manager_ = nullptr;
      wal_.reset();
      return rc;
    }
  }

  RC rc = read_header();
  if (OB_FAIL(rc)) {
    fclose(handle_);
    handle_ = nullptr;
    closed_ = true;
    frame_manager_ = nullptr;
    wal_.reset();
    return rc;
  }

  rc = validate_free_list();
  if (OB_FAIL(rc)) {
    fclose(handle_);
    handle_ = nullptr;
    closed_ = true;
    frame_manager_ = nullptr;
    wal_.reset();
    return rc;
  }

  closed_ = false;
  return RC::SUCCESS;
}
RC DiskBufferPool::close()
{
  {
    std::lock_guard<std::mutex> guard(meta_mutex_);
    if (closed_ && handle_ == nullptr) {
      return RC::SUCCESS;
    }
    if (active_handles_.load(std::memory_order_relaxed) > 0) {
      stats_.handle_busy.fetch_add(1, std::memory_order_relaxed);
      return RC::BUFFERPOOL_HANDLE_BUSY;
    }
    closing_ = true;
  }

  if (frame_manager_ != nullptr) {
    frame_manager_->cancel_owner(this);
  }

  RC rc = flush_all_buffers();
  if (OB_FAIL(rc)) {
    if (frame_manager_ != nullptr) {
      frame_manager_->resume_owner(this);
    }
    std::lock_guard<std::mutex> guard(meta_mutex_);
    closing_ = false;
    return rc;
  }

  if (frame_manager_ != nullptr) {
    RC remove_rc = frame_manager_->remove_owner(this);
    if (OB_FAIL(remove_rc)) {
      /// Keep the file open and the frame ownership intact so a later close()
      /// can retry instead of touching an owner that is still registered.
      frame_manager_->resume_owner(this);
      std::lock_guard<std::mutex> guard(meta_mutex_);
      closing_ = false;
      return remove_rc;
    }
  }

  {
    std::lock_guard<std::mutex> guard(meta_mutex_);
    if (wal_) {
      wal_->close();
      wal_.reset();
    }
    if (handle_ != nullptr) {
      fclose(handle_);
      handle_ = nullptr;
    }
    free_pages_.clear();
    frame_manager_ = nullptr;
    owned_frame_manager_.reset();
    closed_       = true;
    closing_      = false;
    header_dirty_ = false;
  }
  return rc;
}

PageNum DiskBufferPool::page_count() const
{
  std::lock_guard<std::mutex> guard(meta_mutex_);
  return header_.page_count;
}

int32_t DiskBufferPool::free_page_count() const
{
  std::lock_guard<std::mutex> guard(meta_mutex_);
  return header_.free_page_count;
}

bool DiskBufferPool::is_open() const
{
  std::lock_guard<std::mutex> guard(meta_mutex_);
  return !closed_ && !closing_ && handle_ != nullptr;
}

RC DiskBufferPool::read_header()
{
  char page[BP_PAGE_SIZE];
  RC   rc = read_page_raw(BP_HEADER_PAGE, page);
  if (OB_FAIL(rc)) {
    return rc;
  }
  rc = decode_header(page, &header_);
  if (OB_FAIL(rc)) {
    stats_.corrupted.fetch_add(1, std::memory_order_relaxed);
    return rc;
  }
  header_dirty_ = false;
  return validate_header();
}

RC DiskBufferPool::write_header_locked()
{
  char page[BP_PAGE_SIZE];
  encode_header(header_, page);
  RC rc = write_page_with_wal(BP_HEADER_PAGE, page);
  if (OB_SUCC(rc)) {
    header_dirty_ = false;
  }
  return rc;
}

RC DiskBufferPool::validate_header() const
{
  if (header_.magic != BP_FILE_MAGIC || header_.version != BP_FORMAT_VERSION) {
    return RC::BUFFERPOOL_CORRUPTED;
  }
  if (header_.header_size != sizeof(DiskFileHeader) || header_.page_size != BP_PAGE_SIZE) {
    return RC::BUFFERPOOL_CORRUPTED;
  }
  if (header_.page_count < 1 || header_.free_page_count < 0 ||
      header_.free_page_count >= header_.page_count) {
    return RC::BUFFERPOOL_CORRUPTED;
  }
  if (header_.first_free_page != INVALID_PAGE_NUM &&
      (header_.first_free_page <= BP_HEADER_PAGE || header_.first_free_page >= header_.page_count)) {
    return RC::BUFFERPOOL_CORRUPTED;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::validate_free_list()
{
  free_pages_.clear();
  PageNum current = header_.first_free_page;
  int     guard   = 0;
  char    raw[BP_PAGE_SIZE];

  while (current != INVALID_PAGE_NUM) {
    if (current <= BP_HEADER_PAGE || current >= header_.page_count || guard++ > header_.page_count) {
      stats_.corrupted.fetch_add(1, std::memory_order_relaxed);
      return RC::BUFFERPOOL_CORRUPTED;
    }
    if (!free_pages_.insert(current).second) {
      stats_.corrupted.fetch_add(1, std::memory_order_relaxed);
      return RC::BUFFERPOOL_CORRUPTED;
    }

    RC rc = read_page_raw(current, raw);
    if (OB_FAIL(rc)) {
      return rc;
    }
    PageNum next = INVALID_PAGE_NUM;
    memcpy(&next, raw, sizeof(next));
    current = next;
  }

  if (static_cast<int32_t>(free_pages_.size()) != header_.free_page_count) {
    stats_.corrupted.fetch_add(1, std::memory_order_relaxed);
    return RC::BUFFERPOOL_CORRUPTED;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::seek(int64_t offset)
{
  if (handle_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  if (file_seek(handle_, offset) != 0) {
    LOG_ERROR("failed to seek. file=%s, offset=%s", file_name_.c_str(), std::to_string(offset).c_str());
    return RC::IOERR_SEEK;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::read_page_raw_locked(PageNum page_num, char *data)
{
  RC rc = seek(static_cast<int64_t>(page_num) * BP_PAGE_SIZE);
  if (OB_FAIL(rc)) {
    return rc;
  }
  const size_t got = fread(data, 1, BP_PAGE_SIZE, handle_);
  if (got != BP_PAGE_SIZE) {
    LOG_ERROR("short page read. file=%s, page_num=%d, got=%d",
              file_name_.c_str(), page_num, static_cast<int>(got));
    return RC::BUFFERPOOL_CORRUPTED;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::read_page_raw(PageNum page_num, char *data)
{
  std::lock_guard<std::mutex> guard(io_mutex_);
  return read_page_raw_locked(page_num, data);
}

RC DiskBufferPool::write_page_raw_locked(PageNum page_num, const char *data)
{
  RC rc = seek(static_cast<int64_t>(page_num) * BP_PAGE_SIZE);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (fwrite(data, 1, BP_PAGE_SIZE, handle_) != BP_PAGE_SIZE) {
    LOG_ERROR("failed to write page. file=%s, page_num=%d", file_name_.c_str(), page_num);
    return RC::IOERR_WRITE;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::write_page_raw(PageNum page_num, const char *data)
{
  std::lock_guard<std::mutex> guard(io_mutex_);
  return write_page_raw_locked(page_num, data);
}

RC DiskBufferPool::write_page_with_wal(PageNum page_num, const char *data)
{
  /// Serialize WAL append + data-page write with checkpoint. Otherwise a
  /// checkpoint could truncate the WAL between the append and the data write.
  std::lock_guard<std::mutex> flush_guard(flush_mutex_);
  if (wal_) {
    RC rc = wal_->append_and_sync(page_num, data);
    if (OB_FAIL(rc)) {
      LOG_ERROR("WAL append failed. file=%s, page_num=%d, rc=%s",
                file_name_.c_str(), page_num, strrc(rc));
      return rc;
    }
    stats_.wal_writes.fetch_add(1, std::memory_order_relaxed);
    stats_.wal_syncs.fetch_add(1, std::memory_order_relaxed);
  }

  fault_inject_point("before_single_page_write");
  RC rc = write_page_raw(page_num, data);
  if (OB_SUCC(rc)) {
    stats_.writes.fetch_add(1, std::memory_order_relaxed);
    fault_inject_point("after_single_page_write");
  }
  return rc;
}

RC DiskBufferPool::write_two_pages_with_wal(PageNum page_a, const char *data_a,
                                              PageNum page_b, const char *data_b)
{
  /// A single WAL record makes the allocation/disposal of a page atomic with
  /// the header update. Without it a crash can expose a page that the header
  /// does not know about (or a free page that is still reachable).
  std::lock_guard<std::mutex> flush_guard(flush_mutex_);
  if (wal_) {
    RC rc = wal_->append_two_pages_and_sync(page_a, data_a, page_b, data_b);
    if (OB_FAIL(rc)) {
      LOG_ERROR("WAL append failed. file=%s, page_a=%d, page_b=%d, rc=%s",
                file_name_.c_str(), page_a, page_b, strrc(rc));
      return rc;
    }
    stats_.wal_writes.fetch_add(1, std::memory_order_relaxed);
    stats_.wal_syncs.fetch_add(1, std::memory_order_relaxed);
  }

  /// Keep both writes under one io_mutex_ section: a concurrent reader must
  /// not observe the new header before the corresponding data page is zeroed
  /// or linked into the free list.
  std::lock_guard<std::mutex> io_guard(io_mutex_);
  fault_inject_point("before_two_page_write");
  RC rc = write_page_raw_locked(page_a, data_a);
  if (OB_FAIL(rc)) {
    return rc;
  }
  fault_inject_point("after_first_page_write");
  rc = write_page_raw_locked(page_b, data_b);
  if (OB_FAIL(rc)) {
    return rc;
  }
  stats_.writes.fetch_add(2, std::memory_order_relaxed);
  fault_inject_point("after_two_page_write");
  return RC::SUCCESS;
}

RC DiskBufferPool::sync_data_file_locked()
{
  if (handle_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  if (mode_ == ReadWriteMode::READ_ONLY) {
    return RC::SUCCESS;
  }
  RC rc = sync_file(handle_);
  if (OB_FAIL(rc)) {
    LOG_ERROR("failed to sync data file. file=%s, rc=%s", file_name_.c_str(), strrc(rc));
  }
  return rc;
}

RC DiskBufferPool::sync_data_file()
{
  std::lock_guard<std::mutex> guard(io_mutex_);
  return sync_data_file_locked();
}

void DiskBufferPool::lock_frame_shared(FrameId frame_id)
{
  ASSERT(frame_manager_ != nullptr, "buffer pool is not open. file=%s", file_name_.c_str());
  frame_manager_->lock_shared(frame_id);
}

void DiskBufferPool::lock_frame_exclusive(FrameId frame_id)
{
  ASSERT(frame_manager_ != nullptr, "buffer pool is not open. file=%s", file_name_.c_str());
  frame_manager_->lock_exclusive(frame_id);
}

void DiskBufferPool::unlock_frame_shared(FrameId frame_id)
{
  if (frame_manager_ != nullptr) {
    frame_manager_->unlock_shared(frame_id);
  }
}

void DiskBufferPool::unlock_frame_exclusive(FrameId frame_id)
{
  if (frame_manager_ != nullptr) {
    frame_manager_->unlock_exclusive(frame_id);
  }
}

void DiskBufferPool::release_handle()
{
  const int old = active_handles_.fetch_sub(1, std::memory_order_acq_rel);
  ASSERT(old > 0, "handle count underflow. file=%s", file_name_.c_str());
}

RC DiskBufferPool::allocate_page(PageNum *page_num)
{
  if (page_num == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  std::lock_guard<std::mutex> guard(meta_mutex_);
  if (closed_ || closing_) {
    return RC::FILE_NOT_OPENED;
  }
  if (mode_ == ReadWriteMode::READ_ONLY) {
    return RC::IOERR_ACCESS;
  }

  if (header_.first_free_page != INVALID_PAGE_NUM) {
    const PageNum reuse = header_.first_free_page;
    if (free_pages_.count(reuse) == 0) {
      stats_.corrupted.fetch_add(1, std::memory_order_relaxed);
      return RC::BUFFERPOOL_CORRUPTED;
    }

    /// A prefetch may have queued this page before it was freed. Cancel it and
    /// drop any stale cached frame before reusing the page.
    if (frame_manager_ != nullptr) {
      frame_manager_->cancel_prefetch(this, reuse);
      RC remove_rc = frame_manager_->remove_page(this, reuse);
      if (OB_FAIL(remove_rc)) {
        return remove_rc;
      }
    }

    char raw[BP_PAGE_SIZE];
    RC   rc = read_page_raw(reuse, raw);
    if (OB_FAIL(rc)) {
      return rc;
    }

    PageNum next_free = INVALID_PAGE_NUM;
    memcpy(&next_free, raw, sizeof(next_free));
    if (next_free != INVALID_PAGE_NUM &&
        (next_free <= BP_HEADER_PAGE || next_free >= header_.page_count)) {
      stats_.corrupted.fetch_add(1, std::memory_order_relaxed);
      return RC::BUFFERPOOL_CORRUPTED;
    }
    DiskFileHeader new_header           = header_;
    new_header.first_free_page          = next_free;
    new_header.free_page_count         -= 1;
    char header_page[BP_PAGE_SIZE];
    encode_header(new_header, header_page);
    memset(raw, 0, sizeof(raw));

    /// Persist the new header and the cleared page in one WAL record.
    rc = write_two_pages_with_wal(BP_HEADER_PAGE, header_page, reuse, raw);
    if (OB_FAIL(rc)) {
      return rc;
    }

    free_pages_.erase(reuse);
    header_       = new_header;
    header_dirty_ = false;
    *page_num     = reuse;
    return RC::SUCCESS;
  }

  const PageNum new_page = header_.page_count;
  DiskFileHeader new_header = header_;
  new_header.page_count += 1;
  char header_page[BP_PAGE_SIZE];
  encode_header(new_header, header_page);
  char raw[BP_PAGE_SIZE];
  memset(raw, 0, sizeof(raw));
  RC rc = write_two_pages_with_wal(BP_HEADER_PAGE, header_page, new_page, raw);
  if (OB_FAIL(rc)) {
    return rc;
  }

  header_       = new_header;
  header_dirty_ = false;
  *page_num     = new_page;
  return RC::SUCCESS;
}

RC DiskBufferPool::dispose_page(PageNum page_num)
{
  std::lock_guard<std::mutex> guard(meta_mutex_);
  if (closed_ || closing_) {
    return RC::FILE_NOT_OPENED;
  }
  if (mode_ == ReadWriteMode::READ_ONLY) {
    return RC::IOERR_ACCESS;
  }
  if (page_num <= BP_HEADER_PAGE || page_num >= header_.page_count) {
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }
  if (free_pages_.count(page_num) > 0) {
    stats_.double_free.fetch_add(1, std::memory_order_relaxed);
    return RC::BUFFERPOOL_DOUBLE_FREE;
  }

  if (frame_manager_ != nullptr) {
    frame_manager_->cancel_prefetch(this, page_num);
    RC rc = frame_manager_->remove_page(this, page_num);
    if (OB_FAIL(rc)) {
      if (rc == RC::BUFFERPOOL_PAGE_PINNED) {
        LOG_ERROR("the page is pinned. file=%s, page_num=%d", file_name_.c_str(), page_num);
      }
      return rc;
    }
  }

  DiskFileHeader new_header           = header_;
  new_header.first_free_page          = page_num;
  new_header.free_page_count         += 1;
  char header_page[BP_PAGE_SIZE];
  encode_header(new_header, header_page);

  char raw[BP_PAGE_SIZE];
  memset(raw, 0, sizeof(raw));
  memcpy(raw, &header_.first_free_page, sizeof(header_.first_free_page));
  RC rc = write_two_pages_with_wal(BP_HEADER_PAGE, header_page, page_num, raw);
  if (OB_FAIL(rc)) {
    return rc;
  }

  free_pages_.insert(page_num);
  header_       = new_header;
  header_dirty_ = false;
  return RC::SUCCESS;
}

RC DiskBufferPool::flush_page(PageNum page_num)
{
  {
    std::lock_guard<std::mutex> guard(meta_mutex_);
    if (closed_) {
      return RC::FILE_NOT_OPENED;
    }
  }
  if (frame_manager_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  return frame_manager_->flush_page(this, page_num);
}

RC DiskBufferPool::flush_all_buffers()
{
  {
    std::lock_guard<std::mutex> guard(meta_mutex_);
    if (closed_) {
      return RC::FILE_NOT_OPENED;
    }
  }

  RC rc = frame_manager_ != nullptr ? frame_manager_->flush_owner(this) : RC::SUCCESS;

  bool any_dirty = false;
  {
    std::lock_guard<std::mutex> guard(meta_mutex_);
    if (OB_SUCC(rc) && header_dirty_) {
      rc = write_header_locked();
    }
    if (OB_SUCC(rc) && frame_manager_ != nullptr) {
      any_dirty = frame_manager_->has_dirty(this);
    }
  }

  if (OB_SUCC(rc) && !any_dirty) {
    std::lock_guard<std::mutex> flush_guard(flush_mutex_);
    rc = sync_data_file();
    if (OB_SUCC(rc) && wal_) {
      fault_inject_point("before_wal_checkpoint");
      rc = wal_->checkpoint(true);
      if (OB_SUCC(rc)) {
        stats_.checkpoints.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  stats_.flushes.fetch_add(1, std::memory_order_relaxed);
  return rc;
}

RC DiskBufferPool::checkpoint() { return flush_all_buffers(); }

RC DiskBufferPool::read_page_for_cache(PageNum page_num, char *data)
{
  return read_page_raw(page_num, data);
}

RC DiskBufferPool::flush_frame_data(PageNum page_num, const char *data)
{
  return write_page_with_wal(page_num, data);
}

RC DiskBufferPool::get_page(PageNum page_num, PageHandle &handle, PageAccess access)
{
  handle.reset();

  std::unique_lock<std::mutex> guard(meta_mutex_);
  if (closed_ || closing_) {
    return RC::FILE_NOT_OPENED;
  }
  if (page_num <= BP_HEADER_PAGE || page_num >= header_.page_count) {
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }
  if (free_pages_.count(page_num) > 0) {
    return RC::BUFFERPOOL_PAGE_FREED;
  }
  if (frame_manager_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }

  FrameId frame_id = INVALID_FRAME_ID;
  RC      rc       = frame_manager_->acquire(this, page_num, &frame_id);
  if (OB_FAIL(rc)) {
    return rc;
  }

  active_handles_.fetch_add(1, std::memory_order_acq_rel);
  guard.unlock();

  if (access == PageAccess::READ) {
    lock_frame_shared(frame_id);
  } else {
    lock_frame_exclusive(frame_id);
  }

  handle = PageHandle(this, frame_id, page_num, access);
  return RC::SUCCESS;
}

RC DiskBufferPool::get_page_read(PageNum page_num, PageHandle &handle)
{
  return get_page(page_num, handle, PageAccess::READ);
}

RC DiskBufferPool::get_page_write(PageNum page_num, PageHandle &handle)
{
  return get_page(page_num, handle, PageAccess::WRITE);
}

RC DiskBufferPool::unpin_page(FrameId frame_id)
{
  if (frame_manager_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  return frame_manager_->unpin(frame_id);
}

RC DiskBufferPool::mark_page_dirty(FrameId frame_id)
{
  if (frame_manager_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  if (mode_ == ReadWriteMode::READ_ONLY) {
    return RC::IOERR_ACCESS;
  }
  return frame_manager_->mark_dirty(frame_id);
}

char *DiskBufferPool::frame_data(FrameId frame_id)
{
  ASSERT(frame_manager_ != nullptr, "buffer pool is not open. file=%s", file_name_.c_str());
  return frame_manager_->data(frame_id);
}

RC DiskBufferPool::prefetch_page(PageNum page_num)
{
  std::lock_guard<std::mutex> guard(meta_mutex_);
  if (closed_ || closing_) {
    return RC::FILE_NOT_OPENED;
  }
  if (page_num <= BP_HEADER_PAGE || page_num >= header_.page_count) {
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }
  if (free_pages_.count(page_num) > 0) {
    return RC::BUFFERPOOL_PAGE_FREED;
  }
  if (frame_manager_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  frame_manager_->prefetch(this, page_num);
  stats_.prefetches.fetch_add(1, std::memory_order_relaxed);
  return RC::SUCCESS;
}

RC DiskBufferPool::prefetch_range(PageNum start_page, int count)
{
  if (count < 0) {
    return RC::INVALID_ARGUMENT;
  }
  for (int i = 0; i < count; ++i) {
    RC rc = prefetch_page(start_page + i);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  return RC::SUCCESS;
}

// ---------------------------------------------------------------------------
// DiskBufferPoolManager
// ---------------------------------------------------------------------------

DiskBufferPoolManager::DiskBufferPoolManager(int frame_limit_per_file, int total_frame_limit)
    : frame_limit_(std::max(1, frame_limit_per_file)),
      total_frame_limit_(std::max(1, total_frame_limit)),
      frame_manager_(std::max(1, total_frame_limit))
{}

DiskBufferPoolManager::~DiskBufferPoolManager()
{
  RC rc = close();
  if (OB_FAIL(rc)) {
    LOG_WARN("buffer pool manager close on destruction failed. rc=%s", strrc(rc));
  }
}

std::string DiskBufferPoolManager::normalize_path(const std::string &path)
{
  std::error_code ec;
  auto            absolute = std::filesystem::absolute(path, ec);
  if (ec) {
    return path;
  }
  return absolute.lexically_normal().string();
}

RC DiskBufferPoolManager::create_file(const std::string &path)
{
  const std::string normalized = normalize_path(path);
  std::lock_guard<std::mutex> guard(mutex_);
  if (by_path_.find(normalized) != by_path_.end()) {
    return RC::BUFFERPOOL_OPEN;
  }
  if (std::filesystem::exists(path)) {
    return RC::FILE_EXIST;
  }

  DiskBufferPool pool;
  // Keep canonical identity separate from I/O; relative paths remain usable in Chinese Windows directories.
  return pool.create_file(path);
}

RC DiskBufferPoolManager::open_file(const std::string &path, ReadWriteMode mode, DiskBufferPool **pool)
{
  if (pool == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  const std::string normalized = normalize_path(path);
  std::lock_guard<std::mutex> guard(mutex_);

  auto iter = by_path_.find(normalized);
  if (iter != by_path_.end()) {
    DiskBufferPool *existing = iter->second;
    if (mode == ReadWriteMode::READ_WRITE && existing->is_read_only()) {
      return RC::IOERR_ACCESS;
    }
    *pool = existing;
    return RC::SUCCESS;
  }

  auto owner = std::make_unique<DiskBufferPool>();
  const int pool_id = next_pool_id_++;
  RC   rc    = owner->open_with_frame_manager(path, mode, frame_limit_, true, &frame_manager_, pool_id);
  if (OB_FAIL(rc)) {
    LOG_ERROR("failed to open buffer pool file. path=%s, rc=%s", normalized.c_str(), strrc(rc));
    return rc;
  }

  DiskBufferPool *raw = owner.get();
  pools_.push_back(std::move(owner));
  by_path_[normalized] = raw;
  *pool = raw;
  return RC::SUCCESS;
}

DiskBufferPool *DiskBufferPoolManager::get_pool(const std::string &path) const
{
  const std::string normalized = normalize_path(path);
  std::lock_guard<std::mutex> guard(mutex_);
  auto iter = by_path_.find(normalized);
  return iter == by_path_.end() ? nullptr : iter->second;
}

RC DiskBufferPoolManager::close_file(const std::string &path)
{
  const std::string normalized = normalize_path(path);
  std::lock_guard<std::mutex> guard(mutex_);

  auto iter = by_path_.find(normalized);
  if (iter == by_path_.end()) {
    return RC::FILE_NOT_EXIST;
  }

  DiskBufferPool *target = iter->second;
  RC              rc     = target->close();
  if (OB_FAIL(rc)) {
    return rc;
  }

  by_path_.erase(iter);
  pools_.erase(std::remove_if(pools_.begin(), pools_.end(),
                              [target](const std::unique_ptr<DiskBufferPool> &pool) {
                                return pool.get() == target;
                              }),
               pools_.end());
  return RC::SUCCESS;
}

RC DiskBufferPoolManager::flush_all()
{
  std::lock_guard<std::mutex> guard(mutex_);
  RC                          rc = RC::SUCCESS;
  for (auto &pool : pools_) {
    RC one = pool->flush_all_buffers();
    if (OB_FAIL(one) && OB_SUCC(rc)) {
      rc = one;
    }
  }
  return rc;
}

RC DiskBufferPoolManager::close()
{
  std::lock_guard<std::mutex> guard(mutex_);
  RC                          rc = RC::SUCCESS;
  for (auto &pool : pools_) {
    RC one = pool->close();
    if (OB_FAIL(one) && OB_SUCC(rc)) {
      rc = one;
    }
  }
  if (OB_SUCC(rc)) {
    pools_.clear();
    by_path_.clear();
  }
  return rc;
}

int DiskBufferPoolManager::used_frames() const
{
  return frame_manager_.used_frames();
}

int DiskBufferPoolManager::remaining_frames() const
{
  return total_frame_limit_ - frame_manager_.used_frames();
}
