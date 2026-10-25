/* database26 buffer manager - background pins must not cause spurious NOBUF/PINNED */
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kDir  = "./_test_buf";
static const std::string kFile = "./_test_buf/bg_pressure.dat";

static void cleanup()
{
  path_remove_file(kFile);
  path_remove_file(kFile + ".wal");
}

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  cleanup();

  DiskBufferPoolManager bpm(2, 2);
  assert(OB_SUCC(bpm.create_file(kFile)));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));

  constexpr int kPages = 8;
  for (int i = 0; i < kPages; ++i) {
    PageNum page = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&page)));
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_write(page, handle)));
    memset(handle.value(), i + 1, BP_PAGE_SIZE);
    assert(OB_SUCC(handle.mark_dirty()));
  }

  std::atomic<int>  failures{0};
  std::atomic<bool> stop{false};

  std::thread user([&]() {
    int round = 0;
    while (!stop.load()) {
      const PageNum page = (round++ % kPages) + 1;
      PageHandle handle;
      RC rc = pool->get_page_write(page, handle);
      if (rc == RC::BUFFERPOOL_NOBUF) {
        failures.fetch_add(1);
      } else if (OB_SUCC(rc)) {
        handle.value()[0] = static_cast<char>(round);
        handle.mark_dirty();
      }
      handle.reset();

      /// Also exercise dispose while background flush may hold a temporary pin.
      if ((round % 7) == 0) {
        PageNum reuse = INVALID_PAGE_NUM;
        RC rc2 = pool->dispose_page(page);
        if (rc2 == RC::BUFFERPOOL_PAGE_PINNED) {
          failures.fetch_add(1);
        } else if (OB_SUCC(rc2)) {
          pool->allocate_page(&reuse);
        }
      }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  stop.store(true);
  user.join();
  assert(failures.load() == 0);

  assert(OB_SUCC(bpm.close()));
  cleanup();
  printf("test_background_flush_pressure: OK\n");
  return 0;
}
