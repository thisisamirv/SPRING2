// Declares the SpringReader streaming decompression API used by library-style
// consumers of SPRING2 archives.

#ifndef SPRING_ARCHIVE_STREAM_READER_H_
#define SPRING_ARCHIVE_STREAM_READER_H_

#include "archive_record_reconstruction.h"
#include <memory>
#include <string>

namespace spring {

/**
 * @brief Which member of a grouped SPRING2 bundle SpringReader should stream.
 *
 * Non-grouped archives only support Reads. Grouped bundles may also expose
 * Read3 and Index when those lanes were present at compression time.
 *
 * - Reads: R1 (and R2 when paired) from the primary read member.
 * - Read3: the R3 lane (dedicated member, or an R1/R2 alias streamed as
 *   single-end records via next(ReadRecord&)).
 * - Index: I1 (and I2 when present) from the index member.
 */
enum class SpringReaderLane {
  Reads,
  Read3,
  Index,
};

/**
 * @brief High-level streaming interface for decompressing SPRING2 archives.
 *
 * This class provides a library-style "pull" API. It handles archive
 * extraction, temporary resource management, and leverages background
 * pre-fetching to maintain high throughout without stalling the caller.
 *
 * For grouped bundles, pass SpringReaderLane to select Reads, Read3, or Index.
 * The default constructor opens the primary read member only (R1/R2).
 */
class SpringReader {
public:
  /**
   * @brief Opens a SPRING2 archive for streaming the primary read lane.
   *
   * Equivalent to SpringReader(path, SpringReaderLane::Reads, num_thr).
   *
   * @param archive_path Path to the .sp archive.
   * @param num_thr Number of threads to use for background decompression (0 for
   * auto).
   */
  explicit SpringReader(const std::string &archive_path, int num_thr = 0);

  /**
   * @brief Opens a SPRING2 archive for streaming a specific grouped lane.
   *
   * @param archive_path Path to the .sp archive.
   * @param lane Grouped member to stream (Reads, Read3, or Index).
   * @param num_thr Number of threads to use for background decompression (0 for
   * auto).
   * @throws std::runtime_error if the lane is absent or the archive is not a
   * grouped bundle (for Read3/Index).
   */
  SpringReader(const std::string &archive_path, SpringReaderLane lane,
               int num_thr = 0);

  ~SpringReader();

  // Prevent copying to manage background thread safety.
  SpringReader(const SpringReader &) = delete;
  SpringReader &operator=(const SpringReader &) = delete;

  /**
   * @brief Lane selected at construction (always Reads for non-grouped
   * archives).
   */
  SpringReaderLane lane() const;

  /**
   * @brief Access the compression parameters and metadata stored in the
   * selected archive member.
   */
  const compression_params &params() const;

  /**
   * @brief Fetches the next record for single-end archives (or an aliased R3
   * lane exposed as single-end).
   *
   * @param record Output struct to populate.
   * @return true if a record was successfully read, false if EOF is reached.
   * @throws std::runtime_error on paired-end archives (use the paired-end
   * overload).
   */
  bool next(ReadRecord &record);

  /**
   * @brief Fetches the next pair of records for paired-end archives.
   *
   * @param mate1 Output struct for the first mate.
   * @param mate2 Output struct for the second mate.
   * @return true if a pair was successfully read, false if EOF is reached.
   * @throws std::runtime_error on single-end archives (use the single-end
   * overload).
   */
  bool next(ReadRecord &mate1, ReadRecord &mate2);

  /**
   * @brief Retrieves the computed integrity digests for the selected member.
   *
   * Digests are only available after the archive has been fully consumed via
   * next(). Calling earlier throws. Prefer verify_digests() when the goal is
   * to compare against params().read_info.
   */
  void get_digests(uint32_t seq_crc[2], uint32_t qual_crc[2],
                   uint32_t id_crc[2]);

  /**
   * @brief Compare reconstructed digests to params().read_info CRCs.
   *
   * Requires the same full-consume precondition as get_digests(). Compares
   * sequence always when order is preserved; quality only when preserved and
   * not ill_bin/binary-binned; ids when preserved. Returns an empty check
   * (mismatch == false) when no channels are verifiable or digests were not
   * stored.
   */
  [[nodiscard]] lossless_digest_check verify_digests();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace spring

#endif // SPRING_ARCHIVE_STREAM_READER_H_
