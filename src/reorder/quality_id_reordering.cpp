// Reorders and compresses quality-value and identifier streams so they align
// with Spring's reordered read layout before archive packaging.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <omp.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core_utils.h"
#include "io_utils.h"
#include "params.h"
#include "progress.h"
#include "quality_id_reordering.h"
#include "stream_reordering.h"

namespace spring {

namespace {

enum class reorder_compress_mode : uint8_t {
  quality,
  id,
};

struct batch_range {
  uint32_t begin;
  uint32_t end;
};

struct batch_record {
  uint32_t relative_position;
  uint32_t string_length;
};

// Fraction of the compression memory budget this stage may hold in RAM while
// partitioning quality/id records into reorder batches.
constexpr double kSideStreamBufferBudgetFraction = 0.4;
// Fallback cap when no memory budget is supplied.
constexpr uint64_t kDefaultSideStreamBufferCapBytes = 4ULL * 1024 * 1024 * 1024;

void add_archive_member(std::unordered_map<std::string, std::string> &members,
                        const std::string &path,
                        const std::vector<char> &bytes) {
  members[path] = std::string(bytes.begin(), bytes.end());
}

std::string block_file_path(const std::string &base_path,
                            const uint64_t block_num) {
  return base_path + "." + std::to_string(block_num);
}

uint32_t reads_per_file(const uint32_t num_reads, const bool paired_end) {
  return paired_end ? num_reads / 2 : num_reads;
}

uint32_t compute_batch_size(const uint32_t num_reads,
                            const uint32_t num_reads_per_block) {
  return (1 + (num_reads - 1) / num_reads_per_block) * num_reads_per_block;
}

batch_range batch_read_range(const uint32_t batch_index,
                             const uint32_t batch_size,
                             const uint32_t total_reads) {
  const uint32_t batch_begin = batch_index * batch_size;
  const uint32_t batch_end = std::min(batch_begin + batch_size, total_reads);
  return {.begin = batch_begin, .end = batch_end};
}

uint32_t block_read_count(const uint64_t block_begin,
                          const uint64_t block_end) {
  return static_cast<uint32_t>(block_end - block_begin);
}

uint32_t batch_count_for_reads(const uint32_t total_reads,
                               const uint32_t batch_size) {
  return (total_reads + batch_size - 1) / batch_size;
}

void reset_directory(const std::string &path) {
  std::error_code remove_ec;
  std::filesystem::remove_all(path, remove_ec);
  if (remove_ec) {
    throw std::runtime_error("Failed to clear scratch directory '" + path +
                             "': " + remove_ec.message());
  }

  std::error_code create_ec;
  std::filesystem::create_directories(path, create_ec);
  if (create_ec) {
    throw std::runtime_error("Failed to create scratch directory '" + path +
                             "': " + create_ec.message());
  }
}

std::string read_binary_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open side-stream file '" + path + "'.");
  }

  std::ostringstream output(std::ios::binary);
  output << input.rdbuf();
  if (input.bad()) {
    throw std::runtime_error("Failed to read side-stream file '" + path + "'.");
  }
  return output.str();
}

std::string scratch_batch_path(const std::string &root_dir,
                               const std::string &input_path,
                               const uint32_t batch_index) {
  return (std::filesystem::path(root_dir) /
          (input_path + ".batch." + std::to_string(batch_index) + ".bin"))
      .string();
}

void append_buffer_to_scratch(const std::string &path,
                              const std::string &bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output.is_open()) {
    throw std::runtime_error("Failed to open scratch batch file '" + path +
                             "'.");
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("Failed writing scratch batch file '" + path +
                             "'.");
  }
}

void partition_reordered_batches_from_buffer(
    const std::string &input_path, const std::string &input_bytes,
    const std::vector<uint32_t> &reordered_positions, const uint32_t batch_size,
    std::vector<std::string> &batch_buffers) {
  const uint32_t num_batches = batch_count_for_reads(
      static_cast<uint32_t>(reordered_positions.size()), batch_size);
  SPRING_LOG_DEBUG("block_id=reorder-partition, Partitioning reordered stream "
                   "from memory: path=" +
                   input_path +
                   ", reads=" + std::to_string(reordered_positions.size()) +
                   ", batch_size=" + std::to_string(batch_size) +
                   ", num_batches=" + std::to_string(num_batches));
  batch_buffers.resize(num_batches);

  size_t cursor = 0;
  uint32_t read_index = 0;
  while (cursor <= input_bytes.size() &&
         read_index < reordered_positions.size()) {
    const size_t line_end = input_bytes.find('\n', cursor);
    const size_t value_end =
        (line_end == std::string::npos) ? input_bytes.size() : line_end;
    const bool has_trailing_cr =
        value_end > cursor && input_bytes[value_end - 1] == '\r';
    const size_t current_length =
        value_end - cursor - (has_trailing_cr ? 1U : 0U);
    std::string current_string = input_bytes.substr(cursor, current_length);

    const uint32_t reordered_position = reordered_positions[read_index];
    const uint32_t batch_index = reordered_position / batch_size;
    batch_record record{.relative_position = reordered_position % batch_size,
                        .string_length =
                            static_cast<uint32_t>(current_string.size())};
    batch_buffers[batch_index].append(byte_ptr(&record), sizeof(batch_record));
    batch_buffers[batch_index].append(current_string);

    read_index++;
    if (line_end == std::string::npos) {
      cursor = input_bytes.size() + 1;
    } else {
      cursor = line_end + 1;
    }
  }

  if (read_index != reordered_positions.size()) {
    throw std::runtime_error(
        "Side-stream line count does not match expected read count for " +
        input_path);
  }
}

