// Writes reordered alignment-related streams, unaligned payloads, and per-block
// compressed outputs that become part of the final Spring archive.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <mutex>
#include <omp.h>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <vector>

#include "libbsc/libbsc.h"
#include "params.h"
#include "progress.h"
#include "stream_reordering.h"

namespace spring {

namespace {

#pragma pack(push, 1)
struct bsc_block_header {
  long long block_offset;
  signed char record_size;
  signed char sorting_contexts;
};
#pragma pack(pop)

struct reordered_stream_paths {
  std::string flag_path;
  std::string position_path;
  std::string mate_position_path;
  std::string orientation_path;
  std::string mate_orientation_path;
  std::string read_length_path;
  std::string unaligned_path;
  std::string noise_path;
  std::string noise_position_path;
  std::string order_path;
};

struct output_block_buffers {
  std::vector<char> flag_bytes;
  std::vector<char> position_bytes;
  std::vector<char> noise_bytes;
  std::vector<char> noise_position_bytes;
  std::vector<char> orientation_bytes;
  std::vector<char> unaligned_bytes;
  std::vector<char> read_length_bytes;
  std::vector<char> mate_position_bytes;
  std::vector<char> mate_orientation_bytes;
};

#pragma pack(push, 1)
struct staged_stream_record_header {
  uint32_t relative_slot;
  uint16_t read_length;
  uint16_t noise_count;
  uint8_t flags;
  char orientation;
  uint64_t position;
};
#pragma pack(pop)

// Fraction of the compression memory budget the scatter phase may hold as
// in-RAM per-block buffers. Kept conservative so those buffers can coexist
// with the parallel rebuild's working set and the accumulated compressed
// block outputs without risking OOM.
constexpr double kScatterBufferBudgetFraction = 0.4;
// Fallback cap when no memory budget is supplied, so records are still batched
// into large flushes instead of one open/close per read.
constexpr uint64_t kDefaultScatterBufferCapBytes = 4ULL * 1024 * 1024 * 1024;

struct staged_read_state {
  bool present = false;
  bool aligned = false;
  uint16_t read_length = 0;
  char orientation = '\0';
  uint64_t position = 0;
  std::string noise_codes;
  std::vector<uint16_t> noise_positions;
  std::string unaligned_bytes;
};

struct staged_pair_slot_state {
  staged_read_state read_1;
  staged_read_state read_2;
};

struct block_range {
  uint64_t begin;
  uint64_t end;
  bool valid;
};

template <typename T>
void append_binary(std::vector<char> &buffer, const T &value);

std::unordered_map<std::string, std::string>
compress_output_block(const output_block_buffers &block_buffers,
                      const reordered_stream_paths &paths,
                      const uint64_t block_num, const bool paired_end);

std::string block_file_path(const std::string &base_path,
                            const uint64_t block_num) {
  return base_path + '.' + std::to_string(block_num);
}

std::string compressed_block_file_path(const std::string &base_path,
                                       const uint64_t block_num) {
  return block_file_path(base_path, block_num) + ".bsc";
}

std::string stream_scratch_block_path(const std::string &scratch_root_dir,
                                      const uint64_t block_num) {
  return (std::filesystem::path(scratch_root_dir) /
          ("block." + std::to_string(block_num) + ".bin"))
      .string();
}

void reset_directory(const std::string &path) {
  std::error_code remove_ec;
  std::filesystem::remove_all(path, remove_ec);
  if (remove_ec) {
    throw std::runtime_error("Failed to clear stream scratch directory '" +
                             path + "': " + remove_ec.message());
  }

  std::error_code create_ec;
  std::filesystem::create_directories(path, create_ec);
  if (create_ec) {
    throw std::runtime_error("Failed to create stream scratch directory '" +
                             path + "': " + create_ec.message());
  }
}

// Appends a fully-assembled per-block scatter buffer to its scratch file with a
// single open/write/close. Records within a block are order-independent (the
// rebuild places each by its relative slot), so buffers may be flushed in any
// number of batches.
void append_buffer_to_scratch(const std::string &path,
                              const std::string &bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output.is_open()) {
    throw std::runtime_error("Failed to open stream scratch file '" + path +
                             "'.");
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("Failed to append stream scratch file '" + path +
                             "'.");
  }
}

// Reads an entire spilled scratch block into memory, returning empty bytes when
// the block produced no records (its file was never created).
std::string read_scratch_block_bytes(const std::string &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input.is_open()) {
    return {};
  }
  const std::streamoff size = input.tellg();
  if (size <= 0) {
    return {};
  }
  std::string bytes(static_cast<size_t>(size), '\0');
  input.seekg(0);
  input.read(bytes.data(), size);
  if (!input) {
    throw std::runtime_error("Failed to read stream scratch block '" + path +
                             "'.");
  }
  return bytes;
}

// Read-only stream buffer over an existing byte range, letting the block parser
// consume in-RAM scatter buffers and spilled files through one code path
// without copying.
struct span_streambuf : std::streambuf {
  span_streambuf(const char *base, size_t size) {
    char *first = const_cast<char *>(base);
    setg(first, first, first + size);
  }
};

uint16_t read_uint16(std::ifstream &input, const char *label) {
  uint16_t value = 0;
  input.read(reinterpret_cast<char *>(&value), sizeof(value));
  if (!input) {
    throw std::runtime_error(std::string("Failed to read ") + label + ".");
  }
  return value;
}

uint32_t read_uint32(std::ifstream &input, const char *label) {
  uint32_t value = 0;
  input.read(reinterpret_cast<char *>(&value), sizeof(value));
  if (!input) {
    throw std::runtime_error(std::string("Failed to read ") + label + ".");
  }
  return value;
}

uint64_t read_uint64(std::ifstream &input, const char *label) {
  uint64_t value = 0;
  input.read(reinterpret_cast<char *>(&value), sizeof(value));
  if (!input) {
    throw std::runtime_error(std::string("Failed to read ") + label + ".");
  }
  return value;
}

