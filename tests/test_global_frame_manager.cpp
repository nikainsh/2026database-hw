/* database26 buffer manager - global frame manager / per-file quota test */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kDir  = "./_test_buf";
static const std::string kFileA = "./_test_buf/global_a.dat";
static const std::string kFileB = "./_test_buf/global_b.dat";

static void cleanup()
{
  for (const std::string &path : {kFileA, kFileB}) {
    path_remove_file(path);
    path_remove_file(path + ".wal");
  }
}

static void fill_page(char *page, int seed)
{
  memset(page, 0, BP_PAGE_SIZE);
  for (int i = 0; i < 64; ++i) {
    const int value = seed * 1000 + i;
    memcpy(page + i * sizeof(int), &value, sizeof(value));
  }
}

static bool check_page(DiskBufferPool &pool, PageNum page, int seed)
{
  PageHandle handle;
  if (OB_FAIL(pool.get_page_read(page, handle))) {
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

static bool write_page(DiskBufferPool &pool, PageNum page, int seed)
{
  PageHandle handle;
  if (OB_FAIL(pool.get_page_write(page, handle))) {
    return false;
  }
  fill_page(handle.value(), seed);
  if (OB_FAIL(handle.mark_dirty())) {
    return false;
  }
  return true;
}

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  cleanup();

  /// Two files, each quota 1, global total 2. A page fault in A must evict
  /// from A (not B) when A's quota is already full.
  {
    DiskBufferPoolManager bpm(1, 2);
    assert(OB_SUCC(bpm.create_file(kFileA)));
    assert(OB_SUCC(bpm.create_file(kFileB)));
    DiskBufferPool *a = nullptr;
    DiskBufferPool *b = nullptr;
    assert(OB_SUCC(bpm.open_file(kFileA, ReadWriteMode::READ_WRITE, &a)));
    assert(OB_SUCC(bpm.open_file(kFileB, ReadWriteMode::READ_WRITE, &b)));

    PageNum a1 = INVALID_PAGE_NUM, a2 = INVALID_PAGE_NUM;
    PageNum b1 = INVALID_PAGE_NUM;
    assert(OB_SUCC(a->allocate_page(&a1)));
    assert(OB_SUCC(a->allocate_page(&a2)));
    assert(OB_SUCC(b->allocate_page(&b1)));
    assert(write_page(*a, a1, 1));
    assert(write_page(*b, b1, 2));

    /// a's quota is full, so writing a2 must evict a1, leaving b1 resident.
    assert(write_page(*a, a2, 3));
    assert(b->stats().evictions == 0);
    assert(a->stats().evictions > 0);
    assert(check_page(*b, b1, 2));
    assert(check_page(*a, a1, 1));
    assert(check_page(*a, a2, 3));

    assert(bpm.used_frames() <= 2);
    assert(OB_SUCC(bpm.close()));
  }

  cleanup();

  /// Two files, each quota 2, global total 2. A2 can use a frame evicted from
  /// B, proving that victim selection is global rather than per-file.
  {
    DiskBufferPoolManager bpm(2, 2);
    assert(OB_SUCC(bpm.create_file(kFileA)));
    assert(OB_SUCC(bpm.create_file(kFileB)));
    DiskBufferPool *a = nullptr;
    DiskBufferPool *b = nullptr;
    assert(OB_SUCC(bpm.open_file(kFileA, ReadWriteMode::READ_WRITE, &a)));
    assert(OB_SUCC(bpm.open_file(kFileB, ReadWriteMode::READ_WRITE, &b)));

    PageNum a1 = INVALID_PAGE_NUM, a2 = INVALID_PAGE_NUM;
    PageNum b1 = INVALID_PAGE_NUM;
    assert(OB_SUCC(a->allocate_page(&a1)));
    assert(OB_SUCC(a->allocate_page(&a2)));
    assert(OB_SUCC(b->allocate_page(&b1)));
    assert(write_page(*a, a1, 11));
    assert(write_page(*b, b1, 22));
    /// Refresh a1 so b1 becomes the globally oldest victim.
    assert(check_page(*a, a1, 11));

    /// Global total is full, but a still has quota. The victim can come from B.
    assert(write_page(*a, a2, 33));
    assert(b->stats().evictions > 0);
    assert(check_page(*b, b1, 22));
    assert(check_page(*a, a1, 11));
    assert(check_page(*a, a2, 33));

    assert(bpm.used_frames() <= 2);
    assert(OB_SUCC(bpm.close()));
  }

  cleanup();
  printf("test_global_frame_manager: OK\n");
  return 0;
}
