/* database26 buffer manager - edge cases: read-only, double free, paths, handles */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kDir  = "./_test_buf";
static const std::string kFile = "./_test_buf/bp_extra.dat";

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

  {
    DiskBufferPool pool;
    assert(pool.open(kFile, ReadWriteMode::READ_WRITE, 0) == RC::BUFFERPOOL_INVALID_FRAME_LIMIT);
    assert(pool.open(kFile, ReadWriteMode::READ_WRITE, 1 << 21) == RC::BUFFERPOOL_INVALID_FRAME_LIMIT);
  }

  {
    DiskBufferPoolManager bpm(8, 64);
    assert(OB_SUCC(bpm.create_file(kFile)));
    DiskBufferPool *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));

    PageNum page = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&page)));
    assert(page == 1);
    {
      PageHandle handle;
      assert(OB_SUCC(pool->get_page_write(page, handle)));
      fill_page(handle.value(), 7);
      assert(OB_SUCC(handle.mark_dirty()));
    }

    /// Path aliasing must resolve to the same pool.
    DiskBufferPool *alias = nullptr;
    assert(OB_SUCC(bpm.open_file("./_test_buf/../_test_buf/bp_extra.dat", ReadWriteMode::READ_WRITE, &alias)));
    assert(alias == pool);

    assert(OB_SUCC(bpm.close()));
  }

  {
    DiskBufferPoolManager bpm(8, 64);
    DiskBufferPool       *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_ONLY, &pool)));
    assert(pool->is_read_only());
    assert(check_page(*pool, 1, 7));
    assert(pool->dispose_page(1) == RC::IOERR_ACCESS);
    PageNum page = INVALID_PAGE_NUM;
    assert(pool->allocate_page(&page) == RC::IOERR_ACCESS);
    assert(OB_SUCC(bpm.close()));
  }

  {
    DiskBufferPoolManager bpm(8, 64);
    DiskBufferPool       *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
    assert(OB_SUCC(pool->dispose_page(1)));
    assert(pool->dispose_page(1) == RC::BUFFERPOOL_DOUBLE_FREE);
    PageHandle handle;
    assert(pool->get_page(1, handle) == RC::BUFFERPOOL_PAGE_FREED);
    PageNum reused = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&reused)));
    assert(reused == 1);
    assert(OB_SUCC(bpm.close()));
  }

  {
    DiskBufferPoolManager bpm(8, 64);
    assert(OB_SUCC(bpm.create_file("./_test_buf/bp_handle.dat")));
    DiskBufferPool *pool = nullptr;
    assert(OB_SUCC(bpm.open_file("./_test_buf/bp_handle.dat", ReadWriteMode::READ_WRITE, &pool)));
    PageNum page = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&page)));
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_read(page, handle)));
    assert(bpm.close() == RC::BUFFERPOOL_HANDLE_BUSY);
    handle.reset();
    assert(OB_SUCC(bpm.close()));
    path_remove_file("./_test_buf/bp_handle.dat");
    path_remove_file("./_test_buf/bp_handle.dat.wal");
  }

  path_remove_file(kFile);
  path_remove_file(kFile + ".wal");
  printf("test_buffer_pool_extra: OK\n");
  return 0;
}