char read_char(std::ifstream &input, const char *label) {
  char value = '\0';
  input.read(&value, sizeof(value));
  if (!input) {
    throw std::runtime_error(std::string("Failed to read ") + label + ".");
  }
  return value;
}

void write_noise_for_state(std::vector<char> &noise_output,
                           std::vector<char> &noise_position_output,
                           const staged_read_state &state) {
  noise_output.insert(noise_output.end(), state.noise_codes.begin(),
                      state.noise_codes.end());
  for (const uint16_t position : state.noise_positions) {
    append_binary(noise_position_output, position);
  }
  noise_output.push_back('\n');
}

void write_unaligned_state(std::vector<char> &unaligned_output,
                           const staged_read_state &state) {
  unaligned_output.insert(unaligned_output.end(), state.unaligned_bytes.begin(),
                          state.unaligned_bytes.end());
}

void ensure_libbsc_ready() {
  static std::once_flag init_once;
  std::call_once(init_once, []() {
    const int init_result = ::bsc_init(LIBBSC_DEFAULT_FEATURES);
    if (init_result != LIBBSC_NO_ERROR) {
      throw std::runtime_error("Failed to initialize libbsc.");
    }
  });
}

std::string compress_block_buffer(const std::vector<char> &input_bytes,
                                  const std::string &output_path) {
  if (input_bytes.empty()) {
    return {};
  }

  ensure_libbsc_ready();
  if (input_bytes.size() >
      static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("Block too large for libbsc compressor: " +
                             output_path);
  }

  std::vector<unsigned char> compressed(input_bytes.size() +
                                        LIBBSC_HEADER_SIZE);
  int compressed_size = ::bsc_compress(
      reinterpret_cast<const unsigned char *>(input_bytes.data()),
      compressed.data(), static_cast<int>(input_bytes.size()), 16, 128,
      LIBBSC_BLOCKSORTER_BWT, LIBBSC_CODER_QLFC_STATIC,
      LIBBSC_DEFAULT_FEATURES);
  if (compressed_size == LIBBSC_NOT_COMPRESSIBLE) {
    compressed_size =
        ::bsc_store(reinterpret_cast<const unsigned char *>(input_bytes.data()),
                    compressed.data(), static_cast<int>(input_bytes.size()),
                    LIBBSC_DEFAULT_FEATURES);
  }
  if (compressed_size < LIBBSC_NO_ERROR) {
    throw std::runtime_error("libbsc compression failed for block: " +
                             output_path);
  }

  const int nblocks = 1;
  const bsc_block_header header = {
      0,
      1,
      static_cast<signed char>(1) /* LIBBSC_CONTEXTS_FOLLOWING */,
  };

  std::string output_bytes;
  output_bytes.resize(sizeof(nblocks) + sizeof(header) + compressed_size);
  size_t cursor = 0;
  std::memcpy(output_bytes.data() + cursor, &nblocks, sizeof(nblocks));
  cursor += sizeof(nblocks);
  std::memcpy(output_bytes.data() + cursor, &header, sizeof(header));
  cursor += sizeof(header);
  std::memcpy(output_bytes.data() + cursor, compressed.data(), compressed_size);
  return output_bytes;
}

reordered_stream_paths build_reordered_stream_paths() {
  return {.flag_path = "read_flag.txt",
          .position_path = "read_pos.bin",
          .mate_position_path = "read_pos_pair.bin",
          .orientation_path = "read_rev.txt",
          .mate_orientation_path = "read_rev_pair.txt",
          .read_length_path = "read_lengths.bin",
          .unaligned_path = "read_unaligned.txt",
          .noise_path = "read_noise.txt",
          .noise_position_path = "read_noisepos.bin",
          .order_path = "read_order.bin"};
}

block_range block_read_range(const uint64_t block_num,
                             const uint32_t num_reads_per_block,
                             const uint64_t read_limit) {
  const uint64_t begin = block_num * num_reads_per_block;
  if (begin >= read_limit) {
    return {.begin = 0, .end = 0, .valid = false};
  }

  const uint64_t end =
      std::min((block_num + 1) * uint64_t(num_reads_per_block), read_limit);
  return {.begin = begin, .end = end, .valid = true};
}

template <typename T>
void append_binary(std::vector<char> &buffer, const T &value) {
  const size_t old_size = buffer.size();
  buffer.resize(old_size + sizeof(T));
  std::memcpy(buffer.data() + old_size, &value, sizeof(T));
}

void decode_unaligned_reads(const std::vector<char> &encoded,
                            const uint32_t expected_read_count,
                            std::vector<char> &decoded_chars,
                            std::vector<uint16_t> &decoded_lengths) {
  decoded_lengths.assign(expected_read_count, 0);
  std::vector<uint64_t> encoded_offsets(expected_read_count, 0);
  std::vector<uint64_t> decoded_offsets(expected_read_count, 0);

  uint64_t encoded_cursor = 0;
  uint64_t decoded_total = 0;
  for (uint32_t read_index = 0; read_index < expected_read_count;
       ++read_index) {
    if (encoded_cursor + sizeof(uint32_t) > encoded.size()) {
      throw std::runtime_error(
          "Corrupted unaligned stream: truncated read length header.");
    }

    uint32_t read_length = 0;
    std::memcpy(&read_length, encoded.data() + encoded_cursor,
                sizeof(uint32_t));
    encoded_cursor += sizeof(uint32_t);

    const uint64_t encoded_bytes = static_cast<uint64_t>(read_length);
    if (encoded_cursor + encoded_bytes > encoded.size()) {
      throw std::runtime_error(
          "Corrupted unaligned stream: truncated raw payload.");
    }

    decoded_lengths[read_index] = static_cast<uint16_t>(read_length);
    encoded_offsets[read_index] = encoded_cursor;
    decoded_offsets[read_index] = decoded_total;
    decoded_total += read_length;
    encoded_cursor += encoded_bytes;
  }

  if (encoded_cursor != encoded.size()) {
    throw std::runtime_error(
        "Corrupted unaligned stream: trailing bytes after expected records.");
  }

  decoded_chars.assign(decoded_total, 0);

#pragma omp parallel for schedule(static)
  for (size_t read_index = 0; read_index < expected_read_count; ++read_index) {
    const uint16_t read_length = decoded_lengths[read_index];
    const char *encoded_read = encoded.data() + encoded_offsets[read_index];
    char *decoded_read = decoded_chars.data() + decoded_offsets[read_index];
    std::memcpy(decoded_read, encoded_read, read_length);
  }
}

