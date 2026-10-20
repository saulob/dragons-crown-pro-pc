#include "updater/sha256.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <vector>

namespace trg::update {
namespace {

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t RotateRight(std::uint32_t value, int count) {
  return (value >> count) | (value << (32 - count));
}

std::uint32_t LoadBigEndian(const std::uint8_t* data) {
  return (std::uint32_t(data[0]) << 24) | (std::uint32_t(data[1]) << 16) | (std::uint32_t(data[2]) << 8) |
         std::uint32_t(data[3]);
}

void StoreBigEndian(std::uint8_t* data, std::uint32_t value) {
  data[0] = std::uint8_t(value >> 24);
  data[1] = std::uint8_t(value >> 16);
  data[2] = std::uint8_t(value >> 8);
  data[3] = std::uint8_t(value);
}

char HexDigit(unsigned value) {
  return char(value < 10 ? '0' + value : 'a' + (value - 10));
}

}  // namespace

Sha256::Sha256() : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::Transform(const std::uint8_t* block) {
  std::array<std::uint32_t, 64> schedule{};
  for (size_t i = 0; i < 16; ++i) {
    schedule[i] = LoadBigEndian(block + i * 4);
  }
  for (size_t i = 16; i < schedule.size(); ++i) {
    const std::uint32_t s0 = RotateRight(schedule[i - 15], 7) ^ RotateRight(schedule[i - 15], 18) ^
                             (schedule[i - 15] >> 3);
    const std::uint32_t s1 = RotateRight(schedule[i - 2], 17) ^ RotateRight(schedule[i - 2], 19) ^
                             (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (size_t i = 0; i < schedule.size(); ++i) {
    const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + choose + kRound[i] + schedule[i];
    const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::Update(const void* data, size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_ += size;
  while (size > 0) {
    const size_t room = buffer_.size() - buffered_;
    const size_t take = std::min(room, size);
    std::copy_n(bytes, take, buffer_.begin() + buffered_);
    buffered_ += take;
    bytes += take;
    size -= take;
    if (buffered_ == buffer_.size()) {
      Transform(buffer_.data());
      buffered_ = 0;
    }
  }
}

std::array<std::uint8_t, 32> Sha256::Final() {
  const std::uint64_t bits = total_ * 8;
  const std::uint8_t marker = 0x80;
  Update(&marker, 1);
  const std::uint8_t zero = 0;
  while (buffered_ != 56) {
    Update(&zero, 1);
  }
  std::array<std::uint8_t, 8> length{};
  for (int i = 0; i < 8; ++i) {
    length[size_t(7 - i)] = std::uint8_t(bits >> (i * 8));
  }
  Update(length.data(), length.size());

  std::array<std::uint8_t, 32> digest{};
  for (size_t i = 0; i < state_.size(); ++i) {
    StoreBigEndian(digest.data() + i * 4, state_[i]);
  }
  return digest;
}

std::string Sha256Hex(std::string_view data) {
  Sha256 hash;
  hash.Update(data.data(), data.size());
  const std::array<std::uint8_t, 32> digest = hash.Final();
  std::string text;
  text.reserve(digest.size() * 2);
  for (const std::uint8_t byte : digest) {
    text += HexDigit(byte >> 4);
    text += HexDigit(byte & 0x0F);
  }
  return text;
}

std::string Sha256FileHex(const std::filesystem::path& path, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Cannot read " + path.string() + ".";
    return {};
  }
  Sha256 hash;
  std::vector<char> buffer(1 << 20);
  while (file) {
    file.read(buffer.data(), std::streamsize(buffer.size()));
    const std::streamsize count = file.gcount();
    if (count > 0) {
      hash.Update(buffer.data(), size_t(count));
    }
  }
  if (file.bad()) {
    error = "Cannot read " + path.string() + ".";
    return {};
  }
  const std::array<std::uint8_t, 32> digest = hash.Final();
  std::string text;
  text.reserve(digest.size() * 2);
  for (const std::uint8_t byte : digest) {
    text += HexDigit(byte >> 4);
    text += HexDigit(byte & 0x0F);
  }
  return text;
}

bool IsValidSha256(std::string_view text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
         });
}

std::string NormalizeSha256(std::string_view text) {
  std::string normalized(text);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](char c) {
    return c >= 'A' && c <= 'F' ? char(c - 'A' + 'a') : c;
  });
  return normalized;
}

}  // namespace trg::update
