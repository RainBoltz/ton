/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include "td/utils/common.h"
#include "td/utils/logging.h"

#include <atomic>
#include <memory>
#include <utility>

namespace td {
// It is draft object pool implementaion
//
// Compared with std::shared_ptr:
// + WeakPtr are much faster. Just pointer copy. No barriers, no atomics.
// - We can't destroy object, because we don't know if it is pointed to by some weak pointer
//
template <class DataT>
class ObjectPool {
  struct Storage;

 public:
  class WeakPtr {
   public:
    WeakPtr() : generation_(-1), storage_(nullptr) {
    }
    WeakPtr(int32 generation, Storage *storage) : generation_(generation), storage_(storage) {
    }

    DataT &operator*() const {
      return storage_->data;
    }

    DataT *operator->() const {
      return &**this;
    }

    // Pattern of usage: 1. Read an object 2. Check if read was valid via is_alive
    //
    // It is not very usual case of acquire/release use.
    // Instead of publishing an object via some flag we do the opposite.
    // We publish new generation via destruction of the data.
    // In usual case if we see a flag, then we are able to use an object.
    // In our case if we have used an object and it is already invalid, then generation will mismatch
    bool is_alive() const {
      if (!storage_) {
        return false;
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      return generation_ == storage_->generation.load(std::memory_order_relaxed);
    }

    // Used for ActorId
    bool is_alive_unsafe() const {
      if (!storage_) {
        return false;
      }
      return generation_ == storage_->generation.load(std::memory_order_relaxed);
    }

    bool empty() const {
      return storage_ == nullptr;
    }
    void clear() {
      generation_ = -1;
      storage_ = nullptr;
    }
    int32 generation() {
      return generation_;
    }

   private:
    int32 generation_;
    Storage *storage_;
  };

  class OwnerPtr {
   public:
    OwnerPtr() = default;
    OwnerPtr(const OwnerPtr &) = delete;
    OwnerPtr &operator=(const OwnerPtr &) = delete;
    OwnerPtr(OwnerPtr &&other) : storage_(other.storage_), parent_(other.parent_) {
      other.storage_ = nullptr;
      other.parent_ = nullptr;
    }
    OwnerPtr &operator=(OwnerPtr &&other) {
      if (this != &other) {
        storage_ = other.storage_;
        parent_ = other.parent_;
        other.storage_ = nullptr;
        other.parent_ = nullptr;
      }
      return *this;
    }
    ~OwnerPtr() {
      reset();
    }

    DataT *get() {
      return &storage_->data;
    }
    DataT &operator*() {
      return *get();
    }
    DataT *operator->() {
      return get();
    }

    const DataT *get() const {
      return &storage_->data;
    }
    const DataT &operator*() const {
      return *get();
    }
    const DataT *operator->() const {
      return get();
    }

    WeakPtr get_weak() {
      return WeakPtr(storage_->generation.load(std::memory_order_relaxed), storage_);
    }
    int32 generation() {
      return storage_->generation.load(std::memory_order_relaxed);
    }

    Storage *release() {
      auto result = storage_;
      storage_ = nullptr;
      return result;
    }

    bool empty() const {
      return storage_ == nullptr;
    }

    void reset() {
      if (storage_ != nullptr) {
        // for crazy cases when data owns owner pointer to itself.
        auto tmp = storage_;
        storage_ = nullptr;
        parent_->release(OwnerPtr(tmp, parent_));
      }
    }

   private:
    friend class ObjectPool;
    OwnerPtr(Storage *storage, ObjectPool<DataT> *parent) : storage_(storage), parent_(parent) {
    }
    Storage *storage_ = nullptr;
    ObjectPool<DataT> *parent_ = nullptr;
  };

  template <class... ArgsT>
  OwnerPtr create(ArgsT &&... args) {
    Storage *storage = get_storage();
    storage->init_data(std::forward<ArgsT>(args)...);
    return OwnerPtr(storage, this);
  }

  OwnerPtr create_empty() {
    Storage *storage = get_storage();
    return OwnerPtr(storage, this);
  }

  void set_check_empty(bool flag) {
    check_empty_flag_ = flag;
  }

  void release(OwnerPtr &&owner_ptr) {
    Storage *storage = owner_ptr.release();
    storage->destroy_data();
    release_storage(storage);
  }