void write_noise_for_read(std::vector<char> &noise_output,
                          std::vector<char> &noise_position_output,
                          const std::vector<char> &noise_codes,
                          const std::vector<uint16_t> &noise_positions,
                          const std::vector<uint64_t> &noise_offset_by_read,
                          const std::vector<uint16_t> &noise_count_by_read,
                          const uint64_t read_index) {
  for (uint16_t noise_index = 0; noise_index < noise_count_by_read[read_index];
       noise_index++) {
    noise_output.push_back(
        noise_codes[noise_offset_by_read[read_index] + noise_index]);
    append_binary(
        noise_position_output,
        noise_positions[noise_offset_by_read[read_index] + noise_index]);
  }
  noise_output.push_back('\n');
}

void write_unaligned_read(std::vector<char> &unaligned_output,
                          const std::vector<char> &unaligned_chars,
                          const std::vector<uint64_t> &position_by_read,
                          const std::vector<uint16_t> &read_lengths_by_read,
                          const uint64_t read_index) {
  const uint64_t offset = position_by_read[read_index];
  const uint16_t read_length = read_lengths_by_read[read_index];
  unaligned_output.insert(unaligned_output.end(),
                          unaligned_chars.begin() + offset,
                          unaligned_chars.begin() + offset + read_length);
}

void write_aligned_position(std::vector<char> &position_output,
                            const bool preserve_order,
                            const bool first_read_in_block,
                            const uint64_t current_position,
                            uint64_t &previous_position) {
  if (preserve_order || first_read_in_block) {
    append_binary(position_output, current_position);
    previous_position = current_position;
    return;
  }

  const uint64_t position_delta = current_position - previous_position;
  uint16_t encoded_delta = 0;
  if (position_delta < 65535) {
    encoded_delta = static_cast<uint16_t>(position_delta);
    append_binary(position_output, encoded_delta);
  } else {
    encoded_delta = 65535;
    append_binary(position_output, encoded_delta);
    append_binary(position_output, current_position);
  }
  previous_position = current_position;
}

void add_compressed_block(std::unordered_map<std::string, std::string> &members,
                          const std::string &path,
                          const std::vector<char> &input_bytes) {
  members[path] = compress_block_buffer(input_bytes, path);
}

