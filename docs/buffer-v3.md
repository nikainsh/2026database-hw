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
          -> DiskBufferPool::io_mutex_
          -> PageWAL::mutex_

Frame latch 在拿到 frame_id 之后单独获取，不和上面几把锁同时持有。
```

- `meta_mutex_` 保护文件 header、free list、closing 状态。
- `FrameManager::mutex_` 保护全局页表、frame 元数据、owner 配额、LRU-K。
- `flush_mutex_` 串行化“WAL append + 数据页写”和 checkpoint，避免 checkpoint 在 append 与 data write 之间截断 WAL。
- `io_mutex_` 串行化数据文件上的 seek/read/write/sync。异步预取不持 `meta_mutex_`，因此不能只靠文件级元数据锁保护 `FILE*` 偏移。
- 页 miss 会先在全局页表中插入 `loading` frame，然后释放 `FrameManager::mutex_` 做磁盘读；
- 同一页的并发 miss 通过条件变量等待，只发一次 I/O；
- 不同文件可以并行缺页；同一个 `DiskBufferPool` 的 `get_page` 仍持 `meta_mutex_`，因此单文件内缺页串行。

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

## 5. 当前并发与耐久性边界

- 同一个 `DiskBufferPool` 的缺页仍由 `meta_mutex_` 串行化；跨文件缺页可以并行。
- 预取 worker 是单线程；预取是 best-effort，不保证调用返回时页已驻留。
- 后台刷脏每 50ms 最多处理 4 页，不做后台 checkpoint，也没有 group commit。
- WAL 仍是页级 full-page redo；尚未启用 doublewrite / 数据页 checksum。
- 所有数据文件 I/O 都经过 `io_mutex_`；异步预取可以与前台读写、后台刷脏并发，但不会互相踩 `FILE*` 偏移。
- `flush_page` / `flush_all_buffers` 会与页写入串行化；调用方不应在另一个线程同时 `close` 同一个 pool。

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

- `allocate_page` / `dispose_page` 把「新的 header 页 + 清零/挂入 free list 的数据页」作为**一条两页 WAL record** 追加并 fsync，然后依次写两个数据页；
- 崩溃恢复时两条页镜像一起重放，因此不会出现只恢复 header 或只恢复数据页的中间状态；
- 代价是每次分配/释放多一次 header 页写入和两个页面的 WAL 空间。

注意：如果 WAL 追加成功但当前进程内的数据页写入失败，内存 header 不会提交；磁盘上的中间状态由下次打开时的 WAL replay 修正。

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

本机近期基线（MinGW g++ 14.2，512 页，64 帧；单线程，受磁盘缓存影响）：

```text
random_read:   ops=20000 time=176.2 ms throughput=113534 ops/s
sequential:    ops=5120  time=38.8 ms  throughput=132030 ops/s
mixed_80_20:   ops=5000  time=436.0 ms throughput=11469 ops/s
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

---

## 10. 预取与释放页的竞态修复

预取是异步的：`prefetch_page(page)` 入队后，调用方可能立刻 `dispose_page(page)` 或通过 `allocate_page` 复用该页。
如果预取 worker 此时才把旧页读入缓存，会得到一帧已经不属于当前页的数据。

v3 的处理：

- `FrameManager` 维护 `prefetch_pending_` 与 `prefetch_active_`；
- `cancel_prefetch(owner, page)` 先删除尚未执行的队列项，再等待该页在途预取完成；
- `dispose_page` 和 `allocate_page` 复用空闲页前都会先 `cancel_prefetch`，再 `remove_page` 清掉可能存在的旧缓存。

回归测试：

```text
tests/test_prefetch_dispose_race.cpp
```

测试在 1 帧的池中循环“预取 -> 释放 -> 复用”，并检查复用页必须是清零后的新页。

## 11. WAL 两页 record

`PageWAL` 新增：

```cpp
RC append_two_pages_and_sync(PageNum page_a, const char *data_a,
                             PageNum page_b, const char *data_b);
```

