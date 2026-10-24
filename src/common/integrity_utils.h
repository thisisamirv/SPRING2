// Declares CRC and integrity-check helpers used to validate reconstructed
// read, quality, and identifier streams.

#ifndef SPRING_INTEGRITY_UTILS_H_
#define SPRING_INTEGRITY_UTILS_H_

#include <cstdint>
#include <string>
#include <zlib.h>

namespace spring {

/**
 * @brief Utility for updating a running CRC32 digest with record content.
 *
 * @param crc The running CRC32 value.
 * @param data The string content to add to the digest.
 */
inline void update_record_crc(uint32_t &crc, const std::string &data) {
  crc = static_cast<uint32_t>(crc32(
      static_cast<uLong>(crc), reinterpret_cast<const Bytef *>(data.data()),
      static_cast<uInt>(data.size())));
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
