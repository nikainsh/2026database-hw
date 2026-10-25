/* database26 buffer manager - page-level write-ahead log */
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

#include "common/rc.h"
#include "common/types.h"

/**
 * @brief Page-level redo WAL.
 *
 * This is intentionally a *buffer-manager building block*, not a full
 * transaction log. Every record contains a complete 8KB page image. Before a
 * dirty page is written to the data file, its image is appended to the WAL and
 * fsync'ed. On open, valid records are replayed in order and the WAL is then
 * truncated after the data file has been fsync'ed.
 *
 * Consequences:
 *   - Crash after WAL sync but before/during data-page write is recoverable.
 *   - Crash between two page writes can leave a transaction partially applied;
 *     transactional atomicity requires a higher-level transaction manager.
 *   - Full-page images are simple but write-heavy. A production system would
 *     use physiological logging / page LSN and only log changed bytes.
 */
class PageWAL
{
public:
  explicit PageWAL(std::string path);
  ~PageWAL();

  PageWAL(const PageWAL &)            = delete;
  PageWAL &operator=(const PageWAL &) = delete;

  RC open(bool create_if_missing);
  RC close();

  /// Replay valid records in order. Stops at the first torn/corrupt tail.
  RC replay(const std::function<RC(PageNum, const char *)> &writer);

  /// Append a full page image and fsync the WAL before returning.
  RC append_and_sync(PageNum page_num, const char *data);

  /// Append two page images as one atomic WAL record and fsync before returning.
  RC append_two_pages_and_sync(PageNum page_a, const char *data_a,
                               PageNum page_b, const char *data_b);

  /// Truncate the WAL. Caller must have fsync'ed the data file first.
  RC checkpoint();

  uint64_t last_lsn() const { return last_lsn_; }
  bool     is_open() const { return file_ != nullptr; }
  const std::string &path() const { return path_; }

private:
  RC write_header_locked();
  RC read_header_locked();
  RC seek_locked(int64_t offset);
  RC sync_locked();

private:
  std::string path_;
  FILE       *file_      = nullptr;
  mutable std::mutex mutex_;
  uint64_t    last_lsn_  = 0;
};
