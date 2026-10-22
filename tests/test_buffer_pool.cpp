/* minidb - 缓冲管理器自测：页分配/释放、脏页回写、重开校验、时钟淘汰 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/file_util.h"
#include "common/log.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kFile = "./_test_buf/bp.dat";
static const std::string kBig  = "./_test_buf/bp_big.dat";

static void write_pattern(DiskBufferPool &pool, PageNum page_num, int seed)
{
  PageHandle handle;
  assert(OB_SUCC(pool.get_page(page_num, handle)));
  int32_t *words = reinterpret_cast<int32_t *>(handle.value());
  for (int i = 0; i < BP_PAGE_SIZE / (int)sizeof(int32_t); ++i) {
    words[i] = seed * 1000 + i;
  }
  assert(OB_SUCC(handle.mark_dirty()));
  handle.reset();
}

static void check_pattern(DiskBufferPool &pool, PageNum page_num, int seed)
{
  PageHandle handle;
  assert(OB_SUCC(pool.get_page(page_num, handle)));
  const int32_t *words = reinterpret_cast<const int32_t *>(handle.const_data());
  for (int i = 0; i < BP_PAGE_SIZE / (int)sizeof(int32_t); ++i) {
    assert(words[i] == seed * 1000 + i);
  }
}

static void test_allocate_flush_reload()
{
  path_mkdir_recursive("./_test_buf");
  for (const std::string &f : {kFile, kBig}) {
    if (file_exist(f)) {
      assert(OB_SUCC(path_remove_file(f)));
    }
  }

  DiskBufferPoolManager bpm(8);
  assert(OB_SUCC(bpm.create_file(kFile)));

  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
  assert(bpm.get_pool(kFile) == pool);  /// 同一路径只打开一份

  PageNum pages[20];
  for (int i = 0; i < 20; ++i) {
    assert(OB_SUCC(pool->allocate_page(&pages[i])));
    assert(pages[i] == i + 1);  /// 第 0 页是文件头页
    write_pattern(*pool, pages[i], i);
  }
  assert(pool->page_count() == 21);

  /// 第 0 页由缓冲池内部维护，不允许上层直接访问
  PageHandle header_handle;
  assert(pool->get_page(BP_HEADER_PAGE, header_handle) == RC::BUFFERPOOL_INVALID_PAGE_NUM);
  assert(pool->get_page(21, header_handle) == RC::BUFFERPOOL_INVALID_PAGE_NUM);

  assert(OB_SUCC(bpm.close()));  /// flush 全部脏页并关闭

  DiskBufferPoolManager bpm2(8);
  DiskBufferPool       *pool2 = nullptr;
  assert(OB_SUCC(bpm2.open_file(kFile, ReadWriteMode::READ_WRITE, &pool2)));
  assert(pool2->page_count() == 21);
  for (int i = 0; i < 20; ++i) {
    check_pattern(*pool2, pages[i], i);
  }

  /// 释放中间的页，进入空闲链表；再次分配应复用而不是让文件变长
  for (int i = 5; i < 10; ++i) {
    assert(OB_SUCC(pool2->dispose_page(pages[i])));
  }
  assert(pool2->free_page_count() == 5);
  const PageNum page_count_before = pool2->page_count();

  PageNum reuse = INVALID_PAGE_NUM;
  assert(OB_SUCC(pool2->allocate_page(&reuse)));
  assert(pool2->free_page_count() == 4);
  assert(pool2->page_count() == page_count_before);
  assert(reuse == pages[9]);  /// 链表头是最后释放的那一页

  {
    PageHandle handle;
    assert(OB_SUCC(pool2->get_page(reuse, handle)));
    const int32_t *words = reinterpret_cast<const int32_t *>(handle.const_data());
    assert(words[0] == 0 && words[1] == 0);  /// 复用时已经清零
  }

  /// 被 pin 的页不能被释放
  PageHandle pinned;
  assert(OB_SUCC(pool2->get_page(pages[0], pinned)));
  assert(pool2->dispose_page(pages[0]) == RC::BUFFERPOOL_PAGE_PINNED);
  pinned.reset();
  assert(OB_SUCC(pool2->dispose_page(pages[0])));

  assert(OB_SUCC(bpm2.close()));
}

static void test_eviction_under_small_pool()
{
  DiskBufferPoolManager bpm(4);  /// 只有 4 帧，制造淘汰
  DiskBufferPool       *pool     = nullptr;
  assert(OB_SUCC(bpm.create_file(kBig)));
  assert(OB_SUCC(bpm.open_file(kBig, ReadWriteMode::READ_WRITE, &pool)));

  for (int i = 0; i < 12; ++i) {
    PageNum page_num = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&page_num)));
    write_pattern(*pool, page_num, i);
  }
  assert(OB_SUCC(bpm.close()));

  /// 帧数远小于页数：读一遍会不断换入换出，但内容必须完全正确
  DiskBufferPoolManager bpm2(2);
  DiskBufferPool       *pool2 = nullptr;
  assert(OB_SUCC(bpm2.open_file(kBig, ReadWriteMode::READ_WRITE, &pool2)));
  for (int i = 0; i < 12; ++i) {
    check_pattern(*pool2, i + 1, i);
  }
  assert(OB_SUCC(bpm2.close()));
}

static void test_all_frames_pinned()
{
  DiskBufferPoolManager bpm(2);
  DiskBufferPool       *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));

  PageHandle h1, h2, h3;
  assert(OB_SUCC(pool->get_page(2, h1)));
  assert(OB_SUCC(pool->get_page(3, h2)));
  /// 两帧都被 pin，再取别的页会直接失败，而不是死循环
  assert(pool->get_page(4, h3) == RC::BUFFERPOOL_NOBUF);
  /// 已 pin 过的页可以从页表命中
  assert(OB_SUCC(pool->get_page(2, h3)));
  assert(h3.frame_id() == h1.frame_id());
}

int main()
{
  test_allocate_flush_reload();
  test_eviction_under_small_pool();
  test_all_frames_pinned();
  printf("test_buffer_pool: OK\n");
  return 0;
}
