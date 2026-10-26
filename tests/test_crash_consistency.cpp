/* database26 buffer manager - crash recovery matrix */
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kBase = "./_test_crash";
static constexpr int      kPages = 8;

static std::string data_file(const std::string &dir) { return dir + "/crash.dat"; }

static void remove_dir(const std::string &dir)
{
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
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

static void prepare_dispose_case(const std::string &dir)
{
  assert(OB_SUCC(path_mkdir_recursive(dir)));
  DiskBufferPoolManager bpm(kPages, 64);
  assert(OB_SUCC(bpm.create_file(data_file(dir))));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(data_file(dir), ReadWriteMode::READ_WRITE, &pool)));
  PageNum page = INVALID_PAGE_NUM;
  assert(OB_SUCC(pool->allocate_page(&page)));
  assert(page == 1);
  assert(OB_SUCC(bpm.close()));
}

static int child_main(const std::string &case_name, const std::string &dir)
{
  assert(OB_SUCC(path_mkdir_recursive(dir)));
  const std::string file = data_file(dir);

  if (case_name == "dispose") {
    DiskBufferPoolManager bpm(kPages, 64);
    DiskBufferPool       *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(file, ReadWriteMode::READ_WRITE, &pool)));
    assert(OB_SUCC(pool->dispose_page(1)));
    assert(OB_SUCC(bpm.close()));
    return 0;
  }

  DiskBufferPoolManager bpm(kPages, 64);
  assert(OB_SUCC(bpm.create_file(file)));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(file, ReadWriteMode::READ_WRITE, &pool)));

  PageNum page = INVALID_PAGE_NUM;
  assert(OB_SUCC(pool->allocate_page(&page)));
  assert(page == 1);

  if (case_name == "write" || case_name == "checkpoint") {
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_write(page, handle)));
    fill_page(handle.value(), 7);
    assert(OB_SUCC(handle.mark_dirty()));
    handle.reset();
    assert(OB_SUCC(pool->flush_all_buffers()));
  } else {
    /// "allocate": allocate_page() already triggered the two-page WAL write.
  }

  assert(OB_SUCC(bpm.close()));
  return 0;
}

static std::string quote(const std::string &value) { return "\"" + value + "\""; }

static int run_child(const std::string &self, const std::string &case_name, const std::string &point,
                     const std::string &dir)
{
  std::string command;
#ifdef _WIN32
  command = "set MINIDB_CRASH_POINT=" + point + "&& " + quote(self) + " --child " + case_name + " " +
            quote(dir);
#else
  command = "MINIDB_CRASH_POINT=" + point + " " + quote(self) + " --child " + case_name + " " + quote(dir);
#endif
  return std::system(command.c_str());
}

static void verify_write(const std::string &dir)
{
  DiskBufferPoolManager bpm(kPages, 64);
  DiskBufferPool       *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(data_file(dir), ReadWriteMode::READ_WRITE, &pool)));
  assert(check_page(*pool, 1, 7));
  assert(OB_SUCC(bpm.close()));
  assert(std::filesystem::file_size(data_file(dir) + ".wal") == 16);
  assert(!std::filesystem::exists(data_file(dir) + ".wal.tmp"));
}

static void verify_allocate(const std::string &dir)
{
  DiskBufferPoolManager bpm(kPages, 64);
  DiskBufferPool       *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(data_file(dir), ReadWriteMode::READ_WRITE, &pool)));
  assert(pool->page_count() == 2);
  assert(page_is_zero(*pool, 1));

  PageNum next = INVALID_PAGE_NUM;
  assert(OB_SUCC(pool->allocate_page(&next)));
  assert(next == 2);
  assert(OB_SUCC(bpm.close()));
  assert(!std::filesystem::exists(data_file(dir) + ".wal.tmp"));
}

static void verify_dispose(const std::string &dir)
{
  DiskBufferPoolManager bpm(kPages, 64);
  DiskBufferPool       *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(data_file(dir), ReadWriteMode::READ_WRITE, &pool)));
  assert(pool->free_page_count() == 1);
  PageHandle handle;
  assert(pool->get_page_read(1, handle) == RC::BUFFERPOOL_PAGE_FREED);

  PageNum reuse = INVALID_PAGE_NUM;
  assert(OB_SUCC(pool->allocate_page(&reuse)));
  assert(reuse == 1);
  assert(page_is_zero(*pool, reuse));
  assert(OB_SUCC(bpm.close()));
  assert(!std::filesystem::exists(data_file(dir) + ".wal.tmp"));
}

static void run_case(const std::string &self, const std::string &case_name, const std::string &point)
{
  const std::string dir = kBase + "/" + case_name + "_" + point;
  remove_dir(dir);
  if (case_name == "dispose") {
    prepare_dispose_case(dir);
  }

  const int status = run_child(self, case_name, point, dir);
  assert(status != 0);

  if (case_name == "write" || case_name == "checkpoint") {
    verify_write(dir);
  } else if (case_name == "allocate") {
    verify_allocate(dir);
  } else {
    verify_dispose(dir);
  }
  remove_dir(dir);
}

int main(int argc, char **argv)
{
  if (argc == 4 && std::strcmp(argv[1], "--child") == 0) {
    return child_main(argv[2], argv[3]);
  }

  remove_dir(kBase);
  assert(OB_SUCC(path_mkdir_recursive(kBase)));

  const std::string self = std::filesystem::absolute(argv[0]).string();

  run_case(self, "write", "before_single_page_write");
  run_case(self, "write", "after_single_page_write");
  run_case(self, "write", "before_wal_checkpoint");
  run_case(self, "write", "before_checkpoint_replace");
  run_case(self, "write", "after_checkpoint_replace");

  run_case(self, "allocate", "before_two_page_write");
  run_case(self, "allocate", "after_first_page_write");
  run_case(self, "allocate", "after_two_page_write");

  run_case(self, "dispose", "after_first_page_write");

  remove_dir(kBase);
  printf("test_crash_consistency: 9 crash points recovered\n");
  return 0;
}