void partition_alignment_stream_records(
    const compression_params &cp, const std::string &artifact_root_dir,
    const std::string &read_order_entries_path,
    const std::string &scratch_root_dir, const uint64_t memory_budget_bytes,
    std::vector<std::string> &block_buffers, bool &spilled_any) {
  reset_directory(scratch_root_dir);

  const uint32_t num_reads = cp.read_info.num_reads;
  const uint32_t half_read_count = num_reads / 2;
  const bool paired_end = cp.encoding.paired_end;
  const bool preserve_order = cp.encoding.preserve_order;
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  const uint64_t read_limit = paired_end ? half_read_count : num_reads;
  const uint64_t output_blocks =
      read_limit == 0
          ? 0
          : (read_limit + static_cast<uint64_t>(num_reads_per_block) - 1) /
                static_cast<uint64_t>(num_reads_per_block);

  const std::filesystem::path artifact_root(artifact_root_dir);
  const std::filesystem::path orientation_path =
      artifact_root / "orientation_entries.bin";
  const std::filesystem::path position_path =
      artifact_root / "position_entries.bin";
  const std::filesystem::path read_length_path =
      artifact_root / "read_length_entries.bin";
  const std::filesystem::path noise_serialized_path =
      artifact_root / "noise_serialized.bin";
  const std::filesystem::path noise_positions_path =
      artifact_root / "noise_positions.bin";
  const std::filesystem::path unaligned_serialized_path =
      artifact_root / "unaligned_serialized.bin";

  const uint64_t aligned_read_count =
      std::filesystem::file_size(orientation_path) / sizeof(char);

  std::ifstream orientation_input(orientation_path, std::ios::binary);
  std::ifstream position_input(position_path, std::ios::binary);
  std::ifstream read_length_input(read_length_path, std::ios::binary);
  std::ifstream noise_serialized_input(noise_serialized_path, std::ios::binary);
  std::ifstream noise_positions_input(noise_positions_path, std::ios::binary);
  std::ifstream unaligned_input(unaligned_serialized_path, std::ios::binary);
  std::ifstream read_order_input(read_order_entries_path, std::ios::binary);
  if (!orientation_input.is_open() || !position_input.is_open() ||
      !read_length_input.is_open() || !noise_serialized_input.is_open() ||
      !noise_positions_input.is_open() || !unaligned_input.is_open() ||
      ((paired_end || preserve_order) && !read_order_input.is_open())) {
    throw std::runtime_error("Failed to open one or more spilled encoder "
                             "streams for block rebuild.");
  }

  block_buffers.assign(static_cast<size_t>(output_blocks), std::string());
  spilled_any = false;

  // Hold scatter records in RAM, capped at a conservative fraction of the
  // memory budget. When the cap is reached, flush every non-empty buffer to its
  // per-block scratch file in one append each (a few thousand large writes
  // instead of one open/close per read) and keep going in spill mode.
  const uint64_t scatter_cap_bytes =
      memory_budget_bytes > 0
          ? static_cast<uint64_t>(static_cast<double>(memory_budget_bytes) *
                                  kScatterBufferBudgetFraction)
          : kDefaultScatterBufferCapBytes;
  uint64_t buffered_bytes = 0;
  uint64_t flush_count = 0;

  auto flush_all_buffers = [&]() {
    for (uint64_t block_num = 0; block_num < output_blocks; ++block_num) {
      std::string &buffer = block_buffers[static_cast<size_t>(block_num)];
      if (buffer.empty()) {
        continue;
      }
      append_buffer_to_scratch(
          stream_scratch_block_path(scratch_root_dir, block_num), buffer);
      buffer.clear();
      buffer.shrink_to_fit();
    }
    buffered_bytes = 0;
    ++flush_count;
  };

  auto append_record = [&](const uint32_t read_order, const bool aligned,
                           const uint16_t read_length, const char orientation,
                           const uint64_t position, const std::string &payload,
                           const std::vector<uint16_t> &noise_positions,
                           const bool mate_read) {
    const uint32_t slot = paired_end && read_order >= half_read_count
                              ? read_order - half_read_count
                              : read_order;
    if (slot >= read_limit) {
      throw std::runtime_error(
          "Corruption in read order stream: slot index exceeds output limit.");
    }
    const uint64_t block_num = slot / num_reads_per_block;
    if (block_num >= output_blocks) {
      throw std::runtime_error("Corruption in read order stream: block index "
                               "exceeds output blocks.");
    }
    staged_stream_record_header header{};
    header.relative_slot = slot % num_reads_per_block;
    header.read_length = read_length;
    header.noise_count = static_cast<uint16_t>(noise_positions.size());
    header.flags =
        static_cast<uint8_t>((aligned ? 0x1 : 0x0) | (mate_read ? 0x2 : 0x0));
    header.orientation = orientation;
    header.position = position;

    std::string &buffer = block_buffers[static_cast<size_t>(block_num)];
    buffer.append(reinterpret_cast<const char *>(&header), sizeof(header));
    if (!payload.empty()) {
      buffer.append(payload.data(), payload.size());
    }
    if (!noise_positions.empty()) {
      buffer.append(reinterpret_cast<const char *>(noise_positions.data()),
                    noise_positions.size() * sizeof(uint16_t));
    }
    buffered_bytes += sizeof(header) + payload.size() +
                      noise_positions.size() * sizeof(uint16_t);

    if (buffered_bytes >= scatter_cap_bytes) {
      spilled_any = true;
      flush_all_buffers();
    }
  };

  for (uint64_t entry_index = 0; entry_index < aligned_read_count;
       ++entry_index) {
    const uint32_t read_order =
        (paired_end || preserve_order)
            ? read_uint32(read_order_input, "aligned read order entry")
            : static_cast<uint32_t>(entry_index);
    const uint16_t read_length =
        read_uint16(read_length_input, "aligned read length entry");
    const char orientation =
        read_char(orientation_input, "aligned orientation");
    const uint64_t position = read_uint64(position_input, "aligned position");

    std::string noise_codes;
    std::getline(noise_serialized_input, noise_codes);
    if (!noise_codes.empty() && noise_codes.back() == '\r') {
      noise_codes.pop_back();
    }
    std::vector<uint16_t> noise_positions(noise_codes.size());
    if (!noise_positions.empty()) {
      noise_positions_input.read(
          reinterpret_cast<char *>(noise_positions.data()),
          static_cast<std::streamsize>(noise_positions.size() *
                                       sizeof(uint16_t)));
      if (!noise_positions_input) {
        throw std::runtime_error(
            "Corruption in spilled noise position stream.");
      }
    }

    append_record(read_order, true, read_length, orientation, position,
                  noise_codes, noise_positions,
                  paired_end && read_order >= half_read_count);
  }

  const uint64_t unaligned_read_count = num_reads - aligned_read_count;
  for (uint64_t unaligned_index = 0; unaligned_index < unaligned_read_count;
       ++unaligned_index) {
    const uint64_t entry_index = aligned_read_count + unaligned_index;
    const uint32_t read_order =
        (paired_end || preserve_order)
            ? read_uint32(read_order_input, "unaligned read order entry")
            : static_cast<uint32_t>(entry_index);
    const uint16_t read_length =
        read_uint16(read_length_input, "unaligned read length entry");
    const uint32_t payload_length =
        read_uint32(unaligned_input, "unaligned payload length");
    if (payload_length != read_length) {
      throw std::runtime_error(
          "Corruption in unaligned stream: payload length mismatch.");
    }
    std::string payload(payload_length, '\0');
    if (payload_length != 0) {
      unaligned_input.read(payload.data(),
                           static_cast<std::streamsize>(payload_length));
      if (!unaligned_input) {
        throw std::runtime_error(
            "Corruption in spilled unaligned stream payload.");
      }
    }

    append_record(read_order, false, read_length, '\0', 0, payload, {},
                  paired_end && read_order >= half_read_count);
  }

  // If we ever crossed the cap the block data lives in scratch files: flush any
  // buffered tail and drop the in-RAM buffers so the rebuild reads from disk.
  // Otherwise everything stays resident and is handed to the rebuild directly.
  if (spilled_any) {
    flush_all_buffers();
    block_buffers.clear();
    block_buffers.shrink_to_fit();
  }

  SPRING_LOG_DEBUG(
      "partition_alignment_stream_records complete: output_blocks=" +
      std::to_string(output_blocks) +
      ", spilled=" + std::string(spilled_any ? "true" : "false") +
      ", flushes=" + std::to_string(flush_count) +
      ", scatter_cap_bytes=" + std::to_string(scatter_cap_bytes));
}

