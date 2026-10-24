// Implements the top-level archive audit workflows on top of the shared
// workflow helpers.

#include "workflow_internal.h"

#include <iostream>

namespace spring {

namespace {

void perform_audit_standard_artifact(
    const decompression_archive_artifact &artifact,
    const std::string &archive_label) {
  SPRING_LOG_DEBUG("Audit (standard) started for archive: " + archive_label +
                   " (in-memory)");

  compression_params cp{};
  read_archive_compression_params(artifact, cp);
  const archive_decompression_plan decompression_plan =
      build_archive_decompression_plan(cp);
  ensure_archive_decompression_plan_supported(decompression_plan);

  NullDecompressionSink sink;
  execute_archive_decompression_plan(artifact, sink, cp, decompression_plan);

  const bool is_lossless = cp.encoding.preserve_order &&
                           cp.encoding.preserve_quality &&
                           cp.encoding.preserve_id &&
                           !cp.quality.ill_bin_flag && !cp.quality.bin_thr_flag;

  if (is_lossless) {
    uint32_t seq_crc[2], qual_crc[2], id_crc[2];
    sink.get_digests(seq_crc, qual_crc, id_crc);

    const lossless_digest_check check = check_lossless_digests(
        cp.read_info.digests_present, cp.encoding.paired_end,
        cp.read_info.sequence_crc, cp.read_info.quality_crc,
        cp.read_info.id_crc, seq_crc, qual_crc, id_crc);
    for (int i = 0; i < check.stream_count; ++i) {
      if (check.sequence_mismatch[i]) {
        Logger::log_error("Stream " + std::to_string(i + 1) +
                          " sequence digest mismatch: expected=" +
                          std::to_string(cp.read_info.sequence_crc[i]) +
                          " actual=" + std::to_string(seq_crc[i]));
        std::cerr << "Stream " << (i + 1) << " sequence digest mismatch!\n";
      }
      if (check.quality_mismatch[i]) {
        Logger::log_error("Stream " + std::to_string(i + 1) +
                          " quality digest mismatch: expected=" +
                          std::to_string(cp.read_info.quality_crc[i]) +
                          " actual=" + std::to_string(qual_crc[i]));
        std::cerr << "Stream " << (i + 1) << " quality digest mismatch!\n";
      }
      if (check.id_mismatch[i]) {
        Logger::log_error("Stream " + std::to_string(i + 1) +
                          " ID digest mismatch: expected=" +
                          std::to_string(cp.read_info.id_crc[i]) +
                          " actual=" + std::to_string(id_crc[i]));
        std::cerr << "Stream " << (i + 1) << " ID digest mismatch!\n";
      }
    }
    if (check.mismatch)
      throw std::runtime_error("Archive integrity audit failed!");
  }

  std::cout << "Audit successful: " << archive_label << " is valid."
            << std::endl;
}

void perform_audit_standard_bytes(const std::string &archive_contents,
                                  const std::string &archive_label) {
  decompression_archive_artifact artifact;
  artifact.files = read_all_files_from_tar_bytes(archive_contents);
  artifact.scratch_dir.clear();
  perform_audit_standard_artifact(artifact, archive_label);
}

} // namespace

void perform_audit_standard(const std::string &archive_path) {
  decompression_archive_artifact artifact;
  artifact.files = read_all_files_from_tar_memory(archive_path);
  artifact.scratch_dir.clear();
  perform_audit_standard_artifact(artifact, archive_path);
}

void perform_audit(const std::string &archive_path) {
  SPRING_LOG_DEBUG("Audit started for archive: " + archive_path +
                   " (in-memory)");

  const std::unordered_map<std::string, std::string> top_level_files =
      read_all_files_from_tar_memory(archive_path);
  auto manifest_it = top_level_files.find(kBundleManifestName);
  if (manifest_it != top_level_files.end()) {
    const bundle_manifest manifest =
        read_bundle_manifest_from_string(manifest_it->second);

    auto require_member =
        [&](const std::string &member_name) -> const std::string & {
      auto member_it = top_level_files.find(member_name);
      if (member_it == top_level_files.end()) {
        throw std::runtime_error(
            "Grouped archive is missing required member: " + member_name);
      }
      return member_it->second;
    };

    perform_audit_standard_bytes(require_member(manifest.read_archive_name),
                                 manifest.read_archive_name);
    if (manifest.has_r3 && manifest.read3_alias_source.empty()) {
      perform_audit_standard_bytes(require_member(manifest.read3_archive_name),
                                   manifest.read3_archive_name);
    }
    if (manifest.has_index) {
      perform_audit_standard_bytes(require_member(manifest.index_archive_name),
                                   manifest.index_archive_name);
    }

    std::cout << "Audit successful: grouped archive members are valid."
              << std::endl;
    return;
  }

  decompression_archive_artifact artifact;
  artifact.files = top_level_files;
  artifact.scratch_dir.clear();
  perform_audit_standard_artifact(artifact, archive_path);
}

} // namespace spring