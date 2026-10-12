// Secret's storage in SecureHeap (SPG-15b).
#include "spg/secret.hpp"

#include <cstring>
#include <utility>

#include "spg/secure_memory.hpp"

namespace spg {
namespace {

// Wipes every byte the string owns, its unused capacity included.
void wipe_string(std::string& value) noexcept {
  value.resize(value.capacity());  // within capacity: never reallocates
  secure_wipe(value.data(), value.size());
}

}  // namespace

Secret::Secret(std::string value) {
  try {
    assign(value);
  } catch (...) {
    wipe_string(value);
    throw;
  }
  wipe_string(value);
}

Secret Secret::copy_of(std::string_view text) {
  Secret out;
  out.assign(text);
  return out;
}

Secret::Secret(const Secret& other) { assign(other.reveal()); }

Secret::Secret(Secret&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

Secret& Secret::operator=(const Secret& other) {
  if (this != &other) {
    Secret copy{other};
    *this = std::move(copy);
  }
  return *this;
}

Secret& Secret::operator=(Secret&& other) noexcept {
  if (this != &other) {
    release();
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

Secret::~Secret() { release(); }

// Only called on an empty Secret.
void Secret::assign(std::string_view text) {
  if (text.empty()) {
    return;
  }
  data_ = static_cast<char*>(SecureHeap::process().allocate(text.size()));
  std::memcpy(data_, text.data(), text.size());
  size_ = text.size();
}

void Secret::release() noexcept {
  if (data_ != nullptr) {
    SecureHeap::process().deallocate(data_, size_);  // wipes the block
    data_ = nullptr;
    size_ = 0;
  }
}

}  // namespace spg
