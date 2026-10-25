/* database26 buffer manager - thread-safe page cache with WAL */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/rc.h"
#include "common/types.h"
#include "storage/buffer/buffer_pool_stats.h"
#include "storage/buffer/frame.h"
#include "storage/buffer/frame_manager.h"
#include "storage/buffer/page.h"
#include "storage/buffer/page_wal.h"
#include "storage/buffer/replacement.h"

class DiskBufferPool;
class PageHandle;

/// File header magic: 'M''B''P''2'
static constexpr uint32_t BP_FILE_MAGIC = 0x4D425032u;
static constexpr uint16_t BP_FORMAT_VERSION = 2;

#pragma pack(push, 1)
/**
 * @brief Page 0 of every buffer-pool file.
 *
 * Layout is versioned and packed. Page 1 is intentionally left to the upper
 * layer (record file header / index header), so the buffer manager only owns
 * page 0 plus page-number allocation metadata.
 */
struct DiskFileHeader
{
  uint32_t magic           = BP_FILE_MAGIC;
  uint16_t version         = BP_FORMAT_VERSION;
  uint16_t header_size     = 0;
  uint32_t page_size       = BP_PAGE_SIZE;
  int32_t  page_count      = 1;
  int32_t  first_free_page = INVALID_PAGE_NUM;
  int32_t  free_page_count = 0;
  uint64_t checkpoint_lsn  = 0;
  uint32_t header_crc      = 0;
};
#pragma pack(pop)

static_assert(sizeof(DiskFileHeader) <= BP_PAGE_SIZE, "buffer pool header is too large");

enum class PageAccess
{
  READ,
  WRITE,
};

/**
 * @brief RAII page handle.
 *
 * The handle holds a shared (READ) or exclusive (WRITE) page latch until reset
 * or destruction. It also owns one pin on the frame, so the frame cannot be
 * evicted while the handle is alive.
 */
class PageHandle
{
public:
  PageHandle() = default;
  PageHandle(const PageHandle &)            = delete;
  PageHandle &operator=(const PageHandle &) = delete;

  PageHandle(PageHandle &&other) noexcept { swap(other); }
  PageHandle &operator=(PageHandle &&other) noexcept
  {
    if (this != &other) {
      reset();
      swap(other);
    }
    return *this;
  }

  ~PageHandle() { reset(); }

  void swap(PageHandle &other) noexcept
  {
    std::swap(pool_, other.pool_);
    std::swap(frame_id_, other.frame_id_);
    std::swap(page_id_, other.page_id_);
    std::swap(access_, other.access_);
  }

  void reset();

  char       *value() const;
  const char *const_data() const { return value(); }
  PageNum     page_id() const { return page_id_; }
  FrameId     frame_id() const { return frame_id_; }
  PageAccess  access() const { return access_; }
  bool        valid() const { return pool_ != nullptr; }

  /// Modify page content only through a WRITE handle, then call mark_dirty().
  RC mark_dirty();

private:
  friend class DiskBufferPool;
  PageHandle(DiskBufferPool *pool, FrameId frame_id, PageNum page_id, PageAccess access)
      : pool_(pool), frame_id_(frame_id), page_id_(page_id), access_(access)
  {}

  DiskBufferPool *pool_     = nullptr;
  FrameId         frame_id_ = INVALID_FRAME_ID;
  PageNum         page_id_  = INVALID_PAGE_NUM;
  PageAccess      access_   = PageAccess::READ;
};

/**
 * @brief One buffer pool per disk file.
 *
 * Thread-safety:
 *   - meta_mutex_ protects the page table, free list, header, replacement
 *     history, and frame metadata.
 *   - Each Frame has a shared_mutex protecting its 8KB page bytes.
 *   - pin_count and dirty are atomic.
 *   - Page misses / eviction / WAL appends are serialized by meta_mutex_ and
 *     flush_mutex_. This is correct but not the final word in scalability.
 */
class DiskBufferPool
{
public:
  DiskBufferPool();
  ~DiskBufferPool();

  DiskBufferPool(const DiskBufferPool &)            = delete;
  DiskBufferPool &operator=(const DiskBufferPool &) = delete;

  RC create_file(const std::string &path);
  RC open(const std::string &path, ReadWriteMode mode, int frame_limit, bool enable_wal = true);
  RC close();

  const std::string &file_name() const { return file_name_; }
  const std::string &wal_file_name() const { return wal_path_; }
  int                pool_id() const { return pool_id_; }

  PageNum page_count() const;
  int32_t free_page_count() const;
  int     frame_limit() const { return frame_limit_; }
  bool    is_read_only() const { return mode_ == ReadWriteMode::READ_ONLY; }
  bool    is_open() const;