- record header 的 `size` 为 `2 * BP_PAGE_SIZE`，`page_num` / `reserved2` 分别保存两个页号；
- CRC 覆盖两页数据；任意一页损坏都会让整条 record 被丢弃，不会只重放其中一页；
- 单页 record 的格式保持不变，旧 WAL 仍可回放。

回归测试覆盖两页 record 的正常回放和“第二页损坏时整条 record 不生效”。

---

## 12. 数据文件 I/O 与帧唤醒的并发收尾

### 12.1 数据文件 I/O 串行化

异步预取由 `FrameManager` 的 worker 执行，不持有 `DiskBufferPool::meta_mutex_`。  
如果它和前台 `get_page_*` 或后台刷脏同时操作同一个 `FILE*`，`seek` 与 `fread/fwrite` 之间可能交错，导致把错误的页读进缓存。

v3 增加 `DiskBufferPool::io_mutex_`：

- `read_page_raw` / `write_page_raw` / `sync_data_file` 都在该锁下执行；
- 两页分配/释放的 header+data 写入整段持有该锁，运行中的读者不会看到 header 已更新而数据页尚未更新的中间状态；
- 页 latch 仍然只保护页内容，不和 I/O 锁混用。

回归测试：

```text
tests/test_buffer_io_race.cpp
```

它在 8 帧的只读池上同时运行多个前台读者和预取线程，校验每次读到的页内容都属于正确页号。

### 12.2 唤醒不能丢失

`FrameManager::unpin()` 现在在 `FrameManager::mutex_` 下更新 pin/background pin 并通知等待者。  
`acquire()` 等待 `loading`/后台临时 pin、`remove_page()` 等待后台 pin 时都使用谓词等待，避免“检查条件后、进入 wait 前”丢失 `notify` 导致挂起。

回归测试：

```text
tests/test_frame_wakeup_stress.cpp
```

它在 1 帧的池中循环预取、读取、释放和复用页；如果唤醒丢失，测试会在看门狗超时后失败而不是永久挂住。

---

## 13. 原子 checkpoint 与崩溃恢复矩阵

### 13.1 原子 checkpoint

旧实现的 `PageWAL::checkpoint()` 直接用 `fopen("w+b")` 原地截断 WAL。  
如果进程在截断后、header 写回前崩溃，磁盘上会留下零长度或半个 header 的 WAL，即使数据页已经 fsync，下一次打开也会失败。

v3 改为：

1. 在 WAL 同目录创建 `<wal>.tmp`；
2. 写入合法 WAL header 并 fsync；
3. 关闭旧 WAL；
4. 原子替换原 WAL 文件；
5. 重新打开新 WAL；
6. 启动时删除可能残留的 `.wal.tmp`。

这样崩溃后只会看到两种状态：

- 旧 WAL 完整，可以正常重放；
- 新空 WAL 完整，数据页已经 fsync，可以正常打开。

### 13.2 可注入的崩溃点

测试专用环境变量：

```text
MINIDB_CRASH_POINT=<point>
```

设置后，进程会在对应位置直接 `_Exit(99)`，不执行析构，模拟掉电。正常运行时变量缺失，钩子是 no-op。

覆盖的崩溃点：

```text
before_single_page_write        WAL 已 fsync，单页数据还没写
after_single_page_write         单页数据已写，还没 checkpoint
before_wal_checkpoint           数据已 fsync，checkpoint 还没开始
before_checkpoint_replace       临时 WAL 已写完，还没替换旧 WAL
after_checkpoint_replace        新 WAL 已替换，还没重新打开
before_two_page_write           两页 WAL 已 fsync，数据页还没写
after_first_page_write          两页操作只写了第一页
after_two_page_write            两页都写完，还没 checkpoint
```

### 13.3 回归测试

```text
tests/test_crash_consistency.cpp
```

测试通过父子进程模式在 9 个崩溃点退出，然后重新打开数据文件，验证：

- 打开不失败；
- WAL 能重放或已经安全 checkpoint；
- 页内容正确；
- `page_count` / `free_page_count` / free list 一致；
- 分配页清零、释放页可复用；
- 不残留 `.wal.tmp`。

这组测试覆盖“崩溃一致性”，不等价于“介质完整性”：doublewrite / 数据页 checksum 仍作为后续工作，见第 7 节。
