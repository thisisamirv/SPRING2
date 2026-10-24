// Compact SHA-256 (FIPS 180-4) for archive member integrity digests.

#ifndef SPRING_SHA256_H_
#define SPRING_SHA256_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace spring {

constexpr std::size_t kSha256DigestBytes = 32;
using sha256_digest = std::array<uint8_t, kSha256DigestBytes>;

class Sha256 {
public:
  Sha256();
  void update(const void *data, std::size_t length);
  void update(std::string_view data);
  sha256_digest final();

private:
  void transform(const uint8_t block[64]);
  uint64_t bit_length_ = 0;
  uint32_t state_[8]{};
  uint8_t buffer_[64]{};
  std::size_t buffer_len_ = 0;
};

sha256_digest sha256_bytes(std::string_view data);
sha256_digest sha256_file(const std::string &path);
std::string sha256_to_hex(const sha256_digest &digest);
bool sha256_from_hex(std::string_view hex, sha256_digest &out);

} // namespace spring

#endif // SPRING_SHA256_H_
