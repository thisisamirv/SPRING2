// Implements the top-level compression workflows, including grouped bundle
// orchestration on top of the shared workflow helpers.

#include "workflow_internal.h"

#include "assay_bisulfite.h"
#include "assay_detector.h"
#include "assay_sc_bisulfite.h"
#include "compression_dispatch.h"
#include "input_preparation.h"
#include "io_utils.h"
#include "paired_end_mate_ordering.h"
#include "progress.h"
#include "quality_id_reordering.h"
#include "stream_reordering.h"
#include "version.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <omp.h>
#include <sstream>
#include <system_error>

namespace spring {

namespace {

void release_string(std::string &value) { std::string().swap(value); }

template <typename T> void release_vector(std::vector<T> &value) {
  std::vector<T>().swap(value);
}

void release_post_encode_side_stream_artifact(
    post_encode_side_stream_artifact &artifact) {
  for (std::string &stream : artifact.raw_id_streams) {
    release_string(stream);
  }
  for (std::string &stream : artifact.raw_quality_streams) {
    release_string(stream);
  }
  for (std::string &stream : artifact.raw_tail_streams) {
    release_string(stream);
  }
  for (std::string &stream : artifact.compressed_atac_adapter_streams) {
    release_string(stream);
  }
}

void release_reorder_encoder_artifact(reorder_encoder_artifact &artifact) {
  for (reorder_encoder_shard &shard : artifact.aligned_shards) {
    release_string(shard.read_bytes);
    release_string(shard.orientation_bytes);
    release_string(shard.flag_bytes);
    release_string(shard.position_bytes);
    release_string(shard.order_bytes);
    release_string(shard.read_length_bytes);
  }
  std::vector<reorder_encoder_shard>().swap(artifact.aligned_shards);
  release_string(artifact.singleton_read_bytes);
  release_string(artifact.singleton_order_bytes);
  release_string(artifact.n_read_bytes);
  release_string(artifact.n_read_order_bytes);
  artifact.singleton_count = 0;
}

void release_reordered_stream_artifact(reordered_stream_artifact &artifact) {
  release_vector(artifact.orientation_entries);
  release_vector(artifact.position_entries);
  release_vector(artifact.read_length_entries);
  release_vector(artifact.read_order_entries);
  release_vector(artifact.noise_serialized);
  release_vector(artifact.noise_positions);
  release_vector(artifact.unaligned_serialized);
  std::unordered_map<std::string, std::string>().swap(artifact.archive_members);
  artifact.unaligned_char_count = 0;
}

std::string read_binary_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open intermediate file '" + path +
                             "'.");
  }

  std::ostringstream contents(std::ios::binary);
  contents << input.rdbuf();
  if (input.bad()) {
    throw std::runtime_error("Failed to read intermediate file '" + path +
                             "'.");
  }
  return contents.str();
}

template <typename T>
std::string vector_to_bytes(const std::vector<T> &values) {
  if (values.empty()) {
    return {};
  }

  return std::string(reinterpret_cast<const char *>(values.data()),
                     values.size() * sizeof(T));
}

template <typename T>
std::vector<T> bytes_to_vector(const std::string &bytes,
                               const char *field_name) {
  if ((bytes.size() % sizeof(T)) != 0) {
    throw std::runtime_error(std::string("Corrupt intermediate artifact for ") +
                             field_name + ".");
  }

  std::vector<T> values(bytes.size() / sizeof(T));
  if (!bytes.empty()) {
    std::memcpy(values.data(), bytes.data(), bytes.size());
  }
  return values;
}

void reset_directory(const std::filesystem::path &path) {
  std::error_code remove_ec;
  std::filesystem::remove_all(path, remove_ec);
  if (remove_ec) {
    throw std::runtime_error("Failed to clear intermediate directory '" +
                             path.string() + "': " + remove_ec.message());
  }

  std::error_code create_ec;
  std::filesystem::create_directories(path, create_ec);
  if (create_ec) {
    throw std::runtime_error("Failed to create intermediate directory '" +
                             path.string() + "': " + create_ec.message());
  }
}

void spill_reorder_encoder_artifact(const reorder_encoder_artifact &artifact,
                                    const std::string &root_dir) {
  reset_directory(root_dir);
  write_archive_member_file(root_dir, "meta/shard_count.txt",
                            std::to_string(artifact.aligned_shards.size()));
  write_archive_member_file(root_dir, "meta/singleton_count.txt",
                            std::to_string(artifact.singleton_count));
  write_archive_member_file(root_dir, "singleton_read_bytes.bin",
                            artifact.singleton_read_bytes);
  write_archive_member_file(root_dir, "singleton_order_bytes.bin",
                            artifact.singleton_order_bytes);
  write_archive_member_file(root_dir, "n_read_bytes.bin",
                            artifact.n_read_bytes);
  write_archive_member_file(root_dir, "n_read_order_bytes.bin",
                            artifact.n_read_order_bytes);

  for (size_t index = 0; index < artifact.aligned_shards.size(); ++index) {
    const std::string prefix = "aligned_shards/" + std::to_string(index) + "/";
    const reorder_encoder_shard &shard = artifact.aligned_shards[index];
    write_archive_member_file(root_dir, prefix + "read_bytes.bin",
                              shard.read_bytes);
    write_archive_member_file(root_dir, prefix + "orientation_bytes.bin",
                              shard.orientation_bytes);
    write_archive_member_file(root_dir, prefix + "flag_bytes.bin",
                              shard.flag_bytes);
    write_archive_member_file(root_dir, prefix + "position_bytes.bin",
                              shard.position_bytes);
    write_archive_member_file(root_dir, prefix + "order_bytes.bin",
                              shard.order_bytes);
    write_archive_member_file(root_dir, prefix + "read_length_bytes.bin",
                              shard.read_length_bytes);
  }
}