void load_partitioned_batch_from_bytes(
    const std::string &batch_bytes, const uint32_t reads_in_batch,
    std::vector<std::string> &reordered_strings) {
  const char *cursor = batch_bytes.data();
  const char *end = cursor + batch_bytes.size();
  while (cursor < end) {
    batch_record record;
    std::memcpy(&record, cursor, sizeof(batch_record));
    cursor += sizeof(batch_record);
    reordered_strings[record.relative_position].assign(cursor,
                                                       record.string_length);
    cursor += record.string_length;
  }

  for (uint32_t read_index = reads_in_batch;
       read_index < reordered_strings.size(); read_index++) {
    reordered_strings[read_index].clear();
  }
}

void partition_reordered_batches_from_file(
    const std::string &input_path, const std::string &source_path,
    const std::vector<uint32_t> &reordered_positions, const uint32_t batch_size,
    const std::string &scratch_root_dir, const uint64_t memory_budget_bytes,
    std::vector<std::string> &batch_paths,
    std::vector<std::string> &batch_buffers, bool &spilled_any) {
  const uint32_t num_batches = batch_count_for_reads(
      static_cast<uint32_t>(reordered_positions.size()), batch_size);
  SPRING_LOG_DEBUG("block_id=reorder-partition, Partitioning reordered stream "
                   "from file: path=" +
                   input_path +
                   ", reads=" + std::to_string(reordered_positions.size()) +
                   ", batch_size=" + std::to_string(batch_size) +
                   ", num_batches=" + std::to_string(num_batches));

  batch_paths.resize(num_batches);
  batch_buffers.resize(num_batches);
  for (uint32_t batch_index = 0; batch_index < num_batches; ++batch_index) {
    batch_paths[batch_index] =
        scratch_batch_path(scratch_root_dir, input_path, batch_index);
  }

  spilled_any = false;
  uint64_t buffered_bytes = 0;
  const uint64_t scatter_cap_bytes =
      memory_budget_bytes > 0
          ? std::max<uint64_t>(1, static_cast<uint64_t>(
                                      static_cast<double>(memory_budget_bytes) *
                                      kSideStreamBufferBudgetFraction))
          : kDefaultSideStreamBufferCapBytes;

  const auto flush_batch_buffers = [&]() {
    if (!spilled_any) {
      reset_directory(scratch_root_dir);
      spilled_any = true;
    }
    for (uint32_t batch_index = 0; batch_index < num_batches; ++batch_index) {
      std::string &batch_bytes = batch_buffers[batch_index];
      if (batch_bytes.empty()) {
        continue;
      }
      append_buffer_to_scratch(batch_paths[batch_index], batch_bytes);
      batch_bytes.clear();
    }
    buffered_bytes = 0;
  };

  std::ifstream input(source_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open side-stream source '" +
                             source_path + "'.");
  }

  std::string current_string;
  uint32_t read_index = 0;
  while (std::getline(input, current_string)) {
    if (!current_string.empty() && current_string.back() == '\r') {
      current_string.pop_back();
    }
    if (read_index >= reordered_positions.size()) {
      throw std::runtime_error(
          "Side-stream line count exceeds expected read count for " +
          input_path);
    }

    const uint32_t reordered_position = reordered_positions[read_index];
    const uint32_t batch_index = reordered_position / batch_size;
    batch_record record{.relative_position = reordered_position % batch_size,
                        .string_length =
                            static_cast<uint32_t>(current_string.size())};

    std::string &batch_bytes = batch_buffers[batch_index];
    batch_bytes.append(byte_ptr(&record), sizeof(batch_record));
    batch_bytes.append(current_string);
    buffered_bytes +=
        static_cast<uint64_t>(sizeof(batch_record) + current_string.size());

    if (buffered_bytes >= scatter_cap_bytes) {
      flush_batch_buffers();
    }

    ++read_index;
  }

  if (input.bad()) {
    throw std::runtime_error("Failed reading side-stream source '" +
                             source_path + "'.");
  }

  if (read_index != reordered_positions.size()) {
    throw std::runtime_error(
        "Side-stream line count does not match expected read count for " +
        input_path);
  }

  // Flush any buffered tail if this stream crossed the spill threshold.
  if (spilled_any && buffered_bytes > 0) {
    flush_batch_buffers();
  }

  SPRING_LOG_DEBUG(
      "block_id=reorder-partition, Partition complete: path=" + input_path +
      ", spilled=" + std::string(spilled_any ? "true" : "false") +
      ", scatter_cap_bytes=" + std::to_string(scatter_cap_bytes));
}

void load_partitioned_batch_from_file(
    const std::string &batch_path, const uint32_t reads_in_batch,
    std::vector<std::string> &reordered_strings) {
  load_partitioned_batch_from_bytes(read_binary_file(batch_path),
                                    reads_in_batch, reordered_strings);
}

