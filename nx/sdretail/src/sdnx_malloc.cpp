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

#if defined(__SWITCH__) && defined(__aarch64__)

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
};

struct ThreadCache {
  FreeBlock* head[kClasses];
  uint16_t count[kClasses];
};

thread_local ThreadCache t_cache;

std::atomic<bool> g_cache_on{false};

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

}  // extern "C"

#else
extern "C" void SdnxMallocThreadCache(bool) {}
#endif
