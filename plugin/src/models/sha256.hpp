#pragma once

// SHA-256 (FIPS 180-4), so the plugin checks each model file against the
// hash pinned in models/manifest.json before ONNX Runtime opens it. No OBS.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace spg::plugin {

class Sha256 {
 public:
  Sha256() noexcept;
  void update(std::span<const std::uint8_t> bytes) noexcept;
  // The digest of everything passed to update(). Call once.
  [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept;

 private:
  void block(const std::uint8_t* data) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t length_ = 0;  // bytes
};

// Lower-case hex.
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);

// Lower-case hex SHA-256 of a file's bytes; nullopt when it cannot be read.
[[nodiscard]] std::optional<std::string> sha256_file(const std::filesystem::path& path);

}  // namespace spg::plugin