void compress_block_batch(
    const std::string &input_path, const reorder_compress_mode mode,
    compression_params &cp, std::vector<std::string> &reordered_strings,
    const batch_range &batch, const uint32_t num_reads_per_block,
    const int num_threads, std::vector<std::vector<char>> &block_outputs,
    std::vector<std::vector<char>> *id_block_outputs = nullptr) {
  const char *mode_name =
      (mode == reorder_compress_mode::id) ? "id" : "quality";
  SPRING_LOG_DEBUG(
      "block_id=reorder-batch-compress, Compressing batch blocks: mode=" +
      std::string(mode_name) + ", input=" + input_path + ", begin=" +
      std::to_string(batch.begin) + ", end=" + std::to_string(batch.end) +
      ", reads_per_block=" + std::to_string(num_reads_per_block));
#pragma omp parallel
  {
    const uint64_t thread_id = omp_get_thread_num();
    const uint64_t block_offset = batch.begin / num_reads_per_block;
    uint64_t block_index = thread_id;
    std::vector<uint32_t> read_lengths;
    if (mode == reorder_compress_mode::quality)
      read_lengths.resize(num_reads_per_block);

    while (true) {
      const uint64_t block_begin = block_index * num_reads_per_block;
      if (block_begin >= batch.end - batch.begin)
        break;

      uint64_t block_end = (block_index + 1) * num_reads_per_block;
      if (block_end > batch.end - batch.begin)
        block_end = batch.end - batch.begin;

      const uint32_t reads_in_block = block_read_count(block_begin, block_end);
      const uint64_t global_block_idx = block_offset + block_index;

      if (mode == reorder_compress_mode::id) {
        if (id_block_outputs != nullptr) {
          (*id_block_outputs)[block_index] = compress_id_block_bytes(
              reordered_strings.data() + block_begin, reads_in_block);
        } else {
          block_outputs[block_index] = compress_id_block_bytes(
              reordered_strings.data() + block_begin, reads_in_block);
        }
        if (global_block_idx <
            compression_params::ReadMetadata::kFileLenThrSize) {
          cp.read_info.file_len_id_thr[global_block_idx] =
              (id_block_outputs != nullptr)
                  ? (*id_block_outputs)[block_index].size()
                  : block_outputs[block_index].size();
          SPRING_LOG_DEBUG(
              "block_id=id-block-" + std::to_string(global_block_idx) +
              ", Compressed id block=" + std::to_string(global_block_idx) +
              ", reads=" + std::to_string(reads_in_block) + ", bytes=" +
              std::to_string(cp.read_info.file_len_id_thr[global_block_idx]));
        } else {
          throw std::runtime_error(
              std::string("Exceeded maximum supported block count (") +
              std::to_string(
                  compression_params::ReadMetadata::kFileLenThrSize) +
              "). Increase array size in util.h.");
        }
      } else {
        for (uint64_t read_offset = 0; read_offset < reads_in_block;
             read_offset++) {
          read_lengths[read_offset] = static_cast<uint32_t>(
              reordered_strings[block_begin + read_offset].size());
        }
        block_outputs[block_index] =
            bsc_str_array_compress_bytes(reordered_strings.data() + block_begin,
                                         reads_in_block, read_lengths.data());
        SPRING_LOG_DEBUG(
            "block_id=quality-block-" + std::to_string(global_block_idx) +
            ", Compressed quality block=" + std::to_string(global_block_idx) +
            ", reads=" + std::to_string(reads_in_block));
      }

      block_index += num_threads;
    }
  }
}

bool should_process_stream(const int stream_index, const bool paired_end,
                           const bool skip_second_stream) {
  if (!paired_end && stream_index == 1)
    return false;
  if (skip_second_stream && stream_index == 1)
    return false;
  return true;
}

// Builds a quality-slot permutation for paired-end mode.
// reordered_positions[raw_R1_pos] = corrected_original_R1_index.
// The sequence stream uses corrected original indices for block assignment
// (see reorder_compress_streams), so quality blocks must use the same
void generate_order_pe(const std::vector<uint32_t> &read_order_entries,
                       std::vector<uint32_t> &reordered_positions,
                       const uint32_t num_reads) {
  const uint32_t spot_count = num_reads / 2;
  uint32_t corrected = 0;

  for (const uint32_t orig_spot : read_order_entries) {
    if (orig_spot < spot_count) {
      reordered_positions[orig_spot] = corrected++;
    }
    if (corrected >= spot_count) {
      return;
    }
  }
}

void generate_order_se(const std::vector<uint32_t> &read_order_entries,
                       std::vector<uint32_t> &reordered_positions,
                       const uint32_t num_reads) {
  uint32_t corrected = 0;
  for (const uint32_t orig_pos : read_order_entries) {
    if (orig_pos < num_reads) {
      reordered_positions[orig_pos] = corrected++;
    }
    if (corrected >= num_reads) {
      return;
    }
  }
}

