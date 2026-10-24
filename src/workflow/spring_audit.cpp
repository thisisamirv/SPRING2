// Implements the top-level archive audit workflows on top of the shared
// workflow helpers.

#include "workflow_internal.h"

#include <iostream>

namespace spring {

namespace {

digest_verification_result
verify_sink_digests(const NullDecompressionSink &sink,
                    const compression_params &cp) {
  const digest_channel_mask channels = digest_channels_for_archive(
      cp.encoding.preserve_order, cp.encoding.preserve_quality,
      cp.encoding.preserve_id, cp.quality.ill_bin_flag, cp.quality.bin_thr_flag);
  uint32_t seq_crc[2] = {0, 0};
  uint32_t qual_crc[2] = {0, 0};
  uint32_t id_crc[2] = {0, 0};
  if (channels.any()) {
    sink.get_digests(seq_crc, qual_crc, id_crc);
  }
  return verify_reconstructed_digests(
      channels, cp.read_info.digests_present, cp.encoding.paired_end,
      cp.read_info.sequence_crc, cp.read_info.quality_crc, cp.read_info.id_crc,
      seq_crc, qual_crc, id_crc);
}

void report_digest_mismatches(const compression_params &cp,
                              const uint32_t seq_crc[2],
                              const uint32_t qual_crc[2],
                              const uint32_t id_crc[2],
                              const lossless_digest_check &check) {
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
}

void finish_audit_digest_outcome(const std::string &archive_label,
                                 const compression_params &cp,
                                 const NullDecompressionSink &sink,
                                 const digest_verification_result &result) {
  switch (result.status) {
  case digest_check_status::verified:
    std::cout << "Audit verified digests: " << archive_label << " is valid."
              << std::endl;
    return;
  case digest_check_status::skipped_lossy:
    std::cout << "Audit completed (digest check skipped: no verifiable "
                 "channels, e.g. order stripped): "
              << archive_label << std::endl;
    return;
  case digest_check_status::skipped_no_digests:
    throw std::runtime_error(
        "Audit failed: archive has verifiable channels but no stored digests (" +
        archive_label + ").");
  case digest_check_status::mismatch: {
    uint32_t seq_crc[2] = {0, 0};
    uint32_t qual_crc[2] = {0, 0};
    uint32_t id_crc[2] = {0, 0};
    sink.get_digests(seq_crc, qual_crc, id_crc);
    report_digest_mismatches(cp, seq_crc, qual_crc, id_crc, result.check);
    throw std::runtime_error("Archive integrity audit failed!");
  }
  }
}

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
  sink.set_digest_length_framed(cp.read_info.digest_length_framed);
  execute_archive_decompression_plan(artifact, sink, cp, decompression_plan);

  const digest_verification_result result = verify_sink_digests(sink, cp);
  finish_audit_digest_outcome(archive_label, cp, sink, result);
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

    std::cout << "Audit finished: all grouped archive members processed."
              << std::endl;
    return;
  }

  decompression_archive_artifact artifact;
  artifact.files = top_level_files;
  artifact.scratch_dir.clear();
  perform_audit_standard_artifact(artifact, archive_path);
}

} // namespace spring