std::unordered_map<std::string, std::string>
rebuild_stream_blocks(const compression_params &cp,
                      const std::string &scratch_root_dir,
                      std::vector<std::string> *in_ram_blocks) {
  const reordered_stream_paths paths = build_reordered_stream_paths();
  const uint32_t num_reads = cp.read_info.num_reads;
  const uint32_t half_read_count = num_reads / 2;
  const bool paired_end = cp.encoding.paired_end;
  const bool preserve_order = cp.encoding.preserve_order;
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  const uint64_t read_limit = paired_end ? half_read_count : num_reads;
  const uint64_t output_blocks =
      read_limit == 0
          ? 0
          : (read_limit + static_cast<uint64_t>(num_reads_per_block) - 1) /
                static_cast<uint64_t>(num_reads_per_block);

  std::vector<std::unordered_map<std::string, std::string>> block_members(
      static_cast<size_t>(output_blocks));

#pragma omp parallel for schedule(dynamic)
  for (int64_t block_index = 0;
       block_index < static_cast<int64_t>(output_blocks); ++block_index) {
    const block_range current_block = block_read_range(
        static_cast<uint64_t>(block_index), num_reads_per_block, read_limit);
    const uint32_t reads_in_block =
        static_cast<uint32_t>(current_block.end - current_block.begin);
    const std::string scratch_path =
        stream_scratch_block_path(scratch_root_dir, block_index);

    // The block bytes come from the in-RAM scatter buffer when it fit in the
    // budget, otherwise from the spilled scratch file. Both share the same
    // record layout and are parsed through span_streambuf.
    std::string block_bytes =
        in_ram_blocks != nullptr
            ? std::move((*in_ram_blocks)[static_cast<size_t>(block_index)])
            : read_scratch_block_bytes(scratch_path);

    output_block_buffers block_buffers;
    uint64_t previous_position = 0;
    if (!paired_end) {
      std::vector<staged_read_state> states(reads_in_block);
      span_streambuf block_sb(block_bytes.data(), block_bytes.size());
      std::istream input(&block_sb);
      if (!block_bytes.empty()) {
        while (input.peek() != EOF) {
          staged_stream_record_header header{};
          input.read(reinterpret_cast<char *>(&header), sizeof(header));
          if (!input) {
            throw std::runtime_error("Corruption in stream scratch block '" +
                                     scratch_path + "'.");
          }
          if (header.relative_slot >= reads_in_block) {
            throw std::runtime_error("Corruption in stream scratch block: "
                                     "relative slot out of range.");
          }
          staged_read_state &state = states[header.relative_slot];
          state.present = true;
          state.aligned = (header.flags & 0x1) != 0;
          state.read_length = header.read_length;
          state.orientation = header.orientation;
          state.position = header.position;
          if (state.aligned) {
            state.noise_codes.assign(header.noise_count, '\0');
            if (header.noise_count != 0) {
              input.read(state.noise_codes.data(),
                         static_cast<std::streamsize>(header.noise_count));
              state.noise_positions.resize(header.noise_count);
              input.read(reinterpret_cast<char *>(state.noise_positions.data()),
                         static_cast<std::streamsize>(header.noise_count *
                                                      sizeof(uint16_t)));
            }
          } else {
            state.unaligned_bytes.assign(header.read_length, '\0');
            if (header.read_length != 0) {
              input.read(state.unaligned_bytes.data(),
                         static_cast<std::streamsize>(header.read_length));
            }
          }
          if (!input) {
            throw std::runtime_error("Corruption in stream scratch block '" +
                                     scratch_path + "'.");
          }
        }
      }

      for (uint32_t slot = 0; slot < reads_in_block; ++slot) {
        const staged_read_state &state = states[slot];
        if (!state.present) {
          throw std::runtime_error(
              "Missing single-end read state in stream scratch block.");
        }
        append_binary(block_buffers.read_length_bytes, state.read_length);
        if (state.aligned) {
          block_buffers.flag_bytes.push_back('0');
          block_buffers.orientation_bytes.push_back(state.orientation);
          write_aligned_position(block_buffers.position_bytes, preserve_order,
                                 slot == 0, state.position, previous_position);
          write_noise_for_state(block_buffers.noise_bytes,
                                block_buffers.noise_position_bytes, state);
        } else {
          block_buffers.flag_bytes.push_back('2');
          write_unaligned_state(block_buffers.unaligned_bytes, state);
        }
      }
    } else {
      std::vector<staged_pair_slot_state> states(reads_in_block);
      span_streambuf block_sb(block_bytes.data(), block_bytes.size());
      std::istream input(&block_sb);
      if (!block_bytes.empty()) {
        while (input.peek() != EOF) {
          staged_stream_record_header header{};
          input.read(reinterpret_cast<char *>(&header), sizeof(header));
          if (!input) {
            throw std::runtime_error("Corruption in stream scratch block '" +
                                     scratch_path + "'.");
          }
          if (header.relative_slot >= reads_in_block) {
            throw std::runtime_error("Corruption in stream scratch block: "
                                     "relative slot out of range.");
          }
          staged_read_state &state = (header.flags & 0x2) != 0
                                         ? states[header.relative_slot].read_2
                                         : states[header.relative_slot].read_1;
          state.present = true;
          state.aligned = (header.flags & 0x1) != 0;
          state.read_length = header.read_length;
          state.orientation = header.orientation;
          state.position = header.position;
          if (state.aligned) {
            state.noise_codes.assign(header.noise_count, '\0');
            if (header.noise_count != 0) {
              input.read(state.noise_codes.data(),
                         static_cast<std::streamsize>(header.noise_count));
              state.noise_positions.resize(header.noise_count);
              input.read(reinterpret_cast<char *>(state.noise_positions.data()),
                         static_cast<std::streamsize>(header.noise_count *
                                                      sizeof(uint16_t)));
            }
          } else {
            state.unaligned_bytes.assign(header.read_length, '\0');
            if (header.read_length != 0) {
              input.read(state.unaligned_bytes.data(),
                         static_cast<std::streamsize>(header.read_length));
            }
          }
          if (!input) {
            throw std::runtime_error("Corruption in stream scratch block '" +
                                     scratch_path + "'.");
          }
        }
      }

      for (uint32_t slot = 0; slot < reads_in_block; ++slot) {
        const staged_read_state &read_1 = states[slot].read_1;
        const staged_read_state &read_2 = states[slot].read_2;
        if (!read_1.present || !read_2.present) {
          throw std::runtime_error(
              "Missing paired-end read state in stream scratch block.");
        }
        append_binary(block_buffers.read_length_bytes, read_1.read_length);
        append_binary(block_buffers.read_length_bytes, read_2.read_length);
        const int64_t mate_position_delta =
            static_cast<int64_t>(read_2.position) -
            static_cast<int64_t>(read_1.position);
        int read_flag = 2;
        if (read_1.aligned && read_2.aligned && mate_position_delta >= 0 &&
            mate_position_delta < 32767) {
          read_flag = 0;
        } else if (read_1.aligned && read_2.aligned) {
          read_flag = 1;
        } else if (read_1.aligned && !read_2.aligned) {
          read_flag = 3;
        } else if (!read_1.aligned && read_2.aligned) {
          read_flag = 4;
        }

        block_buffers.flag_bytes.push_back(static_cast<char>('0' + read_flag));
        if (read_flag == 0) {
          append_binary(block_buffers.mate_position_bytes,
                        static_cast<int16_t>(mate_position_delta));
          block_buffers.mate_orientation_bytes.push_back(
              read_1.orientation != read_2.orientation ? '0' : '1');
        }
        if (read_flag == 0 || read_flag == 1 || read_flag == 3) {
          write_aligned_position(block_buffers.position_bytes, preserve_order,
                                 slot == 0, read_1.position, previous_position);
          write_noise_for_state(block_buffers.noise_bytes,
                                block_buffers.noise_position_bytes, read_1);
          block_buffers.orientation_bytes.push_back(read_1.orientation);
        } else {
          write_unaligned_state(block_buffers.unaligned_bytes, read_1);
        }

        if (read_flag == 0 || read_flag == 1 || read_flag == 4) {
          write_noise_for_state(block_buffers.noise_bytes,
                                block_buffers.noise_position_bytes, read_2);
          if (read_flag == 1 || read_flag == 4) {
            append_binary(block_buffers.position_bytes, read_2.position);
            block_buffers.orientation_bytes.push_back(read_2.orientation);
          }
        } else {
          write_unaligned_state(block_buffers.unaligned_bytes, read_2);
        }
      }
    }

    block_members[static_cast<size_t>(block_index)] =
        compress_output_block(block_buffers, paths, block_index, paired_end);
  }

  std::unordered_map<std::string, std::string> archive_members;
  for (std::unordered_map<std::string, std::string> &block_output :
       block_members) {
    archive_members.insert(std::make_move_iterator(block_output.begin()),
                           std::make_move_iterator(block_output.end()));
  }
  return archive_members;
}

