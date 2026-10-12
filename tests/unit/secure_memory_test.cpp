// SecureHeap and Secret storage (SPG-15b): every region is excluded from crash
// dumps and locked before a block is handed out (checked through a recording
// platform), every released block is zeroed (checked through the wipe
// observer), and on the real OS the process heap uses what the platform
// offers: on Linux /proc/self/smaps must show the "dd" (do not dump) flag on
// the mapping that holds a Secret's text. All text here is synthetic.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <functional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "spg/policy.hpp"
#include "spg/policy_config.hpp"
#include "spg/secret.hpp"
#include "spg/secure_memory.hpp"

namespace {

constexpr std::string_view kSentinel = "zq-SECURE-SENTINEL-41c7";

struct Call {
  const void* region;
  std::size_t bytes;
  friend bool operator==(const Call&, const Call&) = default;
};

// A platform that records every call and maps ordinary aligned memory.
class RecordingPlatform final : public spg::SecureMemoryPlatform {
 public:
  static constexpr std::size_t kPage = 4096;

  bool exclude_result = true;
  bool lock_result = true;
  bool map_fails = false;
  std::vector<Call> mapped, unmapped, excluded, included, locked, unlocked;
  std::vector<std::string> order;  // "map", "exclude", "lock", "unlock", "include", "unmap"
  // Runs inside map() and unmap(), with the region's size.
  std::function<void(std::size_t)> on_map, on_unmap;

  [[nodiscard]] std::size_t page_size() const noexcept override { return kPage; }
  [[nodiscard]] void* map(std::size_t bytes) noexcept override {
    if (on_map) {
      on_map(bytes);
    }
    if (map_fails) {
      return nullptr;
    }
    void* region = ::operator new(bytes, std::align_val_t{kPage}, std::nothrow);
    if (region != nullptr) {
      std::memset(region, 0, bytes);
      mapped.push_back({region, bytes});
      order.emplace_back("map");
    }
    return region;
  }
  void unmap(void* region, std::size_t bytes) noexcept override {
    if (on_unmap) {
      on_unmap(bytes);
    }
    unmapped.push_back({region, bytes});
    order.emplace_back("unmap");
    ::operator delete(region, std::align_val_t{kPage});
  }
  [[nodiscard]] bool exclude_from_dumps(void* region, std::size_t bytes) noexcept override {
    excluded.push_back({region, bytes});
    order.emplace_back("exclude");
    return exclude_result;
  }
  void include_in_dumps(void* region, std::size_t bytes) noexcept override {
    included.push_back({region, bytes});
    order.emplace_back("include");
  }
  [[nodiscard]] bool lock(void* region, std::size_t bytes) noexcept override {
    locked.push_back({region, bytes});
    order.emplace_back("lock");
    return lock_result;
  }
  void unlock(void* region, std::size_t bytes) noexcept override {
    unlocked.push_back({region, bytes});
    order.emplace_back("unlock");
  }
  [[nodiscard]] const char* dump_exclusion_method() const noexcept override { return "recording"; }
};

bool inside(const void* block, const Call& region) {
  const auto* p = static_cast<const unsigned char*>(block);
  const auto* base = static_cast<const unsigned char*>(region.region);
  return p >= base && p < base + region.bytes;
}

// Records each wiped block: where it was, how long, and whether every byte
// was zero when the observer ran.
struct WipeLog {
  struct Entry {
    const void* block;
    std::size_t bytes;
    bool all_zero;
  };
  std::vector<Entry> entries;

  spg::SecureHeap::WipeObserver observer() {
    return [this](const unsigned char* block, std::size_t bytes) {
      entries.push_back({block, bytes, std::all_of(block, block + bytes, [](unsigned char b) { return b == 0; })});
    };
  }
  // The most recent wipe of `block`.
  [[nodiscard]] const Entry* find(const void* block) const {
    const auto it =
        std::find_if(entries.rbegin(), entries.rend(), [block](const Entry& e) { return e.block == block; });
    return it == entries.rend() ? nullptr : &*it;
  }
};

// Installs a wipe observer on a heap for one scope.
class ObserveWipes {
 public:
  ObserveWipes(spg::SecureHeap& heap, WipeLog& log) : heap_(heap) { heap_.set_wipe_observer(log.observer()); }
  ~ObserveWipes() { heap_.set_wipe_observer(nullptr); }
  ObserveWipes(const ObserveWipes&) = delete;
  ObserveWipes& operator=(const ObserveWipes&) = delete;

