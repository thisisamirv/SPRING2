// Normalizes input FASTQ/FASTA records into Spring's temporary block files and
// side streams before reordering, encoding, and archive assembly.

#include "input_preparation.h"
#include "assay_atac.h"
#include "assay_rna.h"
#include "assay_sc_atac.h"
#include "assay_sc_bisulfite.h"
#include "assay_sc_rna.h"
#include "preprocess_artifacts.h"
#include "progress.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <omp.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "io_utils.h"
#include "params.h"
#include "parse_utils.h"

namespace spring {

bool has_non_acgtn_symbol(const std::string &read);

namespace {

bool is_gzip_input_path(const std::string &path) {
  return has_suffix(path, ".gz");
}

struct preprocess_paths {
  std::array<std::string, 2> input_paths;
  std::array<std::string, 2> id_output_paths;
  std::array<std::string, 2> quality_output_paths;
  std::array<std::string, 2> read_block_paths;
  std::array<std::string, 2> read_length_paths;
};

std::string block_file_path(const std::string &base_path,
                            const uint32_t block_num) {
  return base_path + "." + std::to_string(block_num);
}

std::string compressed_block_file_path(const std::string &base_path,
                                       const uint32_t block_num) {
  return block_file_path(base_path, block_num) + ".bsc";
}

uint32_t block_count(const uint64_t num_reads,
                     const uint32_t num_reads_per_block) {
  if (num_reads == 0)
    return 0;
  return static_cast<uint32_t>(1 + (num_reads - 1) / num_reads_per_block);
}

void open_input_stream(std::ifstream &file_stream, std::istream *&input_stream,
                       std::unique_ptr<gzip_istream> &gzip_stream,
                       const std::string &path, const bool gzip_enabled) {
  if (gzip_enabled) {
    gzip_stream = std::make_unique<gzip_istream>(path);
    input_stream = gzip_stream.get();
    return;
  }

  file_stream.open(path, std::ios::binary);
  input_stream = &file_stream;
  gzip_stream.reset();
}

void reset_input_stream(std::ifstream &file_stream, std::istream *&input_stream,
                        std::unique_ptr<gzip_istream> &gzip_stream,
                        const std::string &path, const bool gzip_enabled) {
  if (gzip_enabled) {
    file_stream.close();
    gzip_stream.reset();
    input_stream = nullptr;
    open_input_stream(file_stream, input_stream, gzip_stream, path, true);
    return;
  }

  file_stream.clear();
  file_stream.seekg(0);
}

void close_input_stream(std::ifstream &file_stream, std::istream *&input_stream,
                        std::unique_ptr<gzip_istream> &gzip_stream,
                        const bool gzip_enabled) {
  if (gzip_enabled) {
    gzip_stream.reset();
    input_stream = nullptr;
  }
  file_stream.close();
}

preprocess_paths build_preprocess_paths(const std::string &input_path_1,
                                        const std::string &input_path_2) {
  static constexpr std::string_view kInMemoryBaseDir = "in-memory";
  preprocess_paths paths;
  paths.input_paths = {input_path_1, input_path_2};
  paths.id_output_paths = {std::string(kInMemoryBaseDir) + "/id_1",
                           std::string(kInMemoryBaseDir) + "/id_2"};
  paths.quality_output_paths = {std::string(kInMemoryBaseDir) + "/quality_1",
                                std::string(kInMemoryBaseDir) + "/quality_2"};
  paths.read_block_paths = {std::string(kInMemoryBaseDir) + "/read_1",
                            std::string(kInMemoryBaseDir) + "/read_2"};
  paths.read_length_paths = {std::string(kInMemoryBaseDir) + "/readlength_1",
                             std::string(kInMemoryBaseDir) + "/readlength_2"};
  return paths;
}

uint32_t reads_for_thread_step(const uint32_t reads_in_step,
                               const uint32_t num_reads_per_block,
                               const uint64_t thread_id) {
  return static_cast<uint32_t>(
      std::min((uint64_t)reads_in_step, (thread_id + 1) * num_reads_per_block) -
      thread_id * num_reads_per_block);
}

void open_preprocess_streams(
    std::array<std::ifstream, 2> &input_files,
    std::array<std::istream *, 2> &input_streams,
    std::array<std::unique_ptr<gzip_istream>, 2> &gzip_streams,
    const preprocess_paths &paths, const compression_params &compression_params,
    const std::array<bool, 2> &gzip_enabled) {
  for (int stream_index = 0; stream_index < 2; stream_index++) {
    if (stream_index == 1 && !compression_params.encoding.paired_end)
      continue;

    open_input_stream(input_files[stream_index], input_streams[stream_index],
                      gzip_streams[stream_index],
                      paths.input_paths[stream_index],
                      gzip_enabled[stream_index]);
  }
}

void close_preprocess_streams(
    std::array<std::ifstream, 2> &input_files,
    std::array<std::istream *, 2> &input_streams,
    std::array<std::unique_ptr<gzip_istream>, 2> &gzip_streams,
    const compression_params &compression_params,
    const std::array<bool, 2> &gzip_enabled) {
  for (int stream_index = 0; stream_index < 2; stream_index++) {
    if (stream_index == 1 && !compression_params.encoding.paired_end)
      continue;

    close_input_stream(input_files[stream_index], input_streams[stream_index],
                       gzip_streams[stream_index], gzip_enabled[stream_index]);
  }
}

// Detect whether the FASTQ uses "+ID" format or just "+" for quality headers.
// Checks the first FASTQ record and returns true if the plus line contains
// anything beyond the initial '+' character.
void detect_quality_header_format(
    std::array<std::ifstream, 2> &input_files,
    std::array<std::istream *, 2> &input_streams,
    std::array<std::unique_ptr<gzip_istream>, 2> &gzip_streams,
    const preprocess_paths &paths, const compression_params &compression_params,
    const std::array<bool, 2> &gzip_enabled,
    std::array<bool, 2> &quality_header_has_id_by_stream) {
  if (compression_params.encoding.fasta_mode) {
    quality_header_has_id_by_stream = {false, false};
    return;
  }

  for (int stream_index = 0; stream_index < 2; ++stream_index) {
    if (stream_index == 1 && !compression_params.encoding.paired_end)
      continue;

    std::string line;
    if (!std::getline(*input_streams[stream_index], line) ||
        !std::getline(*input_streams[stream_index], line) ||
        !std::getline(*input_streams[stream_index], line)) {
      continue;
    }

    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    quality_header_has_id_by_stream[stream_index] = line.length() > 1;

    SPRING_LOG_DEBUG(
        "Quality header detection: stream=" + std::to_string(stream_index + 1) +
        ", plus_line_length=" + std::to_string(line.length()) + ", has_id=" +
        std::string(quality_header_has_id_by_stream[stream_index] ? "true"
                                                                  : "false"));

    reset_input_stream(input_files[stream_index], input_streams[stream_index],
                       gzip_streams[stream_index],
                       paths.input_paths[stream_index],
                       gzip_enabled[stream_index]);
  }
}

void detect_paired_id_pattern(
    std::array<std::ifstream, 2> &input_files,
    std::array<std::istream *, 2> &input_streams,
    std::array<std::unique_ptr<gzip_istream>, 2> &gzip_streams,
    const preprocess_paths &paths, const compression_params &compression_params,
    const std::array<bool, 2> &gzip_enabled, uint8_t &paired_id_code,
    bool &paired_id_match) {
  if (!compression_params.encoding.paired_end ||
      !compression_params.encoding.preserve_id)
    return;

  std::string id_1;
  std::string id_2;
  std::getline(*input_streams[0], id_1);
  std::getline(*input_streams[1], id_2);
  paired_id_code = find_id_pattern(id_1, id_2);
  paired_id_match = paired_id_code != 0;

  for (int stream_index = 0; stream_index < 2; stream_index++) {
    reset_input_stream(input_files[stream_index], input_streams[stream_index],
                       gzip_streams[stream_index],
                       paths.input_paths[stream_index],
                       gzip_enabled[stream_index]);
  }
}

void merge_paired_n_reads(std::array<std::string, 2> &n_read_streams,
                          std::array<std::vector<uint32_t>, 2> &n_read_orders,
                          const std::array<uint64_t, 2> &num_reads) {
  n_read_streams[0].append(n_read_streams[1]);
  n_read_streams[1].clear();

  for (uint32_t &n_read_order : n_read_orders[1]) {
    n_read_order += static_cast<uint32_t>(num_reads[0]);
  }
  n_read_orders[0].insert(n_read_orders[0].end(), n_read_orders[1].begin(),
                          n_read_orders[1].end());
  n_read_orders[1].clear();
}

void append_n_order_bytes(std::string &output_bytes,
                          const std::vector<uint32_t> &positions) {
  const size_t old_size = output_bytes.size();
  output_bytes.resize(old_size + positions.size() * sizeof(uint32_t));
  if (!positions.empty()) {
    std::memcpy(output_bytes.data() + old_size, positions.data(),
                positions.size() * sizeof(uint32_t));
  }
}

void remove_redundant_mate_ids(
    const preprocess_paths &paths, const compression_params &compression_params,
    const bool paired_id_match,
    std::unordered_map<std::string, std::string> &archive_members,
    const std::array<uint64_t, 2> &num_reads,
    const uint32_t num_reads_per_block) {
  if (!compression_params.encoding.paired_end || !paired_id_match)
    return;

  if (!compression_params.encoding.long_flag &&
      !compression_params.encoding.preserve_order) {
    return;
  }

  const uint32_t num_blocks = block_count(num_reads[0], num_reads_per_block);
  for (uint32_t block_index = 0; block_index < num_blocks; block_index++) {
    const std::string block_path =
        compression_params.encoding.long_flag
            ? compressed_block_file_path(paths.id_output_paths[1], block_index)
            : block_file_path(paths.id_output_paths[1], block_index);
    archive_members.erase(
        std::filesystem::path(block_path).filename().string());
  }
}

uint32_t max_read_length_in_step(const std::vector<uint32_t> &read_lengths,
                                 const uint32_t reads_in_step) {
  if (reads_in_step == 0)
    return 0;
  return *(std::max_element(read_lengths.begin(),
                            read_lengths.begin() + reads_in_step));
}

bool input_summary_differs(const input_detection_summary &expected,
                           const input_detection_summary &observed,
                           const bool paired_end) {
  if (expected.max_read_length != observed.max_read_length ||
      expected.contains_non_acgtn_symbols !=
          observed.contains_non_acgtn_symbols ||
      expected.use_crlf_by_stream[0] != observed.use_crlf_by_stream[0]) {
    return true;
  }
  return paired_end &&
         expected.use_crlf_by_stream[1] != observed.use_crlf_by_stream[1];
}

void detect_input_properties_in_file(const std::string &path,
                                     const bool fasta_input,
                                     input_detection_summary &summary,
                                     const int stream_index) {
  std::ifstream file_stream;
  std::istream *input_stream = nullptr;
  std::unique_ptr<gzip_istream> gzip_stream;
  open_input_stream(file_stream, input_stream, gzip_stream, path,
                    is_gzip_input_path(path));
  if (input_stream == nullptr)
    throw std::runtime_error("Can't open file for pre-scan: " + path);

  std::string line;
  if (fasta_input) {
    uint32_t current_len = 0;
    while (std::getline(*input_stream, line)) {
      if (!line.empty() && line.back() == '\r') {
        summary.use_crlf_by_stream[stream_index] = true;
        line.pop_back();
      }
      if (line.empty())
        continue;
      if (line[0] == '>') {
        summary.max_read_length =
            std::max(summary.max_read_length, current_len);
        current_len = 0;
      } else {
        if (!summary.contains_non_acgtn_symbols && has_non_acgtn_symbol(line)) {
          summary.contains_non_acgtn_symbols = true;
        }
        current_len += static_cast<uint32_t>(line.length());
      }
    }
    summary.max_read_length = std::max(summary.max_read_length, current_len);
    return;
  }

  while (std::getline(*input_stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      summary.use_crlf_by_stream[stream_index] = true;
      line.pop_back();
    }
    if (!std::getline(*input_stream, line)) {
      break;
    }
    if (!line.empty() && line.back() == '\r') {
      summary.use_crlf_by_stream[stream_index] = true;
      line.pop_back();
    }
    if (!summary.contains_non_acgtn_symbols && has_non_acgtn_symbol(line)) {
      summary.contains_non_acgtn_symbols = true;
    }
    summary.max_read_length =
        std::max(summary.max_read_length, static_cast<uint32_t>(line.length()));

    if (std::getline(*input_stream, line) && !line.empty() &&
        line.back() == '\r') {
      summary.use_crlf_by_stream[stream_index] = true;
    }
    if (std::getline(*input_stream, line) && !line.empty() &&
        line.back() == '\r') {
      summary.use_crlf_by_stream[stream_index] = true;
    }
  }

  close_input_stream(file_stream, input_stream, gzip_stream,
                     is_gzip_input_path(path));
}

} // namespace

preprocess_retry_exception::preprocess_retry_exception(
    const input_detection_summary &summary)
    : std::runtime_error(
          "Input properties changed during preprocessing; retry required."),
      updated_summary_(summary) {}

bool has_non_acgtn_symbol(const std::string &read) {
  return std::ranges::any_of(read, [](char base) {
    if (base == '\r' || base == '\n' || base == ' ')
      return false;
    return base != 'A' && base != 'C' && base != 'G' && base != 'T' &&
           base != 'N';
  });
}

input_detection_summary detect_input_properties(const std::string &infile_1,
                                                const std::string &infile_2,
                                                const bool paired_end,
                                                const bool fasta_input) {
  SPRING_LOG_INFO("Auto-detecting read lengths and line endings ...");
  input_detection_summary summary;
  detect_input_properties_in_file(infile_1, fasta_input, summary, 0);
  if (paired_end) {
    detect_input_properties_in_file(infile_2, fasta_input, summary, 1);
  }
  return summary;
}

preprocess_artifact
preprocess(const std::string &infile_1, const std::string &infile_2,
           compression_params &cp, const bool &fasta_input,
           ProgressBar *progress,
           const input_detection_summary *expected_summary) {
  preprocess_artifact output_artifact;
  SPRING_LOG_DEBUG(
      "Preprocess start: input1=" + infile_1 +
      (cp.encoding.paired_end ? (", input2=" + infile_2) : std::string()) +
      ", long_mode=" + std::string(cp.encoding.long_flag ? "true" : "false") +
      ", paired_end=" + std::string(cp.encoding.paired_end ? "true" : "false") +
      ", preserve_order=" +
      std::string(cp.encoding.preserve_order ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false") +
      ", fasta_input=" + std::string(fasta_input ? "true" : "false"));
  const preprocess_paths paths = build_preprocess_paths(infile_1, infile_2);
  const std::array<bool, 2> input_gzip_enabled = {
      is_gzip_input_path(infile_1),
      cp.encoding.paired_end && is_gzip_input_path(infile_2)};
  std::array<std::ifstream, 2> input_files;
  std::array<std::istream *, 2> input_streams = {nullptr, nullptr};
  std::array<std::unique_ptr<gzip_istream>, 2> gzip_streams{};
  std::array<std::string, 2> n_read_streams;
  std::array<std::vector<uint32_t>, 2> n_read_orders;

  open_preprocess_streams(input_files, input_streams, gzip_streams, paths, cp,
                          input_gzip_enabled);

  // Determine whether poly-A/T tail stripping should be applied.
  // Only active for: short-read, non-preserve_order, rna assay with
  // high-confidence auto-detection OR explicit -y rna (confidence == "N/A").
  const bool apply_poly_at = should_enable_rna_tail_stripping(cp);

  const bool allow_grouped_atac_adapter_strip =
      grouped_archive_allows_atac_adapter_stripping(cp.read_info.note);
  const bool sc_atac_assay = cp.read_info.assay == "sc-atac";
  const bool maybe_apply_atac_adapter_strip =
      allow_grouped_atac_adapter_strip &&
      (sc_atac_assay
           ? should_consider_sc_atac_adapter_stripping(cp, fasta_input)
           : should_consider_atac_adapter_stripping(
                 cp, fasta_input, allow_grouped_atac_adapter_strip));
  constexpr double kMinAtacAdapterBasesPerRead = 1.0;
  bool atac_adapter_strip_active = false;
  bool atac_adapter_strip_decided = !maybe_apply_atac_adapter_strip;

  if (apply_poly_at) {
    SPRING_LOG_DEBUG("poly-A/T tail stripping enabled (min run length > " +
                     std::to_string(POLY_AT_TAIL_MIN_LEN) + " bp)");
  }

  if (maybe_apply_atac_adapter_strip) {
    SPRING_LOG_DEBUG("ATAC adapter stripping enabled (min overlap >= " +
                     std::to_string(ATAC_ADAPTER_MIN_MATCH) + " bp)");
  }

  // Determine whether single-cell barcode prefix stripping should be applied.
  // Only active for short-read, preserve_order archives when the assay keeps
  // the CB in R1 instead of a separate index lane.
  //
  // For sc-RNA, CB stripping is always enabled when no external index is
  // present, as RNA protocols consistently place barcodes in R1.
  //
  // For sc-bisulfite, we disable CB stripping entirely as bisulfite protocols
  // vary widely in their read structure - some use R1 for barcodes (like 10x),
  // others use R1 for genomic sequence (like scSPLAT). Without a reliable
  // heuristic to detect which structure is present, we conservatively skip CB
  // extraction to avoid data corruption. Future work could add
  // protocol-specific detection.
  const bool apply_cb_strip =
      should_enable_sc_rna_cb_prefix_stripping(cp) ||
      should_enable_sc_bisulfite_cb_prefix_stripping(cp);
  const bool maybe_strip_grouped_index_id_suffix =
      should_enable_grouped_sc_rna_index_suffix_stripping(cp);
  bool grouped_index_id_suffix_strip_active = false;
  bool grouped_index_id_suffix_strip_decided =
      !maybe_strip_grouped_index_id_suffix;

  std::string cb_seq_bytes;
  std::string cb_qual_bytes;
  if (apply_cb_strip) {
    SPRING_LOG_DEBUG(
        "Single-cell R1 prefix stripping enabled (assay=" + cp.read_info.assay +
        ", cb_len=" + std::to_string(cp.encoding.cb_len) + " bp)");
  }

  uint64_t total_input_bytes = 0;
  total_input_bytes += std::filesystem::exists(infile_1)
                           ? std::filesystem::file_size(infile_1)
                           : 0;
  if (cp.encoding.paired_end) {
    total_input_bytes += std::filesystem::exists(infile_2)
                             ? std::filesystem::file_size(infile_2)
                             : 0;
  }

  uint32_t max_readlen = 0;
  input_detection_summary observed_summary;
  std::array<uint64_t, 2> num_reads = {0, 0};
  std::array<uint64_t, 2> num_reads_clean = {0, 0};
  uint32_t num_reads_per_block;
  if (!cp.encoding.long_flag)
    num_reads_per_block = cp.encoding.num_reads_per_block;
  else
    num_reads_per_block = cp.encoding.num_reads_per_block_long;
  uint8_t paired_id_code = 0;
  bool paired_id_match = false;

  std::array<char, 128> quality_binning_table{};
  if (cp.quality.ill_bin_flag)
    generate_illumina_binning_table(quality_binning_table.data());
  if (cp.quality.bin_thr_flag)
    generate_binary_binning_table(
        quality_binning_table.data(), cp.quality.bin_thr_thr,
        cp.quality.bin_thr_high, cp.quality.bin_thr_low);

  if ((input_gzip_enabled[0] &&
       (!gzip_streams[0] || !gzip_streams[0]->is_open())) ||
      (!input_gzip_enabled[0] && !input_files[0].is_open()))
    throw std::runtime_error("Error opening input file");
  if (cp.encoding.paired_end) {
    if ((input_gzip_enabled[1] &&
         (!gzip_streams[1] || !gzip_streams[1]->is_open())) ||
        (!input_gzip_enabled[1] && !input_files[1].is_open()))
      throw std::runtime_error("Error opening input file");
  }
  detect_paired_id_pattern(input_files, input_streams, gzip_streams, paths, cp,
                           input_gzip_enabled, paired_id_code, paired_id_match);
  if (cp.encoding.paired_end && cp.encoding.preserve_id) {
    SPRING_LOG_DEBUG(
        "Paired ID pattern detection: code=" +
        std::to_string(static_cast<int>(paired_id_code)) +
        ", match=" + std::string(paired_id_match ? "true" : "false"));
  }

  // Detect quality header format ("+ID" vs just "+")
  std::array<bool, 2> quality_header_has_id_by_stream = {false, false};
  detect_quality_header_format(input_files, input_streams, gzip_streams, paths,
                               cp, input_gzip_enabled,
                               quality_header_has_id_by_stream);
  SPRING_LOG_DEBUG(
      "Quality header formats: stream1=" +
      std::string(quality_header_has_id_by_stream[0] ? "+ID" : "+") +
      (cp.encoding.paired_end
           ? (", stream2=" +
              std::string(quality_header_has_id_by_stream[1] ? "+ID" : "+"))
           : std::string()));

  // Initialize integrity digests
  for (int i = 0; i < 2; ++i) {
    cp.read_info.sequence_crc[i] = 0;
    cp.read_info.quality_crc[i] = 0;
    cp.read_info.id_crc[i] = 0;
  }
  if (cp.encoding.num_thr <= 0)
    throw std::runtime_error("Number of threads must be positive.");

  uint64_t num_reads_per_step =
      (uint64_t)cp.encoding.num_thr * num_reads_per_block;
  std::vector<std::string> read_array(num_reads_per_step);
  std::vector<std::string> id_array_1(num_reads_per_step);
  std::vector<std::string> id_array_2;
  if (cp.encoding.paired_end)
    id_array_2.resize(num_reads_per_step);

  std::vector<std::string> quality_array;
  if (!fasta_input)
    quality_array.resize(num_reads_per_step);

  std::vector<uint8_t> read_contains_N_array(num_reads_per_step, 0);
  std::vector<uint32_t> read_lengths_array(num_reads_per_step);
  std::vector<uint8_t> paired_id_match_array(
      static_cast<size_t>(cp.encoding.num_thr), 0);

  std::vector<short_read_thread_buffers> short_read_buffers;
  std::string quality_chunk;
  std::string id_chunk;
  if (!cp.encoding.long_flag) {
    short_read_buffers.resize(static_cast<size_t>(cp.encoding.num_thr));
  }

  omp_set_num_threads(cp.encoding.num_thr);

  uint32_t num_blocks_done = 0;

  while (true) {
    std::array<bool, 2> done = {true, true};
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (stream_index == 1 && !cp.encoding.paired_end)
        continue;
      done[stream_index] = false;
      std::string *id_array =
          (stream_index == 0) ? id_array_1.data() : id_array_2.data();
      uint8_t *contains_n_output =
          cp.encoding.long_flag ? nullptr : read_contains_N_array.data();
      uint32_t *quality_crc_output =
          cp.encoding.preserve_quality ? &cp.read_info.quality_crc[stream_index]
                                       : nullptr;
      uint32_t *id_crc_output = cp.encoding.preserve_id
                                    ? &cp.read_info.id_crc[stream_index]
                                    : nullptr;
      uint32_t *sequence_crc_output = &cp.read_info.sequence_crc[stream_index];
      bool block_saw_crlf = false;
      uint32_t reads_in_step = read_fastq_block(
          input_streams[stream_index], id_array, read_array.data(),
          quality_array.empty() ? nullptr : quality_array.data(),
          static_cast<uint32_t>(num_reads_per_step), fasta_input,
          read_lengths_array.data(), contains_n_output,
          sequence_crc_output, // Compute CRC on original data before stripping
          quality_crc_output, id_crc_output, cp.encoding.preserve_quality,
          &block_saw_crlf);

      observed_summary.use_crlf_by_stream[stream_index] =
          observed_summary.use_crlf_by_stream[stream_index] || block_saw_crlf;
      observed_summary.sampled_fragments += reads_in_step;

      for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
        observed_summary.max_read_length =
            std::max(observed_summary.max_read_length, read_lengths_array[ri]);
        if (!observed_summary.contains_non_acgtn_symbols &&
            has_non_acgtn_symbol(read_array[ri])) {
          observed_summary.contains_non_acgtn_symbols = true;
        }
      }

      if (expected_summary != nullptr && !cp.encoding.long_flag) {
        if (observed_summary.contains_non_acgtn_symbols ||
            observed_summary.max_read_length > MAX_READ_LEN) {
          throw preprocess_retry_exception(observed_summary);
        }
      }

      // Strip poly-A/T tails and accumulate per-read tail info.
      // We keep a local buffer (step_tail_info) indexed by read position in
      // this step so the parallel encoding section below can look it up.
      std::vector<uint16_t> step_tail_info;
      if (apply_poly_at && reads_in_step > 0) {
        step_tail_info.resize(reads_in_step, 0);
        bool any_stripped = false;
        // Strip serially — modifies read_array and read_lengths_array in place.
        for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
          if (read_contains_N_array[ri] != 0) {
            continue;
          }
          const uint16_t strip_info =
              detect_and_strip_rna_tail(read_array[ri], read_lengths_array[ri]);
          step_tail_info[ri] = strip_info;
          if ((strip_info >> 1) > 0) {
            any_stripped = true;
          }
        }
        if (any_stripped)
          cp.encoding.poly_at_stripped = true;
      }

      std::vector<uint8_t> step_atac_adapter_info;
      if (maybe_apply_atac_adapter_strip && reads_in_step > 0) {
        step_atac_adapter_info.resize(reads_in_step, 0);
        uint32_t eligible_reads = 0;
        uint64_t stripped_bases = 0;
        for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
          if (read_contains_N_array[ri] != 0) {
            continue;
          }
          const uint8_t strip_info =
              sc_atac_assay
                  ? detect_sc_atac_adapter_tail_info(read_array[ri],
                                                     read_lengths_array[ri])
                  : detect_atac_adapter_tail_info(read_array[ri],
                                                  read_lengths_array[ri]);
          step_atac_adapter_info[ri] = strip_info;
          eligible_reads++;
          stripped_bases += static_cast<uint64_t>(strip_info >> 1);
        }
        if (!atac_adapter_strip_decided) {
          const double avg_stripped_bases_per_read =
              eligible_reads == 0 ? 0.0
                                  : static_cast<double>(stripped_bases) /
                                        static_cast<double>(eligible_reads);
          atac_adapter_strip_active =
              sc_atac_assay ? should_activate_sc_atac_adapter_stripping(
                                  eligible_reads, stripped_bases,
                                  kMinAtacAdapterBasesPerRead)
                            : should_activate_atac_adapter_stripping(
                                  eligible_reads, stripped_bases,
                                  kMinAtacAdapterBasesPerRead);
          atac_adapter_strip_decided = true;
          SPRING_LOG_DEBUG(
              std::string("ATAC adapter stripping ") +
              (atac_adapter_strip_active ? "enabled" : "disabled") +
              " after sampling " + std::to_string(eligible_reads) +
              " reads (avg stripped bases/read=" +
              std::to_string(avg_stripped_bases_per_read) + ")");
        }
        if (atac_adapter_strip_active) {
          bool any_stripped = false;
          for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
            const uint32_t strip_len = step_atac_adapter_info[ri] >> 1;
            if (strip_len == 0) {
              continue;
            }
            if (sc_atac_assay) {
              strip_sc_atac_adapter_tail(read_array[ri], read_lengths_array[ri],
                                         step_atac_adapter_info[ri]);
            } else {
              strip_atac_adapter_tail(read_array[ri], read_lengths_array[ri],
                                      step_atac_adapter_info[ri]);
            }
            any_stripped = true;
          }
          if (any_stripped)
            cp.encoding.atac_adapter_stripped = true;
        } else {
          step_atac_adapter_info.clear();
        }
      }

      // Sequence CRC was already computed by read_fastq_block above on original
      // data, matching what decompression will compute after full restoration.

      if (maybe_strip_grouped_index_id_suffix && stream_index == 0 &&
          reads_in_step > 0) {
        if (!grouped_index_id_suffix_strip_decided) {
          grouped_index_id_suffix_strip_active = true;
          for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
            std::string candidate_id = id_array[ri];
            if (!strip_grouped_sc_rna_index_suffix_from_id(
                    candidate_id, read_array[ri], cp.encoding.paired_end)) {
              grouped_index_id_suffix_strip_active = false;
              break;
            }
          }
          grouped_index_id_suffix_strip_decided = true;
          if (grouped_index_id_suffix_strip_active) {
            SPRING_LOG_DEBUG(
                "Grouped sc-RNA index-ID suffix stripping enabled "
                "(reconstruct trailing I1/I2 token from index reads)");
          }
        }

        if (grouped_index_id_suffix_strip_active) {
          for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
            if (!strip_grouped_sc_rna_index_suffix_from_id(
                    id_array[ri], read_array[ri], cp.encoding.paired_end)) {
              throw std::runtime_error(
                  "Grouped sc-RNA index-ID suffix stripping became "
                  "inconsistent across reads.");
            }
          }
          cp.encoding.index_id_suffix_reconstructed = true;
        }
      }

      // Single-cell R1 prefix stripping must happen AFTER CRC computation but
      // BEFORE quality/read compression so that compressed data uses the
      // stripped lengths while integrity remains defined on full reads.
      if (apply_cb_strip && stream_index == 0 && reads_in_step > 0) {
        std::string cb_seq_buffer, cb_qual_buffer;
        cb_seq_buffer.reserve(static_cast<size_t>(reads_in_step) *
                              static_cast<size_t>(cp.encoding.cb_len));
        if (cp.encoding.preserve_quality) {
          cb_qual_buffer.reserve(static_cast<size_t>(reads_in_step) *
                                 static_cast<size_t>(cp.encoding.cb_len));
        }
        for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
          const std::string &read_seq = read_array[ri];
          if (read_seq.size() < cp.encoding.cb_len) {
            throw std::runtime_error(
                "CB prefix stripping requires all R1 reads to be at least " +
                std::to_string(cp.encoding.cb_len) + " bases long.");
          }
          const uint32_t strip_len = cp.encoding.cb_len;
          // Store CB sequence.
          cb_seq_buffer.append(read_seq.substr(0, strip_len));
          if (cp.encoding.preserve_quality) {
            const std::string &qual = quality_array[ri];
            cb_qual_buffer.append(qual.substr(0, strip_len));
            // Strip CB from quality.
            quality_array[ri] = qual.substr(strip_len);
          }
          // Strip CB from read sequence.
          read_array[ri] = read_seq.substr(strip_len);
          // Update read length to reflect stripped read.
          read_lengths_array[ri] = static_cast<uint16_t>(read_array[ri].size());
        }
        cb_seq_bytes.append(cb_seq_buffer);
        if (cp.encoding.preserve_quality) {
          cb_qual_bytes.append(cb_qual_buffer);
        }
        cp.encoding.cb_prefix_stripped = true;
        cp.encoding.cb_prefix_len = cp.encoding.cb_len;
      }

      std::string preprocess_step_log;
      preprocess_step_log.reserve(96);
      preprocess_step_log.append("Preprocess step: stream=");
      preprocess_step_log.append(std::to_string(stream_index + 1));
      preprocess_step_log.append(", reads_in_step=");
      preprocess_step_log.append(std::to_string(reads_in_step));
      preprocess_step_log.append(", blocks_done=");
      preprocess_step_log.append(std::to_string(num_blocks_done));
      SPRING_LOG_DEBUG(preprocess_step_log);
      if (reads_in_step < num_reads_per_step)
        done[stream_index] = true;
      if (reads_in_step == 0)
        continue;

      if (num_reads[0] + num_reads[1] + reads_in_step > MAX_NUM_READS) {
        std::cerr << "Max number of reads allowed is " << MAX_NUM_READS << "\n";
        throw std::runtime_error("Too many reads.");
      }
      std::vector<std::unordered_map<std::string, std::string>>
          thread_archive_members(static_cast<size_t>(cp.encoding.num_thr));
      // Use parallel for so every thread_id slot is processed even when
      // ClangCL/libomp on Windows creates fewer threads than requested inside
      // a bare #pragma omp parallel region.
