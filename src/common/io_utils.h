// Declares binary and compressed I/O helpers used across preprocessing,
// archive assembly, and decompression.

#ifndef SPRING_IO_UTILS_H_
#define SPRING_IO_UTILS_H_

#include <cstdint>
#include <fstream>
#include <istream>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace spring {

// Gzip stream wrappers.
class gzip_istreambuf : public std::streambuf {
public:
  gzip_istreambuf();
  explicit gzip_istreambuf(const std::string &path);
  gzip_istreambuf(const gzip_istreambuf &) = delete;
  gzip_istreambuf &operator=(const gzip_istreambuf &) = delete;
  ~gzip_istreambuf() override;

  bool open(const std::string &path);
  void close();
  bool is_open() const;
  uint64_t compressed_offset() const;

protected:
  int_type underflow() override;

private:
  static constexpr std::size_t kBufferSize = 1 << 15;
  gzFile file_;
  char buffer_[kBufferSize];
};

class gzip_istream : public std::istream {
public:
  gzip_istream();
  explicit gzip_istream(const std::string &path);
  gzip_istream(const gzip_istream &) = delete;
  gzip_istream &operator=(const gzip_istream &) = delete;

  bool open(const std::string &path);
  void close();
  bool is_open() const;
  uint64_t compressed_offset() const;

private:
  gzip_istreambuf buffer_;
};

class gzip_ostream {
public:
  gzip_ostream();
  explicit gzip_ostream(const std::string &path,
                        int level = Z_DEFAULT_COMPRESSION);
  gzip_ostream(const gzip_ostream &) = delete;
  gzip_ostream &operator=(const gzip_ostream &) = delete;
  ~gzip_ostream();

  bool open(const std::string &path, int level = Z_DEFAULT_COMPRESSION);
  void write(const char *data, std::streamsize size);
  void close();
  bool is_open() const;

private:
  gzFile file_;
};

std::string gzip_compress_string(const std::string &input, int level);

// FASTQ block helpers.
uint32_t read_fastq_block(
    std::istream *input_stream, std::string *id_array, std::string *read_array,
    std::string *quality_array, const uint32_t &num_reads,
    const bool &fasta_flag, uint32_t *read_lengths = nullptr,
    uint8_t *read_contains_n = nullptr, uint32_t *sequence_crc = nullptr,
    uint32_t *quality_crc = nullptr, uint32_t *id_crc = nullptr,
    bool validate_quality_length = false, bool *saw_crlf = nullptr);

void write_fastq_block(std::ofstream &output_stream, std::string *id_array,
                       std::string *read_array,
                       const std::string *quality_array,
                       const uint32_t &num_reads, const int &num_thr,
                       const bool &gzip_flag, const bool &bgzf_flag,
                       const int &compression_level, const bool use_crlf,
                       const bool fasta_mode,
                       const bool quality_header_has_id = false);

void write_bgzf_fastq_block(std::ofstream &output_stream, std::string *id_array,
                            std::string *read_array,
                            const std::string *quality_array,
                            const uint32_t &num_reads, const int &num_thr,
                            const int &compression_level, const bool use_crlf,
                            const bool fasta_mode,
                            const bool quality_header_has_id = false);

// ID stream helpers (using libbsc).
std::vector<char> compress_id_block_bytes(std::string *id_array,
                                          const uint32_t &num_ids,
                                          bool pack_only = false);
// Guards against stale call sites still passing the removed compression_level
// argument: an int third argument would otherwise silently convert to
// pack_only instead of failing to compile.
std::vector<char> compress_id_block_bytes(std::string *id_array,
                                          const uint32_t &num_ids, int,
                                          bool pack_only = false) = delete;

void decompress_id_block_bytes(std::string_view input_bytes,
                               std::string_view input_label,
                               std::string *id_array, const uint32_t &num_ids,
                               bool pack_only = false);

// Quality helpers.
std::vector<char> bsc_str_array_compress_bytes(std::string *string_array,
                                               uint32_t num_strings,
                                               uint32_t *string_lengths);

void quantize_quality(std::string *quality_array, const uint32_t &num_lines,
                      char *quantization_table);

void generate_illumina_binning_table(char *illumina_binning_table);

void generate_binary_binning_table(char *binary_binning_table,
                                   const unsigned int thr,
                                   const unsigned int high,
                                   const unsigned int low);

// Reliability helpers.
std::vector<char> bsc_compress_bytes(const std::vector<char> &input_bytes);

std::vector<char> bsc_decompress_bytes(const std::vector<char> &input_bytes);

void safe_bsc_str_array_decompress_bytes(std::string_view input_bytes,
                                         std::string_view input_label,
                                         std::string *string_array,
                                         uint32_t num_strings,
                                         uint32_t *string_lengths);

// Detailed Gzip probing.
void extract_gzip_detailed_info(const std::string &path, bool &is_gzipped,
                                uint8_t &flg, uint32_t &mtime, uint8_t &xfl,
                                uint8_t &os, std::string &name, bool &is_bgzf,
                                uint16_t &bgzf_block_size,
                                uint64_t &uncompressed_size,
                                uint64_t &compressed_size,
                                uint32_t &member_count);

} // namespace spring

#endif // SPRING_IO_UTILS_H_