reorder_encoder_artifact
load_reorder_encoder_artifact(const std::string &root_dir,
                              bool stream_from_disk = false) {
  reorder_encoder_artifact artifact;
  artifact.singleton_count = static_cast<uint32_t>(std::stoul(read_binary_file(
      resolve_archive_entry_disk_path(root_dir, "meta/singleton_count.txt"))));
  const size_t shard_count = static_cast<size_t>(std::stoull(read_binary_file(
      resolve_archive_entry_disk_path(root_dir, "meta/shard_count.txt"))));
  if (stream_from_disk) {
    // Record file paths so readsingletons() can stream directly — avoids
    // loading the full raw singleton bytes into RAM.
    artifact.singleton_read_file =
        resolve_archive_entry_disk_path(root_dir, "singleton_read_bytes.bin");
    artifact.singleton_order_file =
        resolve_archive_entry_disk_path(root_dir, "singleton_order_bytes.bin");
    artifact.n_read_file =
        resolve_archive_entry_disk_path(root_dir, "n_read_bytes.bin");
    artifact.n_read_order_file =
        resolve_archive_entry_disk_path(root_dir, "n_read_order_bytes.bin");
    // Record shard file paths so the encoder can stream directly from disk —
    // avoids loading all aligned shard bytes into RAM simultaneously.
    artifact.aligned_shards.resize(shard_count);
    for (size_t index = 0; index < shard_count; ++index) {
      const std::string prefix =
          "aligned_shards/" + std::to_string(index) + "/";
      reorder_encoder_shard &shard = artifact.aligned_shards[index];
      shard.flag_file =
          resolve_archive_entry_disk_path(root_dir, prefix + "flag_bytes.bin");
      shard.read_file =
          resolve_archive_entry_disk_path(root_dir, prefix + "read_bytes.bin");
      shard.orientation_file = resolve_archive_entry_disk_path(
          root_dir, prefix + "orientation_bytes.bin");
      shard.position_file = resolve_archive_entry_disk_path(
          root_dir, prefix + "position_bytes.bin");
      shard.order_file =
          resolve_archive_entry_disk_path(root_dir, prefix + "order_bytes.bin");
      shard.read_length_file = resolve_archive_entry_disk_path(
          root_dir, prefix + "read_length_bytes.bin");
    }
  } else {
    artifact.singleton_read_bytes = read_binary_file(
        resolve_archive_entry_disk_path(root_dir, "singleton_read_bytes.bin"));
    artifact.singleton_order_bytes = read_binary_file(
        resolve_archive_entry_disk_path(root_dir, "singleton_order_bytes.bin"));
    artifact.n_read_bytes = read_binary_file(
        resolve_archive_entry_disk_path(root_dir, "n_read_bytes.bin"));
    artifact.n_read_order_bytes = read_binary_file(
        resolve_archive_entry_disk_path(root_dir, "n_read_order_bytes.bin"));
    artifact.aligned_shards.resize(shard_count);
    for (size_t index = 0; index < shard_count; ++index) {
      const std::string prefix =
          "aligned_shards/" + std::to_string(index) + "/";
      reorder_encoder_shard &shard = artifact.aligned_shards[index];
      shard.read_bytes = read_binary_file(
          resolve_archive_entry_disk_path(root_dir, prefix + "read_bytes.bin"));
      shard.orientation_bytes =
          read_binary_file(resolve_archive_entry_disk_path(
              root_dir, prefix + "orientation_bytes.bin"));
      shard.flag_bytes = read_binary_file(
          resolve_archive_entry_disk_path(root_dir, prefix + "flag_bytes.bin"));
      shard.position_bytes = read_binary_file(resolve_archive_entry_disk_path(
          root_dir, prefix + "position_bytes.bin"));
      shard.order_bytes = read_binary_file(resolve_archive_entry_disk_path(
          root_dir, prefix + "order_bytes.bin"));
      shard.read_length_bytes =
          read_binary_file(resolve_archive_entry_disk_path(
              root_dir, prefix + "read_length_bytes.bin"));
    }
  }
  return artifact;
}

void spill_post_encode_side_stream_artifact(
    const post_encode_side_stream_artifact &artifact,
    const std::string &root_dir) {
  reset_directory(root_dir);
  for (int stream_index = 0; stream_index < 2; ++stream_index) {
    write_archive_member_file(
        root_dir, "quality_" + std::to_string(stream_index + 1) + ".raw",
        artifact.raw_quality_streams[stream_index]);
    write_archive_member_file(root_dir,
                              "id_" + std::to_string(stream_index + 1) + ".raw",
                              artifact.raw_id_streams[stream_index]);
    write_archive_member_file(
        root_dir, "tail_" + std::to_string(stream_index + 1) + ".rawbin",
        artifact.raw_tail_streams[stream_index]);
    write_archive_member_file(
        root_dir, "atac_adapter_" + std::to_string(stream_index + 1) + ".bsc",
        artifact.compressed_atac_adapter_streams[stream_index]);
  }
}

reordered_stream_artifact
load_reordered_stream_artifact(const std::string &root_dir) {
  reordered_stream_artifact artifact;
  artifact.unaligned_char_count = static_cast<uint64_t>(
      std::stoull(read_binary_file(resolve_archive_entry_disk_path(
          root_dir, "meta/unaligned_char_count.txt"))));
  artifact.orientation_entries =
      bytes_to_vector<char>(read_binary_file(resolve_archive_entry_disk_path(
                                root_dir, "orientation_entries.bin")),
                            "orientation_entries");
  artifact.position_entries = bytes_to_vector<uint64_t>(
      read_binary_file(
          resolve_archive_entry_disk_path(root_dir, "position_entries.bin")),
      "position_entries");
  artifact.read_length_entries = bytes_to_vector<uint16_t>(
      read_binary_file(
          resolve_archive_entry_disk_path(root_dir, "read_length_entries.bin")),
      "read_length_entries");
  artifact.read_order_entries = bytes_to_vector<uint32_t>(
      read_binary_file(
          resolve_archive_entry_disk_path(root_dir, "read_order_entries.bin")),
      "read_order_entries");
  artifact.noise_serialized =
      bytes_to_vector<char>(read_binary_file(resolve_archive_entry_disk_path(
                                root_dir, "noise_serialized.bin")),
                            "noise_serialized");
  artifact.noise_positions = bytes_to_vector<uint16_t>(
      read_binary_file(
          resolve_archive_entry_disk_path(root_dir, "noise_positions.bin")),
      "noise_positions");
  artifact.unaligned_serialized =
      bytes_to_vector<char>(read_binary_file(resolve_archive_entry_disk_path(
                                root_dir, "unaligned_serialized.bin")),
                            "unaligned_serialized");

  const std::filesystem::path archive_root =
      std::filesystem::path(root_dir) / "archive_members";
  if (std::filesystem::exists(archive_root)) {
    std::vector<std::filesystem::path> member_paths;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::recursive_directory_iterator(archive_root)) {
      if (entry.is_regular_file()) {
        member_paths.push_back(entry.path());
      }
    }
    std::sort(member_paths.begin(), member_paths.end());
    for (const std::filesystem::path &member_path : member_paths) {
      const std::string archive_path =
          std::filesystem::relative(member_path, archive_root).generic_string();
      artifact.archive_members.insert_or_assign(
          archive_path, read_binary_file(member_path.string()));
    }
  }

  return artifact;
}