#pragma omp parallel for schedule(static, 1)
      for (int thread_id_int = 0; thread_id_int < cp.encoding.num_thr;
           ++thread_id_int) {
        uint64_t thread_id = static_cast<uint64_t>(thread_id_int);
        bool thread_done = false;
        if (stream_index == 1)
          paired_id_match_array[thread_id] = paired_id_match ? 1 : 0;
        if (thread_id * num_reads_per_block >= reads_in_step)
          thread_done = true;
        const uint32_t block_num =
            num_blocks_done + static_cast<uint32_t>(thread_id);
        const uint32_t thread_read_count = reads_for_thread_step(
            reads_in_step, num_reads_per_block, thread_id);
        if (!thread_done) {
          for (uint32_t read_index =
                   static_cast<uint32_t>(thread_id * num_reads_per_block);
               read_index < thread_id * num_reads_per_block + thread_read_count;
               read_index++) {
            const uint32_t read_length = read_lengths_array[read_index];
            if (read_length > MAX_READ_LEN_LONG) {
              std::cerr << "Max read length for long mode is "
                        << MAX_READ_LEN_LONG << ", but found read of length "
                        << read_length << "\n";
              throw std::runtime_error("Too long read length");
            }

            if (stream_index == 1 && paired_id_match_array[thread_id] &&
                !grouped_index_id_suffix_strip_active)
              paired_id_match_array[thread_id] =
                  check_id_pattern(id_array_1[read_index],
                                   id_array_2[read_index], paired_id_code);
          }
          if (cp.encoding.preserve_quality &&
              (cp.quality.ill_bin_flag || cp.quality.bin_thr_flag))
            quantize_quality(quality_array.data() +
                                 thread_id * num_reads_per_block,
                             thread_read_count, quality_binning_table.data());

          if (!cp.encoding.long_flag) {
            if (cp.encoding.preserve_order) {
              if (cp.encoding.preserve_id) {
                add_archive_member(
                    thread_archive_members[static_cast<size_t>(thread_id)],
                    std::filesystem::path(
                        block_file_path(paths.id_output_paths[stream_index],
                                        num_blocks_done +
                                            static_cast<uint32_t>(thread_id)))
                        .filename()
                        .string(),
                    compress_id_block_bytes(id_array +
                                                thread_id * num_reads_per_block,
                                            thread_read_count));
              }
              if (cp.encoding.preserve_quality) {
                add_archive_member(
                    thread_archive_members[static_cast<size_t>(thread_id)],
                    std::filesystem::path(
                        block_file_path(
                            paths.quality_output_paths[stream_index],
                            num_blocks_done + static_cast<uint32_t>(thread_id)))
                        .filename()
                        .string(),
                    bsc_str_array_compress_bytes(
                        quality_array.data() + thread_id * num_reads_per_block,
                        thread_read_count,
                        read_lengths_array.data() +
                            thread_id * num_reads_per_block));
              }
            }
          } else {
            add_archive_member(
                thread_archive_members[static_cast<size_t>(thread_id)],
                std::filesystem::path(
                    compressed_block_file_path(
                        paths.read_length_paths[stream_index], block_num))
                    .filename()
                    .string(),
                bsc_compress_bytes(build_read_length_block_bytes(
                    read_lengths_array.data() + thread_id * num_reads_per_block,
                    thread_read_count)));
            if (cp.encoding.preserve_id) {
              add_archive_member(
                  thread_archive_members[static_cast<size_t>(thread_id)],
                  std::filesystem::path(
                      compressed_block_file_path(
                          paths.id_output_paths[stream_index], block_num))
                      .filename()
                      .string(),
                  compress_id_block_bytes(id_array +
                                              thread_id * num_reads_per_block,
                                          thread_read_count));
            }
            if (cp.encoding.preserve_quality) {
              add_archive_member(
                  thread_archive_members[static_cast<size_t>(thread_id)],
                  std::filesystem::path(
                      block_file_path(paths.quality_output_paths[stream_index],
                                      block_num))
                      .filename()
                      .string(),
                  bsc_compress_bytes(build_raw_string_block_bytes(
                      quality_array.data() + thread_id * num_reads_per_block,
                      thread_read_count,
                      read_lengths_array.data() +
                          thread_id * num_reads_per_block)));
            }
            add_archive_member(
                thread_archive_members[static_cast<size_t>(thread_id)],
                std::filesystem::path(
                    compressed_block_file_path(
                        paths.read_block_paths[stream_index], block_num))
                    .filename()
                    .string(),
                bsc_compress_bytes(build_raw_string_block_bytes(
                    read_array.data() + thread_id * num_reads_per_block,
                    thread_read_count,
                    read_lengths_array.data() +
                        thread_id * num_reads_per_block)));
          }
        }
      }
      for (std::unordered_map<std::string, std::string> &thread_members :
           thread_archive_members) {
        output_artifact.archive_members.insert(
            std::make_move_iterator(thread_members.begin()),
            std::make_move_iterator(thread_members.end()));
      }
      if (cp.encoding.paired_end && (stream_index == 1)) {
        if (paired_id_match)
          for (int thread_id = 0; thread_id < cp.encoding.num_thr; thread_id++)
            paired_id_match =
                paired_id_match && (paired_id_match_array[thread_id] != 0);
        if (!paired_id_match)
          paired_id_code = 0;
      }
      if (!cp.encoding.long_flag) {
        // Clear thread buffers for the next step (including tail bytes).
        for (short_read_thread_buffers &thread_buffer : short_read_buffers) {
          thread_buffer.clean_read_bytes.clear();
          thread_buffer.n_read_bytes.clear();
          thread_buffer.n_read_positions.clear();
          thread_buffer.tail_info_bytes.clear();
          thread_buffer.atac_adapter_bytes.clear();
          thread_buffer.clean_read_count = 0;
        }

#pragma omp parallel for schedule(static)
        for (int thread_id = 0; thread_id < cp.encoding.num_thr; thread_id++) {
          const uint32_t begin_read =
              static_cast<uint32_t>(thread_id) * num_reads_per_block;
          if (begin_read >= reads_in_step)
            continue;
          const uint32_t end_read =
              std::min(begin_read + num_reads_per_block, reads_in_step);

          short_read_thread_buffers &thread_buffer =
              short_read_buffers[static_cast<size_t>(thread_id)];
          const uint32_t thread_read_count = end_read - begin_read;
          thread_buffer.n_read_positions.reserve(thread_read_count / 2 + 1);

          for (uint32_t read_index = begin_read; read_index < end_read;
               read_index++) {
            if (read_contains_N_array[read_index] == 0) {
              append_encoded_dna_bits(thread_buffer.clean_read_bytes,
                                      read_array[read_index]);
              thread_buffer.clean_read_count++;
            } else {
              thread_buffer.n_read_positions.push_back(
                  static_cast<uint32_t>(num_reads[stream_index] + read_index));
              append_encoded_dna_n_bits(thread_buffer.n_read_bytes,
                                        read_array[read_index]);
            }
            // Record tail info for all reads (0 when stripping disabled or
            // N-read).
            if (apply_poly_at) {
              uint16_t info = 0;
              if (read_contains_N_array[read_index] == 0 &&
                  !step_tail_info.empty()) {
                info = step_tail_info[read_index];
                const uint32_t tail_len = info >> 1;
                if (tail_len > 0) {
                  append_uint16(thread_buffer.tail_info_bytes, info);
                  if (!quality_array.empty()) {
                    // Preserve stripped tail quality bytes so decompression can
                    // consume the side stream consistently.
                    std::string &qual = quality_array[read_index];
                    if (tail_len > qual.size()) {
                      throw std::runtime_error(
                          "RNA tail stripping exceeded available quality "
                          "length");
                    }
                    const auto tail_begin =
                        qual.end() - static_cast<std::ptrdiff_t>(tail_len);
                    thread_buffer.tail_info_bytes.append(tail_begin,
                                                         qual.end());
                    qual.erase(tail_begin, qual.end());
                  } else {
                    for (uint32_t index = 0; index < tail_len; ++index) {
                      thread_buffer.tail_info_bytes.push_back('I');
                    }
                  }
                } else {
                  append_uint16(thread_buffer.tail_info_bytes, 0);
                }
              } else {
                append_uint16(thread_buffer.tail_info_bytes, 0);
              }
            }
            if (atac_adapter_strip_active) {
              uint8_t info = 0;
              if (read_contains_N_array[read_index] == 0 &&
                  !step_atac_adapter_info.empty()) {
                info = step_atac_adapter_info[read_index];
                const uint32_t strip_len = info >> 1;
                if (strip_len > 0) {
                  thread_buffer.atac_adapter_bytes.push_back(
                      static_cast<char>(info));
                  if (!quality_array.empty()) {
                    std::string &qual = quality_array[read_index];
                    if (strip_len > qual.size()) {
                      throw std::runtime_error(
                          "ATAC adapter stripping exceeded available "
                          "quality length");
                    }
                    const auto strip_begin =
                        qual.end() - static_cast<std::ptrdiff_t>(strip_len);
                    thread_buffer.atac_adapter_bytes.append(strip_begin,
                                                            qual.end());
                    qual.erase(strip_begin, qual.end());
                  } else {
                    for (uint32_t index = 0; index < strip_len; ++index) {
                      thread_buffer.atac_adapter_bytes.push_back('I');
                    }
                  }
                } else {
                  thread_buffer.atac_adapter_bytes.push_back('\0');
                }
              } else {
                thread_buffer.atac_adapter_bytes.push_back('\0');
              }
            }
          }
        }

        num_reads_clean[stream_index] += flush_short_read_thread_buffers(
            short_read_buffers,
            output_artifact.reorder_inputs.clean_read_streams[stream_index],
            n_read_streams[stream_index], n_read_orders[stream_index],
            apply_poly_at ? &output_artifact.post_encode_side_streams
                                 .raw_tail_streams[stream_index]
                          : nullptr,
            atac_adapter_strip_active
                ? &output_artifact.post_encode_side_streams
                       .compressed_atac_adapter_streams[stream_index]
                : nullptr);
        if (!cp.encoding.preserve_order) {
          quality_chunk.clear();
          id_chunk.clear();
          if (quality_chunk.capacity() <
              static_cast<size_t>(reads_in_step) * 32U)
            quality_chunk.reserve(static_cast<size_t>(reads_in_step) * 32U);
          if (id_chunk.capacity() < static_cast<size_t>(reads_in_step) * 32U)
            id_chunk.reserve(static_cast<size_t>(reads_in_step) * 32U);
          for (uint32_t read_index = 0; read_index < reads_in_step;
               read_index++) {
            quality_chunk.append(quality_array[read_index]);
            quality_chunk.push_back('\n');
          }
          output_artifact.post_encode_side_streams
              .raw_quality_streams[stream_index]
              .append(quality_chunk);

          for (uint32_t read_index = 0; read_index < reads_in_step;
               read_index++) {
            id_chunk.append(id_array[read_index]);
            id_chunk.push_back('\n');
          }
          output_artifact.post_encode_side_streams.raw_id_streams[stream_index]
              .append(id_chunk);
        }
      }
      num_reads[stream_index] += reads_in_step;
      max_readlen =
          std::max(max_readlen,
                   max_read_length_in_step(read_lengths_array, reads_in_step));
    }
    if (cp.encoding.paired_end)
      if (num_reads[0] != num_reads[1])
        throw std::runtime_error(
            "Number of reads in paired files do not match.");
    if (done[0] && done[1])
      break;
    num_blocks_done += cp.encoding.num_thr;
    if (progress && total_input_bytes > 0) {
      uint64_t bytes_read = 0;
      for (int i = 0; i < 2; i++) {
        if (input_gzip_enabled[i]) {
          if (gzip_streams[i]) {
            bytes_read += gzip_streams[i]->compressed_offset();
          }
        } else if (input_streams[i]) {
          const std::streampos current_pos = input_streams[i]->tellg();
          if (current_pos >= 0) {
            bytes_read += static_cast<uint64_t>(current_pos);
          }
        }
      }
      progress->update(static_cast<float>(bytes_read) / total_input_bytes);
    }
  }

  if (apply_cb_strip) {
    if (cp.encoding.cb_prefix_stripped) {
      const std::vector<char> compressed_cb_seq = bsc_compress_bytes(
          std::vector<char>(cb_seq_bytes.begin(), cb_seq_bytes.end()));
      add_archive_member(output_artifact.archive_members, "cb_prefix.dna.bsc",
                         compressed_cb_seq);

      if (cp.encoding.preserve_quality) {
        const std::vector<char> compressed_cb_qual = bsc_compress_bytes(
            std::vector<char>(cb_qual_bytes.begin(), cb_qual_bytes.end()));
        add_archive_member(output_artifact.archive_members,
                           "cb_prefix.qual.bsc", compressed_cb_qual);
      }
    }
  }
  if (maybe_apply_atac_adapter_strip) {
    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (stream_index == 1 && !cp.encoding.paired_end)
        continue;
      if (!cp.encoding.atac_adapter_stripped) {
        output_artifact.post_encode_side_streams
            .compressed_atac_adapter_streams[stream_index]
            .clear();
        continue;
      }
      std::string &adapter_bytes =
          output_artifact.post_encode_side_streams
              .compressed_atac_adapter_streams[stream_index];
      if (!adapter_bytes.empty()) {
        const std::vector<char> compressed_adapter_bytes = bsc_compress_bytes(
            std::vector<char>(adapter_bytes.begin(), adapter_bytes.end()));
        adapter_bytes.assign(compressed_adapter_bytes.begin(),
                             compressed_adapter_bytes.end());
      }
    }
  }

  if (cp.encoding.preserve_order) {
    if (cp.encoding.poly_at_stripped) {
      for (int stream_index = 0; stream_index < 2; ++stream_index) {
        if (stream_index == 1 && !cp.encoding.paired_end)
          continue;
        const std::string &tail_bytes = output_artifact.post_encode_side_streams
                                            .raw_tail_streams[stream_index];
        output_artifact
            .archive_members["tail_" + std::to_string(stream_index + 1) +
                             ".bin"] = tail_bytes;
      }
    }

    if (cp.encoding.atac_adapter_stripped) {
      for (int stream_index = 0; stream_index < 2; ++stream_index) {
        if (stream_index == 1 && !cp.encoding.paired_end)
          continue;
        const std::string adapter_path =
            "atac_adapter_" + std::to_string(stream_index + 1) + ".bin";
        const std::string &adapter_bytes =
            output_artifact.post_encode_side_streams
                .compressed_atac_adapter_streams[stream_index];
        if (adapter_bytes.empty()) {
          output_artifact.archive_members[adapter_path] = std::string();
          continue;
        }
        output_artifact.archive_members[adapter_path + ".bsc"] = adapter_bytes;
      }
    }
  }
  close_preprocess_streams(input_files, input_streams, gzip_streams, cp,
                           input_gzip_enabled);
  if (num_reads[0] == 0)
    throw std::runtime_error("No reads found.");

  if (!cp.encoding.long_flag && cp.encoding.paired_end) {
    // Shift mate-2 N-read positions by file-1 length before merging the
    // streams.
    merge_paired_n_reads(n_read_streams, n_read_orders, num_reads);
  }

  output_artifact.reorder_inputs.n_read_bytes = std::move(n_read_streams[0]);
  append_n_order_bytes(output_artifact.reorder_inputs.n_read_order_bytes,
                       n_read_orders[0]);

  remove_redundant_mate_ids(paths, cp, paired_id_match,
                            output_artifact.archive_members, num_reads,
                            num_reads_per_block);
  cp.read_info.paired_id_code = paired_id_code;
  cp.read_info.paired_id_match = paired_id_match;
  cp.read_info.quality_header_has_id = quality_header_has_id_by_stream[0];
  cp.read_info.quality_header_has_id_by_stream[0] =
      quality_header_has_id_by_stream[0];
  cp.read_info.quality_header_has_id_by_stream[1] =
      cp.encoding.paired_end ? quality_header_has_id_by_stream[1] : false;
  cp.encoding.use_crlf_by_stream[0] = observed_summary.use_crlf_by_stream[0];
  cp.encoding.use_crlf_by_stream[1] =
      cp.encoding.paired_end ? observed_summary.use_crlf_by_stream[1] : false;
  cp.encoding.use_crlf =
      cp.encoding.use_crlf_by_stream[0] ||
      (cp.encoding.paired_end && cp.encoding.use_crlf_by_stream[1]);
  cp.read_info.num_reads = static_cast<uint32_t>(num_reads[0] + num_reads[1]);
  cp.read_info.num_reads_clean[0] = static_cast<uint32_t>(num_reads_clean[0]);
  cp.read_info.num_reads_clean[1] = static_cast<uint32_t>(num_reads_clean[1]);
  cp.read_info.max_readlen = max_readlen;

  if (expected_summary != nullptr && !cp.encoding.long_flag &&
      input_summary_differs(*expected_summary, observed_summary,
                            cp.encoding.paired_end)) {
    throw preprocess_retry_exception(observed_summary);
  }

  SPRING_LOG_DEBUG(
      "Preprocess complete: num_reads=" +
      std::to_string(cp.read_info.num_reads) +
      ", num_reads_clean_1=" + std::to_string(cp.read_info.num_reads_clean[0]) +
      ", num_reads_clean_2=" + std::to_string(cp.read_info.num_reads_clean[1]) +
      ", max_readlen=" + std::to_string(cp.read_info.max_readlen) +
      ", sequence_crc_1=" + std::to_string(cp.read_info.sequence_crc[0]) +
      ", sequence_crc_2=" + std::to_string(cp.read_info.sequence_crc[1]) +
      ", quality_crc_1=" + std::to_string(cp.read_info.quality_crc[0]) +
      ", quality_crc_2=" + std::to_string(cp.read_info.quality_crc[1]) +
      ", id_crc_1=" + std::to_string(cp.read_info.id_crc[0]) +
      ", id_crc_2=" + std::to_string(cp.read_info.id_crc[1]));

  SPRING_LOG_INFO("Max Read length: " +
                  std::to_string(cp.read_info.max_readlen));
  SPRING_LOG_INFO("Total number of reads: " +
                  std::to_string(cp.read_info.num_reads));

  if (cp.encoding.paired_end) {
    SPRING_LOG_INFO("Total number of reads without N: " +
                    std::to_string(cp.read_info.num_reads_clean[0] +
                                   cp.read_info.num_reads_clean[1]));
    SPRING_LOG_INFO("Paired id match code: " +
                    std::to_string((int)cp.read_info.paired_id_code));
  }

  return output_artifact;
}

} // namespace spring
