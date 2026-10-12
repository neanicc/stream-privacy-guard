// SecureHeap and the native platform calls behind it (SPG-15b). What each OS
// does and does not cover is in docs/settings.md, "Watch-list text in memory".
#include "spg/secure_memory.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h first: werapi.h needs its types.
#include <werapi.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace spg {

void secure_wipe(void* data, std::size_t size) noexcept {
  if (data == nullptr || size == 0) {
    return;
  }
#if defined(_WIN32)
  SecureZeroMemory(data, size);
#else
  std::memset(data, 0, size);
  // The compiler must assume the asm reads the zeroed bytes, so the memset is
  // not a dead store (the same barrier BoringSSL's OPENSSL_cleanse uses).
  __asm__ __volatile__("" : : "r"(data) : "memory");
#endif
}

namespace {

class NativePlatform final : public SecureMemoryPlatform {
 public:
  [[nodiscard]] std::size_t page_size() const noexcept override {
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    const long size = ::sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<std::size_t>(size) : std::size_t{4096};
#endif
  }

  [[nodiscard]] void* map(std::size_t bytes) noexcept override {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* region = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return region == MAP_FAILED ? nullptr : region;
#endif
  }

  void unmap(void* region, std::size_t bytes) noexcept override {
#if defined(_WIN32)
    static_cast<void>(bytes);
    VirtualFree(region, 0, MEM_RELEASE);
#else
    ::munmap(region, bytes);
#endif
  }

  [[nodiscard]] bool exclude_from_dumps(void* region, std::size_t bytes) noexcept override {
#if defined(_WIN32)
    // Windows Error Reporting leaves registered blocks out of the dumps it
    // writes (at most WER_MAX_REGISTERED_ENTRIES blocks per process).
    if (bytes > MAXDWORD) {
      return false;
    }
    return WerRegisterExcludedMemoryBlock(region, static_cast<DWORD>(bytes)) == S_OK;
#elif defined(MADV_DONTDUMP)
    // Linux: the kernel's core dump (and so systemd-coredump) skips the region.
    return ::madvise(region, bytes, MADV_DONTDUMP) == 0;
#else
    // macOS has no per-region crash-dump exclusion.
    static_cast<void>(region);
    static_cast<void>(bytes);
    return false;
#endif
  }

  void include_in_dumps(void* region, std::size_t bytes) noexcept override {
#if defined(_WIN32)
    static_cast<void>(bytes);
    WerUnregisterExcludedMemoryBlock(region);
#else
    // munmap drops the madvise flag with the mapping.
    static_cast<void>(region);
    static_cast<void>(bytes);
#endif
  }

  [[nodiscard]] bool lock(void* region, std::size_t bytes) noexcept override {
#if defined(_WIN32)
    return VirtualLock(region, bytes) != 0;
#else
    return ::mlock(region, bytes) == 0;
#endif
  }

  void unlock(void* region, std::size_t bytes) noexcept override {
#if defined(_WIN32)
    VirtualUnlock(region, bytes);
#else
    ::munlock(region, bytes);
#endif
  }

  [[nodiscard]] const char* dump_exclusion_method() const noexcept override {
#if defined(_WIN32)
    return "WerRegisterExcludedMemoryBlock";
#elif defined(MADV_DONTDUMP)
    return "madvise(MADV_DONTDUMP)";
#else
    return "none";
#endif
  }
};

constexpr std::size_t kMinBlock = 16;

// The size class of a small request: 0 for up to 16 bytes, 1 for 17 to 32, ...
std::size_t size_class(std::size_t bytes) noexcept {
  std::size_t index = 0;
  for (std::size_t block = kMinBlock; block < bytes; block <<= 1U) {
    ++index;
  }
  return index;
}

std::size_t class_bytes(std::size_t index) noexcept { return kMinBlock << index; }

std::size_t round_up(std::size_t bytes, std::size_t unit) noexcept { return (bytes + unit - 1) / unit * unit; }

}  // namespace

SecureMemoryPlatform& native_secure_platform() noexcept {
  static auto* const platform = new NativePlatform();  // never destroyed: see SecureHeap::process()
  return *platform;
}

SecureHeap::SecureHeap(SecureMemoryPlatform& platform) : platform_(platform) {
  static_assert((kMinBlock << (kClasses - 1)) == kMaxSmallBlock, "size classes end at kMaxSmallBlock");
  static_assert(kMinBlock >= kAlignment && kMinBlock >= sizeof(void*), "a free block holds a link");
}