void compress_standard(const string_list &input_paths,
                       const string_list &output_paths, const int num_thr,
                       const bool pairing_only_flag, const bool no_quality_flag,
                       const bool no_ids_flag,
                       const string_list &quality_options,
                       const int compression_level, const std::string &note,
                       const log_level /*verbosity_level*/,
                       const bool audit_flag, const std::string &r3_path,
                       const std::string &i1_path, const std::string &i2_path,
                       const std::string &assay_type,
                       const std::string &cb_source_path, uint32_t cb_len,
                       std::string *archive_bytes_output = nullptr,
                       const compression_storage_path storage_path =
                           compression_storage_path::memory_path,
                       const uint64_t available_memory_bytes = 0) {
  const auto compression_start = clock_type::now();

  const compression_io_config io_config =
      resolve_compression_io(input_paths, output_paths);
  validate_compression_target(input_paths, io_config.archive_path);
  const bool use_disk_workspace =
      storage_path == compression_storage_path::disk_path &&
      archive_bytes_output == nullptr;
  const std::filesystem::path standard_work_dir =
      std::filesystem::path(io_config.archive_path).parent_path() /
      (std::filesystem::path(io_config.archive_path).filename().string() +
       ".work-tmp");
  const std::filesystem::path reorder_artifact_dir =
      standard_work_dir / "intermediate" / "reorder";
  const std::filesystem::path encoder_artifact_dir =
      standard_work_dir / "intermediate" / "encoder";
  const std::filesystem::path side_stream_artifact_dir =
      standard_work_dir / "intermediate" / "side-streams";
  std::unordered_map<std::string, std::string> staged_archive_member_paths;
  auto cleanup_standard_work_dir = [&]() noexcept {
    if (!use_disk_workspace) {
      return;
    }

    std::error_code cleanup_ec;
    std::filesystem::remove_all(standard_work_dir, cleanup_ec);
  };
  auto stage_archive_member = [&](const std::string &archive_path,
                                  const std::string &contents) {
    write_archive_member_file(standard_work_dir.string(), archive_path,
                              contents);
    staged_archive_member_paths.insert_or_assign(
        archive_path, resolve_archive_entry_disk_path(
                          standard_work_dir.string(), archive_path));
  };
  auto stage_archive_members =
      [&](const std::unordered_map<std::string, std::string> &archive_members) {
        for (const auto &[archive_path, contents] : archive_members) {
          stage_archive_member(archive_path, contents);
        }
      };
  auto stage_archive_members_from_dir =
      [&](const std::filesystem::path &archive_root_dir) {
        if (!std::filesystem::exists(archive_root_dir)) {
          return;
        }

        std::vector<std::filesystem::path> member_paths;
        for (const std::filesystem::directory_entry &entry :
             std::filesystem::recursive_directory_iterator(archive_root_dir)) {
          if (entry.is_regular_file()) {
            member_paths.push_back(entry.path());
          }
        }
        std::sort(member_paths.begin(), member_paths.end());
        for (const std::filesystem::path &member_path : member_paths) {
          const std::string archive_path =
              std::filesystem::relative(member_path, archive_root_dir)
                  .generic_string();
          stage_archive_member(archive_path,
                               read_binary_file(member_path.string()));
        }
      };
  if (use_disk_workspace) {
    cleanup_standard_work_dir();
    std::filesystem::create_directories(standard_work_dir);
    SPRING_LOG_INFO("Using disk-backed work directory: " +
                    standard_work_dir.string());
  }

  try {
    prepared_compression_inputs prepared_inputs =
        prepare_compression_inputs(io_config, num_thr);
    const input_record_format input_format_1 =
        detect_input_format(prepared_inputs.input_path_1);
    input_record_format input_format = input_format_1;
    if (io_config.paired_end) {
      const input_record_format input_format_2 =
          detect_input_format(prepared_inputs.input_path_2);
      if (input_format_1 != input_format_2) {
        cleanup_prepared_compression_inputs(prepared_inputs, pairing_only_flag);
        throw std::runtime_error(
            "Paired-end inputs must both be FASTQ or both be FASTA.");
      }
      input_format = input_format_2;
    }
    const bool fasta_input = input_format == input_record_format::fasta;
    const bool preserve_order = !pairing_only_flag;
    const bool preserve_id = !no_ids_flag;
    const bool preserve_quality = !no_quality_flag && !fasta_input;

    SPRING_LOG_INFO(
        "Analyzing first 10,000 fragments for startup properties and assay...");
    AssayDetector detector;
    const AssayDetector::StartupAnalysisResult startup_sample =
        detector.analyze_startup_sample(
            prepared_inputs.input_path_1,
            io_config.paired_end ? prepared_inputs.input_path_2 : "", r3_path,
            i1_path, i2_path, io_config.paired_end, fasta_input);

    input_detection_summary detected_input = startup_sample.input_summary;
    if (detected_input.requires_long_mode()) {
      SPRING_LOG_INFO("Startup sample indicates long-read mode; running full "
                      "input pre-scan.");
      detected_input = detect_input_properties(
          prepared_inputs.input_path_1, prepared_inputs.input_path_2,
          io_config.paired_end, fasta_input);
    }

    const bool use_crlf = detected_input.use_crlf();
    bool long_flag = detected_input.requires_long_mode();
    SPRING_LOG_DEBUG(
        "Detected maximum read length=" +
        std::to_string(detected_input.max_read_length) + ", use_crlf=" +
        std::string(use_crlf ? "true" : "false") + ", non_acgtn_symbols=" +
        std::string(detected_input.contains_non_acgtn_symbols ? "true"
                                                              : "false") +
        ", long_mode=" + std::string(long_flag ? "true" : "false"));

    if (detected_input.contains_non_acgtn_symbols) {
      SPRING_LOG_INFO("Detected non-ACGTN symbols in read sequences; "
                      "switching to long-read mode to preserve sequence "
                      "alphabet losslessly.");
    }

    if (long_flag) {
      SPRING_LOG_INFO("Auto-detected long-read mode.");
    } else {
      SPRING_LOG_INFO("Auto-detected short-read mode.");
    }

    compression_params cp{};
    cp.encoding.paired_end = io_config.paired_end;
    cp.encoding.preserve_order = preserve_order;
    cp.encoding.preserve_quality = preserve_quality;
    cp.encoding.preserve_id = preserve_id;
    cp.encoding.long_flag = long_flag;
    cp.encoding.use_crlf = use_crlf;
    cp.encoding.use_crlf_by_stream[0] = detected_input.use_crlf_by_stream[0];
    cp.encoding.use_crlf_by_stream[1] =
        io_config.paired_end ? detected_input.use_crlf_by_stream[1] : false;
    cp.encoding.num_reads_per_block = NUM_READS_PER_BLOCK;
    cp.encoding.num_reads_per_block_long = NUM_READS_PER_BLOCK_LONG;
    cp.encoding.num_thr = num_thr;
    cp.encoding.compression_level = compression_level;
    cp.read_info.note = note;

    std::string final_assay = assay_type;
    std::string final_confidence = "N/A";
    if (assay_type == "auto") {
      const AssayDetector::DetectionResult &res = startup_sample.assay_result;
      final_assay = res.assay;
      final_confidence = res.confidence;

      apply_bisulfite_auto_config(cp, res);
      apply_sc_bisulfite_auto_config(cp, res);
      SPRING_LOG_INFO("Auto-detected assay: " + final_assay +
                      " (confidence: " + final_confidence + ")");
    }

    cp.read_info.assay = final_assay;
    cp.read_info.assay_confidence = final_confidence;
    cp.read_info.compressor_version = spring::VERSION;
    cp.encoding.cb_len = cb_len;
    cp.encoding.cb_prefix_source_external = !cb_source_path.empty();

    cp.encoding.fasta_mode = fasta_input;
    cp.read_info.input_filename_1 =
        std::filesystem::path(io_config.input_path_1).filename().string();
    if (io_config.paired_end) {
      cp.read_info.input_filename_2 =
          std::filesystem::path(io_config.input_path_2).filename().string();
    }

    SPRING_LOG_DEBUG("Archive metadata inputs: name1='" +
                     cp.read_info.input_filename_1 + "'" +
                     (cp.encoding.paired_end
                          ? (", name2='" + cp.read_info.input_filename_2 + "'")
                          : std::string()));

    extract_gzip_detailed_info(
        io_config.input_path_1, cp.gzip.streams[0].was_gzipped,
        cp.gzip.streams[0].flg, cp.gzip.streams[0].mtime,
        cp.gzip.streams[0].xfl, cp.gzip.streams[0].os, cp.gzip.streams[0].name,
        cp.gzip.streams[0].is_bgzf, cp.gzip.streams[0].bgzf_block_size,
        cp.gzip.streams[0].uncompressed_size,
        cp.gzip.streams[0].compressed_size, cp.gzip.streams[0].member_count);
    if (io_config.paired_end) {
      extract_gzip_detailed_info(
          io_config.input_path_2, cp.gzip.streams[1].was_gzipped,
          cp.gzip.streams[1].flg, cp.gzip.streams[1].mtime,
          cp.gzip.streams[1].xfl, cp.gzip.streams[1].os,
          cp.gzip.streams[1].name, cp.gzip.streams[1].is_bgzf,
          cp.gzip.streams[1].bgzf_block_size,
          cp.gzip.streams[1].uncompressed_size,
          cp.gzip.streams[1].compressed_size, cp.gzip.streams[1].member_count);
    }

    if (preserve_quality)
      configure_quality_options(cp, quality_options);

    SPRING_LOG_INFO(std::string("Detected input format: ") +
                    input_format_name(input_format));
    if (fasta_input) {
      SPRING_LOG_INFO(
          "FASTA input detected; quality values will not be stored.");
    }

    if (prepared_inputs.input_1_was_gzipped ||
        prepared_inputs.input_2_was_gzipped) {
      SPRING_LOG_INFO(
          "Detected gzipped input; streaming decompression directly "
          "into compression without staging full plain FASTQ files.");
    }

    SPRING_LOG_DEBUG(
        "Effective encoding options: paired_end=" +
        std::string(cp.encoding.paired_end ? "true" : "false") +
        ", preserve_order=" +
        std::string(cp.encoding.preserve_order ? "true" : "false") +
        ", preserve_id=" +
        std::string(cp.encoding.preserve_id ? "true" : "false") +
        ", preserve_quality=" +
        std::string(cp.encoding.preserve_quality ? "true" : "false") +
        ", fasta_mode=" +
        std::string(cp.encoding.fasta_mode ? "true" : "false"));

    auto *progress_ptr = ProgressBar::GlobalInstance();
    ProgressBar dummy_progress(true);
    auto &progress = progress_ptr ? *progress_ptr : dummy_progress;

    const compression_params preprocess_seed_cp = cp;
    input_detection_summary preprocess_seed_summary = detected_input;
    const bool validate_sample_during_preprocess =
        !startup_sample.input_summary.requires_long_mode();
    preprocess_artifact preprocess_output;

    for (int preprocess_attempt = 0;; ++preprocess_attempt) {
      compression_params attempt_cp = preprocess_seed_cp;
      attempt_cp.encoding.long_flag =
          preprocess_seed_summary.requires_long_mode();
      attempt_cp.encoding.use_crlf = preprocess_seed_summary.use_crlf();
      attempt_cp.encoding.use_crlf_by_stream[0] =
          preprocess_seed_summary.use_crlf_by_stream[0];
      attempt_cp.encoding.use_crlf_by_stream[1] =
          io_config.paired_end ? preprocess_seed_summary.use_crlf_by_stream[1]
                               : false;

      try {
        run_timed_step("Preprocessing ...", "Preprocessing", [&] {
          progress.set_stage("Preprocessing", 0.0F, 0.25F);
          preprocess_output = preprocess(
              prepared_inputs.input_path_1, prepared_inputs.input_path_2,
              attempt_cp, fasta_input, &progress,
              validate_sample_during_preprocess ? &preprocess_seed_summary
                                                : nullptr);
        });
        cp = std::move(attempt_cp);
        long_flag = cp.encoding.long_flag;
        break;
      } catch (const preprocess_retry_exception &retry) {
        if (preprocess_attempt >= 1) {
          throw;
        }

        preprocess_seed_summary = retry.updated_summary();
        if (preprocess_seed_summary.requires_long_mode()) {
          SPRING_LOG_INFO(
              "Preprocessing found long-read properties outside the startup "
              "sample; running full input pre-scan before retry.");
        } else {
          SPRING_LOG_INFO("Preprocessing found startup metadata outside the "
                          "startup sample; "
                          "retrying with updated properties.");
        }

        cleanup_prepared_compression_inputs(prepared_inputs, pairing_only_flag);
        prepared_inputs = prepare_compression_inputs(io_config, num_thr);

        if (preprocess_seed_summary.requires_long_mode()) {
          preprocess_seed_summary = detect_input_properties(
              prepared_inputs.input_path_1, prepared_inputs.input_path_2,
              io_config.paired_end, fasta_input);
        }
      }
    }
    cleanup_prepared_compression_inputs(prepared_inputs, pairing_only_flag);

    std::unordered_map<std::string, std::string> archive_members;
    if (use_disk_workspace) {
      const auto stage_start = clock_type::now();
      SPRING_LOG_INFO("Staging " +
                      std::to_string(preprocess_output.archive_members.size()) +
                      " preprocess archive members to work directory...");
      stage_archive_members(preprocess_output.archive_members);
      SPRING_LOG_INFO(
          "Staging done. Time: " +
          std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                             clock_type::now() - stage_start)
                             .count()) +
          " s");
    } else {
      archive_members = std::move(preprocess_output.archive_members);
    }

    if (!long_flag) {
      reorder_encoder_artifact reorder_artifact;
      post_encode_side_stream_artifact post_encode_side_streams;
      reordered_stream_artifact reordered_streams_artifact;
      bool reordered_streams_loaded = false;
      auto ensure_reordered_streams_loaded = [&]() {
        if (reordered_streams_loaded || !use_disk_workspace) {
          return;
        }

        reordered_streams_artifact =
            load_reordered_stream_artifact(encoder_artifact_dir.string());
        reordered_streams_loaded = true;
      };
      auto release_reordered_streams = [&]() {
        if (!reordered_streams_loaded) {
          return;
        }

        release_reordered_stream_artifact(reordered_streams_artifact);
        reordered_streams_loaded = false;
      };
      const bool needs_post_encode_side_streams =
          !preserve_order &&
          (preserve_quality || preserve_id || cp.encoding.poly_at_stripped ||
           cp.encoding.atac_adapter_stripped);
      if (!needs_post_encode_side_streams) {
        SPRING_LOG_DEBUG("Skipping post-encode quality/id reordering stage "
                         "(preserve_order=true or streams stripped). ");
      }
      if (needs_post_encode_side_streams) {
        post_encode_side_streams =
            std::move(preprocess_output.post_encode_side_streams);
        if (use_disk_workspace) {
          spill_post_encode_side_stream_artifact(
              post_encode_side_streams, side_stream_artifact_dir.string());
          release_post_encode_side_stream_artifact(post_encode_side_streams);
        }
      }

      // disk_path memory reduction: choose between external-MPHF (B) and
      // thread reduction (C) based on available temp-disk capacity.
      if (use_disk_workspace) {
        const uint64_t total_clean_reads =
            static_cast<uint64_t>(cp.read_info.num_reads_clean[0]) +
            static_cast<uint64_t>(cp.read_info.num_reads_clean[1]);
        // Conservative disk-space estimate for pthash external builder:
        // peak ≈ num_keys × (sizeof(bucket_payload_pair)=16 +
        // sizeof(uint64_t)=8) plus bucket-count overhead (~10 bytes/key).  Use
        // 40 bytes/key overall.
        const uint64_t external_mphf_disk_needed = total_clean_reads * 40;
        // Conservative in-memory estimate for the in-core MPHF builder path.
        // Keep headroom for the rest of the pipeline by requiring at least
        // ~2x this estimate before preferring in-memory mode.
        const uint64_t in_memory_mphf_needed = total_clean_reads * 40;
        const bool can_afford_in_memory_mphf =
            available_memory_bytes > 0 &&
            available_memory_bytes >= (in_memory_mphf_needed * 2);
        std::error_code space_ec;
        const auto disk_info =
            std::filesystem::space(standard_work_dir, space_ec);
        const bool disk_ok =
            !space_ec && disk_info.available >= external_mphf_disk_needed;
        if (can_afford_in_memory_mphf) {
          cp.encoding.use_external_mphf = false;
          cp.encoding.mphf_tmp_dir.clear();
          SPRING_LOG_INFO(
              "disk_path: RAM budget sufficient for in-memory MPHF "
              "(available_memory=" +
              std::to_string(available_memory_bytes >> 20) +
              " MiB, estimated_needed=" +
              std::to_string(in_memory_mphf_needed >> 20) +
              " MiB); using in-memory builder to avoid temp-disk latency.");
        } else if (disk_ok) {
          // External-memory MPHF: off-load MPHF key data to disk temp files
          // so the in-memory sort/search structures never materialise
          // simultaneously.
          cp.encoding.use_external_mphf = true;
          cp.encoding.mphf_tmp_dir = standard_work_dir.string();
          SPRING_LOG_INFO(
              "disk_path: using external-memory MPHF builder (available=" +
              std::to_string(disk_info.available >> 20) + " MiB, needed=" +
              std::to_string(external_mphf_disk_needed >> 20) + " MiB)");
        } else {
          // Thread capping: insufficient temp-disk for external MPHF — reduce
          // thread count to keep per-thread encoder buffers within budget.
          // Assume half the available memory is available for thread buffers;
          // allow ~4 GiB per thread.
          constexpr uint64_t kBytesPerThread = 4ULL << 30;
          const uint64_t thread_budget =
              available_memory_bytes > 0 ? available_memory_bytes / 2 : 0;
          const int safe_threads =
              thread_budget > 0
                  ? static_cast<int>(
                        std::max(uint64_t{1}, thread_budget / kBytesPerThread))
                  : cp.encoding.num_thr;
          if (safe_threads < cp.encoding.num_thr) {
            SPRING_LOG_INFO(
                "disk_path: insufficient temp-disk for external MPHF "
                "(available=" +
                std::to_string(space_ec ? 0 : disk_info.available >> 20) +
                " MiB, needed=" +
                std::to_string(external_mphf_disk_needed >> 20) +
                " MiB); reducing encoding threads " +
                std::to_string(cp.encoding.num_thr) + " -> " +
                std::to_string(safe_threads));
            cp.encoding.num_thr = safe_threads;
          }
        }
      }

      if (use_disk_workspace) {
        // Chunked reorder will write streams and per-chunk singleton sequences
        // directly to reorder_artifact_dir to avoid accumulating ~29 GB of
        // stream data + ~7 GB/chunk of singleton bytes in RAM.
        cp.encoding.reorder_spill_dir = reorder_artifact_dir.string();
      }

      run_timed_step("Reordering ...", "Reordering", [&] {
        progress.set_stage("Reordering", 0.25F, 0.50F);
        reorder_artifact =
            call_reorder(std::move(preprocess_output.reorder_inputs), cp);
        if (use_disk_workspace) {
          // reorder_main pre-spills singleton_read_bytes when use_chunked &&
          // reorder_spill_dir is set, so spill_reorder_encoder_artifact (which
          // calls reset_directory and would wipe those files) must be skipped.
          if (reorder_artifact.singleton_read_file.empty()) {
            spill_reorder_encoder_artifact(reorder_artifact,
                                           reorder_artifact_dir.string());
          }
          release_reorder_encoder_artifact(reorder_artifact);
        }
      });

      run_timed_step("Encoding ...", "Encoding", [&] {
        progress.set_stage("Encoding", 0.50F, 0.85F);
        if (use_disk_workspace) {
          // stream_from_disk=true: file paths are recorded in the artifact
          // instead of loading raw bytes into RAM, so both readsingletons()
          // and the aligned-shard encoder loop stream directly from disk —
          // avoiding the ~27+ GB singleton buffer peak and the ~25 GB
          // aligned-shard peak that would otherwise coexist in memory during
          // encoding.
          // encoder_metadata_spill_dir: the encoder flushes per-thread
          // position/orientation/noise/order/length metadata directly to disk
          // instead of accumulating ~15 GB of vectors in RAM.  The per-thread
          // files are stream-merged into final artifact files after the loop.
          cp.encoding.encoder_metadata_spill_dir =
              encoder_artifact_dir.string();
          reorder_encoder_artifact encoder_input =
              load_reorder_encoder_artifact(reorder_artifact_dir.string(),
                                            /*stream_from_disk=*/true);
          reordered_streams_artifact = call_encoder(encoder_input, cp);
          release_reorder_encoder_artifact(encoder_input);
          std::error_code cleanup_ec;
          std::filesystem::remove_all(reorder_artifact_dir, cleanup_ec);
          // encoder_main already wrote all metadata to encoder_artifact_dir;
          // skip spill_reordered_stream_artifact (it would call
          // reset_directory and delete the files we just wrote).
          release_reordered_stream_artifact(reordered_streams_artifact);
          reordered_streams_loaded = false;
        } else {
          reordered_streams_artifact = call_encoder(reorder_artifact, cp);
          reordered_streams_loaded = true;
        }
      });
      release_reorder_encoder_artifact(reorder_artifact);

      if (needs_post_encode_side_streams) {
        run_timed_step(
            "Reordering and compressing quality and/or ids ...",
            "Reordering and compressing quality and/or ids", [&] {
              auto quality_id_members =
                  use_disk_workspace
                      ? reorder_compress_quality_id(
                            side_stream_artifact_dir.string(),
                            resolve_archive_entry_disk_path(
                                encoder_artifact_dir.string(),
                                "read_order_entries.bin"),
                            cp, side_stream_artifact_dir.string(),
                            available_memory_bytes)
                      : reorder_compress_quality_id(
                            post_encode_side_streams,
                            reordered_streams_artifact.read_order_entries, cp);
              if (use_disk_workspace) {
                stage_archive_members(quality_id_members);
              } else {
                merge_archive_members(archive_members,
                                      std::move(quality_id_members));
              }
            });
        if (use_disk_workspace) {
          std::error_code cleanup_ec;
          std::filesystem::remove_all(side_stream_artifact_dir, cleanup_ec);
        }
      }

      if (!preserve_order && io_config.paired_end) {
        run_timed_step(
            "Encoding pairing information ...", "Encoding pairing information",
            [&] {
              ensure_reordered_streams_loaded();
              pe_encode(reordered_streams_artifact.read_order_entries, cp);
              if (use_disk_workspace) {
                write_archive_member_file(
                    encoder_artifact_dir.string(), "read_order_entries.bin",
                    vector_to_bytes(
                        reordered_streams_artifact.read_order_entries));
              }
            });
      }

      run_timed_step(
          "Reordering and compressing streams ...",
          "Reordering and compressing streams", [&] {
            progress.set_stage("Compressing streams", 0.85F, 0.95F);
            if (use_disk_workspace) {
              stage_archive_members_from_dir(encoder_artifact_dir /
                                             "archive_members");
              stage_archive_members(reorder_compress_streams(
                  cp, encoder_artifact_dir.string(),
                  resolve_archive_entry_disk_path(encoder_artifact_dir.string(),
                                                  "read_order_entries.bin"),
                  available_memory_bytes));
            } else {
              ensure_reordered_streams_loaded();
              merge_archive_members(
                  archive_members,
                  std::move(reordered_streams_artifact.archive_members));
              merge_archive_members(
                  archive_members,
                  reorder_compress_streams(
                      cp, reordered_streams_artifact,
                      &reordered_streams_artifact.read_order_entries));
            }
          });

      release_reordered_streams();
      if (use_disk_workspace) {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(encoder_artifact_dir, cleanup_ec);
      }
    }

    std::vector<tar_archive_source> archive_sources;
    if (use_disk_workspace) {
      stage_archive_member("cp.bin", serialize_compression_params(cp));
      print_compressed_stream_sizes_from_disk(staged_archive_member_paths);
      archive_sources =
          build_archive_sources_from_disk(staged_archive_member_paths);
    } else {
      archive_members["cp.bin"] = serialize_compression_params(cp);
      print_compressed_stream_sizes(archive_members);
      archive_sources = build_archive_sources(archive_members);
    }

    run_timed_step("Creating tar archive ...", "Tar archive", [&] {
      progress.set_stage("Creating archive", 0.95F, 1.0F);
      if (archive_bytes_output != nullptr) {
        *archive_bytes_output =
            create_tar_archive_from_sources_bytes(archive_sources);
      } else {
        create_tar_archive_from_sources(io_config.archive_path,
                                        archive_sources);
      }
    });
    if (archive_bytes_output != nullptr) {
      SPRING_LOG_DEBUG("Archive created in memory: bytes=" +
                       std::to_string(archive_bytes_output->size()));
    } else {
      SPRING_LOG_DEBUG("Archive created at: " + io_config.archive_path);
    }

    const auto compression_end = clock_type::now();
    if (Logger::is_info_enabled()) {
      std::cout << "Compression done!\n";
      std::cout << "Total time for compression: "
                << std::chrono::duration_cast<std::chrono::seconds>(
                       compression_end - compression_start)
                       .count()
                << " s\n";
    } else {
      progress.finalize();
    }

    if (Logger::is_info_enabled()) {
      namespace fs = std::filesystem;
      std::cout << "\n";
      if (archive_bytes_output != nullptr) {
        std::cout << "Total size: " << std::setw(12)
                  << archive_bytes_output->size() << " bytes\n";
      } else {
        fs::path archive_file_path{io_config.archive_path};
        std::cout << "Total size: " << std::setw(12)
                  << fs::file_size(archive_file_path) << " bytes\n";
      }
    }

    if (audit_flag && archive_bytes_output == nullptr) {
      SPRING_LOG_DEBUG("Running post-compression audit.");
      perform_audit_standard(io_config.archive_path);
    }

    cleanup_standard_work_dir();
  } catch (...) {
    cleanup_standard_work_dir();
    throw;
  }
}

} // namespace

