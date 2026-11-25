// Sleeping Dogs for Nintendo Switch: a per-thread cache in front of newlib's allocator (SDNX 16.230).
//
// Linked with -Wl,--wrap for newlib's reentrant entry points (_malloc_r, _free_r, _realloc_r, _calloc_r,
// _memalign_r, _malloc_usable_size_r): malloc, free, operator new and libnx's memalign all end there.
//
// Why: newlib serialises every allocation on one recursive process lock. In the console profile of 16.228 the GPU
// command thread - the frame's bottleneck at 63-70 ms - sat in rmutexLock under operator new / memalign in about 31 %
// of its samples, waiting for the lock other threads held. Small blocks now come from a cache of the calling thread
// with no lock; a full or empty cache goes to newlib.
//
// Every block carries a 16-byte header written here, so the release path knows the block's class and where newlib's
// block starts. Nothing of newlib's own is reused: realloc, calloc and memalign are rebuilt on __real__malloc_r and
// __real__free_r (newlib's realloc and memalign call _malloc_r inside libc, which the wrap would redirect here).
//
// sdnx_malloc_thread_cache (sdretail.toml, default true) is read at OnInit; before that, and with false, blocks get the
// header but go straight to newlib.
//
// SDNX 16.245 (sdnx_malloc_central, default true): blocks that one thread allocates and another frees. The thread cache
// alone could not help them: the freeing thread's list filled up (64) and every further free went to newlib, while the
// allocating thread's list stayed empty and every allocation went to newlib - both under newlib's lock. In the stack
// profile of 2026-11-25 (16.243, driving) the guest worker threads had ~10 % and the GPU command thread ~13 % of their
// waits in rmutexLock under _malloc_r / _free_r called from the wait paths, HashedCopy and RexGmProtect. Now a full
// thread list hands a batch of 32 blocks to a per-class central list and an empty one takes a batch from there, under
// a spin lock held for a few pointer moves; newlib is reached only when the central list is empty or full (2 MiB of
// blocks per class).

#if defined(__SWITCH__) && defined(__aarch64__)

#include <switch.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <reent.h>

extern "C" {
void* __real__malloc_r(struct _reent* r, size_t n);
void __real__free_r(struct _reent* r, void* p);
}

namespace {

constexpr uint32_t kMagic = 0x5D7CA11Cu;
constexpr uint32_t kBig = 0xFFu;
constexpr size_t kHeader = 16;

struct Header {
  uint32_t magic;
  uint32_t cls;     // size class, or kBig
  uint32_t offset;  // user pointer - newlib's pointer
  uint32_t size;    // usable bytes after the header
};
static_assert(sizeof(Header) == kHeader);

// Usable sizes of the classes (the header comes on top).
constexpr uint32_t kClassSize[] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096};
constexpr int kClasses = sizeof(kClassSize) / sizeof(kClassSize[0]);
constexpr int kMaxCached = 64;  // blocks per class per thread: at most ~1 MB a thread in all classes

struct FreeBlock {
  FreeBlock* next;
  FreeBlock* next_batch;  // 16.245: in the central list, the first block of a batch links the next batch
};

struct ThreadCache {
  FreeBlock* head[kClasses];
  uint16_t count[kClasses];
};

thread_local ThreadCache t_cache;

std::atomic<bool> g_cache_on{false};

// 16.245: the central lists.
constexpr int kBatch = 32;
constexpr size_t kCentralBytes = 2u << 20;  // per class
std::atomic<bool> g_central_on{false};

struct Central {
  std::atomic<bool> lock{false};
  FreeBlock* batches = nullptr;
  uint32_t count = 0;  // batches
};
Central g_central[kClasses];

inline void CentralLock(Central& c) {
  for (unsigned spins = 0; c.lock.exchange(true, std::memory_order_acquire);) {
    while (c.lock.load(std::memory_order_relaxed)) {
      if (++spins >= 64) {
        spins = 0;
        svcSleepThread(0);  // the holder may have been preempted: let it run
      }
    }
  }
}

inline void CentralUnlock(Central& c) { c.lock.store(false, std::memory_order_release); }

inline uint32_t CentralCap(int cls) {
  const uint32_t batches = uint32_t(kCentralBytes / (size_t(kClassSize[cls]) + kHeader) / kBatch);
  return batches < 4 ? 4 : batches;
}

// The thread list of class c is full: hand its first kBatch blocks to the central list. False if the central list is
// full too (the caller then frees to newlib as before).
bool GiveBatch(ThreadCache& tc, int c) {
  FreeBlock* first = tc.head[c];
  FreeBlock* last = first;
  for (int i = 1; i < kBatch; ++i) last = last->next;
  Central& central = g_central[c];
  CentralLock(central);
  if (central.count >= CentralCap(c)) {
    CentralUnlock(central);
    return false;
  }
  tc.head[c] = last->next;
  tc.count[c] = uint16_t(tc.count[c] - kBatch);
  last->next = nullptr;
  first->next_batch = central.batches;
  central.batches = first;
  ++central.count;
  CentralUnlock(central);
  return true;
}

// The thread list of class c is empty: take a batch from the central list.
FreeBlock* TakeBatch(int c) {
  Central& central = g_central[c];
  if (!central.batches) return nullptr;  // racy peek: an empty list is not worth the lock
  CentralLock(central);
  FreeBlock* batch = central.batches;
  if (batch) {
    central.batches = batch->next_batch;
    --central.count;
  }
  CentralUnlock(central);
  return batch;
}

