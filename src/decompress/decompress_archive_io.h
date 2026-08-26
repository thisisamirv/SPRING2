// Declares archive-loading helpers used by decompression separately from the
// record reconstruction loops.

#ifndef SPRING_DECOMPRESS_ARCHIVE_IO_H_
#define SPRING_DECOMPRESS_ARCHIVE_IO_H_

#include "archive_record_reconstruction.h"
#include "params.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace spring {

struct reference_chunk {
  uint64_t start_offset = 0;
  uint64_t size = 0;
  std::string owned_data;
  const char *data = nullptr;
};

std::vector<char>
archive_member_bytes(const decompression_archive_artifact &artifact,
                     const std::string &member_name);

std::vector<char>
decompress_archive_bsc_member(const decompression_archive_artifact &artifact,
                              const std::string &member_name);

std::vector<char> decompress_legacy_archive_bsc_member(
    const decompression_archive_artifact &artifact,
    const std::string &member_name);

void decompress_legacy_archive_bsc_str_array_member(
    const decompression_archive_artifact &artifact,
    const std::string &member_name, std::string *string_array,
    uint32_t num_strings, uint32_t *string_lengths);

void decompress_legacy_archive_id_member(
    const decompression_archive_artifact &artifact,
    const std::string &member_name, std::string *id_array, uint32_t num_ids);

class memory_cursor {
public:
  explicit memory_cursor(const std::vector<char> &bytes) : bytes_(bytes) {}

  template <typename T> T read(const std::string &label) {
    if (offset_ + sizeof(T) > bytes_.size()) {
      throw std::runtime_error("Corrupt archive: truncated " + label);
    }
    T value{};
    std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
    offset_ += sizeof(T);
    return value;
  }

  char read_char(const std::string &label) { return read<char>(label); }

  void read_bytes(char *destination, size_t count, const std::string &label) {
    if (offset_ + count > bytes_.size()) {
      throw std::runtime_error("Corrupt archive: truncated " + label);
    }
    if (count > 0) {
      std::memcpy(destination, bytes_.data() + offset_, count);
      offset_ += count;
    }
  }

  [[nodiscard]] std::string read_line(const std::string &label) {
    if (offset_ > bytes_.size()) {
      throw std::runtime_error("Corrupt archive: truncated " + label);
    }
    const char *begin = bytes_.data() + offset_;
    const size_t remaining = bytes_.size() - offset_;
    const void *newline_ptr = std::memchr(begin, '\n', remaining);
    const size_t line_len =
        newline_ptr == nullptr
            ? remaining
            : static_cast<size_t>(static_cast<const char *>(newline_ptr) -
                                  begin);
    std::string line(begin, line_len);
    offset_ += line_len;
    if (newline_ptr != nullptr) {
      offset_ += 1;
    }
    return line;
  }

private:
  const std::vector<char> &bytes_;
  size_t offset_ = 0;
};

std::vector<std::string>
slice_monolithic_id_blocks(const decompression_archive_artifact &artifact,
                           const std::string &member_name,
                           const uint64_t *file_len_thr, uint32_t num_reads,
                           uint32_t num_reads_per_block);

std::string make_decompress_step_log_message(const char *label,
                                             uint32_t num_reads_done,
                                             uint32_t num_reads_cur_step,
                                             uint32_t num_blocks_done);

class reference_sequence_store {
public:
  reference_sequence_store(const reference_sequence_store &) = delete;
  reference_sequence_store &
  operator=(const reference_sequence_store &) = delete;
  reference_sequence_store(reference_sequence_store &&) = delete;
  reference_sequence_store &operator=(reference_sequence_store &&) = delete;

  reference_sequence_store(const decompression_archive_artifact &artifact,
                           const std::string &packed_seq_path,
                           int encoding_thread_count, int decode_thread_count,
                           const compression_params &cp);
  ~reference_sequence_store() = default;

  [[nodiscard]] std::string read(uint64_t start_offset,
                                 uint32_t read_length) const;

private:
  [[nodiscard]] size_t find_chunk_index(uint64_t offset) const;

  std::vector<uint64_t> start_offsets_;
  std::vector<reference_chunk> chunks_;
  uint64_t total_size_ = 0;
};

std::string block_file_path(const std::string &base_path, uint32_t block_num);

std::string compressed_block_file_path(const std::string &base_path,
                                       uint32_t block_num);

uint32_t compute_thread_read_count(uint32_t step_read_count,
                                   uint32_t num_reads_per_block,
                                   uint64_t thread_id);

std::string
decode_packed_sequence_chunk_bytes(const std::vector<char> &packed_bytes,
                                   int encoding_thread_id, uint64_t num_bases,
                                   bool bisulfite_ternary);

uint64_t compute_num_reads_per_step(const uint32_t num_reads,
                                    const uint32_t num_reads_per_block,
                                    const int num_thr, const bool paired_end);

uint32_t compute_num_reads_cur_step(const uint32_t num_reads,
                                    const uint32_t num_reads_done,
                                    const uint64_t num_reads_per_step,
                                    const bool paired_end);

int resolve_archive_encoding_thread_count(const compression_params &cp);

void set_dec_noise_array(std::array<std::array<char, 128>, 128> &dec_noise);

} // namespace spring

#endif // SPRING_DECOMPRESS_ARCHIVE_IO_H_