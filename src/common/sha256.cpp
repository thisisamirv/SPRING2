// SHA-256 implementation (public-domain style compact reference).

#include "sha256.h"

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace spring {
namespace {

constexpr uint32_t rotr(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32 - bits));
}

constexpr uint32_t kSha256RoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

} // namespace

Sha256::Sha256() {
  state_[0] = 0x6a09e667U;
  state_[1] = 0xbb67ae85U;
  state_[2] = 0x3c6ef372U;
  state_[3] = 0xa54ff53aU;
  state_[4] = 0x510e527fU;
  state_[5] = 0x9b05688cU;
  state_[6] = 0x1f83d9abU;
  state_[7] = 0x5be0cd19U;
}

void Sha256::transform(const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 =
        rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 =
        rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state_[0];
  uint32_t b = state_[1];
  uint32_t c = state_[2];
  uint32_t d = state_[3];
  uint32_t e = state_[4];
  uint32_t f = state_[5];
  uint32_t g = state_[6];
  uint32_t h = state_[7];

  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + S1 + ch + kSha256RoundConstants[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = S0 + maj;
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

void Sha256::update(const void *data, std::size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  bit_length_ += static_cast<uint64_t>(length) * 8U;
  while (length > 0) {
    const std::size_t space = 64 - buffer_len_;
    const std::size_t take = length < space ? length : space;
    for (std::size_t i = 0; i < take; ++i) {
      buffer_[buffer_len_ + i] = bytes[i];
    }
    buffer_len_ += take;
    bytes += take;
    length -= take;
    if (buffer_len_ == 64) {
      transform(buffer_);
      buffer_len_ = 0;
    }
  }
}

void Sha256::update(std::string_view data) {
  update(data.data(), data.size());
}

sha256_digest Sha256::final() {
  const uint64_t message_bits = bit_length_;
  buffer_[buffer_len_++] = 0x80;
  if (buffer_len_ > 56) {
    while (buffer_len_ < 64) {
      buffer_[buffer_len_++] = 0;
    }
    transform(buffer_);
    buffer_len_ = 0;
  }
  while (buffer_len_ < 56) {
    buffer_[buffer_len_++] = 0;
  }
  for (int i = 7; i >= 0; --i) {
    buffer_[buffer_len_++] =
        static_cast<uint8_t>((message_bits >> (i * 8)) & 0xffU);
  }
  transform(buffer_);
  buffer_len_ = 0;

  sha256_digest digest{};
  for (int i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<uint8_t>((state_[i] >> 24) & 0xffU);
    digest[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xffU);
    digest[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xffU);
    digest[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xffU);
  }
  return digest;
}

sha256_digest sha256_bytes(std::string_view data) {
  Sha256 hasher;
  hasher.update(data);
  return hasher.final();
}

sha256_digest sha256_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Failed to open file for SHA-256: " + path);
  }
  Sha256 hasher;
  char buffer[1 << 16];
  while (input) {
    input.read(buffer, sizeof(buffer));
    const std::streamsize got = input.gcount();
    if (got > 0) {
      hasher.update(buffer, static_cast<std::size_t>(got));
    }
  }
  return hasher.final();
}

std::string sha256_to_hex(const sha256_digest &digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.resize(kSha256DigestBytes * 2);
  for (std::size_t i = 0; i < kSha256DigestBytes; ++i) {
    hex[i * 2] = kHex[(digest[i] >> 4) & 0xf];
    hex[i * 2 + 1] = kHex[digest[i] & 0xf];
  }
  return hex;
}

bool sha256_from_hex(std::string_view hex, sha256_digest &out) {
  if (hex.size() != kSha256DigestBytes * 2) {
    return false;
  }
  for (std::size_t i = 0; i < kSha256DigestBytes; ++i) {
    const int hi = hex_nibble(hex[i * 2]);
    const int lo = hex_nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

} // namespace spring
