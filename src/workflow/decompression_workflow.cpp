// Implements the top-level decompression workflows on top of the shared
// workflow helpers.

#include "workflow_internal.h"

#include "progress.h"

#include <iostream>
#include <omp.h>

namespace spring {

namespace {

void decompress_archive_artifact(const decompression_archive_artifact &artifact,
                                 const std::vector<std::string> &input_paths,
                                 const std::vector<std::string> &output_paths,
                                 const bool unzip_flag) {
  const auto decompression_start = clock_type::now();
  auto *progress_ptr = ProgressBar::GlobalInstance();
  ProgressBar dummy_progress(true);
  auto &progress = progress_ptr ? *progress_ptr : dummy_progress;
  compression_params cp{};

  read_archive_compression_params(artifact, cp);
  const archive_decompression_plan decompression_plan =
      build_archive_decompression_plan(cp);
  ensure_archive_decompression_plan_supported(decompression_plan);

  const bool paired_end = cp.encoding.paired_end;
  SPRING_LOG_DEBUG(
      "Archive metadata: paired_end=" +
      std::string(cp.encoding.paired_end ? "true" : "false") +
      ", long_mode=" + std::string(cp.encoding.long_flag ? "true" : "false") +
      ", preserve_order=" +
      std::string(cp.encoding.preserve_order ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", fasta_mode=" + std::string(cp.encoding.fasta_mode ? "true" : "false") +
      ", archive_format_version=" +
      std::to_string(decompression_plan.archive_format_version) +
      ", compressor_version=" + decompression_plan.compressor_version +
      ", decompression_route=" +
      archive_decompression_route_name(decompression_plan));

  const decompression_io_config io_config =
      resolve_decompression_io(input_paths, output_paths, paired_end);
  validate_output_targets(
      io_config.archive_path,
      paired_end ? std::vector<std::string>{io_config.output_path_1,
                                            io_config.output_path_2}
                 : std::vector<std::string>{io_config.output_path_1});

  auto has_compressed_suffix = [](const std::string &path) {
    return path.ends_with(".gz");
  };

  bool should_gzip[2] = {false, false};
  bool should_bgzf[2] = {false, false};
  int compression_levels[2] = {cp.encoding.compression_level,
                               cp.encoding.compression_level};
  for (int i = 0; i < (paired_end ? 2 : 1); i++) {
    const std::string &path =
        (i == 0) ? io_config.output_path_1 : io_config.output_path_2;
    if (!unzip_flag && has_compressed_suffix(path)) {
      should_gzip[i] = true;
      should_bgzf[i] =
          (i == 0) ? cp.gzip.streams[0].is_bgzf : cp.gzip.streams[1].is_bgzf;
      compression_levels[i] = gzip_output_compression_level(
          (i == 0) ? cp.gzip.streams[0] : cp.gzip.streams[1],
          cp.encoding.compression_level);
    }
  }

  const bool write_enabled[2] = {true, true};
  std::unique_ptr<DecompressionSink> sink =
      std::make_unique<FileDecompressionSink>(
          io_config.output_path_1, io_config.output_path_2, cp,
          compression_levels, should_gzip, should_bgzf, write_enabled);

  execute_archive_decompression_plan(artifact, *sink, cp, decompression_plan);

  run_timed_step("Verifying integrity ...", "Integrity check", [&] {
    const bool is_lossless =
        cp.encoding.preserve_order && cp.encoding.preserve_quality &&
        cp.encoding.preserve_id && !cp.quality.ill_bin_flag &&
        !cp.quality.bin_thr_flag;

    if (is_lossless) {
      uint32_t seq_crc[2], qual_crc[2], id_crc[2];
      sink->get_digests(seq_crc, qual_crc, id_crc);
      bool mismatch = false;
      for (int i = 0; i < (cp.encoding.paired_end ? 2 : 1); ++i) {
        if (cp.read_info.sequence_crc[i] != 0 &&
            seq_crc[i] != cp.read_info.sequence_crc[i]) {
          Logger::log_error("Stream " + std::to_string(i + 1) +
                            " sequence digest mismatch.");
          mismatch = true;
        }
        if (cp.read_info.quality_crc[i] != 0 &&
            qual_crc[i] != cp.read_info.quality_crc[i]) {
          Logger::log_error("Stream " + std::to_string(i + 1) +
                            " quality digest mismatch.");
          mismatch = true;
        }
        if (cp.read_info.id_crc[i] != 0 &&
            id_crc[i] != cp.read_info.id_crc[i]) {
          Logger::log_error("Stream " + std::to_string(i + 1) +
                            " ID digest mismatch.");
          mismatch = true;
        }
      }

      if (mismatch) {
        Logger::log_error("Integrity check failed during decompression.");
        throw std::runtime_error(
            "ARCHIVE INTEGRITY CHECK FAILED: Reconstructed data does not match "
            "original digests. The archive may be corrupted.");
      }
      SPRING_LOG_DEBUG("Integrity check passed for lossless archive.");
    }
  });

  const auto decompression_end = clock_type::now();
  if (Logger::is_info_enabled()) {
    std::cout << "Total time for decompression: "
              << std::chrono::duration_cast<std::chrono::seconds>(
                     decompression_end - decompression_start)
                     .count()
              << " s\n";
  } else {
    progress.finalize();
  }
}

void decompress_standard(const std::vector<std::string> &input_paths,
                         const std::vector<std::string> &output_paths,
                         const bool unzip_flag) {
  decompression_archive_artifact artifact;
  artifact.files = read_all_files_from_tar_memory(input_paths[0]);
  artifact.scratch_dir.clear();
  decompress_archive_artifact(artifact, input_paths, output_paths, unzip_flag);
}

void decompress_standard_from_memory(
    const std::string &archive_contents, const std::string &archive_label,
    const std::vector<std::string> &output_paths, const bool unzip_flag) {
  decompression_archive_artifact artifact;
  artifact.files = read_all_files_from_tar_bytes(archive_contents);
  artifact.scratch_dir.clear();
  decompress_archive_artifact(artifact, {archive_label}, output_paths,
                              unzip_flag);
}

void materialize_aliased_group_output_from_memory(
    const std::string &read_archive_contents, const std::string &alias_source,
    const std::string &alias_output_path, const bool unzip_flag) {
  decompression_archive_artifact artifact;
  artifact.files = read_all_files_from_tar_bytes(read_archive_contents);
  artifact.scratch_dir.clear();

  compression_params cp{};
  read_archive_compression_params(artifact, cp);
  const archive_decompression_plan decompression_plan =
      build_archive_decompression_plan(cp);
  ensure_archive_decompression_plan_supported(decompression_plan);

  const int selected_stream = (alias_source == "R2") ? 1 : 0;
  bool should_gzip[2] = {false, false};
  bool should_bgzf[2] = {false, false};
  bool write_enabled[2] = {false, false};
  int compression_levels[2] = {cp.encoding.compression_level,
                               cp.encoding.compression_level};
  write_enabled[selected_stream] = true;
  if (!unzip_flag && alias_output_path.ends_with(".gz")) {
    should_gzip[selected_stream] = true;
    should_bgzf[selected_stream] = cp.gzip.streams[selected_stream].is_bgzf;
    compression_levels[selected_stream] = gzip_output_compression_level(
        cp.gzip.streams[selected_stream], cp.encoding.compression_level);
  }

  const std::string output_path_1 =
      (selected_stream == 0) ? alias_output_path : std::string();
  const std::string output_path_2 =
      (selected_stream == 1) ? alias_output_path : std::string();
  FileDecompressionSink sink(output_path_1, output_path_2, cp,
                             compression_levels, should_gzip, should_bgzf,
                             write_enabled);
  execute_archive_decompression_plan(artifact, sink, cp, decompression_plan);

  const bool is_lossless = cp.encoding.preserve_order &&
                           cp.encoding.preserve_quality &&
                           cp.encoding.preserve_id &&
                           !cp.quality.ill_bin_flag && !cp.quality.bin_thr_flag;
  if (is_lossless) {
    uint32_t seq_crc[2], qual_crc[2], id_crc[2];
    sink.get_digests(seq_crc, qual_crc, id_crc);
    bool mismatch = false;
    for (int i = 0; i < (cp.encoding.paired_end ? 2 : 1); ++i) {
      if (cp.read_info.sequence_crc[i] != 0 &&
          seq_crc[i] != cp.read_info.sequence_crc[i]) {
        Logger::log_error("Stream " + std::to_string(i + 1) +
                          " sequence digest mismatch.");
        mismatch = true;
      }
      if (cp.read_info.quality_crc[i] != 0 &&
          qual_crc[i] != cp.read_info.quality_crc[i]) {
        Logger::log_error("Stream " + std::to_string(i + 1) +
                          " quality digest mismatch.");
        mismatch = true;
      }
      if (cp.read_info.id_crc[i] != 0 && id_crc[i] != cp.read_info.id_crc[i]) {
        Logger::log_error("Stream " + std::to_string(i + 1) +
                          " ID digest mismatch.");
        mismatch = true;
      }
    }
    if (mismatch) {
      throw std::runtime_error(
          "ARCHIVE INTEGRITY CHECK FAILED: Reconstructed data does not match "
          "original digests. The archive may be corrupted.");
    }
  }
}

} // namespace

void decompress(const std::vector<std::string> &input_paths,
                const std::vector<std::string> &output_paths,
                const log_level verbosity_level, const bool unzip_flag) {
  Logger::set_level(verbosity_level);
  ProgressBar progress(verbosity_level == log_level::quiet);
  ProgressBar::SetGlobalInstance(&progress);
  omp_set_dynamic(0);

  SPRING_LOG_INFO("Starting decompression...");
  SPRING_LOG_DEBUG("Decompression request: unzip=" +
                   std::string(unzip_flag ? "true" : "false"));

  if (input_paths.size() != 1)
    throw std::runtime_error("Number of input files not equal to 1");

  const auto manifest_contents =
      read_files_from_tar_memory(input_paths[0], {kBundleManifestName});
  if (manifest_contents.contains(kBundleManifestName)) {
    const bundle_manifest manifest = read_bundle_manifest_from_string(
        manifest_contents.at(kBundleManifestName));
    SPRING_LOG_INFO("Detected grouped bundle archive (reads + optional read3 + "
                    "optional index reads).");

    std::vector<std::string> resolved_outputs;
    if (output_paths.empty()) {
      resolved_outputs = build_default_grouped_output_paths(manifest);
    } else if (output_paths.size() == 1) {
      resolved_outputs.push_back(output_paths[0] + ".R1");
      resolved_outputs.push_back(output_paths[0] + ".R2");
      if (manifest.has_r3) {
        resolved_outputs.push_back(output_paths[0] + ".R3");
      }
      if (manifest.has_index) {
        resolved_outputs.push_back(output_paths[0] + ".I1");
        if (manifest.has_i2)
          resolved_outputs.push_back(output_paths[0] + ".I2");
      }
    } else {
      size_t expected = 2;
      if (manifest.has_r3)
        expected += 1;
      if (manifest.has_index)
        expected += manifest.has_i2 ? 2 : 1;
      if (output_paths.size() != expected) {
        throw std::runtime_error(
            "Grouped decompression output count does not match bundle layout.");
      }
      resolved_outputs = output_paths;
    }

    validate_output_targets(input_paths[0], resolved_outputs);

    const auto top_level_files = read_all_files_from_tar_memory(input_paths[0]);
    auto require_member =
        [&](const std::string &member_name) -> const std::string & {
      auto member_it = top_level_files.find(member_name);
      if (member_it == top_level_files.end()) {
        throw std::runtime_error(
            "Grouped archive is missing required member: " + member_name);
      }
      return member_it->second;
    };

    std::vector<std::string> read_outputs = {resolved_outputs[0],
                                             resolved_outputs[1]};
    decompress_standard_from_memory(require_member(manifest.read_archive_name),
                                    manifest.read_archive_name, read_outputs,
                                    unzip_flag);

    size_t next_output = 2;
    if (manifest.has_r3) {
      if (!manifest.read3_alias_source.empty()) {
        materialize_aliased_group_output_from_memory(
            require_member(manifest.read_archive_name),
            manifest.read3_alias_source, resolved_outputs[next_output++],
            unzip_flag);
      } else {
        decompress_standard_from_memory(
            require_member(manifest.read3_archive_name),
            manifest.read3_archive_name, {resolved_outputs[next_output++]},
            unzip_flag);
      }
    }

    if (manifest.has_index) {
      std::vector<std::string> index_outputs = {
          resolved_outputs[next_output++]};
      if (manifest.has_i2) {
        index_outputs.push_back(resolved_outputs[next_output++]);
      }
      decompress_standard_from_memory(
          require_member(manifest.index_archive_name),
          manifest.index_archive_name, index_outputs, unzip_flag);
    }

    ProgressBar::SetGlobalInstance(nullptr);
    return;
  }

  decompress_standard(input_paths, output_paths, unzip_flag);
}

} // namespace spring