/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/buffer/disk_buffer_pool.h"

#include <cstring>

#include "common/log.h"

static int file_seek(FILE *fp, int64_t offset)
{
#ifdef _WIN32
  return _fseeki64(fp, offset, SEEK_SET);
#else
  return fseeko(fp, static_cast<off_t>(offset), SEEK_SET);
#endif
}

// ---------------------------------------------------------------------------
// PageHandle
// ---------------------------------------------------------------------------

void PageHandle::reset()
{
  if (pool_ != nullptr) {
    pool_->unpin_page(frame_id_);
    pool_     = nullptr;
    frame_id_ = INVALID_FRAME_ID;
    page_id_  = INVALID_PAGE_NUM;
  }
}

char *PageHandle::value() const
{
  ASSERT(pool_ != nullptr, "the page handle is not valid");
  return pool_->frame_data(frame_id_);
}

RC PageHandle::mark_dirty()
{
  ASSERT(pool_ != nullptr, "the page handle is not valid");
  return pool_->mark_page_dirty(frame_id_);
}

// ---------------------------------------------------------------------------
// DiskBufferPool
// ---------------------------------------------------------------------------

DiskBufferPool::~DiskBufferPool() { close(); }

RC DiskBufferPool::create_file(const std::string &path)
{
  FILE *fp = fopen(path.c_str(), "wb");
  if (fp == nullptr) {
    LOG_ERROR("failed to create file. path=%s", path.c_str());
    return RC::FILE_CREATE;
  }

  DiskFileHeader header;
  header.page_count = 1;
  header.first_free_page = INVALID_PAGE_NUM;

  char page[BP_PAGE_SIZE];
  memset(page, 0, sizeof(page));
  memcpy(page, &header, sizeof(header));

  if (fwrite(page, 1, sizeof(page), fp) != sizeof(page)) {
    fclose(fp);
    return RC::IOERR_WRITE;
  }
  fclose(fp);
  return RC::SUCCESS;
}

RC DiskBufferPool::open(const std::string &path, ReadWriteMode mode, int frame_limit)
{
  ASSERT(handle_ == nullptr, "buffer pool has been opened. file=%s", path.c_str());

  const char *fmode = mode == ReadWriteMode::READ_WRITE ? "r+b" : "rb";
  handle_           = fopen(path.c_str(), fmode);
  if (handle_ == nullptr) {
    LOG_ERROR("failed to open file. path=%s, mode=%s", path.c_str(), fmode);
    return RC::FILE_OPEN;
  }

  file_name_ = path;
  mode_      = mode;

  RC rc = read_header();
  if (OB_FAIL(rc)) {
    fclose(handle_);
    handle_ = nullptr;
    return rc;
  }

  frames_.clear();
  frames_.reserve(frame_limit);
  for (FrameId i = 0; i < frame_limit; ++i) {
    frames_.emplace_back(i);
    frames_.back().set_owner(this);
  }
  page_table_.clear();
  clock_hand_ = 0;

  return RC::SUCCESS;
}

RC DiskBufferPool::read_header()
{
  char page[BP_PAGE_SIZE];
  RC   rc = read_page(BP_HEADER_PAGE, page);
  if (OB_FAIL(rc)) {
    return rc;
  }

  DiskFileHeader header;
  memcpy(&header, page, sizeof(header));

  if (header.magic != BP_FILE_MAGIC) {
    LOG_ERROR("invalid file magic. file=%s", file_name_.c_str());
    return RC::IOERR_READ;
  }
  if (header.page_size != BP_PAGE_SIZE) {
    LOG_ERROR("page size mismatch. file=%s, page_size=%d, expect=%d",
              file_name_.c_str(), header.page_size, BP_PAGE_SIZE);
    return RC::IOERR_READ;
  }

  header_       = header;
  header_dirty_ = false;
  return RC::SUCCESS;
}

