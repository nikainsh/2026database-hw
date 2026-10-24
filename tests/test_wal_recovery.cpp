/* database26 buffer manager - WAL replay integration test */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/buffer/page_wal.h"

static const std::string kDir  = "./_test_wal";
static const std::string kFile = "./_test_wal/recovery.dat";

static void fill_page(char *page, int seed)
{
  memset(page, 0, BP_PAGE_SIZE);
  for (int i = 0; i < 64; ++i) {
    const int value = seed * 1000 + i;
    memcpy(page + i * sizeof(int), &value, sizeof(value));
  }
}

static bool check_page(DiskBufferPool &pool, PageNum page_num, int seed)
{
  PageHandle handle;
  if (OB_FAIL(pool.get_page_read(page_num, handle))) {
    return false;
  }
  const int *words = reinterpret_cast<const int *>(handle.const_data());
  for (int i = 0; i < 64; ++i) {
    if (words[i] != seed * 1000 + i) {
      return false;
    }
  }
  return true;
}

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  if (file_exist(kFile)) {
    assert(OB_SUCC(path_remove_file(kFile)));
  }
  if (file_exist(kFile + ".wal")) {
    assert(OB_SUCC(path_remove_file(kFile + ".wal")));
  }

  const int seed_a = 101;
  const int seed_b = 202;

  {
    DiskBufferPoolManager bpm(8, 64);
    assert(OB_SUCC(bpm.create_file(kFile)));
    DiskBufferPool *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
    PageNum page = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&page)));
    assert(page == 1);
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_write(page, handle)));
    fill_page(handle.value(), seed_a);
    assert(OB_SUCC(handle.mark_dirty()));
    handle.reset();
    assert(OB_SUCC(bpm.close()));
  }

  /// Simulate a crash after the WAL record was written but before the data
  /// page reached the file: append a new page image to the WAL ourselves.
  {
    char page[BP_PAGE_SIZE];
    fill_page(page, seed_b);
    PageWAL wal(kFile + ".wal");
    assert(OB_SUCC(wal.open(true)));
    assert(OB_SUCC(wal.append_and_sync(1, page)));
    assert(OB_SUCC(wal.close()));
  }

  {
    DiskBufferPoolManager bpm(8, 64);
    DiskBufferPool       *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
    assert(check_page(*pool, 1, seed_b));
    assert(OB_SUCC(bpm.close()));
  }

  path_remove_file(kFile);
  path_remove_file(kFile + ".wal");
  printf("test_wal_recovery: OK\n");
  return 0;
}
