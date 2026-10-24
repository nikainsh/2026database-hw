# 缓冲管理器线程安全模型

> 目标：在多线程下正确工作，同时保留旧代码的源码兼容性。

---

## 1. 两套 API

### 兼容 API（默认）

```cpp
PageHandle handle;
pool.get_page(page_num, handle);   // PageAccess::NONE
```

- 只 pin，**不加页锁**；
- 旧代码不需要修改；
- 多线程下如果多个线程读写同一页，**不安全**；
- `mark_dirty()` 允许（为了兼容旧代码），但同样不提供并发保护。

### 显式页锁 API（多线程推荐）

```cpp
PageHandle handle;
pool.get_page_read(page_num, handle);   // 共享锁
pool.get_page_write(page_num, handle);  // 独占锁
```

- `READ`：多个读者可以同时持有；写者等待；
- `WRITE`：独占；其他读者/写者等待；
- `mark_dirty()` 只允许 `WRITE`（以及兼容的 `NONE`）；`READ` 返回 `BUFFERPOOL_READ_ONLY_HANDLE`；
- 句柄析构/reset 时自动释放页锁、unpin、减少 active handle 计数。

---

## 2. 锁与原子量清单

| 机制 | 保护对象 |
| --- | --- |
| `DiskBufferPool::meta_mutex_` | 页表、free list、header、replacement 访问历史、frame 元数据 |
| `DiskBufferPool::flush_mutex_` | 单页 flush / 全量 flush / WAL checkpoint 的串行化 |
| `Frame::latch_`（`std::shared_mutex`） | 8KB 页数据 |
| `PageWAL::mutex_` | WAL append/replay/checkpoint |
| `DiskBufferPoolManager::mutex_` | 打开/关闭 pool、路径映射、全局帧预算 |
| `Frame::pin_count_`（atomic） | 页 pin 计数 |
| `Frame::dirty_`（atomic） | 脏页标记 |
| `DiskBufferPool::active_handles_`（atomic） | 未释放的 PageHandle 数量 |

---

## 3. 锁顺序

为了避免死锁，遵循以下顺序：

```text
DiskBufferPoolManager::mutex_
  -> DiskBufferPool::flush_mutex_
      -> DiskBufferPool::meta_mutex_
          -> PageWAL::mutex_

DiskBufferPool::meta_mutex_
  -> PageWAL::mutex_        (allocate/dispose 的 WAL append)

Frame::latch_               (通常不与其他锁同时持有；flush 时在 flush_mutex_ 下)
```

注意：

- `get_page` / `flush_page` / `flush_all_buffers` 在**释放 meta_mutex_** 之后才去拿 `Frame::latch_`，避免“持页表锁等页锁”。
- 淘汰（`allocate_frame_locked`）在持有 `meta_mutex_` 时只选择 `pin_count == 0` 的帧，因此不会去拿页锁。
- `PageHandle::reset` 不拿 `meta_mutex_`，只释放页锁、unpin、减少 active handle。
- `PageWAL::mutex_` 是最内层锁，不应在持有它时回调上层。

---

## 4. 页的生命周期

```text
get_page(page, READ/WRITE):
  lock(meta)
  page_table hit?
    pin_count++
    active_handles++
    record LRU-K access
    unlock(meta)
    lock frame latch (shared/exclusive)
    return PageHandle
  miss:
    choose victim / flush dirty / read page
    pin_count = 1
    active_handles++
    insert page_table
    unlock(meta)
    lock frame latch
    return PageHandle

PageHandle::reset:
  unlock frame latch
  unpin_page()
  active_handles--
```

- `pin_count > 0` 时 frame 不会被淘汰。
- `active_handles > 0` 时 `close()` 返回 `BUFFERPOOL_HANDLE_BUSY`。
- 页锁在 `PageHandle` 生命周期内一直持有；不要把 `PageHandle` 传出作用域后继续使用 `value()`。

---

## 5. 关闭语义

```cpp
pool.close();
```

- 先检查 `active_handles_`；
- 如果 > 0：返回 `BUFFERPOOL_HANDLE_BUSY`，不关闭、不清空；
- 如果 = 0：设置 `closing_`，阻止新的 `get_page`；
- flush/checkpoint；
- 关闭 WAL、数据文件，清空页表/free list/frames。

上层必须保证关闭前释放所有 `PageHandle`。  
`DiskBufferPoolManager::close_file` 在 pool close 失败时会保留 pool，不会从 map 中删除。

---

## 6. 已知并发限制

1. **页 miss 串行化**  
   当前 `get_page` 在 miss 时持有 `meta_mutex_` 做 I/O；其他线程的页表查找会被阻塞。  
   下一步：per-page in-flight I/O + 条件变量，只锁住同一页的等待者。

2. **同一线程不能重入同一页的写锁**  
   `std::shared_mutex` 不可重入。需要重入时：
   - 使用默认 `get_page`（`NONE`，只 pin）；
   - 或重构调用顺序，避免嵌套锁。

3. **兼容模式不是线程安全的**  
   默认 `get_page` 不加页锁。多线程代码必须迁移到 `get_page_read` / `get_page_write`。

4. **全局帧预算不是全局替换**  
   每个 pool 仍独立淘汰；`total_frame_limit` 只是总额限制。真正全局 frame manager 留作后续。

5. **没有死锁检测**  
   上层需要遵守锁顺序；不要在持有页锁时长时间阻塞。

---

## 7. 上层迁移建议

- 只读扫描 / 读取页：`get_page_read`。
- 修改页内容：`get_page_write` + `mark_dirty`。
- 需要 pin 但不需要页锁（例如只检查页号/状态）：默认 `get_page`。
- 不要在同一个线程里对同一页嵌套 `get_page_write`。
- 不要在 `PageHandle` 未释放时关闭 pool。
- 多线程测试参考 `tests/test_buffer_pool_threads.cpp`。
