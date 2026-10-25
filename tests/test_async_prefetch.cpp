/* database26 buffer manager - asynchronous prefetch smoke test */
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kDir  = "./_test_buf";
static const std::string kFile = "./_test_buf/async_prefetch.dat";

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

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  cleanup();

  {
    DiskBufferPoolManager bpm(16, 64);
    assert(OB_SUCC(bpm.create_file(kFile)));
    DiskBufferPool *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
    for (int i = 0; i < 4; ++i) {
      PageNum page = INVALID_PAGE_NUM;
      assert(OB_SUCC(pool->allocate_page(&page)));
      PageHandle handle;
      assert(OB_SUCC(pool->get_page_write(page, handle)));
      fill_page(handle.value(), i + 1);
      assert(OB_SUCC(handle.mark_dirty()));
    }
    assert(OB_SUCC(bpm.close()));
  }

  {
    DiskBufferPoolManager bpm(16, 64);
    DiskBufferPool       *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));

    const uint64_t reads_before = pool->stats().reads;
    assert(OB_SUCC(pool->prefetch_range(1, 4)));

    bool ready = false;
    for (int i = 0; i < 200; ++i) {
      if (pool->stats().reads >= reads_before + 4) {
        ready = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(ready);

    const BufferPoolStatsSnapshot before = pool->stats();
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_read(1, handle)));
    const BufferPoolStatsSnapshot after = pool->stats();
    assert(after.hits > before.hits);
    assert(after.misses == before.misses);
    handle.reset();

    assert(OB_SUCC(bpm.close()));
  }

  cleanup();
  printf("test_async_prefetch: OK\n");
  return 0;
}
