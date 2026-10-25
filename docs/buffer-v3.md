# 缓冲管理器 v3：全局帧管理

## 1. 目标

v2 的每个 `DiskBufferPool` 自己持有一组 `Frame`，`DiskBufferPoolManager` 只做“总帧数上限”。  
v3 把 frame 的所有权收归 `FrameManager`，所有打开的文件共享同一个全局 frame 池和同一套 LRU-K 淘汰。

对外 API 保持不变：

```cpp
pool->get_page_read(page, handle);
pool->get_page_write(page, handle);
pool->allocate_page(&page);
pool->dispose_page(page);
pool->flush_all_buffers();
```

`Db` / `Table` / `B+Tree` 不需要修改。

---

## 2. 结构

```text
DiskBufferPoolManager
  ├─ FrameManager                 // 全局 frame 池、全局页表、全局 LRU-K
  │    ├─ Frame[]                 // total_frame_limit 个 frame
  │    ├─ page_table              // (pool_id, page) -> FrameId
  │    ├─ owner_frame_counts      // 每个文件的 frame 配额
  │    └─ LruKPolicy
  └─ DiskBufferPool[]             // 文件句柄、header、free list、WAL
       └─ frame_manager_ 指针
```

- `Frame` 的 owner 指向所属 `DiskBufferPool`。
- 页表 key 是 `(pool_id, page_num)`。
- 页淘汰由 `FrameManager` 全局选择；如果 victim 属于其他文件，就回调该文件的 `flush_frame_data()` 写回。
- 每个文件仍有 `frame_limit` 配额，防止一个文件独占全局 frame。
- 直接 `new DiskBufferPool` 并 `open()` 时，pool 会自己创建一个单文件 `FrameManager`，保持旧接口可用。

---

## 3. 锁顺序

```text
DiskBufferPool::meta_mutex_
  -> FrameManager::mutex_
      -> DiskBufferPool::flush_mutex_
          -> PageWAL::mutex_

Frame latch 在拿到 frame_id 之后单独获取，不和上面几把锁同时持有。
```

- `meta_mutex_` 保护文件 header、free list、closing 状态。
- `FrameManager::mutex_` 保护全局页表、frame 元数据、owner 配额、LRU-K。
- `flush_mutex_` 串行化“WAL append + 数据页写”和 checkpoint，避免 checkpoint 在 append 与 data write 之间截断 WAL。
- 页 miss 目前仍持 `FrameManager::mutex_` 做 I/O；这是 Phase 1 的已知性能限制，Phase 2 会改成 per-page in-flight I/O。

---

## 4. 淘汰语义

`FrameManager::acquire()`：

1. 查全局页表；命中则 pin++。
2. 未命中则选择一个可淘汰 frame：
   - 如果当前文件配额已满，只在当前文件的 frame 中选；
   - 否则在全局 frame 中选。
3. victim 若脏，调用其 owner 的 `flush_frame_data()` 写 WAL + 数据页。
4. 从全局页表移除 victim，重置 frame，分配给当前文件/page。
5. 从文件读取页到 frame，插入全局页表。

`FrameManager::remove_owner()` 在 pool 关闭时清理该文件全部 frame。

---

## 5. 当前限制（Phase 2 处理）

- 页 miss 仍持全局 `FrameManager::mutex_`，不同文件的缺页不能并行。
- 预取仍是同步预热。
- 刷脏仍是同步的，没有 page cleaner 线程。
- WAL 仍是 full-page redo，没有 doublewrite / 数据页 checksum。

这些会在 v3 后续阶段继续处理。
