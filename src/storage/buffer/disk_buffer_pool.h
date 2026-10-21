/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/rc.h"
#include "common/types.h"
#include "storage/buffer/frame.h"
#include "storage/buffer/page.h"

class DiskBufferPool;

/// 文件头魔数：'M''B''P''1'
static constexpr int32_t BP_FILE_MAGIC = 0x4D425031;

/**
 * @brief 每个磁盘文件第 0 页的固定内容
 * @details 空闲页用单链表串起来：被释放页的前 4 个字节保存下一个空闲页的页号。
 */
struct DiskFileHeader
{
  int32_t magic           = BP_FILE_MAGIC;
  int32_t page_size       = BP_PAGE_SIZE;
  int32_t page_count      = 1;  /// 已分配的页数，包含第 0 页
  int32_t first_free_page = INVALID_PAGE_NUM;
  int32_t free_page_count = 0;
};

/**
 * @brief 一次页访问的句柄，析构时自动 unpin
 * @details miniob 需要手工调用 unpin_page，这里改成 RAII 以避免漏放 pin 导致帧无法被淘汰。
 */
class PageHandle
{
public:
  PageHandle() = default;
  PageHandle(const PageHandle &other)            = delete;
  PageHandle &operator=(const PageHandle &other) = delete;

  PageHandle(PageHandle &&other) noexcept { swap(other); }
  PageHandle &operator=(PageHandle &&other) noexcept
  {
    if (this != &other) {
      reset();
      swap(other);
    }
    return *this;
  }

  ~PageHandle() { reset(); }

  void swap(PageHandle &other) noexcept
  {
    std::swap(pool_, other.pool_);
    std::swap(frame_id_, other.frame_id_);
    std::swap(page_id_, other.page_id_);
  }

  void     reset();

  char       *value() const;
  const char *const_data() const { return value(); }
  PageNum     page_id() const { return page_id_; }
  FrameId     frame_id() const { return frame_id_; }
  bool        valid() const { return pool_ != nullptr; }

  /// 修改页内容后必须调用，否则页不会写回磁盘
  RC mark_dirty();

private:
  friend class DiskBufferPool;
  PageHandle(DiskBufferPool *pool, FrameId frame_id, PageNum page_id)
      : pool_(pool), frame_id_(frame_id), page_id_(page_id)
  {}

  DiskBufferPool *pool_     = nullptr;
  FrameId         frame_id_ = INVALID_FRAME_ID;
  PageNum         page_id_  = INVALID_PAGE_NUM;
};

/**
 * @brief 缓冲管理器：一个磁盘文件对应一个实例
 * @details 负责 读页/写页、帧分配（时钟替换算法）、pin 计数、脏页回写、页的分配与释放。
 */
class DiskBufferPool
{
public:
  DiskBufferPool() = default;
  ~DiskBufferPool();

  RC create_file(const std::string &path);
  RC open(const std::string &path, ReadWriteMode mode, int frame_limit);
  RC close();

  const std::string &file_name() const { return file_name_; }
  PageNum              page_count() const { return header_.page_count; }
  int32_t              free_page_count() const { return header_.free_page_count; }

  /// 分配一个新页（优先复用空闲页）。返回的页已经清零，调用方随后 get_page 使用。
  RC allocate_page(PageNum *page_num);
  /// 释放一个页，进入空闲链表；该页必须未被 pin
  RC dispose_page(PageNum page_num);

  /// 取出一页并 pin，句柄析构时自动 unpin
  RC get_page(PageNum page_num, PageHandle &handle);
  RC unpin_page(FrameId frame_id);
  /// 页内容被修改后调用，页才会在淘汰或 flush 时写回磁盘
  RC mark_page_dirty(FrameId frame_id);

  /// 把所有脏页写回磁盘，对应 miniob 的 flush_all_buffers
  RC flush_all_buffers();

  bool is_read_only() const { return mode_ == ReadWriteMode::READ_ONLY; }

  /// PageHandle::value() 的实现，返回帧内页数据缓冲区
  char *frame_data(FrameId frame_id);

private:
  RC read_header();
  RC write_header();
  RC read_page(PageNum page_num, char *data);
  RC write_page(PageNum page_num, const char *data);
  RC seek(int64_t offset);

  /// 时钟替换算法挑一个可用的帧；frame_id 返回被选中的帧（已保证未 pin、已写回）
  RC allocate_frame(FrameId *frame_id);
  RC evict_frame(FrameId frame_id);

  void detach_page(PageNum page_num);

private:
  std::string    file_name_;
  FILE          *handle_       = nullptr;
  ReadWriteMode  mode_         = ReadWriteMode::READ_ONLY;
  DiskFileHeader header_;
  bool           header_dirty_ = false;

  std::vector<Frame> frames_;
  PageTable          page_table_;
  FrameId            clock_hand_ = 0;
};

/**
 * @brief 缓冲池管理器：按路径持有所有打开的文件，对应 miniob 的 DiskBufferPoolManager
 */
class DiskBufferPoolManager
{
public:
  explicit DiskBufferPoolManager(int frame_limit_per_file = 32) : frame_limit_(frame_limit_per_file) {}

  RC create_file(const std::string &path);
  /// 同一个路径只会打开一份，重复调用返回已打开的实例
  RC open_file(const std::string &path, ReadWriteMode mode, DiskBufferPool **pool);
  RC close_file(const std::string &path);
  RC close();
  /// 把当前打开的所有文件里的脏页写回磁盘
  RC flush_all();

  DiskBufferPool *get_pool(const std::string &path) const;
  bool            is_open(const std::string &path) const { return get_pool(path) != nullptr; }

private:
  int frame_limit_ = 32;
  std::vector<std::unique_ptr<DiskBufferPool>> pools_;
  std::unordered_map<std::string, DiskBufferPool *> by_path_;
};