void generate_order_pe_from_file(const std::string &read_order_entries_path,
                                 std::vector<uint32_t> &reordered_positions,
                                 const uint32_t num_reads) {
  const uint32_t spot_count = num_reads / 2;
  uint32_t corrected = 0;
  std::ifstream input(read_order_entries_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open read-order stream '" +
                             read_order_entries_path + "'.");
  }

  for (uint32_t entry_index = 0; entry_index < num_reads; ++entry_index) {
    uint32_t orig_spot = 0;
    input.read(reinterpret_cast<char *>(&orig_spot), sizeof(uint32_t));
    if (!input) {
      throw std::runtime_error(
          "Corruption in read order stream: entry count does not match reads.");
    }
    if (orig_spot < spot_count) {
      reordered_positions[orig_spot] = corrected++;
    }
    if (corrected >= spot_count) {
      return;
    }
  }
}

void generate_order_se_from_file(const std::string &read_order_entries_path,
                                 std::vector<uint32_t> &reordered_positions,
                                 const uint32_t num_reads) {
  uint32_t corrected = 0;
  std::ifstream input(read_order_entries_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open read-order stream '" +
                             read_order_entries_path + "'.");
  }

  for (uint32_t entry_index = 0; entry_index < num_reads; ++entry_index) {
    uint32_t orig_pos = 0;
    input.read(reinterpret_cast<char *>(&orig_pos), sizeof(uint32_t));
    if (!input) {
      throw std::runtime_error(
          "Corruption in read order stream: entry count does not match reads.");
    }
    if (orig_pos < num_reads) {
      reordered_positions[orig_pos] = corrected++;
    }
    if (corrected >= num_reads) {
      return;
    }
  }
}

void reorder_compress_from_buffer(
    const std::string &input_path, const std::string &input_bytes,
    const uint32_t num_reads_per_file, const int num_thr,
    const uint32_t num_reads_per_block,
    std::vector<std::string> &reordered_strings, const uint32_t batch_size,
    const std::vector<uint32_t> &reordered_positions,
    const reorder_compress_mode mode, compression_params &cp,
    std::unordered_map<std::string, std::string> &archive_members,
    std::vector<char> *merged_id_block_bytes = nullptr) {
  std::vector<std::string> batch_buffers;
  partition_reordered_batches_from_buffer(
      input_path, input_bytes, reordered_positions, batch_size, batch_buffers);
  SPRING_LOG_DEBUG("block_id=reorder-stream, Reorder/compress stream start "
                   "from memory: path=" +
                   input_path +
                   ", total_reads=" + std::to_string(num_reads_per_file) +
                   ", batches=" + std::to_string(batch_buffers.size()));

  for (uint32_t batch_index = 0;; batch_index++) {
    const batch_range batch =
        batch_read_range(batch_index, batch_size, num_reads_per_file);
    if (batch.begin >= batch.end)
      break;

    const uint32_t blocks_in_batch =
        (batch.end - batch.begin + num_reads_per_block - 1) /
        num_reads_per_block;
    load_partitioned_batch_from_bytes(
        batch_buffers[batch_index], batch.end - batch.begin, reordered_strings);
    std::vector<std::vector<char>> batch_block_outputs(blocks_in_batch);
    std::vector<std::vector<char>> batch_id_block_outputs;
    if (merged_id_block_bytes != nullptr && mode == reorder_compress_mode::id) {
      batch_id_block_outputs.resize(blocks_in_batch);
    }
    compress_block_batch(
        input_path, mode, cp, reordered_strings, batch, num_reads_per_block,
        num_thr, batch_block_outputs,
        batch_id_block_outputs.empty() ? nullptr : &batch_id_block_outputs);
    const uint64_t block_offset = batch.begin / num_reads_per_block;
    for (uint32_t block_index = 0; block_index < blocks_in_batch;
         ++block_index) {
      if (!batch_block_outputs[block_index].empty()) {
        add_archive_member(
            archive_members,
            block_file_path(input_path, block_offset + block_index),
            batch_block_outputs[block_index]);
      }
    }
    if (!batch_id_block_outputs.empty()) {
      for (std::vector<char> &block_bytes : batch_id_block_outputs) {
        merged_id_block_bytes->insert(merged_id_block_bytes->end(),
                                      block_bytes.begin(), block_bytes.end());
      }
    }
    batch_buffers[batch_index].clear();
    batch_buffers[batch_index].shrink_to_fit();
    SPRING_LOG_DEBUG("block_id=reorder-batch-" + std::to_string(batch_index) +
                     ", Reorder/compress batch done: path=" + input_path +
                     ", batch_index=" + std::to_string(batch_index) +
                     ", begin=" + std::to_string(batch.begin) +
                     ", end=" + std::to_string(batch.end));
  }
}

