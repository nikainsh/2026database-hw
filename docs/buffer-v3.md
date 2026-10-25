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

---

## 6. Phase 2：in-flight I/O、异步预取与后台刷脏

### 6.1 页级 in-flight I/O

- 页 miss 时先在全局页表插入一个 `loading` frame，再释放 `FrameManager::mutex_` 做磁盘读。
- 同一页的并发 miss 会等待条件变量，只发一次磁盘读。
- 不同文件可以并行缺页；同一个 `DiskBufferPool` 的 `get_page` 仍持 `meta_mutex_`，因此单文件内缺页暂时串行。

### 6.2 异步预取

- `FrameManager` 有一个后台 worker 和预取队列。
- `prefetch_page` / `prefetch_range` 只入队，立即返回。
- 页已在缓存、正在 loading 或已在队列中时去重。
- worker 通过 `acquire()` 读页，然后立即 `unpin()`。
- `cancel_owner()` 在文件关闭时清空该文件的队列并等待在途预取完成。

### 6.3 后台刷脏

- worker 每 50ms 检查一次，最多刷 4 个脏页。
- 只选择未 pin、未 loading、owner 未关闭的帧。
- 使用 `owner_active_ops_` 统计后台操作，`cancel_owner()` 会等待这些操作完成后再关闭文件。
- 仍然不会在后台做 WAL checkpoint；checkpoint 仍在 `flush_all_buffers()` / `close()` 时同步执行。

### 6.4 已知限制

- 同一个 pool 的缺页仍被 `meta_mutex_` 串行化。
- 后台刷脏没有 group commit / 批量 WAL 优化。
- 预取 worker 是单线程；后续可按 CPU/IO 并发度扩展。

---

## 7. Phase 3：分配元数据持久化

v2 中 `allocate_page` / `dispose_page` 只修改内存 header，header 要等到 flush/checkpoint 才落盘。  
如果进程在 header 落盘前崩溃，可能丢失最新的页分配/释放信息。

v3 改为：

- `allocate_page` 更新 `first_free_page` / `page_count` 后，立即把 header 页写入 WAL 和数据文件；
- `dispose_page` 更新 free list 后，同样立即持久化 header；
- 代价是每次分配/释放多一次 header 页写入。

这样分配元数据在崩溃恢复时不会落后于数据页。

### 尚未做的 durability 项

以下两项需要在数据文件旁边增加 sidecar（如 `.dblwr` / `.chk`），  
而 `Db::drop_table` 目前只清理 `.heap` / `.index` / `.wal`；新增 sidecar 需要修改 `src/storage/db/db.cpp`，与当前“只改缓冲管理器”的约束冲突，因此暂不启用：

- doublewrite buffer / torn page 防护；
- 数据页 checksum。

当前页级 full-page WAL 已经能提供 redo；上述两项属于更高等级的介质/崩溃保护，后续如果要启用，需要先确认 sidecar 清理方式。

---

## 8. Phase 4：基准测试

`bench/bench_buffer_pool.cpp` 提供三个通用负载：

- 随机读：512 页，20000 次随机读；
- 顺序扫描：10 遍全文件扫描；
- 80/20 混合读写：5000 次操作。

运行：

```bash
bash bench/run_bench.sh
```

本机基线（MinGW g++ 14.2，512 页，64 帧）：

```text
random_read:   ops=20000 time=147.9 ms throughput=135248 ops/s
sequential:    ops=5120  time=33.7 ms  throughput=151888 ops/s
mixed_80_20:   ops=5000  time=459.1 ms throughput=10892 ops/s
```

混合写吞吐明显低于读，主要因为当前 WAL 每次写页都 fsync。后续如果要优化写路径，可以做 group commit / 批量 fsync。

---

## 9. 后台临时 pin 与误报 NOBUF 的修复

后台刷脏/预取会临时 pin 帧，可能让小 frame 池在极短时间内看起来“没有可淘汰帧”。  
v3 对这类临时占用做了区分：

- `Frame` 增加独立的 `background_pin_count`；
- 前台 `acquire` 找不到 victim 时：
  - 如果仍有后台临时 pin，等待条件变量；
  - 如果只剩用户 pin，立即返回 `BUFFERPOOL_NOBUF`；
- 后台刷脏使用 `try_lock` 获取页锁，拿不到就跳过该页并立即 unpin，避免用户等后台；
- `dispose_page` 遇到“只有后台临时 pin”的页面会等待，遇到真实用户 pin 仍立即返回 `BUFFERPOOL_PAGE_PINNED`；
- 后台预取属于 best-effort，`acquire(..., background=true)` 在无可用帧时直接放弃，不会阻塞 stop/close。

新增回归测试：

```text
tests/test_background_flush_pressure.cpp
```

它用 2 帧的小池持续读写/dispose，同时让后台刷脏运行，要求不出现误报的 `BUFFERPOOL_NOBUF` 或 `BUFFERPOOL_PAGE_PINNED`。