 private:
  spg::SecureHeap& heap_;
};

}  // namespace

TEST_CASE("secure_wipe zeroes every byte", "[secure]") {
  std::vector<unsigned char> bytes(37, 0xA5);
  spg::secure_wipe(bytes.data(), bytes.size());
  CHECK(std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0; }));
  spg::secure_wipe(nullptr, 0);  // allowed
}

TEST_CASE("a region is excluded from dumps and locked before any block is handed out", "[secure]") {
  RecordingPlatform platform;
  spg::SecureHeap heap{platform};
  void* block = heap.allocate(kSentinel.size());

  REQUIRE(platform.mapped.size() == 1);
  const Call region = platform.mapped.front();
  CHECK(region.bytes == spg::SecureHeap::kRegionBytes);
  CHECK(reinterpret_cast<std::uintptr_t>(region.region) % RecordingPlatform::kPage == 0);
  CHECK(platform.excluded == std::vector<Call>{region});
  CHECK(platform.locked == std::vector<Call>{region});
  CHECK(platform.order == std::vector<std::string>{"map", "exclude", "lock"});
  CHECK(inside(block, region));
  CHECK(reinterpret_cast<std::uintptr_t>(block) % spg::SecureHeap::kAlignment == 0);

  const spg::SecureHeapStats stats = heap.stats();
  CHECK(stats.regions == 1);
  CHECK(stats.dump_excluded == 1);
  CHECK(stats.locked == 1);
  CHECK(stats.live_blocks == 1);
  heap.deallocate(block, kSentinel.size());
  CHECK(heap.stats().live_blocks == 0);
}

TEST_CASE("small blocks share protected regions and are wiped when released", "[secure]") {
  RecordingPlatform platform;
  spg::SecureHeap heap{platform};
  WipeLog wipes;
  const ObserveWipes observing{heap, wipes};

  std::vector<void*> blocks;
  for (int i = 0; i < 200; ++i) {
    blocks.push_back(heap.allocate(24));
  }
  REQUIRE(platform.mapped.size() == 1);  // 200 blocks of 32 bytes fit in one region
  for (void* block : blocks) {
    CHECK(inside(block, platform.mapped.front()));
  }

  void* block = blocks.front();
  std::memcpy(block, kSentinel.data(), 24);
  heap.deallocate(block, 24);
  const WipeLog::Entry* wiped = wipes.find(block);
  REQUIRE(wiped != nullptr);
  CHECK(wiped->bytes >= 24);
  CHECK(wiped->all_zero);

  // The freed block is reused, and it comes back zeroed.
  void* again = heap.allocate(20);
  CHECK(again == block);
  const auto* bytes = static_cast<const unsigned char*>(again);
  CHECK(std::all_of(bytes, bytes + 20, [](unsigned char b) { return b == 0; }));
  heap.deallocate(again, 20);
  for (std::size_t i = 1; i < blocks.size(); ++i) {
    heap.deallocate(blocks[i], 24);
  }
  CHECK(heap.stats().live_blocks == 0);
  CHECK(platform.unmapped.empty());  // shared regions stay for reuse
}

TEST_CASE("a large block gets its own region, wiped and released when freed", "[secure]") {
  RecordingPlatform platform;
  spg::SecureHeap heap{platform};
  WipeLog wipes;
  const ObserveWipes observing{heap, wipes};

  const std::size_t size = spg::SecureHeap::kMaxSmallBlock + 1;
  auto* block = static_cast<unsigned char*>(heap.allocate(size));
  REQUIRE(platform.mapped.size() == 1);
  const Call region = platform.mapped.front();
  CHECK(region.region == block);
  CHECK(region.bytes == (size / RecordingPlatform::kPage + 1) * RecordingPlatform::kPage);
  CHECK(platform.excluded == std::vector<Call>{region});

  std::fill(block, block + size, static_cast<unsigned char>('q'));
  platform.order.clear();
  heap.deallocate(block, size);
  const WipeLog::Entry* wiped = wipes.find(block);
  REQUIRE(wiped != nullptr);
  CHECK(wiped->bytes == region.bytes);
  CHECK(wiped->all_zero);
  CHECK(platform.order == std::vector<std::string>{"unlock", "include", "unmap"});
  CHECK(platform.unmapped == std::vector<Call>{region});
  CHECK(platform.included == std::vector<Call>{region});
  CHECK(heap.stats().regions == 0);

  // A pointer the heap never handed out is ignored and leaves the count alone.
  unsigned char foreign[16] = {};
  heap.deallocate(foreign, size);
  CHECK(heap.stats().live_blocks == 0);
  CHECK(platform.unmapped.size() == 1);
}

