// Implements binary, gzip, BGZF, and libbsc I/O helpers shared across the
// compression and decompression pipeline.

#include "io_utils.h"
#include "bgzf.h"
#include "integrity_utils.h"
#include "libbsc/filters.h"
#include "libbsc/libbsc.h"
#include "params.h"
#include "parse_utils.h"
#include "progress.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <libdeflate.h>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <zstd.h>

// I/O utilities: helpers for reading FASTQ/FASTA inputs, BGZF handling, and
// integration with compression libraries used by the preprocessing stage.

namespace spring {

namespace {

const char *const kInvalidFastqError =
    "Invalid FASTQ(A) file. Number of lines not multiple of 4(2)";

constexpr std::streamsize kGzipChunkSize = 1 << 15;

#define MODE_FIXED 1
#define DISTORTION_MSE 2

struct read_range {
  uint64_t start;
  uint64_t end;
};

std::vector<read_range> compute_read_ranges(uint32_t num_reads, int num_thr) {
  if (num_thr <= 0) {
    SPRING_LOG_DEBUG("block_id=io-utils:ranges, compute_read_ranges invalid "
                     "threads: path=compute_read_ranges" +
                     std::string(", expected_bytes=1, actual_bytes=") +
                     std::to_string(num_thr) +
                     ", index=" + std::to_string(num_reads));
    throw std::runtime_error("Number of threads must be positive.");
  }

  std::vector<read_range> ranges(static_cast<size_t>(num_thr));
  const uint64_t num_reads_per_thread = 1 + ((num_reads - 1) / num_thr);
  uint64_t next_start = 0;
  for (int thread_index = 0; thread_index < num_thr; ++thread_index) {
    read_range &range = ranges[static_cast<size_t>(thread_index)];
    range.start = std::min<uint64_t>(next_start, num_reads);
    range.end =
        std::min<uint64_t>(range.start + num_reads_per_thread, num_reads);
    next_start = range.end;
  }
  return ranges;
}

std::runtime_error gzip_runtime_error(gzFile file_handle,
                                      const std::string &prefix) {
  int error_code = Z_OK;
  const char *message = gzerror(file_handle, &error_code);
  if (message == nullptr || error_code == Z_OK) {
    return std::runtime_error(prefix);
  }
  return std::runtime_error(prefix + ": " + message);
}

uint64_t count_gzip_uncompressed_bytes(const std::string &path) {
  gzFile file_handle = gzopen(path.c_str(), "rb");
  if (file_handle == nullptr) {
    return 0;
  }

  std::vector<char> buffer(static_cast<size_t>(kGzipChunkSize));
  uint64_t total_uncompressed_bytes = 0;

  while (true) {
    const int bytes_read = gzread(file_handle, buffer.data(),
                                  static_cast<unsigned int>(buffer.size()));
    if (bytes_read > 0) {
      total_uncompressed_bytes += static_cast<uint64_t>(bytes_read);
      continue;
    }
    if (bytes_read == 0) {
      break;
    }

    gzclose(file_handle);
    return 0;
  }

  gzclose(file_handle);
  return total_uncompressed_bytes;
}

void write_gzip_data(gzFile file_handle, const char *data,
                     std::streamsize size) {
  std::streamsize written_total = 0;
  while (written_total < size) {
    const std::streamsize chunk_size =
        std::min<std::streamsize>(size - written_total, kGzipChunkSize);
    const int written = gzwrite(file_handle, data + written_total,
                                static_cast<unsigned int>(chunk_size));
    if (written == 0) {
      throw gzip_runtime_error(file_handle, "Failed writing gzip stream");
    }
    written_total += written;
  }
}

void append_fastq_record(std::string &out, const std::string &id,
                         const std::string &read,
                         const std::string *quality_or_null,
                         const bool use_crlf, const bool fasta_mode,
                         const bool quality_header_has_id) {
  const char *eol = use_crlf ? "\r\n" : "\n";
  out += id;
  out += eol;
  out += read;
  out += eol;
  if (!fasta_mode) {
    if (quality_header_has_id) {
      // Restore the ID on the quality header line
      out += "+";
      // Extract the ID part without the leading '@'
      if (!id.empty() && id[0] == '@') {
        out.append(id, 1, std::string::npos);
      }
    } else {
      out += "+";
    }
    out += eol;
    if (quality_or_null != nullptr) {
      out += *quality_or_null;
      out += eol;
    } else {
      out.append(read.size(), 'I');
      out += eol;
    }
  }
}

void append_fastq_records_range(std::string &output_buffer,
                                std::string *id_array, std::string *read_array,
                                const std::string *quality_or_null,
                                const uint64_t start_read_index,
                                const uint64_t end_read_index,
                                const bool use_crlf, const bool fasta_mode,
                                const bool quality_header_has_id = false) {
  for (uint64_t read_index = start_read_index; read_index < end_read_index;
       read_index++) {
    append_fastq_record(
        output_buffer, id_array[read_index], read_array[read_index],
        quality_or_null == nullptr ? nullptr : &quality_or_null[read_index],
        use_crlf, fasta_mode, quality_header_has_id);
  }
}

#pragma pack(push, 1)
struct bsc_archive_block_header {
  long long block_offset;
  signed char record_size;
  signed char sorting_contexts;
};

struct bsc_str_array_block_header {
  signed char record_size;
  signed char sorting_contexts;
};
#pragma pack(pop)

constexpr size_t kBscArchiveBlockSize = 25U * 1024U * 1024U;

void ensure_libbsc_ready() {
  static std::once_flag init_once;
  std::call_once(init_once, []() {
    const int init_result = bsc_init(LIBBSC_DEFAULT_FEATURES);
    if (init_result != LIBBSC_NO_ERROR) {
      throw std::runtime_error("Failed to initialize libbsc.");
    }
  });
}

template <typename T>
void append_binary(std::vector<char> &buffer, const T &value) {
  const size_t old_size = buffer.size();
  buffer.resize(old_size + sizeof(T));
  std::memcpy(buffer.data() + old_size, &value, sizeof(T));
}

template <typename T>
T read_binary_or_throw(std::string_view bytes, size_t &cursor,
                       const std::string &label,
                       const std::string &input_label) {
  if (cursor + sizeof(T) > bytes.size()) {
    throw std::runtime_error("Corrupt BSC byte stream for " + input_label +
                             ": truncated " + label + ".");
  }
  T value{};
  std::memcpy(&value, bytes.data() + cursor, sizeof(T));
  cursor += sizeof(T);
  return value;
}

void write_string_array_bytes_or_throw(const unsigned char *buffer,
                                       int data_size, std::string *string_array,
                                       uint32_t size_str_array,
                                       uint32_t *string_lengths,
                                       uint32_t &pos_in_str_array,
                                       uint32_t &pos_in_current_str) {
  int bytes_read = 0;
  while (bytes_read < data_size) {
    if (pos_in_str_array >= size_str_array) {
      throw std::runtime_error(
          "BSC decompression error - string array not large enough.");
    }

    if (pos_in_current_str == string_lengths[pos_in_str_array]) {
      pos_in_str_array++;
      pos_in_current_str = 0;
      continue;
    }

    if (string_array[pos_in_str_array].size() !=
        string_lengths[pos_in_str_array]) {
      string_array[pos_in_str_array].resize(string_lengths[pos_in_str_array]);
    }

    const uint32_t bytes_remaining_in_string =
        string_lengths[pos_in_str_array] - pos_in_current_str;
    const uint32_t bytes_remaining_in_block =
        static_cast<uint32_t>(data_size - bytes_read);
    const uint32_t copy_size =
        (std::min)(bytes_remaining_in_string, bytes_remaining_in_block);
    std::memcpy(&string_array[pos_in_str_array][pos_in_current_str],
                buffer + bytes_read, copy_size);
    pos_in_current_str += copy_size;
    bytes_read += static_cast<int>(copy_size);
  }
}

size_t total_string_array_bytes(uint32_t num_strings,
                                const uint32_t *string_lengths) {
  size_t total_size = 0;
  for (uint32_t i = 0; i < num_strings; ++i) {
    total_size += string_lengths[i];
  }
  return total_size;
}

} // namespace

gzip_istreambuf::gzip_istreambuf() : file_(nullptr), buffer_{} {}

gzip_istreambuf::gzip_istreambuf(const std::string &path)
    : file_(nullptr), buffer_{} {
  open(path);
}

gzip_istreambuf::~gzip_istreambuf() { close(); }

bool gzip_istreambuf::open(const std::string &path) {
  close();
  file_ = gzopen(path.c_str(), "rb");
  return file_ != nullptr;
}

void gzip_istreambuf::close() {
  if (file_) {
    gzclose(file_);
    file_ = nullptr;
  }
}

bool gzip_istreambuf::is_open() const { return file_ != nullptr; }

uint64_t gzip_istreambuf::compressed_offset() const {
  if (!is_open()) {
    return 0;
  }

  const z_off_t current_offset = gzoffset(file_);
  return current_offset >= 0 ? static_cast<uint64_t>(current_offset) : 0;
}

gzip_istreambuf::int_type gzip_istreambuf::underflow() {
  if (!is_open()) {
    return traits_type::eof();
  }

  const int bytes_read =
      gzread(file_, buffer_, static_cast<unsigned int>(kBufferSize));
  if (bytes_read <= 0) {
    return traits_type::eof();
  }

  setg(buffer_, buffer_, buffer_ + bytes_read);
  return traits_type::to_int_type(*gptr());
}

gzip_istream::gzip_istream() : std::istream(&buffer_) {}

gzip_istream::gzip_istream(const std::string &path)
    : std::istream(&buffer_), buffer_(path) {}

bool gzip_istream::open(const std::string &path) { return buffer_.open(path); }

void gzip_istream::close() { buffer_.close(); }

bool gzip_istream::is_open() const { return buffer_.is_open(); }

uint64_t gzip_istream::compressed_offset() const {
  return buffer_.compressed_offset();
}

gzip_ostream::gzip_ostream() : file_(nullptr) {}

gzip_ostream::gzip_ostream(const std::string &path, int level)
    : file_(nullptr) {
  open(path, level);
}

gzip_ostream::~gzip_ostream() { close(); }

bool gzip_ostream::open(const std::string &path, int level) {
  close();
  std::string mode = "wb";
  if (level != Z_DEFAULT_COMPRESSION) {
    mode += std::to_string(level);
  }
  file_ = gzopen(path.c_str(), mode.c_str());
  return file_ != nullptr;
}

void gzip_ostream::write(const char *data, std::streamsize size) {
  if (!is_open()) {
    SPRING_LOG_DEBUG("block_id=io-utils:gzip-ostream, gzip_ostream::write "
                     "failure: path=gzip_ostream::write" +
                     std::string(", expected_bytes=") + std::to_string(size) +
                     ", actual_bytes=0, index=0");
    throw std::runtime_error("gzip_ostream is not open for writing.");
  }
  write_gzip_data(file_, data, size);
}

void gzip_ostream::close() {
  if (file_) {
    gzclose(file_);
    file_ = nullptr;
  }
}

bool gzip_ostream::is_open() const { return file_ != nullptr; }

std::string gzip_compress_string(const std::string &input, int level) {
  SPRING_LOG_DEBUG("block_id=io-utils:gzip-compress, gzip_compress_string "
                   "start: input_bytes=" +
                   std::to_string(input.size()) +
                   ", level=" + std::to_string(level));
  libdeflate_compressor *compressor = libdeflate_alloc_compressor(level);
  if (!compressor) {
    SPRING_LOG_DEBUG("block_id=io-utils:gzip-compress, gzip_compress_string "
                     "alloc failure: input_bytes=" +
                     std::to_string(input.size()) +
                     ", level=" + std::to_string(level));
    throw std::runtime_error("Failed allocating libdeflate gzip compressor.");
  }

  std::string output;
  output.resize(libdeflate_gzip_compress_bound(compressor, input.size()));
  const size_t compressed_size = libdeflate_gzip_compress(
      compressor, input.data(), input.size(), output.data(), output.size());

  libdeflate_free_compressor(compressor);

  if (compressed_size == 0) {
    SPRING_LOG_DEBUG("block_id=io-utils:gzip-compress, gzip_compress_string "
                     "compression failure: input_bytes=" +
                     std::to_string(input.size()) +
                     ", output_capacity=" + std::to_string(output.size()));
    throw std::runtime_error("Failed compressing gzip payload.");
  }

  output.resize(compressed_size);
  SPRING_LOG_DEBUG("block_id=io-utils:gzip-compress, gzip_compress_string "
                   "done: output_bytes=" +
                   std::to_string(output.size()));
  return output;
}

uint32_t read_fastq_block(std::istream *input_stream, std::string *id_array,
                          std::string *read_array, std::string *quality_array,
                          const uint32_t &num_reads, const bool &fasta_flag,
                          uint32_t *read_lengths, uint8_t *read_contains_n,
                          uint32_t *sequence_crc, uint32_t *quality_crc,
                          uint32_t *id_crc, const bool validate_quality_length,
                          bool *saw_crlf) {
  if (!fasta_flag && quality_array == nullptr) {
    throw std::runtime_error(
        "Quality output buffer is required when reading FASTQ blocks.");
  }

  if (saw_crlf != nullptr) {
    *saw_crlf = false;
  }

  uint32_t reads_processed = 0;
  for (; reads_processed < num_reads; reads_processed++) {
    if (!std::getline(*input_stream, id_array[reads_processed]))
      break;
    if (saw_crlf != nullptr && !id_array[reads_processed].empty() &&
        id_array[reads_processed].back() == '\r') {
      *saw_crlf = true;
    }
    remove_CR_from_end(id_array[reads_processed]);
    if (id_crc != nullptr)
      update_record_crc(*id_crc, id_array[reads_processed]);

    if (!std::getline(*input_stream, read_array[reads_processed])) {
      SPRING_LOG_DEBUG(
          "block_id=io-utils:fastq-read, read_fastq_block parse failure: "
          "path=sequence, expected_bytes=1, actual_bytes=0, index=" +
          std::to_string(reads_processed));
      throw std::runtime_error(kInvalidFastqError);
    }
    if (saw_crlf != nullptr && !read_array[reads_processed].empty() &&
        read_array[reads_processed].back() == '\r') {
      *saw_crlf = true;
    }
    remove_CR_from_end(read_array[reads_processed]);
    if (sequence_crc != nullptr)
      update_record_crc(*sequence_crc, read_array[reads_processed]);
    if (read_lengths != nullptr)
      read_lengths[reads_processed] =
          static_cast<uint32_t>(read_array[reads_processed].size());
    if (read_contains_n != nullptr)
      read_contains_n[reads_processed] =
          read_array[reads_processed].find('N') != std::string::npos;

    if (fasta_flag)
      continue;

    std::string plus_line;
    if (!std::getline(*input_stream, plus_line)) {
      SPRING_LOG_DEBUG(
          "block_id=io-utils:fastq-read, read_fastq_block parse failure: "
          "path=plus, expected_bytes=43, actual_bytes=0, index=" +
          std::to_string(reads_processed));
      throw std::runtime_error(kInvalidFastqError);
    }
    if (saw_crlf != nullptr && !plus_line.empty() && plus_line.back() == '\r') {
      *saw_crlf = true;
    }
    if (plus_line.empty() || plus_line[0] != '+') {
      const int actual_plus_char =
          plus_line.empty() ? 0 : static_cast<unsigned char>(plus_line[0]);
      SPRING_LOG_DEBUG("block_id=io-utils:fastq-read, read_fastq_block parse "
                       "failure: path=plus, expected_bytes=43, actual_bytes=" +
                       std::to_string(actual_plus_char) +
                       ", index=" + std::to_string(reads_processed));
      throw std::runtime_error(kInvalidFastqError);
    }
    if (!std::getline(*input_stream, quality_array[reads_processed])) {
      SPRING_LOG_DEBUG(
          "block_id=io-utils:fastq-read, read_fastq_block parse failure: "
          "path=quality, expected_bytes=1, actual_bytes=0, index=" +
          std::to_string(reads_processed));
      throw std::runtime_error(kInvalidFastqError);
    }
    if (saw_crlf != nullptr && !quality_array[reads_processed].empty() &&
        quality_array[reads_processed].back() == '\r') {
      *saw_crlf = true;
    }
    remove_CR_from_end(quality_array[reads_processed]);
    if (validate_quality_length && quality_array[reads_processed].size() !=
                                       read_array[reads_processed].size()) {
      throw std::runtime_error("Read length does not match quality length.");
    }
    if (quality_crc != nullptr)
      update_record_crc(*quality_crc, quality_array[reads_processed]);
  }
  SPRING_LOG_DEBUG("block_id=io-utils:fastq-read, read_fastq_block summary: "
                   "requested_reads=" +
                   std::to_string(num_reads) + ", processed_reads=" +
                   std::to_string(reads_processed) + ", fasta_mode=" +
                   std::string(fasta_flag ? "true" : "false"));
  return reads_processed;
}

void write_fastq_block(std::ofstream &output_stream, std::string *id_array,
                       std::string *read_array,
                       const std::string *quality_array,
                       const uint32_t &num_reads, const int &num_thr,
                       const bool &gzip_flag, const bool &bgzf_flag,
                       const int &compression_level, const bool use_crlf,
                       const bool fasta_mode,
                       const bool quality_header_has_id) {
  if (num_reads == 0)
    return;

  SPRING_LOG_DEBUG(
      "block_id=io-utils:fastq-write, write_fastq_block start: reads=" +
      std::to_string(num_reads) + ", threads=" + std::to_string(num_thr) +
      ", gzip=" + std::string(gzip_flag ? "true" : "false") +
      ", bgzf=" + std::string(bgzf_flag ? "true" : "false") +
      ", fasta_mode=" + std::string(fasta_mode ? "true" : "false") +
      ", compression_level=" + std::to_string(compression_level));

  if (bgzf_flag) {
    write_bgzf_fastq_block(output_stream, id_array, read_array, quality_array,
                           num_reads, num_thr, compression_level, use_crlf,
                           fasta_mode, quality_header_has_id);
  } else if (gzip_flag) {
    std::vector<std::string> compressed(static_cast<size_t>(num_thr));
    const std::vector<read_range> thread_ranges =
        compute_read_ranges(num_reads, num_thr);
    // Use parallel for so every iteration executes even when OMP creates
    // fewer threads than requested (e.g. ClangCL/libomp on Windows).  Each
    // iteration writes to a distinct compressed[tid] slot (thread-safe).
#pragma omp parallel for num_threads(num_thr) schedule(static, 1)
    for (int tid = 0; tid < num_thr; ++tid) {
      // Declare local (not thread_local) to avoid TLS initialization issues
      // with LLVM libomp worker threads under ClangCL.
      std::string local_buf;
      const read_range &range = thread_ranges[static_cast<size_t>(tid)];
      append_fastq_records_range(local_buf, id_array, read_array, quality_array,
                                 range.start, range.end, use_crlf, fasta_mode,
                                 quality_header_has_id);
      compressed[tid] = gzip_compress_string(local_buf, compression_level);
    }
    uint64_t total_compressed_bytes = 0;
    for (int i = 0; i < num_thr; i++)
      total_compressed_bytes += compressed[i].size();
    for (int i = 0; i < num_thr; i++)
      output_stream.write(compressed[i].data(), compressed[i].size());
    SPRING_LOG_DEBUG("block_id=io-utils:fastq-write, write_fastq_block gzip "
                     "summary: chunks=" +
                     std::to_string(num_thr) + ", total_compressed_bytes=" +
                     std::to_string(total_compressed_bytes));
  } else {
    uint64_t total_plain_bytes = 0;
    for (uint32_t i = 0; i < num_reads; i++) {
      std::string rec;
      append_fastq_record(rec, id_array[i], read_array[i],
                          quality_array ? &quality_array[i] : nullptr, use_crlf,
                          fasta_mode, quality_header_has_id);
      total_plain_bytes += rec.size();
      output_stream.write(rec.data(), rec.size());
    }
    SPRING_LOG_DEBUG("block_id=io-utils:fastq-write, write_fastq_block plain "
                     "summary: total_plain_bytes=" +
                     std::to_string(total_plain_bytes));
  }
}

void write_bgzf_fastq_block(std::ofstream &output_stream, std::string *id_array,
                            std::string *read_array,
                            const std::string *quality_array,
                            const uint32_t &num_reads, const int &num_thr,
                            const int &compression_level, const bool use_crlf,
                            const bool fasta_mode,
                            const bool quality_header_has_id) {
  if (num_reads == 0)
    return;

  const std::vector<read_range> thread_ranges =
      compute_read_ranges(num_reads, num_thr);
  std::vector<std::vector<std::string>> bgzf_blocks(num_thr);
  // Use parallel for so every iteration executes even when OMP creates
  // fewer threads than requested (e.g. ClangCL/libomp on Windows).
#pragma omp parallel for num_threads(num_thr) schedule(static, 1)
  for (int tid = 0; tid < num_thr; ++tid) {
    // Declare local (not thread_local) to avoid TLS initialization issues
    // with LLVM libomp worker threads under ClangCL.
    std::string local_buf;
    const read_range &range = thread_ranges[static_cast<size_t>(tid)];
    append_fastq_records_range(local_buf, id_array, read_array, quality_array,
                               range.start, range.end, use_crlf, fasta_mode,
                               quality_header_has_id);
    bgzf_blocks[tid] = bgzf_compress_buffer(local_buf, compression_level);
  }

  for (int i = 0; i < num_thr; i++) {
    uint64_t thread_bytes = 0;
    for (const auto &block : bgzf_blocks[i]) {
      thread_bytes += block.size();
      output_stream.write(block.data(), block.size());
    }
    SPRING_LOG_DEBUG(
        "block_id=io-utils:bgzf-write:thread-" + std::to_string(i) +
        ", write_bgzf_fastq_block thread summary: index=" + std::to_string(i) +
        ", blocks=" + std::to_string(bgzf_blocks[i].size()) +
        ", bytes=" + std::to_string(thread_bytes));
  }
}

std::vector<char> compress_id_block_bytes(std::string *id_array,
                                          const uint32_t &num_ids,
                                          bool pack_only) {
  if (num_ids == 0)
    return {};

  if (pack_only) {
    std::vector<char> output_bytes;
    size_t total_size = 0;
    for (uint32_t i = 0; i < num_ids; i++) {
      total_size += id_array[i].size() + 1;
    }
    output_bytes.reserve(total_size);
    for (uint32_t i = 0; i < num_ids; i++) {
      output_bytes.insert(output_bytes.end(), id_array[i].begin(),
                          id_array[i].end());
      output_bytes.push_back('\n');
    }
    return output_bytes;
  }

  std::vector<std::string> alpha_cols;
  std::vector<std::string> non_alpha_cols;
  std::vector<uint8_t> col_counts;
  col_counts.reserve(num_ids);

  for (uint32_t i = 0; i < num_ids; i++) {
    const std::string &id = id_array[i];
    uint8_t num_pairs = 0;
    size_t pos = 0;

    while (pos < id.size()) {
      if (num_pairs >= non_alpha_cols.size())
        non_alpha_cols.resize(num_pairs + 1);
      if (num_pairs >= alpha_cols.size())
        alpha_cols.resize(num_pairs + 1);

      // 1. Non-Alpha
      size_t end = pos;
      auto is_alnum = [](unsigned char c) { return std::isalnum(c); };
      while (end < id.size() && !is_alnum(static_cast<unsigned char>(id[end])))
        end++;
      non_alpha_cols[num_pairs].append(id.data() + pos, end - pos);
      non_alpha_cols[num_pairs].push_back('\0');
      pos = end;

      // 2. Alpha
      if (pos < id.size()) {
        end = pos;
        while (end < id.size() && is_alnum(static_cast<unsigned char>(id[end])))
          end++;
        alpha_cols[num_pairs].append(id.data() + pos, end - pos);
        alpha_cols[num_pairs].push_back('\0');
        pos = end;
      } else {
        alpha_cols[num_pairs].push_back('\0'); // Empty alpha
      }
      num_pairs++;
    }
    col_counts.push_back(num_pairs);
  }

  uint32_t num_nalpha = static_cast<uint32_t>(non_alpha_cols.size());
  uint32_t num_alpha = static_cast<uint32_t>(alpha_cols.size());
  uint32_t count_sz = static_cast<uint32_t>(col_counts.size());

  std::vector<std::string> new_alpha_cols;
  std::vector<uint8_t> alpha_fmts;
  new_alpha_cols.reserve(alpha_cols.size());
  alpha_fmts.reserve(alpha_cols.size());

  for (size_t c = 0; c < alpha_cols.size(); ++c) {
    const char *ptr = alpha_cols[c].data();
    const char *end_ptr = ptr + alpha_cols[c].size();
    bool is_numeric = true;

    const char *scan = ptr;
    while (scan < end_ptr) {
      size_t len = std::strlen(scan);
      if (len > 0) {
        if (len > 1 && scan[0] == '0') {
          is_numeric = false;
          break;
        }
        if (len > 9) {
          is_numeric = false;
          break;
        } // prevent uint32 overflow
        for (size_t i = 0; i < len; ++i) {
          if (!std::isdigit(static_cast<unsigned char>(scan[i]))) {
            is_numeric = false;
            break;
          }
        }
        if (!is_numeric)
          break;
      }
      scan += len + 1;
    }

    if (is_numeric) {
      std::vector<int32_t> deltas;
      uint32_t last_val = 0;
      scan = ptr;
      while (scan < end_ptr) {
        size_t len = std::strlen(scan);
        int32_t delta = INT32_MIN;
        if (len > 0) {
          uint32_t val = 0;
          for (size_t i = 0; i < len; ++i)
            val = val * 10 + (scan[i] - '0');
          delta = static_cast<int32_t>(val - last_val);
          last_val = val;
        }
        deltas.push_back(delta);
        scan += len + 1;
      }

      std::string deltas_str;
      deltas_str.resize(deltas.size() * sizeof(int32_t));
      size_t n = deltas.size();
      for (size_t i = 0; i < n; ++i) {
        uint32_t udelta;
        std::memcpy(&udelta, &deltas[i], sizeof(int32_t));
        deltas_str[i] = static_cast<char>(udelta & 0xFF);
        deltas_str[n + i] = static_cast<char>((udelta >> 8) & 0xFF);
        deltas_str[2 * n + i] = static_cast<char>((udelta >> 16) & 0xFF);
        deltas_str[3 * n + i] = static_cast<char>((udelta >> 24) & 0xFF);
      }

      alpha_fmts.push_back(1);
      new_alpha_cols.push_back(std::move(deltas_str));
    } else {
      alpha_fmts.push_back(0);
      new_alpha_cols.push_back(std::move(alpha_cols[c]));
    }
  }

  size_t est_size = 3 * sizeof(uint32_t) + count_sz;
  for (const auto &c : non_alpha_cols)
    est_size += sizeof(uint32_t) + c.size();
  for (const auto &c : new_alpha_cols)
    est_size += 1 + sizeof(uint32_t) + c.size();

  std::vector<char> buffer;
  buffer.reserve(est_size);

  auto write_u32 = [&](uint32_t val) {
    char bytes[4];
    std::memcpy(bytes, &val, 4);
    buffer.insert(buffer.end(), bytes, bytes + 4);
  };

  write_u32(num_nalpha);
  write_u32(num_alpha);
  write_u32(count_sz);
  buffer.insert(buffer.end(), col_counts.begin(), col_counts.end());

  for (const auto &c : non_alpha_cols) {
    write_u32(static_cast<uint32_t>(c.size()));
    buffer.insert(buffer.end(), c.begin(), c.end());
  }
  for (size_t i = 0; i < new_alpha_cols.size(); ++i) {
    buffer.push_back(alpha_fmts[i]);
    write_u32(static_cast<uint32_t>(new_alpha_cols[i].size()));
    buffer.insert(buffer.end(), new_alpha_cols[i].begin(),
                  new_alpha_cols[i].end());
  }

  return bsc_compress_bytes(buffer);
}

std::vector<char> bsc_str_array_compress_bytes(std::string *string_array,
                                               uint32_t num_strings,
                                               uint32_t *string_lengths) {
  ensure_libbsc_ready();

  const size_t total_input_size =
      total_string_array_bytes(num_strings, string_lengths);
  const int block_size =
      static_cast<int>((std::min)(total_input_size, kBscArchiveBlockSize));
  const int num_blocks =
      block_size > 0
          ? static_cast<int>((total_input_size + block_size - 1) / block_size)
          : 0;

  std::vector<char> output_bytes;
  append_binary(output_bytes, num_blocks);
  if (block_size == 0) {
    return output_bytes;
  }

  std::vector<unsigned char> read_buffer(static_cast<size_t>(block_size));
  std::vector<unsigned char> work_buffer(static_cast<size_t>(block_size) +
                                         LIBBSC_HEADER_SIZE);

  uint32_t pos_in_str_array = 0;
  uint32_t pos_in_current_str = 0;
  while (true) {
    int data_size = 0;
    while (data_size < block_size && pos_in_str_array < num_strings) {
      if (pos_in_current_str == string_lengths[pos_in_str_array]) {
        ++pos_in_str_array;
        pos_in_current_str = 0;
        continue;
      }
      const uint32_t remaining_in_string =
          string_lengths[pos_in_str_array] - pos_in_current_str;
      const int remaining_in_block = block_size - data_size;
      const uint32_t copy_size =
          (std::min)(remaining_in_string,
                     static_cast<uint32_t>(remaining_in_block));
      std::memcpy(read_buffer.data() + data_size,
                  string_array[pos_in_str_array].data() + pos_in_current_str,
                  copy_size);
      pos_in_current_str += copy_size;
      data_size += static_cast<int>(copy_size);
    }

    if (data_size == 0) {
      break;
    }

    signed char record_size = 1;
    signed char sorting_contexts = LIBBSC_CONTEXTS_FOLLOWING;
    std::memcpy(work_buffer.data(), read_buffer.data(),
                static_cast<size_t>(data_size));
    int compressed_size =
        bsc_compress(work_buffer.data(), work_buffer.data(), data_size, 16, 128,
                     LIBBSC_BLOCKSORTER_BWT, LIBBSC_CODER_QLFC_STATIC,
                     LIBBSC_DEFAULT_FEATURES);
    if (compressed_size == LIBBSC_NOT_COMPRESSIBLE) {
      sorting_contexts = LIBBSC_CONTEXTS_FOLLOWING;
      record_size = 1;
      std::memcpy(work_buffer.data(), read_buffer.data(),
                  static_cast<size_t>(data_size));
      compressed_size = bsc_store(work_buffer.data(), work_buffer.data(),
                                  data_size, LIBBSC_DEFAULT_FEATURES);
    }
    if (compressed_size < LIBBSC_NO_ERROR) {
      throw std::runtime_error("Failed to compress BSC string array block.");
    }

    append_binary(output_bytes,
                  bsc_str_array_block_header{record_size, sorting_contexts});
    const size_t old_size = output_bytes.size();
    output_bytes.resize(old_size + static_cast<size_t>(compressed_size));
    std::memcpy(output_bytes.data() + old_size, work_buffer.data(),
                static_cast<size_t>(compressed_size));
  }

  return output_bytes;
}

void decompress_id_block_bytes(std::string_view input_bytes,
                               std::string_view input_label,
                               std::string *id_array, const uint32_t &num_ids,
                               bool pack_only) {
  if (num_ids == 0)
    return;

  SPRING_LOG_DEBUG(
      "block_id=io-utils:id-decompress, decompress_id_block_bytes start: "
      "input=" +
      std::string(input_label) + ", num_ids=" + std::to_string(num_ids) +
      ", pack_only=" + std::string(pack_only ? "true" : "false"));

  std::vector<char> decoded_bytes;
  if (pack_only) {
    decoded_bytes.assign(input_bytes.begin(), input_bytes.end());
  } else {
    decoded_bytes = bsc_decompress_bytes(
        std::vector<char>(input_bytes.begin(), input_bytes.end()));
  }

  if (pack_only) {
    size_t line_start = 0;
    for (uint32_t i = 0; i < num_ids; i++) {
      if (line_start > decoded_bytes.size()) {
        SPRING_LOG_DEBUG("block_id=io-utils:id-decompress, "
                         "decompress_id_block_bytes pack-only decode "
                         "failure: path=" +
                         std::string(input_label) +
                         ", expected_bytes=" + std::to_string(num_ids) +
                         ", actual_bytes=" + std::to_string(i) +
                         ", index=" + std::to_string(i));
        throw std::runtime_error("Failed to decode raw ID block.");
      }
      const char *begin = decoded_bytes.data() + line_start;
      const size_t remaining = decoded_bytes.size() - line_start;
      const void *newline_ptr = std::memchr(begin, '\n', remaining);
      if (newline_ptr == nullptr) {
        if (i + 1 != num_ids) {
          SPRING_LOG_DEBUG("block_id=io-utils:id-decompress, "
                           "decompress_id_block_bytes pack-only decode "
                           "failure: path=" +
                           std::string(input_label) +
                           ", expected_bytes=" + std::to_string(num_ids) +
                           ", actual_bytes=" + std::to_string(i) +
                           ", index=" + std::to_string(i));
          throw std::runtime_error("Failed to decode raw ID block.");
        }
        id_array[i].assign(begin, remaining);
        line_start = decoded_bytes.size();
        continue;
      }
      const size_t line_len =
          static_cast<size_t>(static_cast<const char *>(newline_ptr) - begin);
      id_array[i].assign(begin, line_len);
      line_start += line_len + 1;
    }
    SPRING_LOG_DEBUG("block_id=io-utils:id-decompress, "
                     "decompress_id_block_bytes pack-only done: input=" +
                     std::string(input_label));
    return;
  }

  const char *curr = decoded_bytes.data();
  const char *end = curr + decoded_bytes.size();
  const std::string block_path(input_label);
  auto read_u32 = [&]() {
    if (curr + 4 > end) {
      const auto remaining = static_cast<uint64_t>(end - curr);
      SPRING_LOG_DEBUG("block_id=io-utils:id-decompress, decompress_id_block "
                       "metadata truncation: path=" +
                       block_path + ", expected_bytes=4, actual_bytes=" +
                       std::to_string(remaining) + ", index=0");
      throw std::runtime_error("Truncated ID block metadata");
    }
    uint32_t val;
    std::memcpy(&val, curr, 4);
    curr += 4;
    return val;
  };

  uint32_t num_nalpha = read_u32();
  uint32_t num_alpha = read_u32();
  uint32_t count_sz = read_u32();
  if (count_sz != num_ids) {
    SPRING_LOG_DEBUG(
        "block_id=io-utils:id-decompress, decompress_id_block count mismatch: "
        "path=" +
        block_path + ", expected_bytes=" + std::to_string(num_ids) +
        ", actual_bytes=" + std::to_string(count_sz) + ", index=0");
    throw std::runtime_error("ID block mismatch in count");
  }

  if (curr + count_sz > end) {
    const auto remaining = static_cast<uint64_t>(end - curr);
    SPRING_LOG_DEBUG(
        "block_id=io-utils:id-decompress, decompress_id_block counts "
        "truncation: path=" +
        block_path + ", expected_bytes=" + std::to_string(count_sz) +
        ", actual_bytes=" + std::to_string(remaining) + ", index=0");
    throw std::runtime_error("Truncated ID counts");
  }
  const uint8_t *counts_ptr = reinterpret_cast<const uint8_t *>(curr);
  curr += count_sz;

  std::vector<const char *> nalpha_ptrs(num_nalpha);
  for (uint32_t i = 0; i < num_nalpha; ++i) {
    uint32_t len = read_u32();
    if (curr + len > end) {
      const auto remaining = static_cast<uint64_t>(end - curr);
      SPRING_LOG_DEBUG("block_id=io-utils:id-decompress, decompress_id_block "
                       "non-alpha truncation: path=" +
                       block_path + ", expected_bytes=" + std::to_string(len) +
                       ", actual_bytes=" + std::to_string(remaining) +
                       ", index=" + std::to_string(i));
      throw std::runtime_error("Truncated ID non-alpha column");
    }
    nalpha_ptrs[i] = curr;
    curr += len;
  }

  std::vector<const char *> alpha_ptrs(num_alpha);
  std::vector<uint32_t> alpha_lens(num_alpha);
  std::vector<uint8_t> alpha_fmts(num_alpha);
  std::vector<uint32_t> alpha_last_vals(num_alpha, 0);
  std::vector<uint32_t> alpha_col_idx(num_alpha, 0);

  for (uint32_t i = 0; i < num_alpha; ++i) {
    if (curr + 1 > end) {
      const auto remaining = static_cast<uint64_t>(end - curr);
      SPRING_LOG_DEBUG("block_id=io-utils:id-decompress, decompress_id_block "
                       "alpha fmt truncation: path=" +
                       block_path + ", expected_bytes=1, actual_bytes=" +
                       std::to_string(remaining) +
                       ", index=" + std::to_string(i));
      throw std::runtime_error("Truncated ID alpha column fmt");
    }
    alpha_fmts[i] = *curr++;
    uint32_t len = read_u32();
    if (curr + len > end) {
      const auto remaining = static_cast<uint64_t>(end - curr);
      SPRING_LOG_DEBUG("block_id=io-utils:id-decompress, decompress_id_block "
                       "alpha truncation: path=" +
                       block_path + ", expected_bytes=" + std::to_string(len) +
                       ", actual_bytes=" + std::to_string(remaining) +
                       ", index=" + std::to_string(i));
      throw std::runtime_error("Truncated ID alpha column");
    }
    alpha_lens[i] = len;
    alpha_ptrs[i] = curr;
    curr += len;
  }

  for (uint32_t i = 0; i < num_ids; i++) {
    id_array[i].clear();
    uint8_t count = counts_ptr[i];
    for (uint8_t c = 0; c < count; ++c) {
      // read non-alpha
      const char *str = nalpha_ptrs[c];
      while (*str) {
        id_array[i].push_back(*str++);
      }
      nalpha_ptrs[c] = str + 1;

      // read alpha
      uint8_t fmt = alpha_fmts[c];
      if (fmt == 0) {
        str = alpha_ptrs[c];
        while (*str) {
          id_array[i].push_back(*str++);
        }
        alpha_ptrs[c] = str + 1;
      } else {
        uint32_t idx = alpha_col_idx[c]++;
        const uint8_t *raw = reinterpret_cast<const uint8_t *>(alpha_ptrs[c]);
        uint32_t n = alpha_lens[c] / 4;
        uint8_t b0 = raw[idx];
        uint8_t b1 = raw[n + idx];
        uint8_t b2 = raw[2 * n + idx];
        uint8_t b3 = raw[3 * n + idx];
        uint32_t udelta = static_cast<uint32_t>(b0) |
                          (static_cast<uint32_t>(b1) << 8) |
                          (static_cast<uint32_t>(b2) << 16) |
                          (static_cast<uint32_t>(b3) << 24);
        int32_t delta;
        std::memcpy(&delta, &udelta, sizeof(int32_t));

        if (delta != INT32_MIN) {
          uint32_t val = alpha_last_vals[c] + static_cast<uint32_t>(delta);
          alpha_last_vals[c] = val;
          id_array[i].append(std::to_string(val));
        }
      }
    }
  }
  SPRING_LOG_DEBUG("block_id=io-utils:id-decompress, decompress_id_block done: "
                   "decoded_ids=" +
                   std::to_string(num_ids));
}

void quantize_quality(std::string *quality_array, const uint32_t &num_lines,
                      char *quantization_table) {
  for (uint32_t i = 0; i < num_lines; i++) {
    for (char &c : quality_array[i]) {
      c = quantization_table[static_cast<uint8_t>(c)];
    }
  }
}

std::vector<char> bsc_compress_bytes(const std::vector<char> &input_bytes) {
  if (input_bytes.empty()) {
    return {};
  }

  ensure_libbsc_ready();

  if (input_bytes.size() >
      static_cast<size_t>(std::numeric_limits<int>::max()) *
          compression_params::ReadMetadata::kFileLenThrSize) {
    throw std::runtime_error("Input too large for in-memory BSC compression.");
  }

  const size_t nblocks =
      (input_bytes.size() + kBscArchiveBlockSize - 1) / kBscArchiveBlockSize;
  if (nblocks > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("Too many blocks for in-memory BSC archive.");
  }

  std::vector<char> output_bytes;
  output_bytes.reserve(sizeof(int) + input_bytes.size());
  append_binary(output_bytes, static_cast<int>(nblocks));

  size_t block_offset = 0;
  while (block_offset < input_bytes.size()) {
    const int data_size = static_cast<int>(
        (std::min)(kBscArchiveBlockSize, input_bytes.size() - block_offset));
    std::vector<unsigned char> compressed(static_cast<size_t>(data_size) +
                                          LIBBSC_HEADER_SIZE);
    int block_size =
        bsc_compress(reinterpret_cast<const unsigned char *>(
                         input_bytes.data() + block_offset),
                     compressed.data(), data_size, 0, 0, LIBBSC_BLOCKSORTER_BWT,
                     LIBBSC_CODER_QLFC_STATIC, LIBBSC_DEFAULT_FEATURES);
    if (block_size == LIBBSC_NOT_COMPRESSIBLE) {
      block_size =
          bsc_store(reinterpret_cast<const unsigned char *>(input_bytes.data() +
                                                            block_offset),
                    compressed.data(), data_size, LIBBSC_DEFAULT_FEATURES);
    }
    if (block_size < LIBBSC_NO_ERROR) {
      throw std::runtime_error("libbsc compression failed for in-memory data.");
    }

    const bsc_archive_block_header header = {
        static_cast<long long>(block_offset),
        static_cast<signed char>(1),
        static_cast<signed char>(LIBBSC_CONTEXTS_FOLLOWING),
    };
    append_binary(output_bytes, header);
    const size_t old_size = output_bytes.size();
    output_bytes.resize(old_size + static_cast<size_t>(block_size));
    std::memcpy(output_bytes.data() + old_size, compressed.data(),
                static_cast<size_t>(block_size));
    block_offset += static_cast<size_t>(data_size);
  }

  return output_bytes;
}

std::vector<char> bsc_decompress_bytes(const std::vector<char> &input_bytes) {
  if (input_bytes.empty()) {
    return {};
  }

  ensure_libbsc_ready();

  if (input_bytes.size() < sizeof(int)) {
    throw std::runtime_error("Compressed BSC byte stream is too small.");
  }

  size_t cursor = 0;
  int nblocks = 0;
  std::memcpy(&nblocks, input_bytes.data(), sizeof(int));
  cursor += sizeof(int);
  if (nblocks < 0) {
    throw std::runtime_error(
        "Compressed BSC byte stream has invalid block count.");
  }

  std::vector<char> output_bytes;
  int parsed_blocks = 0;
  while (cursor < input_bytes.size()) {
    if (cursor + sizeof(bsc_archive_block_header) > input_bytes.size()) {
      throw std::runtime_error(
          "Corrupt BSC byte stream: truncated block header.");
    }

    bsc_archive_block_header header{};
    std::memcpy(&header, input_bytes.data() + cursor,
                sizeof(bsc_archive_block_header));
    cursor += sizeof(bsc_archive_block_header);

    if (header.record_size < 1) {
      throw std::runtime_error("Corrupt BSC byte stream: invalid record size.");
    }
    if (header.sorting_contexts != LIBBSC_CONTEXTS_FOLLOWING &&
        header.sorting_contexts != LIBBSC_CONTEXTS_PRECEDING) {
      throw std::runtime_error(
          "Corrupt BSC byte stream: invalid sorting context.");
    }
    if (header.block_offset < 0) {
      throw std::runtime_error(
          "Corrupt BSC byte stream: negative block offset.");
    }
    if (cursor + LIBBSC_HEADER_SIZE > input_bytes.size()) {
      throw std::runtime_error(
          "Corrupt BSC byte stream: truncated libbsc block header.");
    }

    int block_size = 0;
    int data_size = 0;
    if (bsc_block_info(reinterpret_cast<const unsigned char *>(
                           input_bytes.data() + cursor),
                       LIBBSC_HEADER_SIZE, &block_size, &data_size,
                       LIBBSC_DEFAULT_FEATURES) != LIBBSC_NO_ERROR) {
      throw std::runtime_error(
          "Corrupt BSC byte stream: invalid libbsc block metadata.");
    }
    if (block_size < LIBBSC_HEADER_SIZE || data_size < 0) {
      throw std::runtime_error("Corrupt BSC byte stream: invalid block sizes.");
    }
    if (cursor + static_cast<size_t>(block_size) > input_bytes.size()) {
      throw std::runtime_error(
          "Corrupt BSC byte stream: truncated compressed block.");
    }

    std::vector<unsigned char> block_bytes(
        static_cast<size_t>((std::max)(block_size, data_size)));
    std::memcpy(block_bytes.data(), input_bytes.data() + cursor,
                static_cast<size_t>(block_size));
    cursor += static_cast<size_t>(block_size);

    const int decompress_result =
        bsc_decompress(block_bytes.data(), block_size, block_bytes.data(),
                       data_size, LIBBSC_DEFAULT_FEATURES);
    if (decompress_result < LIBBSC_NO_ERROR) {
      throw std::runtime_error(
          "libbsc decompression failed for in-memory data.");
    }

    if (header.sorting_contexts == LIBBSC_CONTEXTS_PRECEDING) {
      const int reverse_result = bsc_reverse_block(
          block_bytes.data(), data_size, LIBBSC_DEFAULT_FEATURES);
      if (reverse_result != LIBBSC_NO_ERROR) {
        throw std::runtime_error(
            "libbsc reverse-block failed for in-memory data.");
      }
    }

    if (header.record_size > 1) {
      const int reorder_result =
          bsc_reorder_reverse(block_bytes.data(), data_size, header.record_size,
                              LIBBSC_DEFAULT_FEATURES);
      if (reorder_result != LIBBSC_NO_ERROR) {
        throw std::runtime_error(
            "libbsc reorder-reverse failed for in-memory data.");
      }
    }

    const size_t output_offset = static_cast<size_t>(header.block_offset);
    if (output_offset + static_cast<size_t>(data_size) > output_bytes.size()) {
      output_bytes.resize(output_offset + static_cast<size_t>(data_size));
    }
    std::memcpy(output_bytes.data() + output_offset, block_bytes.data(),
                static_cast<size_t>(data_size));
    parsed_blocks++;
  }

  if (parsed_blocks != nblocks) {
    throw std::runtime_error(
        "Corrupt BSC byte stream: block count does not match archive header.");
  }

  return output_bytes;
}

void safe_bsc_str_array_decompress_bytes(std::string_view input_bytes,
                                         std::string_view input_label,
                                         std::string *string_array,
                                         uint32_t num_strings,
                                         uint32_t *string_lengths) {
  SPRING_LOG_DEBUG(
      "block_id=io-utils:bsc-array, safe_bsc_str_array_decompress_bytes "
      "start: input=" +
      std::string(input_label) +
      ", num_strings=" + std::to_string(num_strings));

  ensure_libbsc_ready();

  size_t cursor = 0;
  const int nblocks =
      read_binary_or_throw<int>(input_bytes, cursor, "string-array block count",
                                std::string(input_label));
  if (nblocks < 0) {
    throw std::runtime_error("Corrupt BSC byte stream for " +
                             std::string(input_label) +
                             ": invalid block count.");
  }

  uint32_t pos_in_str_array = 0;
  uint32_t pos_in_current_str = 0;
  for (uint32_t string_index = 0; string_index < num_strings; ++string_index) {
    if (string_lengths[string_index] == 0) {
      string_array[string_index].clear();
    }
  }
  if (num_strings > 0) {
    string_array[0].resize(string_lengths[0]);
  }

  std::vector<unsigned char> buffer;
  int parsed_blocks = 0;
  while (cursor < input_bytes.size()) {
    signed char record_size = read_binary_or_throw<signed char>(
        input_bytes, cursor, "record size", std::string(input_label));
    if (record_size < 1) {
      throw std::runtime_error("Corrupt BSC byte stream for " +
                               std::string(input_label) +
                               ": invalid record size.");
    }

    signed char sorting_contexts = read_binary_or_throw<signed char>(
        input_bytes, cursor, "sorting contexts", std::string(input_label));
    if (sorting_contexts != LIBBSC_CONTEXTS_FOLLOWING &&
        sorting_contexts != LIBBSC_CONTEXTS_PRECEDING) {
      throw std::runtime_error("Corrupt BSC byte stream for " +
                               std::string(input_label) +
                               ": invalid sorting context.");
    }

    if (cursor + LIBBSC_HEADER_SIZE > input_bytes.size()) {
      throw std::runtime_error("Corrupt BSC byte stream for " +
                               std::string(input_label) +
                               ": truncated libbsc block header.");
    }

    int block_size = 0;
    int data_size = 0;
    if (bsc_block_info(reinterpret_cast<const unsigned char *>(
                           input_bytes.data() + cursor),
                       LIBBSC_HEADER_SIZE, &block_size, &data_size,
                       LIBBSC_DEFAULT_FEATURES) != LIBBSC_NO_ERROR) {
      throw std::runtime_error("Corrupt BSC byte stream for " +
                               std::string(input_label) +
                               ": invalid libbsc block metadata.");
    }
    if (block_size < LIBBSC_HEADER_SIZE || data_size < 0) {
      throw std::runtime_error("Corrupt BSC byte stream for " +
                               std::string(input_label) +
                               ": invalid block sizes.");
    }
    if (cursor + static_cast<size_t>(block_size) > input_bytes.size()) {
      throw std::runtime_error("Corrupt BSC byte stream for " +
                               std::string(input_label) +
                               ": truncated compressed block.");
    }

    buffer.resize(static_cast<size_t>((std::max)(block_size, data_size)));
    std::memcpy(buffer.data(), input_bytes.data() + cursor,
                static_cast<size_t>(block_size));
    cursor += static_cast<size_t>(block_size);

    const int decompress_result =
        bsc_decompress(buffer.data(), block_size, buffer.data(), data_size,
                       LIBBSC_DEFAULT_FEATURES);
    if (decompress_result < LIBBSC_NO_ERROR) {
      throw std::runtime_error("libbsc string-array decompression failed for " +
                               std::string(input_label) + ".");
    }

    if (sorting_contexts == LIBBSC_CONTEXTS_PRECEDING) {
      const int reverse_result =
          bsc_reverse_block(buffer.data(), data_size, LIBBSC_DEFAULT_FEATURES);
      if (reverse_result != LIBBSC_NO_ERROR) {
        throw std::runtime_error(
            "libbsc reverse-block failed for string-array data: " +
            std::string(input_label));
      }
    }

    if (record_size > 1) {
      const int reorder_result = bsc_reorder_reverse(
          buffer.data(), data_size, record_size, LIBBSC_DEFAULT_FEATURES);
      if (reorder_result != LIBBSC_NO_ERROR) {
        throw std::runtime_error(
            "libbsc reorder-reverse failed for string-array data: " +
            std::string(input_label));
      }
    }

    write_string_array_bytes_or_throw(buffer.data(), data_size, string_array,
                                      num_strings, string_lengths,
                                      pos_in_str_array, pos_in_current_str);
    parsed_blocks++;
  }

  if (parsed_blocks != nblocks) {
    throw std::runtime_error("Corrupt BSC byte stream for " +
                             std::string(input_label) +
                             ": block count does not match archive header.");
  }

  if (num_strings > 0 &&
      (pos_in_str_array != num_strings - 1 ||
       pos_in_current_str != string_lengths[num_strings - 1])) {
    throw std::runtime_error("Corrupt BSC byte stream for " +
                             std::string(input_label) +
                             ": decoded string payload length mismatch.");
  }

  SPRING_LOG_DEBUG(
      "block_id=io-utils:bsc-array, safe_bsc_str_array_decompress_bytes "
      "done: input=" +
      std::string(input_label));
}

void generate_illumina_binning_table(char *illumina_binning_table) {
  for (int i = 0; i < 128; i++) {
    if (i <= 33 + 1)
      illumina_binning_table[i] = (char)i;
    else if (i <= 33 + 9)
      illumina_binning_table[i] = (char)(33 + 6);
    else if (i <= 33 + 19)
      illumina_binning_table[i] = (char)(33 + 15);
    else if (i <= 33 + 24)
      illumina_binning_table[i] = (char)(33 + 22);
    else if (i <= 33 + 29)
      illumina_binning_table[i] = (char)(33 + 27);
    else if (i <= 33 + 34)
      illumina_binning_table[i] = (char)(33 + 33);
    else if (i <= 33 + 39)
      illumina_binning_table[i] = (char)(33 + 37);
    else
      illumina_binning_table[i] = (char)(33 + 40);
  }
}

void generate_binary_binning_table(char *binary_binning_table,
                                   const unsigned int thr,
                                   const unsigned int high,
                                   const unsigned int low) {
  unsigned int split = 33 + thr;
  for (unsigned int i = 0; i < split; i++)
    binary_binning_table[i] = static_cast<char>(33 + low);
  for (unsigned int i = split; i <= 127; i++)
    binary_binning_table[i] = static_cast<char>(33 + high);
}

void extract_gzip_detailed_info(const std::string &path, bool &is_gzipped,
                                uint8_t &flg, uint32_t &mtime, uint8_t &xfl,
                                uint8_t &os, std::string &name, bool &is_bgzf,
                                uint16_t &bgzf_block_size,
                                uint64_t &uncompressed_size,
                                uint64_t &compressed_size,
                                uint32_t &member_count) {
  is_gzipped = false;
  is_bgzf = false;
  flg = 0;
  mtime = 0;
  xfl = 0;
  os = 0;
  name.clear();
  bgzf_block_size = 0;
  uncompressed_size = 0;
  compressed_size = 0;
  member_count = 0;

  std::ifstream fin(path, std::ios::binary);
  if (!fin)
    return;

  fin.seekg(0, std::ios::end);
  compressed_size = fin.tellg();
  fin.seekg(0, std::ios::beg);

  while (true) {
    unsigned char header[10];
    if (!fin.read(reinterpret_cast<char *>(header), 10))
      break;
    if (header[0] != 0x1f || header[1] != 0x8b) {
      if (member_count == 0)
        return;
      break;
    }
    is_gzipped = true;
    member_count++;
    const uint8_t current_flg = header[3];
    uint16_t current_bsiz = 0;

    if (current_flg & 0x04) {
      uint16_t xlen;
      fin.read(reinterpret_cast<char *>(&xlen), 2);
      const std::streampos extra_start = fin.tellg();
      while (fin.tellg() - extra_start < xlen) {
        char si1, si2;
        uint16_t slen;
        fin.read(&si1, 1);
        fin.read(&si2, 1);
        fin.read(reinterpret_cast<char *>(&slen), 2);
        if (si1 == 'B' && si2 == 'C' && slen == 2) {
          fin.read(reinterpret_cast<char *>(&current_bsiz), 2);
          if (member_count == 1) {
            is_bgzf = true;
            bgzf_block_size = current_bsiz + 1;
          }
        } else
          fin.seekg(slen, std::ios::cur);
      }
    }
    if (current_flg & 0x08) {
      char c;
      while (fin.read(&c, 1) && c != '\0') {
        if (member_count == 1)
          name += c;
      }
    }
    if (current_flg & 0x10) {
      char c;
      while (fin.read(&c, 1) && c != '\0')
        ;
    }
    if (current_flg & 0x02)
      fin.seekg(2, std::ios::cur);

    uncompressed_size = count_gzip_uncompressed_bytes(path);
    break;
  }
}

} // namespace spring
