#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace spg {

// Text that must never be logged, printed, sent anywhere or written to an
// artifact: watch-list entries and OCR output. Secret deliberately has no
// stream operator, no formatter and no implicit conversion, so printing one
// needs an explicit, greppable reveal().
class Secret {
 public:
  Secret() = default;
  explicit Secret(std::string value) : value_(std::move(value)) {}

  // The only way to read the text. Callers pass it to matching code, never to
  // logging, error messages or files other than the user's own config.
  [[nodiscard]] std::string_view reveal() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return value_.size(); }

  friend bool operator==(const Secret&, const Secret&) = default;

 private:
  std::string value_;
};

}  // namespace spg
