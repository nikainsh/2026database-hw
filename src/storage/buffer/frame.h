/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "storage/buffer/page.h"

class DiskBufferPool;

/**
 * @brief 缓冲池中的一帧，对应内存里的一页
 * @details 与 miniob 的 Frame 一致：页号、pin 计数、脏标记、引用位（时钟算法用）。
 *          本实现是单线程的，因此不需要 mutex 保护。
 */
class Frame
{
public:
  Frame() = default;
  explicit Frame(FrameId frame_id) : frame_id_(frame_id) {}

  void     reset();
  void     init(FrameId frame_id);

  FrameId      frame_id() const { return frame_id_; }
  PageNum      page_id() const { return page_id_; }
  void         set_page_id(PageNum page_id) { page_id_ = page_id; }

  int          pin_count() const { return pin_count_; }
  void         increase_pin_count() { ++pin_count_; }
  void         decrease_pin_count() { --pin_count_; }

  bool         is_dirty() const { return dirty_; }
  void         mark_dirty() { dirty_ = true; }
  void         clear_dirty() { dirty_ = false; }

  bool         referenced() const { return referenced_; }
  void         set_referenced(bool v) { referenced_ = v; }

  DiskBufferPool *owner() const { return owner_; }
  void             set_owner(DiskBufferPool *owner) { owner_ = owner; }

  char       *data() { return page_.data; }
  const char *data() const { return page_.data; }

private:
  FrameId        frame_id_  = INVALID_FRAME_ID;
  PageNum        page_id_   = INVALID_PAGE_NUM;
  int            pin_count_ = 0;
  bool           dirty_     = false;
  bool           referenced_ = false;
  DiskBufferPool *owner_    = nullptr;
  Page           page_;
};

/// 页号 -> 帧号 的映射，对应 miniob 的 PageTable（这里直接用哈希表）
class PageTable
{
public:
  void insert(PageNum page_id, FrameId frame_id) { map_[page_id] = frame_id; }
  bool get(FrameId *frame_id, PageNum page_id) const;
  bool erase(PageNum page_id);
  bool contains(PageNum page_id) const { return map_.count(page_id) > 0; }
  void clear() { map_.clear(); }
  size_t size() const { return map_.size(); }

private:
  std::unordered_map<PageNum, FrameId> map_;
};