std::unordered_map<std::string, std::string>
compress_output_block(const output_block_buffers &block_buffers,
                      const reordered_stream_paths &paths,
                      const uint64_t block_num, const bool paired_end) {
  std::unordered_map<std::string, std::string> members;
  add_compressed_block(members,
                       compressed_block_file_path(paths.flag_path, block_num),
                       block_buffers.flag_bytes);
  add_compressed_block(
      members, compressed_block_file_path(paths.position_path, block_num),
      block_buffers.position_bytes);
  add_compressed_block(members,
                       compressed_block_file_path(paths.noise_path, block_num),
                       block_buffers.noise_bytes);
  add_compressed_block(
      members, compressed_block_file_path(paths.noise_position_path, block_num),
      block_buffers.noise_position_bytes);
  add_compressed_block(
      members, compressed_block_file_path(paths.unaligned_path, block_num),
      block_buffers.unaligned_bytes);
  add_compressed_block(
      members, compressed_block_file_path(paths.read_length_path, block_num),
      block_buffers.read_length_bytes);
  add_compressed_block(
      members, compressed_block_file_path(paths.orientation_path, block_num),
      block_buffers.orientation_bytes);

  if (paired_end) {
    add_compressed_block(
        members,
        compressed_block_file_path(paths.mate_position_path, block_num),
        block_buffers.mate_position_bytes);
    add_compressed_block(
        members,
        compressed_block_file_path(paths.mate_orientation_path, block_num),
        block_buffers.mate_orientation_bytes);
  }

  return members;
}

} // namespace