  BufferPoolStatsSnapshot stats() const { return stats_.snapshot(); }

  /// Allocate a zero-filled page. The page is not returned pinned; call get_page.
  RC allocate_page(PageNum *page_num);
  /// Dispose an unpinned page and put it on the free list.
  RC dispose_page(PageNum page_num);

  /// Explicit access is required. Use get_page_read() / get_page_write().
  RC get_page(PageNum page_num, PageHandle &handle, PageAccess access);
  RC get_page_read(PageNum page_num, PageHandle &handle);
  RC get_page_write(PageNum page_num, PageHandle &handle);

  RC unpin_page(FrameId frame_id);
  RC mark_page_dirty(FrameId frame_id);

  /// Flush one cached page if dirty. Page must not be locked by a writer.
  RC flush_page(PageNum page_num);
  /// Flush all dirty pages, fsync data, then truncate the WAL (checkpoint).
  RC flush_all_buffers();
  /// Alias for flush_all_buffers().
  RC checkpoint();

  /// Synchronous warm-up prefetch: read the page into the cache and unpin it.
  RC prefetch_page(PageNum page_num);
  RC prefetch_range(PageNum start_page, int count);

  /// PageHandle::value() implementation.
  char *frame_data(FrameId frame_id);

private:
  friend class PageHandle;
  friend class FrameManager;
  friend class DiskBufferPoolManager;

  RC open_with_frame_manager(const std::string &path, ReadWriteMode mode, int frame_limit,
                             bool enable_wal, FrameManager *frame_manager, int pool_id);

  RC read_header();
  RC write_header_locked();
  RC validate_header() const;
  RC validate_free_list();

  RC read_page_raw(PageNum page_num, char *data);
  RC write_page_raw(PageNum page_num, const char *data);
  RC write_page_with_wal(PageNum page_num, const char *data);
  RC seek(int64_t offset);
  RC sync_data_file();

  /// Called by FrameManager while it owns the global frame lock.
  RC read_page_for_cache(PageNum page_num, char *data);
  RC flush_frame_data(PageNum page_num, const char *data);

  void lock_frame_shared(FrameId frame_id);
  void lock_frame_exclusive(FrameId frame_id);
  void unlock_frame_shared(FrameId frame_id);
  void unlock_frame_exclusive(FrameId frame_id);
  void release_handle();

private:
  std::string   file_name_;
  std::string   wal_path_;
  FILE         *handle_ = nullptr;
  ReadWriteMode mode_   = ReadWriteMode::READ_ONLY;
  bool          enable_wal_ = true;
  bool          header_dirty_ = false;
  bool          closed_       = true;
  bool          closing_      = false;

  DiskFileHeader header_;
  int            frame_limit_ = 0;
  int            pool_id_     = -1;
  std::unordered_set<PageNum> free_pages_;
  std::unique_ptr<PageWAL>    wal_;

  FrameManager             *frame_manager_ = nullptr;
  std::unique_ptr<FrameManager> owned_frame_manager_;

  mutable std::mutex meta_mutex_;
  std::mutex         flush_mutex_;
  std::atomic<int>   active_handles_{0};

  mutable BufferPoolStats stats_;
};

/**
 * @brief Manages all open buffer pools.
 *
 * A global frame budget (total_frame_limit) prevents "32 frames per file"
 * from growing without bound when many tables/indexes are open. Eviction is
 * still per-file; a later iteration can lift replacement to a truly global
 * frame manager.
 */
class DiskBufferPoolManager
{
public:
  explicit DiskBufferPoolManager(int frame_limit_per_file = 32, int total_frame_limit = 4096);
  ~DiskBufferPoolManager();

  DiskBufferPoolManager(const DiskBufferPoolManager &)            = delete;
  DiskBufferPoolManager &operator=(const DiskBufferPoolManager &) = delete;

  RC create_file(const std::string &path);
  RC open_file(const std::string &path, ReadWriteMode mode, DiskBufferPool **pool);
  RC close_file(const std::string &path);
  RC close();
  RC flush_all();

  DiskBufferPool *get_pool(const std::string &path) const;
  bool            is_open(const std::string &path) const { return get_pool(path) != nullptr; }

  int  total_frame_limit() const { return total_frame_limit_; }
  int  used_frames() const;
  int  remaining_frames() const;

private:
  static std::string normalize_path(const std::string &path);

private:
  int frame_limit_       = 32;
  int total_frame_limit_ = 4096;

  FrameManager frame_manager_;
  int          next_pool_id_ = 0;

  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<DiskBufferPool>> pools_;
  std::unordered_map<std::string, DiskBufferPool *> by_path_;
};
