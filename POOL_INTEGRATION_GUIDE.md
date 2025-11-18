# Memory Pool Integration Guide

## Overview

Phase 5C introduced thread-local memory pools to reduce allocation overhead in hot paths. This guide explains how to integrate the pools into existing code.

## Available Pools

### 1. CellBuilderPool (`crypto/vm/cells/CellBuilderPool.h`)

**Purpose:** Pool CellBuilder objects to reduce allocation overhead during cell construction.

**API:**
```cpp
#include "vm/cells/CellBuilderPool.h"

// Acquire a CellBuilder from the pool
auto builder = vm::CellBuilderPool::acquire();

// Use it
builder->store_bits(...);

// Release back to pool (automatic when unique_ptr goes out of scope)
vm::CellBuilderPool::release(std::move(builder));

// Get statistics
auto stats = vm::CellBuilderPool::get_stats();
LOG(INFO) << "Pool hits: " << stats.pool_hits << "/" << stats.allocations;
```

**Integration Points:**

1. **CellBuilder::make_copy()** (`crypto/vm/cells/CellBuilder.cpp:568`)
   - Current: Returns raw `CellBuilder*`
   - Refactoring needed: Change API to return `unique_ptr<CellBuilder>`
   - Impact: High (frequently called during cell operations)

2. **New CellBuilder allocations**
   - Replace: `auto builder = std::make_unique<CellBuilder>()`
   - With: `auto builder = vm::CellBuilderPool::acquire()`

3. **Validator/Collator code**
   - `validator/impl/collator.cpp`: Multiple CellBuilder creations
   - Can wrap in pool acquire/release for batch operations

### 2. BufferSlicePool (`rldp2/PacketPool.h`)

**Purpose:** Pool BufferSlice objects to reduce allocation overhead in network operations.

**API:**
```cpp
#include "rldp2/PacketPool.h"

// Acquire a buffer of specified size
auto buffer = ton::rldp2::BufferSlicePool::acquire(4096);

// Use it
memcpy(buffer.data(), ...);

// Release back to pool
ton::rldp2::BufferSlicePool::release(std::move(buffer));

// Get statistics
auto stats = ton::rldp2::BufferSlicePool::get_stats();
LOG(INFO) << "Buffer pool hit rate: "
          << (100 * stats.pool_hits / stats.total_allocations) << "%";
```

**Integration Points:**

1. **ADNL packet send** (`adnl/adnl-peer.cpp:453`)
   ```cpp
   // Before:
   auto enc = td::BufferSlice(X.size() + 32);

   // After (requires adding dependency):
   auto enc = ton::rldp2::BufferSlicePool::acquire(X.size() + 32);
   // Note: Need to add rldp2 dependency to adnl, or move pool to tdutils
   ```

2. **ADNL channel send** (`adnl/adnl-channel.cpp:110`)
   ```cpp
   // Before:
   auto B = td::BufferSlice(enc.size() + 32);

   // After:
   auto B = ton::rldp2::BufferSlicePool::acquire(enc.size() + 32);
   ```

3. **RLDP2 packet handling**
   - `rldp2/OutboundTransfer.cpp`: Packet buffer allocations
   - `rldp2/InboundTransfer.cpp`: Reception buffer allocations
   - `rldp2/SenderPackets.cpp`: Packet queue buffers

### 3. Generic ObjectPool Template (`rldp2/PacketPool.h`)

**Purpose:** Pool any object type with simple free-list management.

**API:**
```cpp
#include "rldp2/PacketPool.h"

// For any type T:
auto obj = ton::rldp2::ObjectPool<MyClass>::acquire();
obj->do_something();
ton::rldp2::ObjectPool<MyClass>::release(std::move(obj));
```

**Integration Points:**

1. **OutboundTransfer/InboundTransfer objects**
2. **Small message structures**
3. **Temporary calculation objects**

## Integration Strategy

### Phase 1: Low-Risk Integration (Recommended First)

1. **New code only**: Use pools in new features
2. **Batch operations**: Wrap existing code in pool acquire/release
3. **Monitoring**: Add statistics logging to validate benefit

### Phase 2: Hot Path Integration