  ObjectPool() = default;
  ObjectPool(const ObjectPool &) = delete;
  ObjectPool &operator=(const ObjectPool &) = delete;
  ObjectPool(ObjectPool &&other) = delete;
  ObjectPool &operator=(ObjectPool &&other) = delete;
  ~ObjectPool() {
    // Walk the atomic chunk list and delete each chunk
    Storage *node = chunk_list_.load(std::memory_order_relaxed);
    while (node != nullptr) {
      // node points to chunk[CHUNK_SIZE-1], so chunk start is (node - (CHUNK_SIZE-1))
      Storage *chunk = node - (CHUNK_SIZE - 1);
      Storage *next = node->next;
      delete[] chunk;
      node = next;
    }
  }

 private:
  struct Storage {
    // union {
    DataT data;
    //};
    Storage *next = nullptr;
    std::atomic<int32> generation{1};

    template <class... ArgsT>
    void init_data(ArgsT &&... args) {
      // new  (&data) DataT(std::forward<ArgsT>(args)...);
      data = DataT(std::forward<ArgsT>(args)...);
    }
    void destroy_data() {
      generation.fetch_add(1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      data.clear();
    }
  };

  std::atomic<int32> storage_count_{0};
  std::atomic<Storage *> head_{static_cast<Storage *>(nullptr)};
  bool check_empty_flag_ = false;
  std::atomic<Storage *> chunk_list_{nullptr};  // Lock-free list of allocated chunks

  // Performance optimization: allocate Storages in chunks to reduce allocation overhead
  static constexpr size_t CHUNK_SIZE = 64;

  Storage *allocate_chunk() {
    // Allocate a chunk of Storage objects
    Storage *chunk = new Storage[CHUNK_SIZE];
    storage_count_.fetch_add(CHUNK_SIZE, std::memory_order_relaxed);

    // Link items 1 to CHUNK_SIZE-2 together for free list
    // Item 0 is returned to caller, item CHUNK_SIZE-1 is reserved for chunk tracking
    for (size_t i = 1; i < CHUNK_SIZE - 1; i++) {
      chunk[i].next = &chunk[i + 1];
    }
    chunk[CHUNK_SIZE - 2].next = nullptr;

    // Add items 1 to CHUNK_SIZE-2 to the free list
    if (CHUNK_SIZE > 2) {
      Storage *chunk_head = &chunk[1];
      while (true) {
        auto *save_head = head_.load(std::memory_order_relaxed);
        chunk[CHUNK_SIZE - 2].next = save_head;
        if (likely(head_.compare_exchange_weak(save_head, chunk_head, std::memory_order_release, std::memory_order_relaxed))) {
          break;
        }
      }
    }

    // Track this chunk using lock-free atomic list
    // Use chunk[CHUNK_SIZE-1] as the link node for chunk tracking
    Storage *chunk_node = &chunk[CHUNK_SIZE - 1];
    while (true) {
      auto *save_head = chunk_list_.load(std::memory_order_relaxed);
      chunk_node->next = save_head;
      if (likely(chunk_list_.compare_exchange_weak(save_head, chunk_node, std::memory_order_release, std::memory_order_relaxed))) {
        break;
      }
    }

    return &chunk[0];
  }

  Storage *get_storage() {
    // Try to get from free list first (fast path - likely case)
    Storage *res = head_.load(std::memory_order_acquire);
    if (unlikely(res == nullptr)) {
      // Allocate a new chunk (slow path - rare)
      return allocate_chunk();
    }

    // Fast path: try to pop from free list
    while (true) {
      res = head_.load(std::memory_order_acquire);
      if (unlikely(res == nullptr)) {
        return allocate_chunk();
      }
      auto *next = res->next;
      if (likely(head_.compare_exchange_weak(res, next, std::memory_order_release, std::memory_order_relaxed))) {
        break;
      }
    }
    return res;
  }

  // release can be called from other thread
  void release_storage(Storage *storage) {
    // Optimized memory ordering: use relaxed for load, release for CAS
    while (true) {
      auto *save_head = head_.load(std::memory_order_relaxed);
      storage->next = save_head;
      if (likely(head_.compare_exchange_weak(save_head, storage, std::memory_order_release, std::memory_order_relaxed))) {
        break;
      }
    }
  }
};
}  // namespace td