void reorder_compress_from_file(
    const std::string &input_path, const std::string &source_path,
    const uint32_t num_reads_per_file, const int num_thr,
    const uint32_t num_reads_per_block,
    std::vector<std::string> &reordered_strings, const uint32_t batch_size,
    const std::vector<uint32_t> &reordered_positions,
    const reorder_compress_mode mode, compression_params &cp,
    std::unordered_map<std::string, std::string> &archive_members,
    std::vector<char> *merged_id_block_bytes = nullptr,
    const uint64_t memory_budget_bytes = 0) {
  const std::string scratch_root_dir =
      (std::filesystem::path(source_path).parent_path() /
       (input_path + ".partition"))
          .string();
  std::vector<std::string> batch_paths;
  std::vector<std::string> batch_buffers;
  bool spilled_any = false;
  partition_reordered_batches_from_file(
      input_path, source_path, reordered_positions, batch_size,
      scratch_root_dir, memory_budget_bytes, batch_paths, batch_buffers,
      spilled_any);
  SPRING_LOG_DEBUG("block_id=reorder-stream, Reorder/compress stream start "
                   "from file: path=" +
                   input_path +
                   ", total_reads=" + std::to_string(num_reads_per_file) +
                   ", batches=" + std::to_string(batch_paths.size()) +
                   ", spilled=" + std::string(spilled_any ? "true" : "false"));

  for (uint32_t batch_index = 0;; ++batch_index) {
    const batch_range batch =
        batch_read_range(batch_index, batch_size, num_reads_per_file);
    if (batch.begin >= batch.end) {
      break;
    }

    const uint32_t blocks_in_batch =
        (batch.end - batch.begin + num_reads_per_block - 1) /
        num_reads_per_block;
    if (spilled_any) {
      load_partitioned_batch_from_file(
          batch_paths[batch_index], batch.end - batch.begin, reordered_strings);
    } else {
      load_partitioned_batch_from_bytes(batch_buffers[batch_index],
                                        batch.end - batch.begin,
                                        reordered_strings);
    }
    std::vector<std::vector<char>> batch_block_outputs(blocks_in_batch);
    std::vector<std::vector<char>> batch_id_block_outputs;
    if (merged_id_block_bytes != nullptr && mode == reorder_compress_mode::id) {
      batch_id_block_outputs.resize(blocks_in_batch);
    }
    compress_block_batch(
        input_path, mode, cp, reordered_strings, batch, num_reads_per_block,
        num_thr, batch_block_outputs,
        batch_id_block_outputs.empty() ? nullptr : &batch_id_block_outputs);
    const uint64_t block_offset = batch.begin / num_reads_per_block;
    for (uint32_t block_index = 0; block_index < blocks_in_batch;
         ++block_index) {
      if (!batch_block_outputs[block_index].empty()) {
        add_archive_member(
            archive_members,
            block_file_path(input_path, block_offset + block_index),
            batch_block_outputs[block_index]);
      }
    }
    if (!batch_id_block_outputs.empty()) {
      for (std::vector<char> &block_bytes : batch_id_block_outputs) {
        merged_id_block_bytes->insert(merged_id_block_bytes->end(),
                                      block_bytes.begin(), block_bytes.end());
      }
    }
    if (spilled_any) {
      std::error_code remove_ec;
      std::filesystem::remove(batch_paths[batch_index], remove_ec);
    } else {
      batch_buffers[batch_index].clear();
      batch_buffers[batch_index].shrink_to_fit();
    }
    SPRING_LOG_DEBUG("block_id=reorder-batch-" + std::to_string(batch_index) +
                     ", Reorder/compress batch done: path=" + input_path +
                     ", batch_index=" + std::to_string(batch_index) +
                     ", begin=" + std::to_string(batch.begin) +
                     ", end=" + std::to_string(batch.end));
  }

  std::error_code cleanup_ec;
  std::filesystem::remove_all(scratch_root_dir, cleanup_ec);
}

} // namespace

