/* database26 buffer manager - prefetch must not resurrect a freed page */
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kDir  = "./_test_buf";
static const std::string kFile = "./_test_buf/prefetch_race.dat";

static void cleanup()
{
  path_remove_file(kFile);
  path_remove_file(kFile + ".wal");
}

static void fill_page(char *page, int seed)
{
  memset(page, 0, BP_PAGE_SIZE);
  for (int i = 0; i < 64; ++i) {
    const int value = seed * 1000 + i;
    memcpy(page + i * sizeof(int), &value, sizeof(value));
  }
}

static void write_page(DiskBufferPool &pool, PageNum page, int seed)
{
  PageHandle handle;
  assert(OB_SUCC(pool.get_page_write(page, handle)));
  fill_page(handle.value(), seed);
  assert(OB_SUCC(handle.mark_dirty()));
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

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  cleanup();

  DiskBufferPoolManager bpm(1, 1);
  assert(OB_SUCC(bpm.create_file(kFile)));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));

  PageNum page1 = INVALID_PAGE_NUM;
  PageNum page2 = INVALID_PAGE_NUM;
  assert(OB_SUCC(pool->allocate_page(&page1)));
  assert(OB_SUCC(pool->allocate_page(&page2)));
  write_page(*pool, page1, 1);
  write_page(*pool, page2, 2);

  for (int i = 0; i < 100; ++i) {
    /// Evict page1 from the one-frame pool, then prefetch it, dispose it and
    /// immediately reuse it. The worker must not reinsert the freed page.
    {
      PageHandle handle;
      assert(OB_SUCC(pool->get_page_read(page2, handle)));
    }
    assert(OB_SUCC(pool->prefetch_page(page1)));
    assert(OB_SUCC(pool->dispose_page(page1)));

    PageNum reuse = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&reuse)));
    assert(reuse == page1);

    /// Give the prefetch worker time to run after reuse.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(page_is_zero(*pool, page1));

    write_page(*pool, page1, i + 10);
  }

  assert(OB_SUCC(bpm.close()));
  cleanup();
  printf("test_prefetch_dispose_race: OK\n");
  return 0;
}
