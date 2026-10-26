/* database26 buffer manager - two-page allocation/disposal WAL recovery */
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "common/file_util.h"
#include "storage/buffer/checksum.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/buffer/page_wal.h"

static const std::string kDir    = "./_test_buf";
static const std::string kFile   = "./_test_buf/alloc_recovery.dat";
static const std::string kBefore = "./_test_buf/alloc_recovery.before";

static void cleanup()
{
  for (const auto &path : {kFile, kFile + ".wal", kBefore}) {
    path_remove_file(path);
  }
}

static void copy_file(const std::string &src, const std::string &dst)
{
  std::error_code ec;
  std::filesystem::remove(dst, ec);
  ec.clear();
  std::filesystem::copy_file(src, dst, std::filesystem::copy_options::none, ec);
  if (ec) {
    fprintf(stderr, "copy failed: %s -> %s: %s\n", src.c_str(), dst.c_str(), ec.message().c_str());
  }
  assert(!ec);
}

static void fill_page(char *page, int seed)
{
  memset(page, 0, BP_PAGE_SIZE);
  for (int i = 0; i < 64; ++i) {
    const int value = seed * 1000 + i;
    memcpy(page + i * sizeof(int), &value, sizeof(value));
  }
}

static void encode_header_for_test(const DiskFileHeader &in, char *page)
{
  memset(page, 0, BP_PAGE_SIZE);
  DiskFileHeader header = in;
  header.header_size    = sizeof(DiskFileHeader);
  header.header_crc     = 0;
  memcpy(page, &header, sizeof(header));
  header.header_crc = crc32(page, offsetof(DiskFileHeader, header_crc));
  memcpy(page, &header, sizeof(header));
}

static bool page_is_zero(DiskBufferPool &pool, PageNum page)
{
  PageHandle handle;
  if (OB_FAIL(pool.get_page_read(page, handle))) {
    return false;
  }
  const int *words = reinterpret_cast<const int *>(handle.const_data());
  for (int i = 0; i < 64; ++i) {
    if (words[i] != 0) {
      return false;
    }
  }
  return true;
}

static void test_allocate_recovery()
{
  cleanup();
  {
    DiskBufferPoolManager bpm(8, 64);
    assert(OB_SUCC(bpm.create_file(kFile)));
    DiskBufferPool *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
    assert(OB_SUCC(bpm.close()));
    copy_file(kFile, kBefore);
  }

  /// Simulate a crash after the atomic allocation record was fsync'ed but
  /// before either data page reached the file.
  {
    DiskFileHeader new_header;
    new_header.page_count = 2;
    char header_page[BP_PAGE_SIZE];
    encode_header_for_test(new_header, header_page);
    char zero_page[BP_PAGE_SIZE];
    memset(zero_page, 0, sizeof(zero_page));

    PageWAL wal(kFile + ".wal");
    assert(OB_SUCC(wal.open(true)));
    assert(OB_SUCC(wal.append_two_pages_and_sync(BP_HEADER_PAGE, header_page, 1, zero_page)));
    assert(OB_SUCC(wal.close()));
  }
  copy_file(kBefore, kFile);

  {
    DiskBufferPoolManager bpm(8, 64);
    DiskBufferPool       *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
    assert(pool->page_count() == 2);
    assert(page_is_zero(*pool, 1));

    PageNum next = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&next)));
    assert(next == 2);
    assert(OB_SUCC(bpm.close()));
  }
}

static void test_dispose_recovery()
{
  cleanup();
  {
    DiskBufferPoolManager bpm(8, 64);
    assert(OB_SUCC(bpm.create_file(kFile)));
    DiskBufferPool *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
    PageNum page1 = INVALID_PAGE_NUM;
    PageNum page2 = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&page1)));
    assert(OB_SUCC(pool->allocate_page(&page2)));
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_write(page1, handle)));
    fill_page(handle.value(), 7);
    assert(OB_SUCC(handle.mark_dirty()));
    handle.reset();
    assert(OB_SUCC(bpm.close()));
    copy_file(kFile, kBefore);
  }

  /// Simulate a crash after the disposal record was fsync'ed but before the
  /// header and free-list link reached the data file.
  {
    DiskFileHeader new_header;
    new_header.page_count      = 2;
    new_header.first_free_page = 1;
    new_header.free_page_count = 1;
    char header_page[BP_PAGE_SIZE];
    encode_header_for_test(new_header, header_page);

    char free_page[BP_PAGE_SIZE];
    memset(free_page, 0, sizeof(free_page));
    const PageNum next_free = INVALID_PAGE_NUM;
    memcpy(free_page, &next_free, sizeof(next_free));

    PageWAL wal(kFile + ".wal");
    assert(OB_SUCC(wal.open(true)));
    assert(OB_SUCC(wal.append_two_pages_and_sync(BP_HEADER_PAGE, header_page, 1, free_page)));
    assert(OB_SUCC(wal.close()));
  }
  copy_file(kBefore, kFile);

  {
    DiskBufferPoolManager bpm(8, 64);
    DiskBufferPool       *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
    assert(pool->free_page_count() == 1);
    PageHandle handle;
    assert(pool->get_page_read(1, handle) == RC::BUFFERPOOL_PAGE_FREED);

    PageNum reuse = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&reuse)));
    assert(reuse == 1);
    assert(page_is_zero(*pool, reuse));
    assert(OB_SUCC(bpm.close()));
  }
}

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  test_allocate_recovery();
  test_dispose_recovery();
  cleanup();
  printf("test_alloc_recovery: OK\n");
  return 0;
}
