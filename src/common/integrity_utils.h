// Declares CRC and integrity-check helpers used to validate reconstructed
// read, quality, and identifier streams.

#ifndef SPRING_INTEGRITY_UTILS_H_
#define SPRING_INTEGRITY_UTILS_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <zlib.h>

namespace spring {

/**
 * @brief Update a running CRC32 with one record.
 *
 * When length_framed is true (new archives), a little-endian uint32 length is
 * mixed in before the payload so boundary shifts between records (e.g. AA|TT
 * vs A|ATT) cannot collide. When false, only the raw payload is hashed for
 * compatibility with older archives.
 *
 * Payloads larger than zlib's uInt are fed in chunks.
 */
inline void update_record_crc(uint32_t &crc, const std::string &data,
                              const bool length_framed = true) {
  if (length_framed) {
    const uint32_t len = static_cast<uint32_t>(std::min(
        data.size(), static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    const uint8_t len_bytes[4] = {
        static_cast<uint8_t>(len),
        static_cast<uint8_t>(len >> 8),
        static_cast<uint8_t>(len >> 16),
        static_cast<uint8_t>(len >> 24),
    };
    crc = static_cast<uint32_t>(
        crc32(static_cast<uLong>(crc), len_bytes, sizeof(len_bytes)));
  }

  const Bytef *ptr = reinterpret_cast<const Bytef *>(data.data());
  size_t remaining = data.size();
  while (remaining > 0) {
    const uInt chunk = static_cast<uInt>(std::min(
        remaining, static_cast<size_t>(std::numeric_limits<uInt>::max())));
    crc = static_cast<uint32_t>(
        crc32(static_cast<uLong>(crc), ptr, chunk));
    ptr += chunk;
    remaining -= chunk;
  }
}

/**
 * @brief Result of comparing reconstructed digests against archive metadata.
 *
 * When digests were not stored in the archive, mismatch stays false and no
 * per-stream flags are set. A stored CRC of 0 is valid and is compared when
 * digests are present.
 */
struct lossless_digest_check {
  bool mismatch = false;
  int stream_count = 0;
  bool sequence_mismatch[2] = {false, false};
  bool quality_mismatch[2] = {false, false};
  bool id_mismatch[2] = {false, false};
};

/**
 * @brief Compare reconstructed digests to stored archive digests.
 *
 * @param digests_present Whether the archive metadata includes the CRC block.
 * @param paired_end Whether both mate streams should be checked.
 */
inline lossless_digest_check check_lossless_digests(
    const bool digests_present, const bool paired_end,
    const uint32_t stored_seq[2], const uint32_t stored_qual[2],
    const uint32_t stored_id[2], const uint32_t actual_seq[2],
    const uint32_t actual_qual[2], const uint32_t actual_id[2]) {
  lossless_digest_check result;
  if (!digests_present) {
    return result;
  }

  result.stream_count = paired_end ? 2 : 1;
  for (int i = 0; i < result.stream_count; ++i) {
    result.sequence_mismatch[i] = actual_seq[i] != stored_seq[i];
    result.quality_mismatch[i] = actual_qual[i] != stored_qual[i];
    result.id_mismatch[i] = actual_id[i] != stored_id[i];
    if (result.sequence_mismatch[i] || result.quality_mismatch[i] ||
        result.id_mismatch[i]) {
      result.mismatch = true;
    }
  }
  return result;
}

} // namespace spring

#endif // SPRING_INTEGRITY_UTILS_H_
