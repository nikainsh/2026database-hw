/* database26 buffer manager - corruption / truncated-file tests */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kDir  = "./_test_buf";
static const std::string kFile = "./_test_buf/bp_corrupt.dat";

static void cleanup()
{
  path_remove_file(kFile);
  path_remove_file(kFile + ".wal");
}

static void create_two_pages_and_dispose_first(bool dispose_first)
{
  cleanup();
  assert(OB_SUCC(path_mkdir_recursive(kDir)));

  DiskBufferPoolManager bpm(8, 64);
  assert(OB_SUCC(bpm.create_file(kFile)));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));

  PageNum page1 = INVALID_PAGE_NUM;
  PageNum page2 = INVALID_PAGE_NUM;
  assert(OB_SUCC(pool->allocate_page(&page1)));
  assert(OB_SUCC(pool->allocate_page(&page2)));
  assert(page1 == 1 && page2 == 2);
  {
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_write(page1, handle)));
    memset(handle.value(), 0x11, BP_PAGE_SIZE);
    assert(OB_SUCC(handle.mark_dirty()));
  }
  {
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_write(page2, handle)));
    memset(handle.value(), 0x22, BP_PAGE_SIZE);
    assert(OB_SUCC(handle.mark_dirty()));
  }
  if (dispose_first) {
    assert(OB_SUCC(pool->dispose_page(page1)));
  }
  assert(OB_SUCC(bpm.close()));
}

static void test_bad_header_crc()
{
  create_two_pages_and_dispose_first(false);

  FILE *fp = fopen(kFile.c_str(), "r+b");
  assert(fp != nullptr);
  const long crc_offset = 32;
  assert(fseek(fp, crc_offset, SEEK_SET) == 0);
  int crc = 0;
  assert(fread(&crc, 1, sizeof(crc), fp) == sizeof(crc));
  crc ^= 0x01020304;
  assert(fseek(fp, crc_offset, SEEK_SET) == 0);
  assert(fwrite(&crc, 1, sizeof(crc), fp) == sizeof(crc));
  fclose(fp);

  DiskBufferPoolManager bpm(8, 64);
  DiskBufferPool       *pool = nullptr;
  assert(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool) == RC::BUFFERPOOL_CORRUPTED);
  cleanup();
}

static void test_truncated_data_page()
{
  create_two_pages_and_dispose_first(false);

  /// Keep page 0 and only part of page 1.
  std::filesystem::resize_file(kFile, BP_PAGE_SIZE + 100);

  DiskBufferPoolManager bpm(8, 64);
  DiskBufferPool       *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
  PageHandle handle;
  assert(pool->get_page_read(1, handle) == RC::BUFFERPOOL_CORRUPTED);
  assert(OB_SUCC(bpm.close()));
  cleanup();
}

static void test_free_list_cycle_or_bad_next()
{
  create_two_pages_and_dispose_first(true);

  /// first_free_page should be page 1; make its "next" point out of range.
  FILE *fp = fopen(kFile.c_str(), "r+b");
  assert(fp != nullptr);
  const int32_t bad_next = 0x7FFFFFFF;
  assert(fseek(fp, BP_PAGE_SIZE, SEEK_SET) == 0);
  assert(fwrite(&bad_next, 1, sizeof(bad_next), fp) == sizeof(bad_next));
  fclose(fp);

  DiskBufferPoolManager bpm(8, 64);
  DiskBufferPool       *pool = nullptr;
  assert(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool) == RC::BUFFERPOOL_CORRUPTED);
  cleanup();
}

int main()
{
  test_bad_header_crc();
  test_truncated_data_page();
  test_free_list_cycle_or_bad_next();
  printf("test_buffer_pool_corruption: OK\n");
  return 0;
}