std::unordered_map<std::string, std::string>
reorder_compress_quality_id(const post_encode_side_stream_artifact &artifact,
                            const std::vector<uint32_t> &read_order_entries,
                            compression_params &cp) {
  std::unordered_map<std::string, std::string> archive_members;
  const uint32_t num_reads = cp.read_info.num_reads;
  const int num_thr = cp.encoding.num_thr;
  const bool preserve_id = cp.encoding.preserve_id;
  const bool preserve_quality = cp.encoding.preserve_quality;
  const bool paired_end = cp.encoding.paired_end;
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  const bool paired_id_match = cp.read_info.paired_id_match;

  std::string id_paths[2];
  std::string quality_paths[2];
  id_paths[0] = "id_1";
  id_paths[1] = "id_2";
  quality_paths[0] = "quality_1";
  quality_paths[1] = "quality_2";

  std::vector<uint32_t> reordered_positions;
  if (paired_end) {
    reordered_positions.resize(num_reads / 2);
    if (read_order_entries.size() != num_reads) {
      throw std::runtime_error(
          "Corruption in read order stream: entry count does not match reads.");
    }
    generate_order_pe(read_order_entries, reordered_positions, num_reads);
    SPRING_LOG_DEBUG("block_id=reorder-map-pe, Quality/ID reorder map "
                     "generated for paired-end reads: spots=" +
                     std::to_string(reordered_positions.size()));
  } else {
    reordered_positions.resize(num_reads);
    if (read_order_entries.size() != num_reads) {
      throw std::runtime_error(
          "Corruption in read order stream: entry count does not match reads.");
    }
    generate_order_se(read_order_entries, reordered_positions, num_reads);
    SPRING_LOG_DEBUG("block_id=reorder-map-se, Quality/ID reorder map "
                     "generated for single-end reads: reads=" +
                     std::to_string(reordered_positions.size()));
  }

  omp_set_num_threads(num_thr);

  const uint32_t batch_size =
      compute_batch_size(num_reads, num_reads_per_block);
  std::vector<std::string> reordered_strings(batch_size);
  // Bound the working set so this stage stays within the reorder memory budget.

  if (preserve_quality) {
    SPRING_LOG_INFO("Compressing qualities");
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (!should_process_stream(stream_index, paired_end, false))
        continue;
      SPRING_LOG_DEBUG(
          "block_id=quality-stream-" + std::to_string(stream_index) +
          ", Quality stream selected: index=" + std::to_string(stream_index) +
          ", path=" + quality_paths[stream_index]);
      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      reorder_compress_from_buffer(
          quality_paths[stream_index],
          artifact.raw_quality_streams[stream_index], file_read_count, num_thr,
          num_reads_per_block, reordered_strings, batch_size,
          reordered_positions, reorder_compress_mode::quality, cp,
          archive_members);
    }
  }
  if (preserve_id) {
    SPRING_LOG_INFO("Compressing ids");
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (!should_process_stream(stream_index, paired_end, paired_id_match))
        continue;
      SPRING_LOG_DEBUG(
          "block_id=id-stream-" + std::to_string(stream_index) +
          ", ID stream selected: index=" + std::to_string(stream_index) +
          ", path=" + id_paths[stream_index] + ", paired_id_match=" +
          std::string(paired_id_match ? "true" : "false"));
      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      std::vector<char> merged_packed_id_blocks;
      reorder_compress_from_buffer(
          id_paths[stream_index], artifact.raw_id_streams[stream_index],
          file_read_count, num_thr, num_reads_per_block, reordered_strings,
          batch_size, reordered_positions, reorder_compress_mode::id, cp,
          archive_members, paired_end ? nullptr : &merged_packed_id_blocks);

      if (paired_end) {
        SPRING_LOG_DEBUG("block_id=id-stream-" + std::to_string(stream_index) +
                         ", Skipping monolithic ID merge for paired-end mode; "
                         "keeping per-block compressed ID files.");
        continue;
      }

      const uint32_t num_blocks =
          (file_read_count + num_reads_per_block - 1) / num_reads_per_block;
      if (num_blocks > compression_params::ReadMetadata::kFileLenThrSize) {
        throw std::runtime_error(
            std::string("Exceeded maximum supported block count (") +
            std::to_string(compression_params::ReadMetadata::kFileLenThrSize) +
            "). Increase array size in params.h.");
      }
      const std::string monolithic_path = id_paths[stream_index] + ".bsc";
      add_archive_member(archive_members, monolithic_path,
                         bsc_compress_bytes(merged_packed_id_blocks));
      SPRING_LOG_DEBUG(
          "block_id=id-merge-stream-" + std::to_string(stream_index) +
          ", Monolithic ID block merge/compress complete from memory: stream=" +
          std::to_string(stream_index) +
          ", blocks=" + std::to_string(num_blocks) +
          ", packed_bytes=" + std::to_string(merged_packed_id_blocks.size()) +
          ", output=" + monolithic_path);
    }
  }

  if (cp.encoding.poly_at_stripped) {
    SPRING_LOG_INFO("Reordering poly-A/T tails");
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (stream_index == 1 && !paired_end)
        continue;
      std::string tail_path =
          "tail_" + std::to_string(stream_index + 1) + ".bin";
      if (artifact.raw_tail_streams[stream_index].empty())
        continue;

      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      struct TailRecord {
        uint16_t info = 0;
        std::string qual;
      };
      std::vector<TailRecord> tails(file_read_count);
      size_t tail_cursor = 0;
      for (uint32_t i = 0; i < file_read_count; i++) {
        uint16_t info = 0;
        if (tail_cursor + sizeof(uint16_t) >
            artifact.raw_tail_streams[stream_index].size()) {
          throw std::runtime_error("Truncated poly-A/T tail stream: " +
                                   tail_path);
        }
        std::memcpy(
            &info, artifact.raw_tail_streams[stream_index].data() + tail_cursor,
            sizeof(uint16_t));
        tail_cursor += sizeof(uint16_t);
        tails[i].info = info;
        const uint32_t tail_len = info >> 1;
        if (tail_len > 0) {
          if (tail_cursor + tail_len >
              artifact.raw_tail_streams[stream_index].size()) {
            throw std::runtime_error("Truncated poly-A/T tail payload: " +
                                     tail_path);
          }
          tails[i].qual.assign(artifact.raw_tail_streams[stream_index].data() +
                                   tail_cursor,
                               tail_len);
          tail_cursor += tail_len;
        }
      }

      std::vector<TailRecord> reordered_tails(file_read_count);
      for (uint32_t i = 0; i < file_read_count; i++) {
        reordered_tails[reordered_positions[i]] = std::move(tails[i]);
      }

      std::string tail_bytes;
      tail_bytes.reserve(artifact.raw_tail_streams[stream_index].size());
      for (uint32_t i = 0; i < file_read_count; i++) {
        tail_bytes.append(
            reinterpret_cast<const char *>(&reordered_tails[i].info),
            sizeof(uint16_t));
        const uint32_t tail_len = reordered_tails[i].info >> 1;
        if (tail_len > 0) {
          tail_bytes.append(reordered_tails[i].qual);
        }
      }
      archive_members[tail_path] = std::move(tail_bytes);
    }
  }

  if (cp.encoding.atac_adapter_stripped) {
    SPRING_LOG_INFO("Reordering ATAC adapter tails");
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (stream_index == 1 && !paired_end)
        continue;
      std::string adapter_path =
          "atac_adapter_" + std::to_string(stream_index + 1) + ".bin";
      if (artifact.compressed_atac_adapter_streams[stream_index].empty()) {
        archive_members[adapter_path] = std::string();
        continue;
      }

      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      struct AdapterRecord {
        uint8_t info = 0;
        std::string qual;
      };
      std::vector<AdapterRecord> adapters(file_read_count);
      const std::vector<char> adapter_bytes =
          bsc_decompress_bytes(std::vector<char>(
              artifact.compressed_atac_adapter_streams[stream_index].begin(),
              artifact.compressed_atac_adapter_streams[stream_index].end()));
      size_t adapter_cursor = 0;
      for (uint32_t i = 0; i < file_read_count; i++) {
        uint8_t info = 0;
        if (adapter_cursor + sizeof(uint8_t) > adapter_bytes.size()) {
          throw std::runtime_error("Truncated ATAC adapter stream: " +
                                   adapter_path);
        }
        std::memcpy(&info, adapter_bytes.data() + adapter_cursor,
                    sizeof(uint8_t));
        adapter_cursor += sizeof(uint8_t);
        adapters[i].info = info;
        const uint32_t overlap = info >> 1;
        if (overlap > 0) {
          if (adapter_cursor + overlap > adapter_bytes.size()) {
            throw std::runtime_error("Truncated ATAC adapter payload: " +
                                     adapter_path);
          }
          adapters[i].qual.assign(adapter_bytes.data() + adapter_cursor,
                                  overlap);
          adapter_cursor += overlap;
        }
      }

      std::vector<AdapterRecord> reordered_adapters(file_read_count);
      for (uint32_t i = 0; i < file_read_count; i++) {
        reordered_adapters[reordered_positions[i]] = std::move(adapters[i]);
      }

      std::string reordered_adapter_bytes;
      reordered_adapter_bytes.reserve(adapter_bytes.size());
      for (uint32_t i = 0; i < file_read_count; i++) {
        reordered_adapter_bytes.push_back(
            static_cast<char>(reordered_adapters[i].info));
        const uint32_t overlap = reordered_adapters[i].info >> 1;
        if (overlap > 0) {
          reordered_adapter_bytes.append(reordered_adapters[i].qual);
        }
      }
      const std::vector<char> compressed_adapter_bytes =
          bsc_compress_bytes(std::vector<char>(reordered_adapter_bytes.begin(),
                                               reordered_adapter_bytes.end()));
      add_archive_member(archive_members, adapter_path + ".bsc",
                         compressed_adapter_bytes);
    }
  }

  return archive_members;
}