std::unordered_map<std::string, std::string>
reorder_compress_streams(const compression_params &cp,
                         const reordered_stream_artifact &artifact,
                         const std::vector<uint32_t> *read_order_override) {
  const reordered_stream_paths paths = build_reordered_stream_paths();
  SPRING_LOG_DEBUG("reorder_compress_streams start: num_reads=" +
                   std::to_string(cp.read_info.num_reads) + ", paired_end=" +
                   std::string(cp.encoding.paired_end ? "true" : "false") +
                   ", preserve_order=" +
                   std::string(cp.encoding.preserve_order ? "true" : "false") +
                   ", threads=" + std::to_string(cp.encoding.num_thr));

  const uint32_t num_reads = cp.read_info.num_reads;
  uint32_t aligned_read_count = 0;
  uint32_t unaligned_read_count = 0;
  const uint32_t half_read_count = num_reads / 2;
  const int num_thr = cp.encoding.num_thr;
  const bool paired_end = cp.encoding.paired_end;
  const bool preserve_order = cp.encoding.preserve_order;

  std::vector<char> orientation_by_read(num_reads);
  std::vector<uint16_t> read_lengths_by_read(num_reads);
  std::vector<bool> aligned_flags(num_reads);
  std::vector<uint64_t> noise_offset_by_read(num_reads);
  std::vector<uint64_t> position_by_read(num_reads);
  std::vector<uint16_t> noise_count_by_read(num_reads);

  const std::vector<char> &orientation_entries = artifact.orientation_entries;
  const std::vector<uint64_t> &position_entries = artifact.position_entries;
  const std::vector<uint16_t> &read_length_entries =
      artifact.read_length_entries;
  const std::vector<uint32_t> *read_order_entries =
      read_order_override != nullptr ? read_order_override
                                     : &artifact.read_order_entries;
  const std::vector<char> &noise_serialized = artifact.noise_serialized;
  const std::vector<uint16_t> &noise_positions = artifact.noise_positions;

  if (orientation_entries.size() != position_entries.size()) {
    throw std::runtime_error(
        "Corruption in aligned streams: orientation/position size mismatch.");
  }

  aligned_read_count = static_cast<uint32_t>(orientation_entries.size());
  if (aligned_read_count > num_reads) {
    throw std::runtime_error("Corruption in aligned streams: aligned read "
                             "count exceeds total reads.");
  }

  if (read_length_entries.size() != num_reads) {
    throw std::runtime_error(
        "Corruption in read length stream: entry count does not match reads.");
  }

  if ((paired_end || preserve_order) &&
      read_order_entries->size() != num_reads) {
    throw std::runtime_error(
        "Corruption in read order stream: entry count does not match reads.");
  }

  uint64_t next_noise_offset = 0;
  size_t noise_cursor = 0;
  std::vector<char> noise_codes(noise_positions.size());

  for (uint32_t entry_index = 0; entry_index < aligned_read_count;
       ++entry_index) {
    const uint32_t read_order = (paired_end || preserve_order)
                                    ? (*read_order_entries)[entry_index]
                                    : entry_index;

    orientation_by_read[read_order] = orientation_entries[entry_index];
    read_lengths_by_read[read_order] = read_length_entries[entry_index];
    aligned_flags[read_order] = true;
    position_by_read[read_order] = position_entries[entry_index];
    noise_offset_by_read[read_order] = next_noise_offset;

    const char *line_begin = noise_serialized.data() + noise_cursor;
    const size_t bytes_remaining = noise_serialized.size() - noise_cursor;
    const void *line_end_ptr = std::memchr(line_begin, '\n', bytes_remaining);
    const size_t line_len =
        line_end_ptr == nullptr
            ? bytes_remaining
            : static_cast<size_t>(static_cast<const char *>(line_end_ptr) -
                                  line_begin);

    if (next_noise_offset + line_len > noise_positions.size()) {
      throw std::runtime_error(
          "Corruption in noise stream: excess codes found beyond position "
          "metadata limit.");
    }

    if (line_len > 0) {
      std::memcpy(noise_codes.data() + next_noise_offset, line_begin, line_len);
    }
    noise_count_by_read[read_order] = static_cast<uint16_t>(line_len);
    next_noise_offset += line_len;
    noise_cursor += line_len;
    if (line_end_ptr != nullptr) {
      noise_cursor += 1;
    }
  }

  while (noise_cursor < noise_serialized.size() &&
         noise_serialized[noise_cursor] == '\n') {
    noise_cursor++;
  }

  if (noise_cursor != noise_serialized.size()) {
    throw std::runtime_error(
        "Corruption in noise stream: extra non-delimiter payload after "
        "aligned entries.");
  }

  if (next_noise_offset != noise_positions.size()) {
    throw std::runtime_error(
        "Corruption in noise stream: code/position entry count mismatch.");
  }

  unaligned_read_count = num_reads - aligned_read_count;
  SPRING_LOG_DEBUG(
      "reorder_compress_streams parsed input streams: aligned_reads=" +
      std::to_string(aligned_read_count) +
      ", unaligned_reads=" + std::to_string(unaligned_read_count) +
      ", noise_entries=" + std::to_string(noise_positions.size()));

  std::vector<char> unaligned_chars;
  std::vector<uint16_t> unaligned_lengths;
  decode_unaligned_reads(artifact.unaligned_serialized, unaligned_read_count,
                         unaligned_chars, unaligned_lengths);
  if (unaligned_chars.size() != artifact.unaligned_char_count) {
    throw std::runtime_error(
        "Corruption in unaligned stream: decoded size does not match recorded "
        "character count.");
  }

  uint64_t current_unaligned_offset = 0;
  for (uint32_t read_index = 0; read_index < unaligned_read_count;
       read_index++) {
    const uint32_t read_order =
        (paired_end || preserve_order)
            ? (*read_order_entries)[aligned_read_count + read_index]
            : (aligned_read_count + read_index);
    const uint16_t read_length =
        read_length_entries[aligned_read_count + read_index];
    if (unaligned_lengths[read_index] != read_length) {
      throw std::runtime_error(
          "Corruption in unaligned stream: decoded read length does not match "
          "read-length metadata.");
    }
    read_lengths_by_read[read_order] = read_length;
    position_by_read[read_order] = current_unaligned_offset;
    current_unaligned_offset += read_length;
    aligned_flags[read_order] = false;
  }

  omp_set_num_threads(num_thr);
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  const uint64_t read_limit = paired_end ? half_read_count : num_reads;
  const uint64_t output_blocks =
      (read_limit == 0)
          ? 0
          : (read_limit + static_cast<uint64_t>(num_reads_per_block) - 1) /
                static_cast<uint64_t>(num_reads_per_block);
  SPRING_LOG_DEBUG("reorder_compress_streams output planning: read_limit=" +
                   std::to_string(read_limit) + ", num_reads_per_block=" +
                   std::to_string(num_reads_per_block) +
                   ", output_blocks=" + std::to_string(output_blocks));

  std::vector<std::unordered_map<std::string, std::string>> block_members(
      static_cast<size_t>(output_blocks));

#pragma omp parallel
  {
    uint64_t thread_id = omp_get_thread_num();
    uint64_t block_num = thread_id;
    while (true) {
      const block_range current_block =
          block_read_range(block_num, num_reads_per_block, read_limit);
      if (!current_block.valid) {
        break;
      }

      output_block_buffers block_buffers;
      uint64_t previous_position = 0;
      for (uint64_t read_index = current_block.begin;
           read_index < current_block.end; read_index++) {
        if (!paired_end) {
          append_binary(block_buffers.read_length_bytes,
                        read_lengths_by_read[read_index]);
          if (aligned_flags[read_index]) {
            block_buffers.flag_bytes.push_back('0');
            block_buffers.orientation_bytes.push_back(
                orientation_by_read[read_index]);
            write_aligned_position(block_buffers.position_bytes, preserve_order,
                                   read_index == current_block.begin,
                                   position_by_read[read_index],
                                   previous_position);
            write_noise_for_read(
                block_buffers.noise_bytes, block_buffers.noise_position_bytes,
                noise_codes, noise_positions, noise_offset_by_read,
                noise_count_by_read, read_index);
          } else {
            block_buffers.flag_bytes.push_back('2');
            write_unaligned_read(block_buffers.unaligned_bytes, unaligned_chars,
                                 position_by_read, read_lengths_by_read,
                                 read_index);
          }
        } else {
          const uint64_t mate_read_index = half_read_count + read_index;
          append_binary(block_buffers.read_length_bytes,
                        read_lengths_by_read[read_index]);
          append_binary(block_buffers.read_length_bytes,
                        read_lengths_by_read[mate_read_index]);
          const int64_t mate_position_delta =
              static_cast<int64_t>(position_by_read[mate_read_index]) -
              static_cast<int64_t>(position_by_read[read_index]);
          int read_flag = 2;
          if (aligned_flags[read_index] && aligned_flags[mate_read_index] &&
              mate_position_delta >= 0 && mate_position_delta < 32767) {
            read_flag = 0;
          } else if (aligned_flags[read_index] &&
                     aligned_flags[mate_read_index]) {
            read_flag = 1;
          } else if (!aligned_flags[read_index] &&
                     !aligned_flags[mate_read_index]) {
            read_flag = 2;
          } else if (aligned_flags[read_index] &&
                     !aligned_flags[mate_read_index]) {
            read_flag = 3;
          } else if (!aligned_flags[read_index] &&
                     aligned_flags[mate_read_index]) {
            read_flag = 4;
          }

          block_buffers.flag_bytes.push_back(
              static_cast<char>('0' + read_flag));
          if (read_flag == 0) {
            const int16_t mate_position_delta_16 =
                static_cast<int16_t>(mate_position_delta);
            append_binary(block_buffers.mate_position_bytes,
                          mate_position_delta_16);
            block_buffers.mate_orientation_bytes.push_back(
                orientation_by_read[read_index] !=
                        orientation_by_read[mate_read_index]
                    ? '0'
                    : '1');
          }
          if (read_flag == 0 || read_flag == 1 || read_flag == 3) {
            write_aligned_position(block_buffers.position_bytes, preserve_order,
                                   read_index == current_block.begin,
                                   position_by_read[read_index],
                                   previous_position);
            write_noise_for_read(
                block_buffers.noise_bytes, block_buffers.noise_position_bytes,
                noise_codes, noise_positions, noise_offset_by_read,
                noise_count_by_read, read_index);
            block_buffers.orientation_bytes.push_back(
                orientation_by_read[read_index]);
          } else {
            write_unaligned_read(block_buffers.unaligned_bytes, unaligned_chars,
                                 position_by_read, read_lengths_by_read,
                                 read_index);
          }

          if (read_flag == 0 || read_flag == 1 || read_flag == 4) {
            write_noise_for_read(
                block_buffers.noise_bytes, block_buffers.noise_position_bytes,
                noise_codes, noise_positions, noise_offset_by_read,
                noise_count_by_read, mate_read_index);
            if (read_flag == 1 || read_flag == 4) {
              append_binary(block_buffers.position_bytes,
                            position_by_read[mate_read_index]);
              block_buffers.orientation_bytes.push_back(
                  orientation_by_read[mate_read_index]);
            }
          } else {
            write_unaligned_read(block_buffers.unaligned_bytes, unaligned_chars,
                                 position_by_read, read_lengths_by_read,
                                 mate_read_index);
          }
        }
      }

      block_members[static_cast<size_t>(block_num)] =
          compress_output_block(block_buffers, paths, block_num, paired_end);
      block_num += num_thr;
    }
  }

  SPRING_LOG_DEBUG("reorder_compress_streams complete: blocks_written=" +
                   std::to_string(output_blocks));
  std::unordered_map<std::string, std::string> archive_members;
  for (std::unordered_map<std::string, std::string> &block_output :
       block_members) {
    archive_members.insert(std::make_move_iterator(block_output.begin()),
                           std::make_move_iterator(block_output.end()));
  }
  return archive_members;
}