RC DiskBufferPool::write_header()
{
  if (mode_ == ReadWriteMode::READ_ONLY) {
    return RC::SUCCESS;
  }

  char page[BP_PAGE_SIZE];
  memset(page, 0, sizeof(page));
  memcpy(page, &header_, sizeof(header_));

  RC rc = write_page(BP_HEADER_PAGE, page);
  if (OB_FAIL(rc)) {
    return rc;
  }
  header_dirty_ = false;
  return RC::SUCCESS;
}

RC DiskBufferPool::seek(int64_t offset)
{
  if (handle_ == nullptr) {
    return RC::FILE_NOT_OPENED;
  }
  if (file_seek(handle_, offset) != 0) {
    LOG_ERROR("failed to seek. file=%s, offset=%s", file_name_.c_str(), std::to_string(offset).c_str());
    return RC::IOERR_SEEK;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::read_page(PageNum page_num, char *data)
{
  RC rc = seek(static_cast<int64_t>(page_num) * BP_PAGE_SIZE);
  if (OB_FAIL(rc)) {
    return rc;
  }

  const size_t got = fread(data, 1, BP_PAGE_SIZE, handle_);
  if (got != BP_PAGE_SIZE) {
    /// 文件被截断或未写满时，用空页兜底，保证上层拿到确定的页
    memset(data + got, 0, BP_PAGE_SIZE - got);
    if (got == 0) {
      LOG_ERROR("failed to read page. file=%s, page_num=%d", file_name_.c_str(), page_num);
      return RC::IOERR_READ;
    }
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::write_page(PageNum page_num, const char *data)
{
  RC rc = seek(static_cast<int64_t>(page_num) * BP_PAGE_SIZE);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (fwrite(data, 1, BP_PAGE_SIZE, handle_) != BP_PAGE_SIZE) {
    LOG_ERROR("failed to write page. file=%s, page_num=%d", file_name_.c_str(), page_num);
    return RC::IOERR_WRITE;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::allocate_page(PageNum *page_num)
{
  if (mode_ == ReadWriteMode::READ_ONLY) {
    return RC::IOERR_ACCESS;
  }

  if (header_.first_free_page != INVALID_PAGE_NUM) {
    /// 复用空闲链表上的页：页首 4 字节保存着下一个空闲页的页号
    const PageNum reuse = header_.first_free_page;

    char raw[BP_PAGE_SIZE];
    RC   rc = read_page(reuse, raw);
    if (OB_FAIL(rc)) {
      return rc;
    }

    PageNum next_free = INVALID_PAGE_NUM;
    memcpy(&next_free, raw, sizeof(next_free));

    memset(raw, 0, sizeof(raw));
    rc = write_page(reuse, raw);
    if (OB_FAIL(rc)) {
      return rc;
    }

    header_.first_free_page = next_free;
    header_.free_page_count -= 1;
    header_dirty_           = true;

    *page_num = reuse;
    return RC::SUCCESS;
  }

  char page[BP_PAGE_SIZE];
  memset(page, 0, sizeof(page));
  RC rc = write_page(header_.page_count, page);
  if (OB_FAIL(rc)) {
    return rc;
  }

  *page_num             = header_.page_count;
  header_.page_count   += 1;
  header_dirty_         = true;
  return RC::SUCCESS;
}

RC DiskBufferPool::dispose_page(PageNum page_num)
{
  if (page_num <= BP_HEADER_PAGE || page_num >= header_.page_count) {
    LOG_ERROR("invalid page to dispose. file=%s, page_num=%d", file_name_.c_str(), page_num);
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }

  FrameId frame_id = INVALID_FRAME_ID;
  if (page_table_.get(&frame_id, page_num)) {
    if (frames_[frame_id].pin_count() > 0) {
      LOG_ERROR("the page is pinned. file=%s, page_num=%d", file_name_.c_str(), page_num);
      return RC::BUFFERPOOL_PAGE_PINNED;
    }
    detach_page(page_num);
  }

  /// 页首 4 字节保存下一个空闲页，形成单链表
  char    raw[BP_PAGE_SIZE];
  memset(raw, 0, sizeof(raw));
  memcpy(raw, &header_.first_free_page, sizeof(header_.first_free_page));
  RC rc = write_page(page_num, raw);
  if (OB_FAIL(rc)) {
    return rc;
  }

  header_.first_free_page = page_num;
  header_.free_page_count += 1;
  header_dirty_           = true;
  return RC::SUCCESS;
}

void DiskBufferPool::detach_page(PageNum page_num)
{
  FrameId frame_id = INVALID_FRAME_ID;
  if (!page_table_.get(&frame_id, page_num)) {
    return;
  }
  page_table_.erase(page_num);
  frames_[frame_id].reset();
  frames_[frame_id].set_owner(this);
}

RC DiskBufferPool::allocate_frame(FrameId *frame_id)
{
  const size_t total = frames_.size();
  if (total == 0) {
    return RC::BUFFERPOOL_NOBUF;
  }

  /// 两圈：第一圈把引用位清零（第二次机会），第二圈一定能挑到候选帧
  for (size_t round = 0; round < 2 * total; ++round) {
    FrameId      fid = clock_hand_;
    clock_hand_      = static_cast<FrameId>((clock_hand_ + 1) % total);

    Frame &frame = frames_[fid];
    if (frame.page_id() == INVALID_PAGE_NUM) {
      *frame_id = fid;
      return RC::SUCCESS;
    }
    if (frame.pin_count() > 0) {
      continue;
    }
    /// 第二次机会：给过引用位，跳过
    if (frame.referenced()) {
      frame.set_referenced(false);
      continue;
    }

    RC rc = evict_frame(fid);
    if (OB_FAIL(rc)) {
      return rc;
    }
    *frame_id = fid;
    return RC::SUCCESS;
  }

  LOG_WARN("all frames are pinned. file=%s, frames=%d", file_name_.c_str(), (int)total);
  return RC::BUFFERPOOL_NOBUF;
}

RC DiskBufferPool::evict_frame(FrameId frame_id)
{
  Frame &frame = frames_[frame_id];
  if (frame.is_dirty()) {
    RC rc = write_page(frame.page_id(), frame.data());
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  page_table_.erase(frame.page_id());
  frame.reset();
  frame.set_owner(this);
  return RC::SUCCESS;
}

RC DiskBufferPool::get_page(PageNum page_num, PageHandle &handle)
{
  if (page_num <= BP_HEADER_PAGE || page_num >= header_.page_count) {
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }

  FrameId frame_id = INVALID_FRAME_ID;
  if (page_table_.get(&frame_id, page_num)) {
    Frame &frame = frames_[frame_id];
    ASSERT(frame.page_id() == page_num, "page table is inconsistent. file=%s", file_name_.c_str());
    frame.increase_pin_count();
    handle = PageHandle(this, frame_id, page_num);
    return RC::SUCCESS;
  }

  RC rc = allocate_frame(&frame_id);
  if (OB_FAIL(rc)) {
    return rc;
  }

  Frame &frame = frames_[frame_id];
  rc           = read_page(page_num, frame.data());
  if (OB_FAIL(rc)) {
    frame.reset();
    frame.set_owner(this);
    return rc;
  }

  frame.set_page_id(page_num);
  frame.clear_dirty();
  frame.set_referenced(true);
  frame.increase_pin_count();
  page_table_.insert(page_num, frame_id);

  handle = PageHandle(this, frame_id, page_num);
  return RC::SUCCESS;
}

RC DiskBufferPool::unpin_page(FrameId frame_id)
{
  if (frame_id < 0 || frame_id >= static_cast<FrameId>(frames_.size())) {
    return RC::INVALID_ARGUMENT;
  }

  Frame &frame = frames_[frame_id];
  ASSERT(frame.pin_count() > 0, "unpin a page which is not pinned. file=%s", file_name_.c_str());
  frame.decrease_pin_count();
  if (frame.pin_count() == 0) {
    frame.set_referenced(true);  /// 重新加入时钟算法的候选集
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::mark_page_dirty(FrameId frame_id)
{
  if (frame_id < 0 || frame_id >= static_cast<FrameId>(frames_.size())) {
    return RC::INVALID_ARGUMENT;
  }
  if (mode_ == ReadWriteMode::READ_ONLY) {
    return RC::IOERR_ACCESS;
  }
  frames_[frame_id].mark_dirty();
  return RC::SUCCESS;
}

char *DiskBufferPool::frame_data(FrameId frame_id)
{
  ASSERT(frame_id >= 0 && frame_id < static_cast<FrameId>(frames_.size()),
         "invalid frame id. file=%s, frame_id=%d", file_name_.c_str(), frame_id);
  return frames_[frame_id].data();
}

RC DiskBufferPool::flush_all_buffers()
{
  for (Frame &frame : frames_) {
    if (frame.is_dirty()) {
      RC rc = write_page(frame.page_id(), frame.data());
      if (OB_FAIL(rc)) {
        return rc;
      }
      frame.clear_dirty();
    }
  }

  if (header_dirty_) {
    RC rc = write_header();
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  if (handle_ != nullptr && fflush(handle_) != 0) {
    return RC::IOERR_SYNC;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::close()
{
  if (handle_ == nullptr) {
    return RC::SUCCESS;
  }

  RC rc = flush_all_buffers();
  if (OB_FAIL(rc)) {
    LOG_ERROR("failed to flush buffer pool on close. file=%s, rc=%s", file_name_.c_str(), strrc(rc));
  }

  fclose(handle_);
  handle_     = nullptr;
  page_table_.clear();
  frames_.clear();
  return rc;
}

// ---------------------------------------------------------------------------
// DiskBufferPoolManager
// ---------------------------------------------------------------------------

RC DiskBufferPoolManager::create_file(const std::string &path)
{
  DiskBufferPool pool;
  return pool.create_file(path);
}

RC DiskBufferPoolManager::open_file(const std::string &path, ReadWriteMode mode, DiskBufferPool **pool)
{
  auto iter = by_path_.find(path);
  if (iter != by_path_.end()) {
    *pool = iter->second;
    return RC::SUCCESS;
  }

  auto owner = std::make_unique<DiskBufferPool>();
  RC   rc    = owner->open(path, mode, frame_limit_);
  if (OB_FAIL(rc)) {
    LOG_ERROR("failed to open buffer pool file. path=%s, rc=%s", path.c_str(), strrc(rc));
    return rc;
  }

  DiskBufferPool *raw = owner.get();
  pools_.push_back(std::move(owner));
  by_path_[path] = raw;

  *pool = raw;
  return RC::SUCCESS;
}

DiskBufferPool *DiskBufferPoolManager::get_pool(const std::string &path) const
{
  auto iter = by_path_.find(path);
  return iter == by_path_.end() ? nullptr : iter->second;
}

RC DiskBufferPoolManager::close_file(const std::string &path)
{
  auto iter = by_path_.find(path);
  if (iter == by_path_.end()) {
    return RC::FILE_NOT_EXIST;
  }

  DiskBufferPool *target = iter->second;
  RC              rc     = target->close();

  by_path_.erase(iter);
  for (auto it = pools_.begin(); it != pools_.end(); ++it) {
    if (it->get() == target) {
      pools_.erase(it);
      break;
    }
  }
  return rc;
}

RC DiskBufferPoolManager::flush_all()
{
  RC rc = RC::SUCCESS;
  for (auto &pool : pools_) {
    RC one = pool->flush_all_buffers();
    if (OB_FAIL(one)) {
      rc = one;
    }
  }
  return rc;
}

RC DiskBufferPoolManager::close()
{
  RC rc = RC::SUCCESS;
  for (auto &pool : pools_) {
    RC one = pool->close();
    if (OB_FAIL(one)) {
      rc = one;
    }
  }
  pools_.clear();
  by_path_.clear();
  return rc;
}
