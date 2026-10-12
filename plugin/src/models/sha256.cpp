#include "models/sha256.hpp"

#include <bit>
#include <fstream>
#include <vector>

namespace spg::plugin {

namespace {

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

void Sha256::block(const std::uint8_t* data) noexcept {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (std::uint32_t{data[4 * i]} << 24U) | (std::uint32_t{data[4 * i + 1]} << 16U) |
           (std::uint32_t{data[4 * i + 2]} << 8U) | std::uint32_t{data[4 * i + 3]};
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
    const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::array<std::uint32_t, 8> v = state_;
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = std::rotr(v[4], 6) ^ std::rotr(v[4], 11) ^ std::rotr(v[4], 25);
    const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
    const std::uint32_t t1 = v[7] + s1 + ch + kRound[i] + w[i];
    const std::uint32_t s0 = std::rotr(v[0], 2) ^ std::rotr(v[0], 13) ^ std::rotr(v[0], 22);
    const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
    const std::uint32_t t2 = s0 + maj;
    v[7] = v[6];
    v[6] = v[5];
    v[5] = v[4];
    v[4] = v[3] + t1;
    v[3] = v[2];
    v[2] = v[1];
    v[1] = v[0];
    v[0] = t1 + t2;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    state_[i] += v[i];
  }
}

void Sha256::update(std::span<const std::uint8_t> bytes) noexcept {
  length_ += bytes.size();
  std::size_t at = 0;
  if (buffered_ > 0) {
    while (at < bytes.size() && buffered_ < buffer_.size()) {
      buffer_[buffered_++] = bytes[at++];
    }
    if (buffered_ < buffer_.size()) {
      return;
    }
    block(buffer_.data());
    buffered_ = 0;
  }
  for (; bytes.size() - at >= buffer_.size(); at += buffer_.size()) {
    block(bytes.data() + at);
  }
  while (at < bytes.size()) {
    buffer_[buffered_++] = bytes[at++];
  }
}

std::array<std::uint8_t, 32> Sha256::finish() noexcept {
  const std::uint64_t bits = length_ * 8U;
  const std::uint8_t one = 0x80;
  update(std::span{&one, 1});
  const std::uint8_t zero = 0;
  while (buffered_ != 56) {
    update(std::span{&zero, 1});
  }
  std::array<std::uint8_t, 8> tail{};
  for (std::size_t i = 0; i < 8; ++i) {
    tail[i] = static_cast<std::uint8_t>(bits >> (56U - 8U * i));
  }
  update(tail);
  std::array<std::uint8_t, 32> out{};
  for (std::size_t i = 0; i < 8; ++i) {
    for (std::size_t b = 0; b < 4; ++b) {
      out[4 * i + b] = static_cast<std::uint8_t>(state_[i] >> (24U - 8U * b));
    }
  }
  return out;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  constexpr const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t b : bytes) {
    out.push_back(kDigits[b >> 4U]);
    out.push_back(kDigits[b & 0x0FU]);
  }
  return out;
}

std::optional<std::string> sha256_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  Sha256 hash;
  std::vector<char> chunk(1 << 16);
  while (in) {
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      hash.update(std::span{reinterpret_cast<const std::uint8_t*>(chunk.data()), static_cast<std::size_t>(got)});
    }
  }
  if (in.bad()) {
    return std::nullopt;
  }
  return to_hex(hash.finish());
}

}  // namespace spg::plugin