void compress(const std::vector<std::string> &input_paths,
              const std::vector<std::string> &output_paths, const int num_thr,
              const bool pairing_only_flag, const bool no_quality_flag,
              const bool no_ids_flag,
              const std::vector<std::string> &quality_options,
              const int compression_level, const std::string &note,
              const log_level verbosity_level, const bool audit_flag,
              const std::string &r3_path, const std::string &i1_path,
              const std::string &i2_path, const std::string &assay_type,
              const std::string &cb_source_path, uint32_t cb_len,
              const double memory_cap_gb) {
  Logger::set_level(verbosity_level);
  const compression_storage_plan storage_plan =
      build_compression_storage_plan(input_paths, memory_cap_gb);
  if (storage_plan.selected_path == compression_storage_path::disk_path) {
    std::cout << "Disk-backed compression path selected based on estimated "
                 "peak working memory and available memory.\n";
  }
  ProgressBar progress(verbosity_level == log_level::quiet);
  ProgressBar::SetGlobalInstance(&progress);
  omp_set_dynamic(0);

  SPRING_LOG_INFO("Starting compression...");
  SPRING_LOG_DEBUG(
      "Compression request: num_threads=" + std::to_string(num_thr) +
      ", level=" + std::to_string(compression_level) +
      ", strip_order=" + std::string(pairing_only_flag ? "true" : "false") +
      ", strip_quality=" + std::string(no_quality_flag ? "true" : "false") +
      ", strip_ids=" + std::string(no_ids_flag ? "true" : "false") +
      ", audit=" + std::string(audit_flag ? "true" : "false"));

  SPRING_LOG_INFO(
      "Compression storage path: " +
      std::string(storage_plan.selected_path ==
                          compression_storage_path::memory_path
                      ? "memory_path"
                      : "disk_path") +
      " (estimated_input_bytes=" +
      std::to_string(storage_plan.estimated_input_bytes) +
      ", estimated_peak_intermediate_bytes=" +
      std::to_string(storage_plan.estimated_peak_intermediate_bytes) +
      ", safety_margin_bytes=" +
      std::to_string(storage_plan.safety_margin_bytes) +
      ", required_peak_memory_bytes=" +
      std::to_string(storage_plan.required_peak_memory_bytes) +
      ", available_memory_bytes=" +
      std::to_string(storage_plan.available_memory_bytes) + ")");

  const bool has_r3 = !r3_path.empty();
  const bool has_i1 = !i1_path.empty();
  const bool has_i2 = !i2_path.empty();
  const bool grouped_bundle = has_r3 || has_i1 || has_i2;

  if (grouped_bundle) {
    if (input_paths.size() < 2) {
      throw std::runtime_error(
          "Grouped compression requires at least R1 and R2.");
    }
    if (output_paths.size() > 1) {
      throw std::runtime_error(
          "Number of output files not equal to 1 for grouped compression.");
    }

    const std::string output_archive_path =
        output_paths.empty() ? default_archive_name_from_input(input_paths[0])
                             : output_paths[0];
    validate_compression_target(input_paths, output_archive_path);
    const std::string read_archive_name = "reads_group.sp";
    const std::string read3_archive_name = "read3_group.sp";
    const std::string index_archive_name = "index_group.sp";

    const string_list read_inputs = {input_paths[0], input_paths[1]};
    string_list read3_inputs;
    string_list index_inputs;
    if (has_r3) {
      read3_inputs.push_back(r3_path);
    }
    if (has_i1) {
      index_inputs.push_back(i1_path);
      if (has_i2) {
        index_inputs.push_back(i2_path);
      }
    }

    SPRING_LOG_INFO("Detected grouped lanes; compressing as grouped bundle "
                    "(read pair + optional read3 + optional index pair).");

    const std::filesystem::path output_archive_fs_path(output_archive_path);
    const std::filesystem::path grouped_temp_dir =
        output_archive_fs_path.parent_path() /
        (output_archive_fs_path.filename().string() + ".grouped-tmp");
    std::filesystem::create_directories(grouped_temp_dir);

    const std::string read_archive_path =
        (grouped_temp_dir / read_archive_name).string();
    const std::string read3_archive_path =
        (grouped_temp_dir / read3_archive_name).string();
    const std::string index_archive_path =
        (grouped_temp_dir / index_archive_name).string();

    auto cleanup_grouped_temp = [&]() noexcept {
      std::error_code cleanup_ec;
      std::filesystem::remove_all(grouped_temp_dir, cleanup_ec);
    };

    try {
      compress_standard(read_inputs, {read_archive_path}, num_thr,
                        pairing_only_flag, no_quality_flag, no_ids_flag,
                        quality_options, compression_level, note,
                        verbosity_level, false, "", i1_path, "", assay_type,
                        i1_path, cb_len, nullptr, storage_plan.selected_path,
                        storage_plan.available_memory_bytes);

      const std::string grouped_assay =
          (assay_type == "auto") ? assay_from_archive_metadata_path(
                                       read_archive_path, read_archive_name)
                                 : assay_type;

      std::string read3_alias_source;
      if (has_r3) {
        if (paths_refer_to_same_file(r3_path, input_paths[0])) {
          read3_alias_source = "R1";
        } else if (paths_refer_to_same_file(r3_path, input_paths[1])) {
          read3_alias_source = "R2";
        } else {
          compress_standard(read3_inputs, {read3_archive_path}, num_thr,
                            pairing_only_flag, no_quality_flag, no_ids_flag,
                            quality_options, compression_level,
                            note.empty() ? std::string("read3-group")
                                         : (note + " | read3-group"),
                            verbosity_level, false, "", "", "", grouped_assay,
                            "", cb_len, nullptr, storage_plan.selected_path,
                            storage_plan.available_memory_bytes);
        }
      }

      if (has_i1) {
        compress_standard(index_inputs, {index_archive_path}, num_thr,
                          pairing_only_flag, no_quality_flag, no_ids_flag,
                          quality_options, compression_level,
                          note.empty() ? std::string("index-group")
                                       : (note + " | index-group"),
                          verbosity_level, false, "", "", "", grouped_assay, "",
                          cb_len, nullptr, storage_plan.selected_path,
                          storage_plan.available_memory_bytes);
      }

      const bundle_manifest manifest{
          .version = kBundleVersion,
          .read_archive_name = read_archive_name,
          .has_r3 = has_r3,
          .read3_archive_name = has_r3 && read3_alias_source.empty()
                                    ? read3_archive_name
                                    : std::string(),
          .read3_alias_source = read3_alias_source,
          .has_index = has_i1,
          .index_archive_name = has_i1 ? index_archive_name : std::string(),
          .has_i2 = has_i2,
          .r1_name = std::filesystem::path(input_paths[0]).filename().string(),
          .r2_name = std::filesystem::path(input_paths[1]).filename().string(),
          .r3_name = has_r3 ? std::filesystem::path(r3_path).filename().string()
                            : std::string(),
          .i1_name = has_i1 ? std::filesystem::path(i1_path).filename().string()
                            : std::string(),
          .i2_name = has_i2 ? std::filesystem::path(i2_path).filename().string()
                            : std::string()};
      std::vector<tar_archive_source> bundle_sources;
      bundle_sources.push_back({.archive_path = read_archive_name,
                                .disk_path = read_archive_path,
                                .contents = std::string(),
                                .from_memory = false});
      if (has_r3 && read3_alias_source.empty()) {
        bundle_sources.push_back({.archive_path = read3_archive_name,
                                  .disk_path = read3_archive_path,
                                  .contents = std::string(),
                                  .from_memory = false});
      }
      if (has_i1) {
        bundle_sources.push_back({.archive_path = index_archive_name,
                                  .disk_path = index_archive_path,
                                  .contents = std::string(),
                                  .from_memory = false});
      }
      bundle_sources.push_back({.archive_path = kBundleManifestName,
                                .disk_path = std::string(),
                                .contents = serialize_bundle_manifest(manifest),
                                .from_memory = true});

      run_timed_step("Creating grouped bundle archive ...", "Tar archive", [&] {
        progress.set_stage("Creating archive", 0.95F, 1.0F);
        create_tar_archive_from_sources(output_archive_path, bundle_sources);
      });
      SPRING_LOG_DEBUG("Grouped archive created at: " + output_archive_path);

      if (audit_flag) {
        SPRING_LOG_DEBUG("Running post-compression audit for grouped archive.");
        perform_audit(output_archive_path);
      }

      cleanup_grouped_temp();
    } catch (...) {
      cleanup_grouped_temp();
      throw;
    }

    ProgressBar::SetGlobalInstance(nullptr);
    return;
  }

  compress_standard(
      input_paths, output_paths, num_thr, pairing_only_flag, no_quality_flag,
      no_ids_flag, quality_options, compression_level, note, verbosity_level,
      audit_flag, r3_path, i1_path, i2_path, assay_type, cb_source_path, cb_len,
      nullptr, storage_plan.selected_path, storage_plan.available_memory_bytes);
}

} // namespace spring