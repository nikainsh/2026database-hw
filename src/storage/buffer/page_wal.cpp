/* database26 buffer manager - page-level write-ahead log */
#include "storage/buffer/page_wal.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "common/fault_inject.h"
#include "storage/buffer/checksum.h"
#include "storage/buffer/page.h"

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
constexpr uint32_t WAL_HEADER_MAGIC = 0x4D57414Cu;  // 'MWAL'
constexpr uint32_t WAL_RECORD_MAGIC = 0x4D575243u;  // 'MWRC'
constexpr uint16_t WAL_VERSION      = 1;

#pragma pack(push, 1)
struct WALHeader
{
  uint32_t magic       = WAL_HEADER_MAGIC;
  uint16_t version     = WAL_VERSION;
  uint16_t header_size = 0;
  uint32_t page_size   = BP_PAGE_SIZE;
  uint32_t reserved    = 0;
};

struct WALRecordHeader
{
  uint32_t magic     = WAL_RECORD_MAGIC;
  uint32_t size      = BP_PAGE_SIZE;
  uint32_t crc       = 0;
  uint32_t reserved  = 0;
  uint64_t lsn       = 0;
  int32_t  page_num  = INVALID_PAGE_NUM;
  int32_t  reserved2 = 0;
};
#pragma pack(pop)

static_assert(sizeof(WALHeader) == 16, "unexpected WAL header layout");
static_assert(sizeof(WALRecordHeader) == 32, "unexpected WAL record header layout");

int file_seek(FILE *fp, int64_t offset)
{
#ifdef _WIN32
  return _fseeki64(fp, offset, SEEK_SET);
#else
  return fseeko(fp, static_cast<off_t>(offset), SEEK_SET);
#endif
}

int file_seek_end(FILE *fp)
{
#ifdef _WIN32
  return _fseeki64(fp, 0, SEEK_END);
#else
  return fseeko(fp, 0, SEEK_END);
#endif
}

RC sync_file(FILE *fp)
{
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

RC write_wal_header(FILE *fp)
{
  WALHeader header;
  header.header_size = sizeof(WALHeader);
  if (file_seek(fp, 0) != 0) {
    return RC::IOERR_SEEK;
  }
  if (fwrite(&header, 1, sizeof(header), fp) != sizeof(header)) {
    return RC::IOERR_WRITE;
  }
  return sync_file(fp);
}

bool atomic_replace(const std::string &from, const std::string &to)
{
  /// std::filesystem::rename replaces an existing target on the supported
  /// platforms (MoveFileEx/rename semantics); keeping both paths in the same
  /// directory makes the replacement atomic from the reader's perspective.
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  return !ec;
}

RC sync_parent_directory(const std::string &path)
{
#ifdef _WIN32
  (void)path;
  return RC::SUCCESS;
#else
  std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  const int fd = open(parent.c_str(), O_RDONLY);
  if (fd < 0) {
    return RC::IOERR_SYNC;
  }
  const int rc = fsync(fd);
  close(fd);
  return rc == 0 ? RC::SUCCESS : RC::IOERR_SYNC;
#endif
}
}  // namespace

PageWAL::PageWAL(std::string path) : path_(std::move(path)) {}

PageWAL::~PageWAL() { close(); }

RC PageWAL::open(bool create_if_missing)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (file_ != nullptr) {
    return RC::SUCCESS;
  }

  const bool exists = std::filesystem::exists(path_);
  if (!exists && !create_if_missing) {
    return RC::FILE_NOT_EXIST;
  }

  /// A crash between writing the checkpoint temp file and replacing the old
  /// WAL leaves a stale temp file behind. The old WAL (or the new one after a
  /// successful replace) is always the source of truth.
  std::error_code temp_ec;
  std::filesystem::remove(path_ + ".tmp", temp_ec);

  file_ = fopen(path_.c_str(), exists ? "r+b" : "w+b");
  if (file_ == nullptr) {
    return RC::FILE_OPEN;
  }

  std::error_code ec;
  const auto      size = exists ? std::filesystem::file_size(path_, ec) : 0;
  if (!exists || (!ec && size == 0)) {
    RC rc = write_header_locked();
    if (OB_FAIL(rc)) {
      fclose(file_);
      file_ = nullptr;
      return rc;
    }
    return RC::SUCCESS;
  }

  RC rc = read_header_locked();
  if (OB_FAIL(rc)) {
    fclose(file_);
    file_ = nullptr;
    return rc;
  }

  if (file_seek(file_, 0) != 0) {
    fclose(file_);
    file_ = nullptr;
    return RC::IOERR_SEEK;
  }
  return RC::SUCCESS;
}

