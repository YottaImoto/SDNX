/**
 * Sleeping Dogs port (SDNX): native renderer, a flat hash map for 64-bit hash keys.
 *
 * The content-hashed geometry is looked up several times per draw (index buffer, every vertex binding) in a table
 * of ~20-30k entries. std::unordered_map chases a node pointer per lookup (two cache misses, a heap node per entry);
 * here the keys sit in one array probed linearly, the values in a deque so that a pointer returned to a draw stays
 * valid even when the table grows during the same draw. Keys are already hashes (XXH3), used as they are.
 *
 * §16.156: key, state and value slot in one 16-byte bucket. With three separate arrays (keys, slots, states) every
 * probe touched three cache lines, and with ~28k entries none of them stays in cache: HashedReuse was 11 % of the
 * GPU command thread when driving (4,000-5,500 draws a frame). Now a probe reads one line, then the value.
 * §16.157: the values in chunks of 1024 instead of a std::deque (512-byte blocks: 9 values each, the slot divided
 * by 9 and a load from the block map before the value - a third dependent miss). Chunk pointers stay in L1.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace rex::graphics::sdnx {

template <typename V>
class FlatMap64 {
 public:
  V* find(uint64_t key) {
    if (!count_) return nullptr;
    const size_t mask = buckets_.size() - 1;
    for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
      const Bucket& b = buckets_[i];
      if (b.state == kEmpty) return nullptr;
      if (b.state == kFull && b.key == key) return &Value(b.slot);
    }
  }

  // The value for `key`, default-constructed if new; second is true when it was inserted.
  std::pair<V*, bool> try_emplace(uint64_t key) {
    if ((count_ + tombstones_ + 1) * 2 > buckets_.size()) Rehash(buckets_.empty() ? 1024 : (count_ + 1) * 4);
    const size_t mask = buckets_.size() - 1;
    size_t grave = SIZE_MAX;
    for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
      Bucket& b = buckets_[i];
      if (b.state == kFull) {
        if (b.key == key) return {&Value(b.slot), false};
        continue;
      }
      if (b.state == kTombstone) {
        if (grave == SIZE_MAX) grave = i;
        continue;
      }
      // Empty: the key is absent. Reuse the first tombstone met on the way, if any.
      Bucket& at = buckets_[grave != SIZE_MAX ? grave : i];
      if (grave != SIZE_MAX) --tombstones_;
      at.slot = NewSlot();
      at.key = key;
      at.state = kFull;
      ++count_;
      return {&Value(at.slot), true};
    }
  }

  void erase(uint64_t key) {
    if (!count_) return;
    const size_t mask = buckets_.size() - 1;
    for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
      Bucket& b = buckets_[i];
      if (b.state == kEmpty) return;
      if (b.state == kFull && b.key == key) {
        EraseAt(b);
        return;
      }
    }
  }

  // Erases every entry for which f(key, value) returns true.
  template <typename F>
  void erase_if(F f) {
    for (Bucket& b : buckets_) {
      if (b.state == kFull && f(b.key, Value(b.slot))) EraseAt(b);
    }
  }

  size_t size() const { return count_; }

  void clear() {
    buckets_.clear();
    chunks_.clear();
    used_ = 0;
    free_.clear();
    count_ = tombstones_ = 0;
  }

 private:
  static constexpr uint32_t kEmpty = 0, kFull = 1, kTombstone = 2;
  struct Bucket {
    uint64_t key = 0;
    uint32_t slot = 0;
    uint32_t state = kEmpty;
  };
  static_assert(sizeof(Bucket) == 16, "four buckets per cache line");

  static constexpr uint32_t kChunkShift = 10, kChunkSize = 1u << kChunkShift;

  V& Value(uint32_t slot) { return chunks_[slot >> kChunkShift][slot & (kChunkSize - 1)]; }

  uint32_t NewSlot() {
    if (!free_.empty()) {
      uint32_t slot = free_.back();
      free_.pop_back();
      Value(slot) = V();
      return slot;
    }
    if ((used_ >> kChunkShift) >= chunks_.size()) chunks_.push_back(std::make_unique<V[]>(kChunkSize));
    return used_++;
  }

  void EraseAt(Bucket& b) {
    free_.push_back(b.slot);
    b.state = kTombstone;
    --count_;
    ++tombstones_;
  }

  void Rehash(size_t wanted) {
    size_t capacity = 1024;
    while (capacity < wanted) capacity <<= 1;
    std::vector<Bucket> buckets(capacity);
    const size_t mask = capacity - 1;
    for (const Bucket& b : buckets_) {
      if (b.state != kFull) continue;
      size_t i = size_t(b.key) & mask;
      while (buckets[i].state == kFull) i = (i + 1) & mask;
      buckets[i] = b;
    }
    buckets_.swap(buckets);
    tombstones_ = 0;
  }

  std::vector<Bucket> buckets_;
  std::vector<std::unique_ptr<V[]>> chunks_;  // values; a pointer to one stays valid until clear()
  uint32_t used_ = 0;                          // slots handed out (free_ holds the released ones)
  std::vector<uint32_t> free_;
  size_t count_ = 0, tombstones_ = 0;
};

// §16.166: the values inside the buckets, one 64-byte cache line each. In FlatMap64 a lookup is two dependent
// misses (the bucket, then the value in its chunk): in HashedReuse each held ~38 % of the function (16.165, about
// 7 % of the GPU command thread). Here it is one. Values move only in Maintain(), which the caller runs where no
// pointer into the table is held (the start of a frame); between two Maintain() calls the table never grows, and
// try_emplace refuses a new key (nullptr) once it is 3/4 full.
template <typename V>
class InlineFlatMap64 {
 public:
  V* find(uint64_t key) {
    if (!count_) return nullptr;
    const size_t mask = capacity_ - 1;
    for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
      Bucket& b = buckets_[i];
      if (b.state == kEmpty) return nullptr;
      if (b.state == kFull && b.key == key) return &b.value;
    }
  }

  std::pair<V*, bool> try_emplace(uint64_t key) {
    if (!capacity_) Rehash(kMinCapacity);
    if ((count_ + tombstones_ + 1) * 4 > capacity_ * 3) {
      if (V* found = find(key)) return {found, false};
      ++refused_;
      return {nullptr, false};
    }
    const size_t mask = capacity_ - 1;
    size_t grave = SIZE_MAX;
    for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
      Bucket& b = buckets_[i];
      if (b.state == kFull) {
        if (b.key == key) return {&b.value, false};
        continue;
      }
      if (b.state == kTombstone) {
        if (grave == SIZE_MAX) grave = i;
        continue;
      }
      Bucket& at = buckets_[grave != SIZE_MAX ? grave : i];
      if (grave != SIZE_MAX) --tombstones_;
      at.key = key;
      at.state = kFull;
      at.value = V();
      ++count_;
      return {&at.value, true};
    }
  }

  void erase(uint64_t key) {
    if (!count_) return;
    const size_t mask = capacity_ - 1;
    for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
      Bucket& b = buckets_[i];
      if (b.state == kEmpty) return;
      if (b.state == kFull && b.key == key) {
        b.state = kTombstone;
        --count_;
        ++tombstones_;
        return;
      }
    }
  }

  template <typename F>
  void erase_if(F f) {
    for (size_t i = 0; i < capacity_; ++i) {
      Bucket& b = buckets_[i];
      if (b.state == kFull && f(b.key, b.value)) {
        b.state = kTombstone;
        --count_;
        ++tombstones_;
      }
    }
  }

  // Room for `headroom` more keys at most half full, and the tombstones cleared when they are many. Moves the values.
  void Maintain(size_t headroom) {
    const size_t wanted = (count_ + headroom) * 2;
    if (capacity_ < wanted || tombstones_ * 4 > capacity_) Rehash(wanted);
  }

  size_t size() const { return count_; }
  uint64_t refused() const { return refused_; }

  void clear() {
    buckets_.reset();
    capacity_ = count_ = tombstones_ = 0;
  }

 private:
  static constexpr uint32_t kEmpty = 0, kFull = 1, kTombstone = 2;
  static constexpr size_t kMinCapacity = 1024;
  static_assert(sizeof(V) <= 48, "the value must fit a 64-byte bucket");
  struct alignas(64) Bucket {
    uint64_t key = 0;
    uint32_t state = kEmpty;
    V value;
  };

  void Rehash(size_t wanted) {
    size_t capacity = kMinCapacity;
    while (capacity < wanted) capacity <<= 1;
    std::unique_ptr<Bucket[]> buckets(new Bucket[capacity]);
    const size_t mask = capacity - 1;
    for (size_t j = 0; j < capacity_; ++j) {
      const Bucket& b = buckets_[j];
      if (b.state != kFull) continue;
      size_t i = size_t(b.key) & mask;
      while (buckets[i].state == kFull) i = (i + 1) & mask;
      buckets[i] = b;
    }
    buckets_ = std::move(buckets);
    capacity_ = capacity;
    tombstones_ = 0;
  }

  std::unique_ptr<Bucket[]> buckets_;
  size_t capacity_ = 0, count_ = 0, tombstones_ = 0;
  uint64_t refused_ = 0;
};

// §16.166: FlatMap64 or InlineFlatMap64, chosen once at start (cvar sdnx_hash_inline), for A/B runs.
template <typename V>
class SwitchableFlatMap64 {
 public:
  void set_inline(bool on) { inline_ = on; }
  bool is_inline() const { return inline_; }
  V* find(uint64_t key) { return inline_ ? b_.find(key) : a_.find(key); }
  std::pair<V*, bool> try_emplace(uint64_t key) { return inline_ ? b_.try_emplace(key) : a_.try_emplace(key); }
  void erase(uint64_t key) { inline_ ? b_.erase(key) : a_.erase(key); }
  template <typename F>
  void erase_if(F f) {
    if (inline_) {
      b_.erase_if(f);
    } else {
      a_.erase_if(f);
    }
  }
  void Maintain(size_t headroom) {
    if (inline_) b_.Maintain(headroom);
  }
  size_t size() const { return inline_ ? b_.size() : a_.size(); }
  uint64_t refused() const { return inline_ ? b_.refused() : 0; }
  void clear() {
    a_.clear();
    b_.clear();
  }

 private:
  bool inline_ = false;
  FlatMap64<V> a_;
  InlineFlatMap64<V> b_;
};

}  // namespace rex::graphics::sdnx
