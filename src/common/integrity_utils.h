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
 * @brief Which digest channels are bit-exact and should be compared.
 *
 * Quality is omitted when ill_bin/binary binning is used: stored quality CRCs
 * historically hashed pre-bin values and are not written for new binned
 * archives. Sequence (and ids when preserved) are still verified.
 */
struct digest_channel_mask {
  bool sequence = false;
  bool quality = false;
  bool id = false;

  [[nodiscard]] bool any() const { return sequence || quality || id; }
};

/**
 * @brief Outcome of an integrity digest verification attempt.
 *
 * Callers must not treat skipped_* as a successful digest compare.
 */
enum class digest_check_status {
  verified,           // Compared enabled channels; match
  skipped_lossy,      // No verifiable channels (e.g. order stripped)
  skipped_no_digests, // Channels exist but metadata has no CRC block
  mismatch,           // Compared and failed
};

struct digest_verification_result {
  digest_check_status status = digest_check_status::skipped_lossy;
  lossless_digest_check check{};
  digest_channel_mask channels{};
};

/**
 * @brief Channels that can be verified for the given archive flags.
 *
 * Requires preserved order (CRC is order-sensitive). Quality is only checked
 * when preserved and not quality-binned.
 */
inline digest_channel_mask digest_channels_for_archive(
    const bool preserve_order, const bool preserve_quality,
    const bool preserve_id, const bool ill_bin_flag, const bool bin_thr_flag) {
  digest_channel_mask channels;
  if (!preserve_order) {
    return channels;
  }
  channels.sequence = true;
  channels.quality = preserve_quality && !ill_bin_flag && !bin_thr_flag;
  channels.id = preserve_id;
  return channels;
}

/**
 * @brief Whether any digest channel can be verified for this archive.
 */
inline bool archive_supports_digest_check(const bool preserve_order,
                                          const bool preserve_quality,
                                          const bool preserve_id,
                                          const bool ill_bin_flag,
                                          const bool bin_thr_flag) {
  return digest_channels_for_archive(preserve_order, preserve_quality,
                                     preserve_id, ill_bin_flag, bin_thr_flag)
      .any();
}

// Backward-compatible name used by existing call sites.
inline bool archive_supports_lossless_digest_check(
    const bool preserve_order, const bool preserve_quality,
    const bool preserve_id, const bool ill_bin_flag, const bool bin_thr_flag) {
  return archive_supports_digest_check(preserve_order, preserve_quality,
                                       preserve_id, ill_bin_flag, bin_thr_flag);
}

/**
 * @brief Compare reconstructed digests to stored archive digests.
 *
 * @param digests_present Whether the archive metadata includes the CRC block.
 * @param paired_end Whether both mate streams should be checked.
 * @param channels Which of seq/quality/id to compare (others ignored).
 */
inline lossless_digest_check check_lossless_digests(
    const bool digests_present, const bool paired_end,
    const digest_channel_mask channels, const uint32_t stored_seq[2],
    const uint32_t stored_qual[2], const uint32_t stored_id[2],
    const uint32_t actual_seq[2], const uint32_t actual_qual[2],
    const uint32_t actual_id[2]) {
  lossless_digest_check result;
  if (!digests_present || !channels.any()) {
    return result;
  }

  result.stream_count = paired_end ? 2 : 1;
  for (int i = 0; i < result.stream_count; ++i) {
    if (channels.sequence) {
      result.sequence_mismatch[i] = actual_seq[i] != stored_seq[i];
    }
    if (channels.quality) {
      result.quality_mismatch[i] = actual_qual[i] != stored_qual[i];
    }
    if (channels.id) {
      result.id_mismatch[i] = actual_id[i] != stored_id[i];
    }
    if (result.sequence_mismatch[i] || result.quality_mismatch[i] ||
        result.id_mismatch[i]) {
      result.mismatch = true;
    }
  }
  return result;
}

/**
 * @brief Compare all channels (legacy helper for fully lossless archives).
 */
inline lossless_digest_check check_lossless_digests(
    const bool digests_present, const bool paired_end,
    const uint32_t stored_seq[2], const uint32_t stored_qual[2],
    const uint32_t stored_id[2], const uint32_t actual_seq[2],
    const uint32_t actual_qual[2], const uint32_t actual_id[2]) {
  return check_lossless_digests(
      digests_present, paired_end,
      digest_channel_mask{.sequence = true, .quality = true, .id = true},
      stored_seq, stored_qual, stored_id, actual_seq, actual_qual, actual_id);
}

/**
 * @brief Classify digest verification: verified, skipped, or mismatch.
 */
inline digest_verification_result verify_reconstructed_digests(
    const digest_channel_mask channels, const bool digests_present,
    const bool paired_end, const uint32_t stored_seq[2],
    const uint32_t stored_qual[2], const uint32_t stored_id[2],
    const uint32_t actual_seq[2], const uint32_t actual_qual[2],
    const uint32_t actual_id[2]) {
  digest_verification_result result;
  result.channels = channels;
  if (!channels.any()) {
    result.status = digest_check_status::skipped_lossy;
    return result;
  }
  if (!digests_present) {
    result.status = digest_check_status::skipped_no_digests;
    return result;
  }
  result.check =
      check_lossless_digests(true, paired_end, channels, stored_seq,
                             stored_qual, stored_id, actual_seq, actual_qual,
                             actual_id);
  result.status = result.check.mismatch ? digest_check_status::mismatch
                                        : digest_check_status::verified;
  return result;
}

/**
 * @brief Classify digest verification from archive preserve/bin flags.
 */
inline digest_verification_result verify_reconstructed_digests(
    const bool preserve_order, const bool preserve_quality,
    const bool preserve_id, const bool ill_bin_flag, const bool bin_thr_flag,
    const bool digests_present, const bool paired_end,
    const uint32_t stored_seq[2], const uint32_t stored_qual[2],
    const uint32_t stored_id[2], const uint32_t actual_seq[2],
    const uint32_t actual_qual[2], const uint32_t actual_id[2]) {
  return verify_reconstructed_digests(
      digest_channels_for_archive(preserve_order, preserve_quality, preserve_id,
                                  ill_bin_flag, bin_thr_flag),
      digests_present, paired_end, stored_seq, stored_qual, stored_id,
      actual_seq, actual_qual, actual_id);
}

} // namespace spring

#endif // SPRING_INTEGRITY_UTILS_H_