std::unordered_map<std::string, std::string>
reorder_compress_streams(const compression_params &cp,
                         const reordered_stream_artifact &artifact,
                         const std::string &read_order_entries_path) {
  std::ifstream input(read_order_entries_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open read-order stream '" +
                             read_order_entries_path + "'.");
  }

  std::vector<uint32_t> read_order_entries(cp.read_info.num_reads);
  if (!read_order_entries.empty()) {
    input.read(reinterpret_cast<char *>(read_order_entries.data()),
               static_cast<std::streamsize>(read_order_entries.size() *
                                            sizeof(uint32_t)));
    if (!input) {
      throw std::runtime_error(
          "Corruption in read order stream: entry count does not match reads.");
    }
  }

  return reorder_compress_streams(cp, artifact, &read_order_entries);
}

std::unordered_map<std::string, std::string>
reorder_compress_streams(const compression_params &cp,
                         const std::string &artifact_root_dir,
                         const std::string &read_order_entries_path,
                         const uint64_t memory_budget_bytes) {
  const std::string scratch_root_dir =
      (std::filesystem::path(artifact_root_dir).parent_path() /
       "stream-rebuild")
          .string();
  std::vector<std::string> block_buffers;
  bool spilled_any = false;
  partition_alignment_stream_records(
      cp, artifact_root_dir, read_order_entries_path, scratch_root_dir,
      memory_budget_bytes, block_buffers, spilled_any);
  std::unordered_map<std::string, std::string> archive_members =
      rebuild_stream_blocks(cp, scratch_root_dir,
                            spilled_any ? nullptr : &block_buffers);
  std::error_code cleanup_ec;
  std::filesystem::remove_all(scratch_root_dir, cleanup_ec);
  return archive_members;
}

} // namespace spring