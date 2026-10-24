/* database26 buffer manager - lightweight statistics */
#pragma once

#include <atomic>
#include <cstdint>

struct BufferPoolStatsSnapshot
{
  uint64_t hits           = 0;
  uint64_t misses         = 0;
  uint64_t evictions      = 0;
  uint64_t dirty_evictions = 0;
  uint64_t reads          = 0;
  uint64_t writes         = 0;
  uint64_t wal_writes     = 0;
  uint64_t wal_syncs      = 0;
  uint64_t prefetches     = 0;
  uint64_t no_buffer      = 0;
  uint64_t handle_busy    = 0;
  uint64_t double_free    = 0;
  uint64_t corrupted      = 0;
  uint64_t flushes        = 0;
  uint64_t checkpoints    = 0;
};

struct BufferPoolStats
{
  std::atomic<uint64_t> hits{0};
  std::atomic<uint64_t> misses{0};
  std::atomic<uint64_t> evictions{0};
  std::atomic<uint64_t> dirty_evictions{0};
  std::atomic<uint64_t> reads{0};
  std::atomic<uint64_t> writes{0};
  std::atomic<uint64_t> wal_writes{0};
  std::atomic<uint64_t> wal_syncs{0};
  std::atomic<uint64_t> prefetches{0};
  std::atomic<uint64_t> no_buffer{0};
  std::atomic<uint64_t> handle_busy{0};
  std::atomic<uint64_t> double_free{0};
  std::atomic<uint64_t> corrupted{0};
  std::atomic<uint64_t> flushes{0};
  std::atomic<uint64_t> checkpoints{0};

  BufferPoolStatsSnapshot snapshot() const
  {
    BufferPoolStatsSnapshot s;
    s.hits            = hits.load(std::memory_order_relaxed);
    s.misses          = misses.load(std::memory_order_relaxed);
    s.evictions       = evictions.load(std::memory_order_relaxed);
    s.dirty_evictions = dirty_evictions.load(std::memory_order_relaxed);
    s.reads           = reads.load(std::memory_order_relaxed);
    s.writes          = writes.load(std::memory_order_relaxed);
    s.wal_writes      = wal_writes.load(std::memory_order_relaxed);
    s.wal_syncs       = wal_syncs.load(std::memory_order_relaxed);
    s.prefetches      = prefetches.load(std::memory_order_relaxed);
    s.no_buffer       = no_buffer.load(std::memory_order_relaxed);
    s.handle_busy     = handle_busy.load(std::memory_order_relaxed);
    s.double_free     = double_free.load(std::memory_order_relaxed);
    s.corrupted       = corrupted.load(std::memory_order_relaxed);
    s.flushes         = flushes.load(std::memory_order_relaxed);
    s.checkpoints     = checkpoints.load(std::memory_order_relaxed);
    return s;
  }
};