SecureHeap::~SecureHeap() {
  const std::lock_guard lock{mutex_};
  for (const Region& region : regions_) {
    secure_wipe(region.base, region.bytes);
    release_region(region);
  }
}

SecureHeap& SecureHeap::process() {
  static auto* const heap = new SecureHeap(native_secure_platform());
  return *heap;
}

SecureHeap::Region SecureHeap::map_region(std::size_t bytes) {
  regions_.reserve(regions_.size() + 1);  // may throw; nothing is mapped yet
  Region region;
  region.bytes = bytes;
  region.base = static_cast<unsigned char*>(platform_.map(bytes));
  if (region.base == nullptr) {
    throw std::bad_alloc();
  }
  region.excluded = platform_.exclude_from_dumps(region.base, bytes);
  region.locked = platform_.lock(region.base, bytes);
  regions_.push_back(region);
  return region;
}

void SecureHeap::release_region(const Region& region) noexcept {
  if (region.locked) {
    platform_.unlock(region.base, region.bytes);
  }
  if (region.excluded) {
    platform_.include_in_dumps(region.base, region.bytes);
  }
  platform_.unmap(region.base, region.bytes);
}

void* SecureHeap::allocate(std::size_t bytes) {
  bytes = std::max<std::size_t>(bytes, 1);
  const std::lock_guard lock{mutex_};
  const std::size_t page = std::max<std::size_t>(platform_.page_size(), kAlignment);
  if (bytes > kMaxSmallBlock) {
    if (bytes > std::numeric_limits<std::size_t>::max() - page) {
      throw std::bad_alloc();
    }
    const Region region = map_region(round_up(bytes, page));
    ++live_blocks_;
    return region.base;
  }
  const std::size_t index = size_class(bytes);
  if (free_lists_[index] == nullptr) {
    const std::size_t block = class_bytes(index);
    const Region region = map_region(round_up(kRegionBytes, page));
    // Thread the new blocks onto the free list, lowest address first.
    void* next = nullptr;
    for (std::size_t i = region.bytes / block; i-- > 0;) {
      unsigned char* const slot = region.base + i * block;
      std::memcpy(slot, &next, sizeof next);
      next = slot;
    }
    free_lists_[index] = next;
  }
  auto* const block = static_cast<unsigned char*>(free_lists_[index]);
  std::memcpy(&free_lists_[index], block, sizeof(void*));
  std::memset(block, 0, sizeof(void*));
  ++live_blocks_;
  return block;
}

void SecureHeap::deallocate(void* block, std::size_t bytes) noexcept {
  if (block == nullptr) {
    return;
  }
  bytes = std::max<std::size_t>(bytes, 1);
  const std::lock_guard lock{mutex_};
  --live_blocks_;
  if (bytes > kMaxSmallBlock) {
    const auto found = std::find_if(regions_.begin(), regions_.end(),
                                    [block](const Region& region) { return region.base == block; });
    if (found == regions_.end()) {
      return;  // not ours; never happens through SecureAllocator or Secret
    }
    const Region region = *found;
    regions_.erase(found);
    secure_wipe(region.base, region.bytes);
    wiped(region.base, region.bytes);
    release_region(region);
    return;
  }
  const std::size_t index = size_class(bytes);
  auto* const bytes_ptr = static_cast<unsigned char*>(block);
  secure_wipe(bytes_ptr, class_bytes(index));
  wiped(bytes_ptr, class_bytes(index));
  std::memcpy(bytes_ptr, &free_lists_[index], sizeof(void*));
  free_lists_[index] = block;
}

void SecureHeap::wiped(const unsigned char* block, std::size_t bytes) const noexcept {
  if (observer_) {
    try {
      observer_(block, bytes);
    } catch (...) {
      // A test hook must not break deallocation.
    }
  }
}

SecureHeapStats SecureHeap::stats() const {
  const std::lock_guard lock{mutex_};
  SecureHeapStats out;
  out.regions = regions_.size();
  out.live_blocks = live_blocks_;
  for (const Region& region : regions_) {
    out.dump_excluded += region.excluded ? 1U : 0U;
    out.locked += region.locked ? 1U : 0U;
  }
  return out;
}

void SecureHeap::set_wipe_observer(WipeObserver observer) {
  const std::lock_guard lock{mutex_};
  observer_ = std::move(observer);
}

}  // namespace spg
