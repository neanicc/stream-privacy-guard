#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace spg {

// Text that must never be logged, printed, sent anywhere or written to an
// artifact: watch-list entries and OCR output. Secret deliberately has no
// stream operator, no formatter and no implicit conversion, so printing one
// needs an explicit, greppable reveal().
//
// The text lives in SecureHeap::process() (spg/secure_memory.hpp): memory kept
// out of crash dumps where the OS allows it and wiped when the Secret is
// destroyed, assigned or moved from. Every copy is a new protected block.
class Secret {
 public:
  Secret() noexcept = default;
  // Copies `value` into protected memory, then wipes `value`'s buffer, so a
  // temporary std::string (from a settings field or a file) leaves nothing
  // behind on the ordinary heap.
  explicit Secret(std::string value);
  // Copies `text` into protected memory; the caller's buffer is left alone.
  [[nodiscard]] static Secret copy_of(std::string_view text);

  Secret(const Secret& other);
  Secret(Secret&& other) noexcept;
  Secret& operator=(const Secret& other);
  Secret& operator=(Secret&& other) noexcept;
  ~Secret();

  // The only way to read the text. Callers pass it to matching code, never to
  // logging, error messages or files other than the user's own config.
  [[nodiscard]] std::string_view reveal() const noexcept { return {data_, size_}; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  friend bool operator==(const Secret& a, const Secret& b) noexcept { return a.reveal() == b.reveal(); }

 private:
  void assign(std::string_view text);
  void release() noexcept;

  char* data_ = nullptr;  // nullptr when empty
  std::size_t size_ = 0;
};

}  // namespace spg