inline int ClassOf(size_t n) {
  for (int c = 0; c < kClasses; ++c) {
    if (n <= kClassSize[c]) return c;
  }
  return -1;
}

inline Header* HeaderOf(void* user) { return reinterpret_cast<Header*>(static_cast<char*>(user) - kHeader); }

void* AllocFromNewlib(struct _reent* r, size_t n, size_t align, uint32_t cls) {
  // Blocks of newlib are 16-byte aligned: the header keeps the user pointer at 16.
  if (align < 16) align = 16;
  const size_t extra = align > 16 ? align : 0;
  if (n > 0xFFFFFFF0u - kHeader - extra) return nullptr;
  char* raw = static_cast<char*>(__real__malloc_r(r, n + kHeader + extra));
  if (!raw) return nullptr;
  uintptr_t user = reinterpret_cast<uintptr_t>(raw) + kHeader;
  user = (user + align - 1) & ~(uintptr_t(align) - 1);
  Header* h = reinterpret_cast<Header*>(user - kHeader);
  h->magic = kMagic;
  h->cls = cls;
  h->offset = static_cast<uint32_t>(user - reinterpret_cast<uintptr_t>(raw));
  h->size = static_cast<uint32_t>(n);
  return reinterpret_cast<void*>(user);
}

void* Alloc(struct _reent* r, size_t n, size_t align) {
  if (n == 0) n = 1;
  if (align <= 16) {
    const int c = ClassOf(n);
    if (c >= 0) {
      if (g_cache_on.load(std::memory_order_relaxed)) {
        ThreadCache& tc = t_cache;
        if (FreeBlock* b = tc.head[c]) {
          tc.head[c] = b->next;
          --tc.count[c];
          return b;  // the header is still in place from the first allocation
        }
        if (g_central_on.load(std::memory_order_relaxed)) {
          if (FreeBlock* b = TakeBatch(c)) {  // 16.245
            tc.head[c] = b->next;
            tc.count[c] = kBatch - 1;
            return b;
          }
        }
      }
      return AllocFromNewlib(r, kClassSize[c], 16, static_cast<uint32_t>(c));
    }
  }
  return AllocFromNewlib(r, n, align, kBig);
}

void Free(struct _reent* r, void* p) {
  if (!p) return;
  Header* h = HeaderOf(p);
  if (h->magic != kMagic) {  // not a block of this allocator (cannot happen while everything goes through the wrap)
    __real__free_r(r, p);
    return;
  }
  if (h->cls != kBig && g_cache_on.load(std::memory_order_relaxed)) {
    ThreadCache& tc = t_cache;
    const uint32_t c = h->cls;
    if (tc.count[c] >= kMaxCached && g_central_on.load(std::memory_order_relaxed)) {
      GiveBatch(tc, int(c));  // 16.245: room again unless the central list is full as well
    }
    if (tc.count[c] < kMaxCached) {
      FreeBlock* b = static_cast<FreeBlock*>(p);
      b->next = tc.head[c];
      tc.head[c] = b;
      ++tc.count[c];
      return;
    }
  }
  h->magic = 0;
  __real__free_r(r, static_cast<char*>(p) - h->offset);
}

}  // namespace

extern "C" {

void* __wrap__malloc_r(struct _reent* r, size_t n) { return Alloc(r, n, 16); }

void __wrap__free_r(struct _reent* r, void* p) { Free(r, p); }

void* __wrap__memalign_r(struct _reent* r, size_t align, size_t n) {
  if (align & (align - 1)) return nullptr;
  return Alloc(r, n, align);
}

void* __wrap__calloc_r(struct _reent* r, size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return nullptr;
  const size_t n = count * size;
  void* p = Alloc(r, n, 16);
  if (p) std::memset(p, 0, n);
  return p;
}

size_t __wrap__malloc_usable_size_r(struct _reent* r, void* p) {
  (void)r;
  if (!p) return 0;
  Header* h = HeaderOf(p);
  return h->magic == kMagic ? h->size : 0;
}

void* __wrap__realloc_r(struct _reent* r, void* p, size_t n) {
  if (!p) return Alloc(r, n, 16);
  if (n == 0) {
    Free(r, p);
    return nullptr;
  }
  Header* h = HeaderOf(p);
  const size_t old = h->magic == kMagic ? h->size : 0;
  if (n <= old && (h->cls != kBig || n * 2 > old)) return p;  // fits (a big block shrinking by half is copied)
  void* q = Alloc(r, n, 16);
  if (!q) return nullptr;
  std::memcpy(q, p, old < n ? old : n);
  Free(r, p);
  return q;
}

// OnInit: the setting is known and the thread pointers are set up.
void SdnxMallocThreadCache(bool on) { g_cache_on.store(on, std::memory_order_relaxed); }

// 16.245: the central lists (only with the thread cache on).
void SdnxMallocCentral(bool on) { g_central_on.store(on, std::memory_order_relaxed); }

}  // extern "C"

#else
extern "C" void SdnxMallocThreadCache(bool) {}
extern "C" void SdnxMallocCentral(bool) {}
#endif
