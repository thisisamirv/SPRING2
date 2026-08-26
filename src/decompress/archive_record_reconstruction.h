// Declares the short-read, long-read, and packed-sequence archive record
// reconstruction entry points used by Spring's restoration flow.

#ifndef SPRING_ARCHIVE_RECORD_RECONSTRUCTION_H_
#define SPRING_ARCHIVE_RECORD_RECONSTRUCTION_H_

#include "integrity_utils.h"
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace spring {

struct compression_params;

struct decompression_archive_artifact {
  std::unordered_map<std::string, std::string> files;
  std::string scratch_dir;

  [[nodiscard]] bool contains(const std::string &path) const {
    return files.contains(path);
  }

  [[nodiscard]] const std::string &require(const std::string &path) const {
    auto it = files.find(path);
    if (it == files.end()) {
      throw std::runtime_error("Archive is missing required member: " + path);
    }
    return it->second;
  }
};

/**
 * @brief Represents a single genomic record (from FASTA or FASTQ).
 */
struct ReadRecord {
  std::string id;       ///< Identifier line (excluding '@' or '>')
  std::string sequence; ///< DNA sequence
  std::string quality;  ///< Phred quality scores (empty for FASTA)
};

/**
 * @brief Abstract sink for receiving decompressed records.
 */
class DecompressionSink {
public:
  virtual ~DecompressionSink() = default;

  /**
   * @brief Called when a batch of records for a specific stream has been
   * reconstructed.
   */
  virtual void consume_step(std::string *id_buffer, std::string *read_buffer,
                            const std::string *quality_buffer, uint32_t count,
                            int stream_index) = 0;

  /**
   * @brief Finalizes and returns the computed integrity digests.
   */
  void get_digests(uint32_t seq_crc[2], uint32_t qual_crc[2],
                   uint32_t id_crc[2]) const {
    for (int i = 0; i < 2; ++i) {
      seq_crc[i] = sequence_crc_[i];
      qual_crc[i] = quality_crc_[i];
      id_crc[i] = id_crc_[i];
    }
  }

protected:
  uint32_t sequence_crc_[2] = {0, 0};
  uint32_t quality_crc_[2] = {0, 0};
  uint32_t id_crc_[2] = {0, 0};
};

/**
 * @brief Standard sink implementation that writes records to FASTQ/FASTA files.
 */
class FileDecompressionSink : public DecompressionSink {
public:
  FileDecompressionSink(const std::string &outfile_1,
                        const std::string &outfile_2,
                        const compression_params &cp,
                        const int (&compression_levels)[2],
                        const bool (&should_gzip)[2],
                        const bool (&should_bgzf)[2],
                        const bool (&write_enabled)[2]);
  ~FileDecompressionSink() override;

  void consume_step(std::string *id_buffer, std::string *read_buffer,
                    const std::string *quality_buffer, uint32_t count,
                    int stream_index) override;

private:
  std::ofstream output_streams[2];
  bool should_gzip[2];
  bool should_bgzf[2];
  bool use_crlf_[2];
  bool write_enabled_[2];
  bool fasta_mode;
  bool quality_header_has_id_[2];
  int compression_level_[2];
  int num_thr;
  bool paired_end;
};

/**
 * @brief Sink that discards all data but computes integrity hashes.
 * Useful for --verify dry-runs.
 */
class NullDecompressionSink : public DecompressionSink {
public:
  void consume_step(std::string *id_buffer, std::string *read_buffer,
                    const std::string *quality_buffer, uint32_t count,
                    int stream_index) override {
    for (uint32_t i = 0; i < count; ++i) {
      update_record_crc(sequence_crc_[stream_index], read_buffer[i]);
      if (quality_buffer) {
        update_record_crc(quality_crc_[stream_index], quality_buffer[i]);
      }
      update_record_crc(id_crc_[stream_index], id_buffer[i]);
    }
  }
};

// Short-read archives reconstruct aligned and unaligned records separately.
void decompress_short(const decompression_archive_artifact &artifact,
                      DecompressionSink &sink, compression_params &cp);

// Long-read archives store read streams directly, without reference-based
// reconstruction.
void decompress_long(const decompression_archive_artifact &artifact,
                     DecompressionSink &sink, compression_params &cp);

// Packed reference chunks are decoded once, then concatenated by callers.
std::vector<std::string> decompress_unpack_seq_chunks(
    const decompression_archive_artifact &artifact,
    const std::string &packed_seq_base_path, int encoding_thread_count,
    int decoding_thread_count, const compression_params &cp);

} // namespace spring

#endif // SPRING_ARCHIVE_RECORD_RECONSTRUCTION_H_