std::unordered_map<std::string, std::string> reorder_compress_quality_id(
    const std::string &artifact_root_dir,
    const std::string &read_order_entries_path, compression_params &cp,
    const std::string &scratch_root_dir, uint64_t memory_budget_bytes) {
  std::unordered_map<std::string, std::string> archive_members;
  const uint32_t num_reads = cp.read_info.num_reads;
  const int num_thr = cp.encoding.num_thr;
  const bool preserve_id = cp.encoding.preserve_id;
  const bool preserve_quality = cp.encoding.preserve_quality;
  const bool paired_end = cp.encoding.paired_end;
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  const bool paired_id_match = cp.read_info.paired_id_match;

  std::string id_paths[2] = {"id_1", "id_2"};
  std::string quality_paths[2] = {"quality_1", "quality_2"};

  std::vector<uint32_t> reordered_positions;
  if (paired_end) {
    reordered_positions.resize(num_reads / 2);
    generate_order_pe_from_file(read_order_entries_path, reordered_positions,
                                num_reads);
  } else {
    reordered_positions.resize(num_reads);
    generate_order_se_from_file(read_order_entries_path, reordered_positions,
                                num_reads);
  }

  omp_set_num_threads(num_thr);

  const uint32_t batch_size =
      compute_batch_size(num_reads, num_reads_per_block);
  std::vector<std::string> reordered_strings(batch_size);

  if (preserve_quality) {
    SPRING_LOG_INFO("Compressing qualities");
    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (!should_process_stream(stream_index, paired_end, false)) {
        continue;
      }
      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      reorder_compress_from_file(
          quality_paths[stream_index],
          (std::filesystem::path(artifact_root_dir) /
           (quality_paths[stream_index] + ".raw"))
              .string(),
          file_read_count, num_thr, num_reads_per_block, reordered_strings,
          batch_size, reordered_positions, reorder_compress_mode::quality, cp,
          archive_members, nullptr, memory_budget_bytes);
    }
  }

  if (preserve_id) {
    SPRING_LOG_INFO("Compressing ids");
    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (!should_process_stream(stream_index, paired_end, paired_id_match)) {
        continue;
      }
      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      std::vector<char> merged_packed_id_blocks;
      reorder_compress_from_file(
          id_paths[stream_index],
          (std::filesystem::path(artifact_root_dir) /
           (id_paths[stream_index] + ".raw"))
              .string(),
          file_read_count, num_thr, num_reads_per_block, reordered_strings,
          batch_size, reordered_positions, reorder_compress_mode::id, cp,
          archive_members, paired_end ? nullptr : &merged_packed_id_blocks,
          memory_budget_bytes);

      if (!paired_end) {
        add_archive_member(archive_members, id_paths[stream_index] + ".bsc",
                           bsc_compress_bytes(merged_packed_id_blocks));
      }
    }
  }

  post_encode_side_stream_artifact tail_artifact;
  for (int stream_index = 0; stream_index < 2; ++stream_index) {
    tail_artifact.raw_tail_streams[stream_index] = read_binary_file(
        (std::filesystem::path(artifact_root_dir) /
         ("tail_" + std::to_string(stream_index + 1) + ".rawbin"))
            .string());
    tail_artifact.compressed_atac_adapter_streams[stream_index] =
        read_binary_file(
            (std::filesystem::path(artifact_root_dir) /
             ("atac_adapter_" + std::to_string(stream_index + 1) + ".bsc"))
                .string());
  }

  if (cp.encoding.poly_at_stripped) {
    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (stream_index == 1 && !paired_end) {
        continue;
      }
      const std::string tail_path =
          "tail_" + std::to_string(stream_index + 1) + ".bin";
      if (tail_artifact.raw_tail_streams[stream_index].empty()) {
        continue;
      }

      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      struct TailRecord {
        uint16_t info = 0;
        std::string qual;
      };
      std::vector<TailRecord> tails(file_read_count);
      size_t tail_cursor = 0;
      for (uint32_t i = 0; i < file_read_count; ++i) {
        uint16_t info = 0;
        std::memcpy(&info,
                    tail_artifact.raw_tail_streams[stream_index].data() +
                        tail_cursor,
                    sizeof(uint16_t));
        tail_cursor += sizeof(uint16_t);
        tails[i].info = info;
        const uint32_t tail_len = info >> 1;
        if (tail_len > 0) {
          tails[i].qual.assign(
              tail_artifact.raw_tail_streams[stream_index].data() + tail_cursor,
              tail_len);
          tail_cursor += tail_len;
        }
      }

      std::vector<TailRecord> reordered_tails(file_read_count);
      for (uint32_t i = 0; i < file_read_count; ++i) {
        reordered_tails[reordered_positions[i]] = std::move(tails[i]);
      }

      std::string tail_bytes;
      tail_bytes.reserve(tail_artifact.raw_tail_streams[stream_index].size());
      for (uint32_t i = 0; i < file_read_count; ++i) {
        tail_bytes.append(
            reinterpret_cast<const char *>(&reordered_tails[i].info),
            sizeof(uint16_t));
        const uint32_t tail_len = reordered_tails[i].info >> 1;
        if (tail_len > 0) {
          tail_bytes.append(reordered_tails[i].qual);
        }
      }
      archive_members[tail_path] = std::move(tail_bytes);
    }
  }

  if (cp.encoding.atac_adapter_stripped) {
    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (stream_index == 1 && !paired_end) {
        continue;
      }
      const std::string adapter_path =
          "atac_adapter_" + std::to_string(stream_index + 1) + ".bin";
      if (tail_artifact.compressed_atac_adapter_streams[stream_index].empty()) {
        archive_members[adapter_path] = std::string();
        continue;
      }

      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      struct AdapterRecord {
        uint8_t info = 0;
        std::string qual;
      };
      std::vector<AdapterRecord> adapters(file_read_count);
      const std::vector<char> adapter_bytes = bsc_decompress_bytes(std::vector<
                                                                   char>(
          tail_artifact.compressed_atac_adapter_streams[stream_index].begin(),
          tail_artifact.compressed_atac_adapter_streams[stream_index].end()));
      size_t adapter_cursor = 0;
      for (uint32_t i = 0; i < file_read_count; ++i) {
        uint8_t info = 0;
        std::memcpy(&info, adapter_bytes.data() + adapter_cursor,
                    sizeof(uint8_t));
        adapter_cursor += sizeof(uint8_t);
        adapters[i].info = info;
        const uint32_t overlap = info >> 1;
        if (overlap > 0) {
          adapters[i].qual.assign(adapter_bytes.data() + adapter_cursor,
                                  overlap);
          adapter_cursor += overlap;
        }
      }

      std::vector<AdapterRecord> reordered_adapters(file_read_count);
      for (uint32_t i = 0; i < file_read_count; ++i) {
        reordered_adapters[reordered_positions[i]] = std::move(adapters[i]);
      }

      std::string reordered_adapter_bytes;
      reordered_adapter_bytes.reserve(adapter_bytes.size());
      for (uint32_t i = 0; i < file_read_count; ++i) {
        reordered_adapter_bytes.push_back(
            static_cast<char>(reordered_adapters[i].info));
        const uint32_t overlap = reordered_adapters[i].info >> 1;
        if (overlap > 0) {
          reordered_adapter_bytes.append(reordered_adapters[i].qual);
        }
      }
      add_archive_member(
          archive_members, adapter_path + ".bsc",
          bsc_compress_bytes(std::vector<char>(reordered_adapter_bytes.begin(),
                                               reordered_adapter_bytes.end())));
    }
  }

  std::error_code cleanup_ec;
  std::filesystem::remove_all(scratch_root_dir, cleanup_ec);
  return archive_members;
}

} // namespace spring