TEST_CASE("a folded line of 4096 code points reuses a shared block", "[secure]") {
  // The policy engine folds each recognized line into a SecureU32String; a
  // 4096-character line is 16 KiB. Repeated calls must not map anything new.
  RecordingPlatform platform;
  spg::SecureHeap heap{platform};
  constexpr std::size_t kLine = 4096 * sizeof(char32_t);
  static_assert(kLine == spg::SecureHeap::kMaxSmallBlock);
  void* first = heap.allocate(kLine);
  REQUIRE(platform.mapped.size() == 1);
  CHECK(platform.mapped.front().bytes == spg::SecureHeap::kRegionBytes);
  CHECK(inside(first, platform.mapped.front()));
  heap.deallocate(first, kLine);
  for (int i = 0; i < 100; ++i) {
    void* again = heap.allocate(kLine);
    CHECK(again == first);
    heap.deallocate(again, kLine);
  }
  CHECK(platform.mapped.size() == 1);
  CHECK(platform.unmapped.empty());
  CHECK(heap.stats().live_blocks == 0);
}

TEST_CASE("growing or shrinking the heap runs its system calls outside the lock", "[secure]") {
  // While the heap maps a large or a new shared region, or unmaps a large
  // one, another thread's small allocation must still go through (it has a
  // free block, so it needs no system call of its own).
  RecordingPlatform platform;
  spg::SecureHeap heap{platform};
  heap.deallocate(heap.allocate(8), 8);

  std::thread other;
  std::atomic<bool> other_done{false};
  bool armed = false;
  bool ran_during_call = false;
  const auto small_allocation_meanwhile = [&](std::size_t /*bytes*/) {
    if (!armed) {
      return;
    }
    armed = false;
    other_done = false;
    other = std::thread([&heap, &other_done] {
      heap.deallocate(heap.allocate(8), 8);
      other_done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!other_done && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    ran_during_call = other_done;
  };
  constexpr std::size_t kLarge = 2 * spg::SecureHeap::kRegionBytes;

  // Joins only after the heap call returned, so a held lock cannot hang the test.
  const auto finish = [&] {
    if (other.joinable()) {
      other.join();
    }
    const bool ran = ran_during_call;
    ran_during_call = false;
    return ran;
  };
  platform.on_map = small_allocation_meanwhile;
  platform.on_unmap = small_allocation_meanwhile;

  armed = true;
  void* block = heap.allocate(kLarge);
  CHECK(finish());  // large region mapped

  armed = true;
  heap.deallocate(block, kLarge);
  CHECK(finish());  // large region unmapped

  armed = true;
  void* mid = heap.allocate(1000);  // the 1 KiB class has no region yet
  CHECK(finish());  // shared region mapped
  CHECK(platform.mapped.size() == 3);
  heap.deallocate(mid, 1000);

  platform.on_map = nullptr;
  platform.on_unmap = nullptr;
  CHECK(heap.stats().live_blocks == 0);
}

TEST_CASE("the heap keeps working when the platform refuses exclusion or locking", "[secure]") {
  RecordingPlatform platform;
  platform.exclude_result = false;
  platform.lock_result = false;
  {
    spg::SecureHeap heap{platform};
    void* block = heap.allocate(8);
    CHECK(block != nullptr);
    const spg::SecureHeapStats stats = heap.stats();
    CHECK(stats.regions == 1);
    CHECK(stats.dump_excluded == 0);
    CHECK(stats.locked == 0);
    heap.deallocate(block, 8);
  }
  // Nothing was registered or locked, so nothing is undone.
  CHECK(platform.included.empty());
  CHECK(platform.unlocked.empty());
  CHECK(platform.unmapped.size() == 1);

  RecordingPlatform empty;
  empty.map_fails = true;
  spg::SecureHeap heap{empty};
  CHECK_THROWS_AS(heap.allocate(8), std::bad_alloc);
  CHECK_THROWS_AS(heap.allocate(spg::SecureHeap::kMaxSmallBlock * 4), std::bad_alloc);
}

TEST_CASE("destroying a heap wipes and releases every region", "[secure]") {
  RecordingPlatform platform;
  {
    spg::SecureHeap heap{platform};
    static_cast<void>(heap.allocate(8));
    static_cast<void>(heap.allocate(spg::SecureHeap::kMaxSmallBlock * 2));
  }
  CHECK(platform.unmapped.size() == platform.mapped.size());
  CHECK(platform.included.size() == platform.excluded.size());
  CHECK(platform.unlocked.size() == platform.locked.size());
}

TEST_CASE("a Secret's text is in the process heap and is wiped when released", "[secure][secret]") {
  spg::SecureHeap& heap = spg::SecureHeap::process();
  WipeLog wipes;
  const ObserveWipes observing{heap, wipes};
  const std::size_t live_before = heap.stats().live_blocks;

  const void* destroyed = nullptr;
  {
    const spg::Secret secret = spg::Secret::copy_of(kSentinel);
    CHECK(secret.reveal() == kSentinel);
    CHECK(heap.stats().live_blocks == live_before + 1);
    destroyed = secret.reveal().data();
  }
  const WipeLog::Entry* wiped = wipes.find(destroyed);
  REQUIRE(wiped != nullptr);
  CHECK(wiped->all_zero);
  CHECK(heap.stats().live_blocks == live_before);

  // Assignment releases (and wipes) the old text; a copy is a block of its own.
  spg::Secret a{std::string(kSentinel)};
  const void* old_block = a.reveal().data();
  spg::Secret b = a;
  CHECK(b == a);
  CHECK(b.reveal().data() != a.reveal().data());
  const std::size_t wipes_before = wipes.entries.size();
  a = spg::Secret::copy_of("other synthetic text");
  REQUIRE(wipes.entries.size() > wipes_before);
  REQUIRE(wipes.find(old_block) == &wipes.entries.back());
  CHECK(wipes.entries.back().all_zero);

  // A move hands the block over without copying it.
  const void* moved_block = b.reveal().data();
  spg::Secret c = std::move(b);
  CHECK(c.reveal().data() == moved_block);
  CHECK(b.empty());  // NOLINT(bugprone-use-after-move): moved-from is empty by contract

  // An empty Secret holds no block.
  const std::size_t live = heap.stats().live_blocks;
  const spg::Secret none;
  const spg::Secret also_none = spg::Secret::copy_of("");
  CHECK(none.empty());
  CHECK(also_none.empty());
  CHECK(heap.stats().live_blocks == live);
}

TEST_CASE("the policy engine keeps its compiled entries in the process heap", "[secure][policy]") {
  spg::PolicyConfig config;
  config.watchlist = {spg::Secret::copy_of("Qelvaran Thistlewood"), spg::Secret::copy_of("Morvane Quillet")};
  const std::size_t before = spg::SecureHeap::process().stats().live_blocks;
  {
    const spg::PolicyEngine engine{config};
    // Two normalized entries and the vector that holds them.
    CHECK(spg::SecureHeap::process().stats().live_blocks >= before + 3);
  }
  CHECK(spg::SecureHeap::process().stats().live_blocks == before);
}

TEST_CASE("the process heap uses the OS's crash-dump exclusion where there is one", "[secure][platform]") {
  const spg::Secret secret = spg::Secret::copy_of(kSentinel);
  const spg::SecureHeap& heap = spg::SecureHeap::process();
  const spg::SecureHeapStats stats = heap.stats();
  REQUIRE(stats.regions >= 1);
  const std::string_view method = heap.platform().dump_exclusion_method();
#if defined(_WIN32)
  CHECK(method == "WerRegisterExcludedMemoryBlock");
  CHECK(stats.dump_excluded == stats.regions);
#elif defined(__linux__)
  CHECK(method == "madvise(MADV_DONTDUMP)");
  CHECK(stats.dump_excluded == stats.regions);
  // The kernel's view: the mapping holding the text carries VmFlags "dd".
  const auto address = reinterpret_cast<std::uintptr_t>(secret.reveal().data());
  std::ifstream smaps("/proc/self/smaps");
  std::string line;
  bool in_mapping = false;
  bool found = false;
  bool dontdump = false;
  while (std::getline(smaps, line)) {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    char dash = 0;
    std::istringstream header(line);
    if (header >> std::hex >> start >> dash >> end && dash == '-') {
      in_mapping = address >= start && address < end;
      found = found || in_mapping;
    } else if (in_mapping && line.rfind("VmFlags:", 0) == 0) {
      dontdump = (line + " ").find(" dd ") != std::string::npos;
      break;
    }
  }
  CHECK(found);
  CHECK(dontdump);
#else
  // macOS has no per-region exclusion; docs/settings.md records the limit.
  CHECK(method == "none");
  CHECK(stats.dump_excluded == 0);
#endif
}