RC PageWAL::close()
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (file_ != nullptr) {
    fclose(file_);
    file_ = nullptr;
  }
  return RC::SUCCESS;
}

RC PageWAL::write_header_locked() { return write_wal_header(file_); }

RC PageWAL::read_header_locked()
{
  WALHeader header;
  if (file_seek(file_, 0) != 0) {
    return RC::IOERR_SEEK;
  }
  const size_t got = fread(&header, 1, sizeof(header), file_);
  if (got != sizeof(header)) {
    return RC::BUFFERPOOL_WAL_ERROR;
  }
  if (header.magic != WAL_HEADER_MAGIC || header.version != WAL_VERSION ||
      header.header_size != sizeof(WALHeader) || header.page_size != BP_PAGE_SIZE) {
    return RC::BUFFERPOOL_WAL_ERROR;
  }
  return RC::SUCCESS;
}

RC PageWAL::seek_locked(int64_t offset)
{
  if (file_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  if (file_seek(file_, offset) != 0) {
    return RC::IOERR_SEEK;
  }
  return RC::SUCCESS;
}

RC PageWAL::sync_locked()
{
  if (file_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  return sync_file(file_);
}

RC PageWAL::replay(const std::function<RC(PageNum, const char *)> &writer)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (file_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  if (writer == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  RC rc = read_header_locked();
  if (OB_FAIL(rc)) {
    return rc;
  }

  std::vector<char> record(sizeof(WALRecordHeader) + BP_PAGE_SIZE);
  while (true) {
    WALRecordHeader header;
    const size_t header_got = fread(&header, 1, sizeof(header), file_);
    if (header_got == 0) {
      break;  // clean EOF
    }
    if (header_got != sizeof(header)) {
      break;  // torn tail
    }
    const bool two_pages = header.size == 2 * BP_PAGE_SIZE;
    if (header.magic != WAL_RECORD_MAGIC ||
        (header.size != BP_PAGE_SIZE && !two_pages) ||
        header.page_num < 0 || header.page_num > (1 << 30) ||
        (two_pages && (header.reserved2 < 0 || header.reserved2 > (1 << 30)))) {
      break;
    }

    if (two_pages) {
      record.resize(sizeof(header) + 2 * BP_PAGE_SIZE);
    } else {
      record.resize(sizeof(header) + BP_PAGE_SIZE);
    }
    const size_t data_got = fread(record.data() + sizeof(header), 1, header.size, file_);
    if (data_got != header.size) {
      break;  // torn tail
    }

    memcpy(record.data(), &header, sizeof(header));
    WALRecordHeader *stored = reinterpret_cast<WALRecordHeader *>(record.data());
    const uint32_t   expect_crc = stored->crc;
    stored->crc = 0;
    const uint32_t actual_crc = crc32(record.data(), record.size());
    if (actual_crc != expect_crc) {
      break;  // corrupt tail
    }

    rc = writer(static_cast<PageNum>(header.page_num), record.data() + sizeof(header));
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (two_pages) {
      rc = writer(static_cast<PageNum>(header.reserved2),
                  record.data() + sizeof(header) + BP_PAGE_SIZE);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
    if (header.lsn >= last_lsn_) {
      last_lsn_ = header.lsn + 1;
    }
  }

  return RC::SUCCESS;
}

RC PageWAL::append_and_sync(PageNum page_num, const char *data)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (file_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  if (data == nullptr || page_num < 0) {
    return RC::INVALID_ARGUMENT;
  }

  WALRecordHeader header;
  header.lsn      = last_lsn_;
  header.page_num = page_num;
  header.crc      = 0;

  std::vector<char> record(sizeof(header) + BP_PAGE_SIZE);
  memcpy(record.data(), &header, sizeof(header));
  memcpy(record.data() + sizeof(header), data, BP_PAGE_SIZE);
  header.crc = crc32(record.data(), record.size());
  memcpy(record.data(), &header, sizeof(header));

  if (file_seek_end(file_) != 0) {
    return RC::IOERR_SEEK;
  }
  if (fwrite(record.data(), 1, record.size(), file_) != record.size()) {
    return RC::IOERR_WRITE;
  }
  RC rc = sync_file(file_);
  if (OB_SUCC(rc)) {
    ++last_lsn_;
  }
  return rc;
}

RC PageWAL::append_two_pages_and_sync(PageNum page_a, const char *data_a,
                                         PageNum page_b, const char *data_b)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (file_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  if (data_a == nullptr || data_b == nullptr || page_a < 0 || page_b < 0) {
    return RC::INVALID_ARGUMENT;
  }

  WALRecordHeader header;
  header.lsn       = last_lsn_;
  header.size      = 2 * BP_PAGE_SIZE;
  header.page_num  = page_a;
  header.reserved2 = page_b;
  header.crc       = 0;

  std::vector<char> record(sizeof(header) + 2 * BP_PAGE_SIZE);
  memcpy(record.data(), &header, sizeof(header));
  memcpy(record.data() + sizeof(header), data_a, BP_PAGE_SIZE);
  memcpy(record.data() + sizeof(header) + BP_PAGE_SIZE, data_b, BP_PAGE_SIZE);
  header.crc = crc32(record.data(), record.size());
  memcpy(record.data(), &header, sizeof(header));

  if (file_seek_end(file_) != 0) {
    return RC::IOERR_SEEK;
  }
  if (fwrite(record.data(), 1, record.size(), file_) != record.size()) {
    return RC::IOERR_WRITE;
  }
  RC rc = sync_file(file_);
  if (OB_SUCC(rc)) {
    ++last_lsn_;
  }
  return rc;
}

RC PageWAL::checkpoint(bool crash_points)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (file_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }

  /// Replace the WAL atomically instead of truncating it in place. A crash
  /// must leave either the old WAL with all records or a valid empty WAL; a
  /// zero-length/partially-written header would make the data file unusable
  /// even though its pages were already fsync'ed.
  const std::string temp_path = path_ + ".tmp";
  FILE             *temp_file = fopen(temp_path.c_str(), "wb");
  if (temp_file == nullptr) {
    return RC::FILE_OPEN;
  }

  RC rc = write_wal_header(temp_file);
  if (fclose(temp_file) != 0 && OB_SUCC(rc)) {
    rc = RC::IOERR_CLOSE;
  }
  if (OB_FAIL(rc)) {
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    return rc;
  }

  if (crash_points) {
    fault_inject_point("before_checkpoint_replace");
  }

  if (fclose(file_) != 0) {
    file_ = fopen(path_.c_str(), "r+b");
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    return RC::IOERR_CLOSE;
  }
  file_ = nullptr;

  if (!atomic_replace(temp_path, path_)) {
    /// The old WAL is still in place on failure; reopen it so the caller can
    /// retry checkpoint after handling the I/O problem.
    file_ = fopen(path_.c_str(), "r+b");
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    return RC::IOERR_WRITE;
  }

  rc = sync_parent_directory(path_);
  if (OB_FAIL(rc)) {
    file_ = fopen(path_.c_str(), "r+b");
    return rc;
  }

  if (crash_points) {
    fault_inject_point("after_checkpoint_replace");
  }

  file_ = fopen(path_.c_str(), "r+b");
  if (file_ == nullptr) {
    return RC::FILE_OPEN;
  }
  return RC::SUCCESS;
}