1. **RLDP2 buffers**: Highest impact, isolated subsystem
2. **ADNL send paths**: High frequency, medium risk
3. **Validator allocations**: High impact, careful testing needed

### Phase 3: API Refactoring

1. **CellBuilder::make_copy()**: Change return type to unique_ptr
2. **Update all call sites**: Systematic refactoring
3. **Performance validation**: Measure before/after

## Architecture Considerations

### Dependency Management

**Current Issue:** BufferSlicePool is in `rldp2/` namespace but needed in `adnl/`.

**Solutions:**

1. **Option A (Quick):** Add rldp2 dependency to adnl
   - CMakeLists.txt: `target_link_libraries(adnl ... rldp2)`
   - Pro: Simple, immediate use
   - Con: Circular dependency risk

2. **Option B (Clean):** Move BufferSlicePool to `tdutils/td/utils/`
   - Rename to `td::BufferPool`
   - Make it a core utility
   - Pro: Available everywhere, clean architecture
   - Con: Requires file moves and namespace changes

3. **Option C (Minimal):** Duplicate pools per subsystem
   - ADNL has its own BufferPool
   - RLDP2 has its own
   - Pro: No dependencies
   - Con: Code duplication

**Recommended:** Option B for production, Option A for quick testing.

## Performance Monitoring

### Adding Statistics Logging

```cpp
// In main event loop or periodic timer:
void log_pool_statistics() {
  auto cell_stats = vm::CellBuilderPool::get_stats();
  auto buffer_stats = ton::rldp2::BufferSlicePool::get_stats();

  LOG(INFO) << "CellBuilder pool: "
            << cell_stats.pool_hits << "/" << cell_stats.allocations
            << " hits (" << (100 * cell_stats.pool_hits / cell_stats.allocations) << "%), "
            << "pool_size=" << cell_stats.pool_size;

  LOG(INFO) << "BufferSlice pool: "
            << buffer_stats.pool_hits << "/" << buffer_stats.total_allocations
            << " hits (" << (100 * buffer_stats.pool_hits / buffer_stats.total_allocations) << "%), "
            << "cached=" << buffer_stats.cached_buffers;
}
```

### Expected Results

| Pool | Hit Rate Target | Impact |
|------|----------------|--------|
| CellBuilder | 60-80% | 3-5% CPU reduction |
| BufferSlice | 40-60% | 2-3% CPU reduction |
| Combined | N/A | **5-10% overall** |

## Safety Notes

1. **Thread safety**: Pools are thread-local, no locking needed
2. **Leak prevention**: Use RAII (unique_ptr) to auto-release
3. **Size limits**: Pools have max capacity, excess objects are freed
4. **No fragmentation**: Pools reuse exact or similar sizes

## Quick Start Example

```cpp
// Example: Integrate buffer pool in RLDP2 send path

// File: rldp2/OutboundTransfer.cpp
#include "PacketPool.h"

// Before:
void send_packet() {
  auto buffer = td::BufferSlice(packet_size);
  // ... fill buffer ...
  send(std::move(buffer));
}

// After:
void send_packet() {
  auto buffer = BufferSlicePool::acquire(packet_size);
  // ... fill buffer ...
  send(std::move(buffer));
  // Pool automatically gets buffer back when 'buffer' is destroyed
}
```

## Testing Integration

1. **Compile test**: Ensure no build errors
2. **Unit test**: Verify pool acquire/release cycles
3. **Stress test**: High allocation rate under load
4. **Performance test**: Measure latency/throughput improvement
5. **Memory test**: Verify no leaks with valgrind/asan

## Future Enhancements

1. **Per-thread statistics**: Track per-worker-thread metrics
2. **Auto-tuning**: Adjust pool sizes based on usage patterns
3. **Prefetching**: Pre-allocate chunks during idle time
4. **Size classes**: Multiple pools for different size ranges

## Contact

For questions or issues with pool integration, see:
- `crypto/vm/cells/CellBuilderPool.{h,cpp}`
- `rldp2/PacketPool.{h,cpp}`
- This guide: `POOL_INTEGRATION_GUIDE.md`
