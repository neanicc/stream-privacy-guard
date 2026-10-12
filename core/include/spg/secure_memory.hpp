#pragma once

// Memory for watch-list text (SPG-15b). AGENTS.md: watch-list content is never
// written to crash dumps. SecureHeap hands out blocks from page-aligned regions
// that are excluded from crash dumps where the operating system allows it
// (madvise(MADV_DONTDUMP) on Linux, WerRegisterExcludedMemoryBlock on Windows;
// macOS has no such call), locked in RAM where that is cheap (mlock,
// VirtualLock; best effort), and zeroed with a wipe the compiler cannot remove
// whenever a block is released. Secret, the policy engine's compiled entries and
// the plugin's watch-list store use it. Limits are in docs/settings.md.

#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace spg {

// Sets `size` bytes at `data` to zero in a way the compiler may not drop as a
// dead store (SecureZeroMemory on Windows, memset plus a compiler barrier
// elsewhere).
void secure_wipe(void* data, std::size_t size) noexcept;

// What the operating system offers for one memory region. The process heap
// uses native_secure_platform(); tests pass a recorder.
class SecureMemoryPlatform {
 public:
  SecureMemoryPlatform() = default;
  SecureMemoryPlatform(const SecureMemoryPlatform&) = delete;
  SecureMemoryPlatform& operator=(const SecureMemoryPlatform&) = delete;
  virtual ~SecureMemoryPlatform() = default;

  // Every region is a multiple of this and aligned to it.
  [[nodiscard]] virtual std::size_t page_size() const noexcept = 0;
  // A new zeroed, read-write region of `bytes`, or nullptr.
  [[nodiscard]] virtual void* map(std::size_t bytes) noexcept = 0;
  virtual void unmap(void* region, std::size_t bytes) noexcept = 0;
  // Keeps the region out of crash dumps. False when the platform has no way
  // to do that or the call failed.
  [[nodiscard]] virtual bool exclude_from_dumps(void* region, std::size_t bytes) noexcept = 0;
  // Undoes exclude_from_dumps before the region is unmapped.
  virtual void include_in_dumps(void* region, std::size_t bytes) noexcept = 0;
  // Keeps the region out of swap. Best effort: limits such as RLIMIT_MEMLOCK
  // or the Windows working-set quota can refuse it.
  [[nodiscard]] virtual bool lock(void* region, std::size_t bytes) noexcept = 0;
  virtual void unlock(void* region, std::size_t bytes) noexcept = 0;
  // The dump-exclusion mechanism for logs, or "none".
  [[nodiscard]] virtual const char* dump_exclusion_method() const noexcept = 0;
};

// The platform of the running OS. Never destroyed.
[[nodiscard]] SecureMemoryPlatform& native_secure_platform() noexcept;

struct SecureHeapStats {
  std::size_t regions = 0;           // regions currently mapped
  std::size_t dump_excluded = 0;     // of those, excluded from crash dumps
  std::size_t locked = 0;            // of those, locked in RAM
  std::size_t live_blocks = 0;       // blocks handed out and not yet released
};

// A small thread-safe allocator over protected regions. Requests up to
// kMaxSmallBlock bytes come from fixed size classes (16 bytes to 16 KiB) carved
// out of shared regions of kRegionBytes, so the policy engine's per-call
// temporaries (a folded line of up to 4096 code points) reuse blocks without a
// system call. Larger requests get a region of their own, unmapped when
// released; the map, exclude, lock and their undo calls for such a region run
// outside the heap's mutex, so they never stall another thread's small
// allocation. Shared regions are kept for reuse until the heap is destroyed;
// their free blocks are zero.
class SecureHeap {
 public:
  static constexpr std::size_t kAlignment = 16;
  static constexpr std::size_t kMaxSmallBlock = std::size_t{16} * 1024;
  static constexpr std::size_t kRegionBytes = std::size_t{64} * 1024;

  explicit SecureHeap(SecureMemoryPlatform& platform);
  // Wipes and unmaps every region. The process heap is never destroyed.
  ~SecureHeap();
  SecureHeap(const SecureHeap&) = delete;
  SecureHeap& operator=(const SecureHeap&) = delete;

  // At least `bytes` (at least 1), aligned to kAlignment. Throws
  // std::bad_alloc when the platform maps nothing.
  [[nodiscard]] void* allocate(std::size_t bytes);
  // Wipes the whole block, then makes it reusable (or unmaps a large one).
  // `bytes` is the size passed to allocate().
  void deallocate(void* block, std::size_t bytes) noexcept;

  [[nodiscard]] SecureHeapStats stats() const;
  [[nodiscard]] const SecureMemoryPlatform& platform() const noexcept { return platform_; }

  // Test hook: called with each block right after it is wiped, before it is
  // reused or unmapped. Must not allocate from this heap.
  using WipeObserver = std::function<void(const unsigned char* block, std::size_t bytes)>;
  void set_wipe_observer(WipeObserver observer);

  // The heap Secret and SecureAllocator use, on the native platform. Never
  // destroyed, so a Secret in static storage can still release its block.
  [[nodiscard]] static SecureHeap& process();

 private:
  static constexpr std::size_t kClasses = 11;  // 16 << 0 .. 16 << 10

  struct Region {
    unsigned char* base = nullptr;
    std::size_t bytes = 0;
    bool excluded = false;
    bool locked = false;
  };

  // Maps, excludes and locks a region. Touches no member but platform_, so it
  // runs with or without mutex_ held. Throws std::bad_alloc.
  [[nodiscard]] Region new_region(std::size_t bytes);
  // Unlocks, includes and unmaps a region; with or without mutex_ held.
  void release_region(const Region& region) noexcept;
  [[nodiscard]] void* allocate_large(std::size_t bytes);
  void wiped(const unsigned char* block, std::size_t bytes) const noexcept;

  SecureMemoryPlatform& platform_;
  mutable std::mutex mutex_;
  std::array<void*, kClasses> free_lists_{};
  std::vector<Region> regions_;
  WipeObserver observer_;
  std::size_t live_blocks_ = 0;
};

// A standard allocator over SecureHeap::process(). Containers that use it keep
// their elements in protected memory; a std::basic_string still keeps a few
// characters inside the string object itself (its short-string buffer), which
// lives wherever the object does.
template <class T>
struct SecureAllocator {
  using value_type = T;
  static_assert(alignof(T) <= SecureHeap::kAlignment, "SecureHeap aligns to 16 bytes");

  SecureAllocator() noexcept = default;
  template <class U>
  SecureAllocator(const SecureAllocator<U>& /*other*/) noexcept {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] T* allocate(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length();
    }
    return static_cast<T*>(SecureHeap::process().allocate(n * sizeof(T)));
  }
  void deallocate(T* p, std::size_t n) noexcept { SecureHeap::process().deallocate(p, n * sizeof(T)); }

  // Every SecureAllocator draws from the same heap.
  friend bool operator==(const SecureAllocator& /*a*/, const SecureAllocator& /*b*/) noexcept { return true; }
};

using SecureString = std::basic_string<char, std::char_traits<char>, SecureAllocator<char>>;
using SecureU32String = std::basic_string<char32_t, std::char_traits<char32_t>, SecureAllocator<char32_t>>;

}  // namespace spg
