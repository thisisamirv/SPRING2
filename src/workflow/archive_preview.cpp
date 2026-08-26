// Implements archive preview helpers used by the spring2 --preview mode.

#include "archive_preview.h"

#include "common/bundle_manifest.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>

#include "archive_record_reconstruction.h"
#include "fs_utils.h"
#include "params.h"
#include "workflow_api.h"
#include "workflow_internal.h"

namespace spring {

namespace {

std::string preview_archive_version(const compression_params &cp) {
  if (cp.read_info.legacy_spring) {
    return "legacy spring";
  }

  return cp.read_info.compressor_version.empty()
             ? "1.0.0-rc.1"
             : cp.read_info.compressor_version;
}

} // namespace

// Helper function to print gzip compression info for a single file
void print_gzip_compression_info(const std::string &filename, bool was_gzipped,
                                 uint8_t flg, uint32_t mtime, uint8_t xfl,
                                 uint8_t os, const std::string &name,
                                 bool is_bgzf, uint16_t bgzf_bsiz,
                                 uint64_t uncomp_sz, uint64_t comp_sz,
                                 uint32_t members) {
  if (!was_gzipped)
    return;

  auto to_mb = [](uint64_t bytes) { return (double)bytes / (1024.0 * 1024.0); };

  auto get_suggested_uncomp_name = [](const std::string &path) {
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".gz")
      return path.substr(0, path.size() - 3);
    return path;
  };

  std::cout << "--------------------------------\n";
  std::cout << filename << " Original Compression:\n";
  std::string profile = "UNKNOWN";
  if (is_bgzf)
    profile = "BGZF (Default)";
  else if (xfl == 2)
    profile = "MAX (Slowest)";
  else if (xfl == 4)
    profile = "FAST (Fastest)";
  else
    profile = "DEFAULT/OTHER";

  std::cout << "  Profile:         " << profile << "\n";
  std::cout << "  Format:          "
            << (is_bgzf ? "BGZF (Block Gzip)" : "Standard Gzip") << "\n";
  if (is_bgzf) {
    std::cout << "  Block Size:      " << bgzf_bsiz << "\n";
  }
  std::string suggested = get_suggested_uncomp_name(filename);
  std::cout << "  Uncompressed Name: " << (name.empty() ? suggested : name)
            << (name.empty() ? "" : " (from header)") << "\n";
  std::cout << "  Gzip Header:     FLG=0x" << std::hex << (int)flg << std::dec
            << ", MTIME=" << mtime << ", OS=" << (int)os << "\n";
  std::cout << "  Member Count:    " << members << "\n";
  if (comp_sz > 0) {
    double ratio = (double)uncomp_sz / comp_sz;
    std::cout << "  Original Ratio:  " << std::fixed << std::setprecision(2)
              << ratio << "x (" << (uint64_t)to_mb(uncomp_sz) << " / "
              << (uint64_t)to_mb(comp_sz) << " MB)\n";
  }

  std::string origin = "Unknown";
  if (is_bgzf)
    origin = "htslib/samtools/clib";
  else if (mtime == 0 && os == 255)
    origin = "Modern pipeline/programmatic";
  else if (!(flg & 0x08))
    origin = "Programmatic (No filename)";
  std::cout << "  Likely Origin:   " << origin << "\n";
}

void preview_single(const std::string &archive_path, bool audit_only) {
  if (audit_only) {
    perform_audit(archive_path);
    return;
  }

  auto contents = read_files_from_tar_memory(archive_path, {"cp.bin"});

  if (!contents.contains("cp.bin")) {
    throw std::runtime_error("Could not find cp.bin in the archive.");
  }

  decompression_archive_artifact artifact;
  artifact.files = std::move(contents);
  artifact.scratch_dir.clear();

  compression_params cp{};
  try {
    read_archive_compression_params(artifact, cp);
  } catch (const std::exception &) {
    throw std::runtime_error("Could not parse cp.bin from the archive.");
  }
  build_archive_decompression_plan(cp);

  std::cout << "SPRING2 Archive Metadata Preview:\n";
  std::cout << "--------------------------------\n";
  std::cout << "Archive Version:   " << preview_archive_version(cp) << "\n";
  if (cp.read_info.legacy_spring) {
    std::cout << "Original Inputs:   Unavailable in legacy spring archives\n";
  } else {
    std::cout << "Original Input 1:  " << cp.read_info.input_filename_1 << "\n";
    if (cp.encoding.paired_end) {
      std::cout << "Original Input 2:  " << cp.read_info.input_filename_2
                << "\n";
    }
  }
  uint64_t archive_size = std::filesystem::file_size(archive_path);
  if (!cp.read_info.note.empty()) {
    std::cout << "Note:              " << cp.read_info.note << "\n";
  }
  std::cout << "Assay Type:        ";
  if (cp.read_info.legacy_spring) {
    std::cout << "Unavailable in legacy spring archives";
  } else {
    std::cout << (!cp.read_info.assay.empty() ? cp.read_info.assay : "auto");
    if (!cp.read_info.assay_confidence.empty() &&
        cp.read_info.assay_confidence != "N/A") {
      std::cout << " (" << cp.read_info.assay_confidence << ")";
    }
  }
  std::cout << "\n";
  if (cp.encoding.cb_prefix_stripped) {
    std::cout << "CB Prefix:         Extracted (" << cp.encoding.cb_prefix_len
              << " bp from R1 single-cell prefix)\n";
  }
  if (cp.encoding.index_id_suffix_reconstructed) {
    std::cout << "Index IDs:         Reconstructed trailing I1/I2 token from "
                 "index reads\n";
  }
  if (cp.encoding.atac_adapter_stripped) {
    std::cout
        << "ATAC Adapters:     Stripped terminal Tn5/Nextera read-through\n";
  }
  if (cp.encoding.barcode_sort) {
    std::cout << "Barcode Sort:      Yes (CB: R1 prefix or I1 lane, "
              << cp.encoding.cb_len << " bp) [Legacy]\n";
  }
  const uint32_t input_1_total_reads = cp.encoding.paired_end
                                           ? cp.read_info.num_reads / 2
                                           : cp.read_info.num_reads;
  const uint32_t input_2_total_reads =
      cp.encoding.paired_end ? cp.read_info.num_reads / 2 : 0;
  const uint32_t input_1_non_clean_reads =
      input_1_total_reads >= cp.read_info.num_reads_clean[0]
          ? input_1_total_reads - cp.read_info.num_reads_clean[0]
          : 0;
  const uint32_t input_2_non_clean_reads =
      input_2_total_reads >= cp.read_info.num_reads_clean[1]
          ? input_2_total_reads - cp.read_info.num_reads_clean[1]
          : 0;
  std::cout << "Mode:              "
            << (cp.encoding.paired_end ? "Paired-end" : "Single-end") << "\n";
  std::cout << "Total Read Records:" << std::setw(4) << " "
            << cp.read_info.num_reads << "\n";
  if (cp.encoding.paired_end) {
    std::cout << "Clean Reads:       Input 1: "
              << cp.read_info.num_reads_clean[0];
    if (input_1_non_clean_reads > 0) {
      std::cout << " (+ " << input_1_non_clean_reads << " non-clean)";
    }
    std::cout << "\n";
    std::cout << "                   Input 2: "
              << cp.read_info.num_reads_clean[1];
    if (input_2_non_clean_reads > 0) {
      std::cout << " (+ " << input_2_non_clean_reads << " non-clean)";
    }
    std::cout << "\n";
  }

  uint64_t total_orig_compressed_size = cp.gzip.streams[0].compressed_size;
  if (cp.encoding.paired_end) {
    total_orig_compressed_size += cp.gzip.streams[1].compressed_size;
  }
  if (total_orig_compressed_size > 0) {
    double to_mb_factor = 1024.0 * 1024.0;
    double overall_ratio = (double)total_orig_compressed_size / archive_size;
    std::cout << "Compression Ratio: " << std::fixed << std::setprecision(2)
              << overall_ratio << "x ("
              << (uint64_t)(total_orig_compressed_size / to_mb_factor) << " / "
              << (uint64_t)(archive_size / to_mb_factor) << " MB)\n";
  }
  std::cout << "Max Read Length:   " << cp.read_info.max_readlen << " (using "
            << (cp.encoding.long_flag ? "long" : "short") << "-read encoder)\n";
  std::cout << "Preserve Order:    "
            << (cp.encoding.preserve_order ? "Yes" : "No") << "\n";
  std::cout << "Preserve IDs:      " << (cp.encoding.preserve_id ? "Yes" : "No")
            << "\n";
  std::cout << "Preserve Quality:  "
            << (cp.encoding.preserve_quality ? "Yes" : "No") << "\n";
  if (cp.encoding.preserve_quality) {
    std::cout << "Quality Mode:      ";
    if (cp.quality.ill_bin_flag)
      std::cout << "Illumina 8-level binning";
    else if (cp.quality.bin_thr_flag)
      std::cout << "Binary binning (thr: " << cp.quality.bin_thr_thr << ")";
    else
      std::cout << "Lossless";
    std::cout << "\n";
  }
  std::cout << "Compression Level: " << cp.encoding.compression_level << "\n";
  std::cout << "Use CRLF:          " << (cp.encoding.use_crlf ? "Yes" : "No")
            << "\n";

  print_gzip_compression_info(
      cp.read_info.input_filename_1, cp.gzip.streams[0].was_gzipped,
      cp.gzip.streams[0].flg, cp.gzip.streams[0].mtime, cp.gzip.streams[0].xfl,
      cp.gzip.streams[0].os, cp.gzip.streams[0].name,
      cp.gzip.streams[0].is_bgzf, cp.gzip.streams[0].bgzf_block_size,
      cp.gzip.streams[0].uncompressed_size, cp.gzip.streams[0].compressed_size,
      cp.gzip.streams[0].member_count);

  if (cp.encoding.paired_end) {
    print_gzip_compression_info(
        cp.read_info.input_filename_2, cp.gzip.streams[1].was_gzipped,
        cp.gzip.streams[1].flg, cp.gzip.streams[1].mtime,
        cp.gzip.streams[1].xfl, cp.gzip.streams[1].os, cp.gzip.streams[1].name,
        cp.gzip.streams[1].is_bgzf, cp.gzip.streams[1].bgzf_block_size,
        cp.gzip.streams[1].uncompressed_size,
        cp.gzip.streams[1].compressed_size, cp.gzip.streams[1].member_count);
  }
}

void preview(const std::string &archive_path, bool audit_only) {
  if (audit_only) {
    preview_single(archive_path, true);
    return;
  }

  auto contents =
      read_files_from_tar_memory(archive_path, {kBundleManifestName});

  if (contents.contains(kBundleManifestName)) {
    const bundle_manifest manifest =
        read_bundle_manifest_from_string(contents[kBundleManifestName]);

    // Extract only cp.bin from each member archive via nested-tar streaming.
    // read_files_from_nested_tars streams the outer archive from disk without
    // buffering any member archive in RAM; each member is opened on-the-fly
    // via a libarchive read callback fed from the outer entry stream.
    std::unordered_map<std::string, std::vector<std::string>> nested_targets;
    nested_targets[manifest.read_archive_name] = {"cp.bin"};
    if (manifest.has_r3 && manifest.read3_alias_source.empty()) {
      nested_targets[manifest.read3_archive_name] = {"cp.bin"};
    }
    if (manifest.has_index) {
      nested_targets[manifest.index_archive_name] = {"cp.bin"};
    }
    const auto cp_bin_map =
        read_files_from_nested_tars(archive_path, {}, nested_targets);

    auto get_cp_bin =
        [&](const std::string &member_name) -> const std::string & {
      auto it = cp_bin_map.find(member_name + "/cp.bin");
      if (it == cp_bin_map.end()) {
        throw std::runtime_error("Could not find cp.bin in member archive: " +
                                 member_name);
      }
      return it->second;
    };

    // Parse compression params from the main reads archive cp.bin
    decompression_archive_artifact reads_artifact;
    reads_artifact.files.emplace("cp.bin",
                                 get_cp_bin(manifest.read_archive_name));
    reads_artifact.scratch_dir.clear();
    compression_params cp_reads{};
    try {
      read_archive_compression_params(reads_artifact, cp_reads);
    } catch (const std::exception &) {
      throw std::runtime_error("Could not parse cp.bin in reads archive.");
    }
    build_archive_decompression_plan(cp_reads);

    // Parse compression params from R3 archive cp.bin if present
    compression_params cp_r3{};
    if (manifest.has_r3 && manifest.read3_alias_source.empty()) {
      auto r3_it = cp_bin_map.find(manifest.read3_archive_name + "/cp.bin");
      if (r3_it != cp_bin_map.end()) {
        decompression_archive_artifact r3_artifact;
        r3_artifact.files.emplace("cp.bin", r3_it->second);
        r3_artifact.scratch_dir.clear();
        try {
          read_archive_compression_params(r3_artifact, cp_r3);
        } catch (const std::exception &) {
          throw std::runtime_error("Could not parse cp.bin in read3 archive.");
        }
      }
    }

    // Parse compression params from index archive cp.bin if present
    compression_params cp_index{};
    if (manifest.has_index) {
      auto idx_it = cp_bin_map.find(manifest.index_archive_name + "/cp.bin");
      if (idx_it != cp_bin_map.end()) {
        decompression_archive_artifact index_artifact;
        index_artifact.files.emplace("cp.bin", idx_it->second);
        index_artifact.scratch_dir.clear();
        try {
          read_archive_compression_params(index_artifact, cp_index);
        } catch (const std::exception &) {
          throw std::runtime_error("Could not parse cp.bin in index archive.");
        }
      }
    }

    // Display unified metadata
    std::cout << "\nSPRING2 Archive Metadata Preview:\n";
    std::cout << "--------------------------------\n";
    std::cout << "Archive Version:   " << preview_archive_version(cp_reads)
              << "\n";
    if (!cp_reads.read_info.note.empty()) {
      std::cout << "Note:              " << cp_reads.read_info.note << "\n";
    }
    std::cout << "Original Input 1:  " << manifest.r1_name << "\n";
    std::cout << "Original Input 2:  " << manifest.r2_name << "\n";
    if (manifest.has_r3) {
      std::cout << "Original Input 3:  " << manifest.r3_name << "\n";
    }
    if (manifest.has_index) {
      std::cout << "Original Input I1: " << manifest.i1_name << "\n";
    }
    if (manifest.has_index && manifest.has_i2) {
      std::cout << "Original Input I2: " << manifest.i2_name << "\n";
    }

    uint64_t archive_size = std::filesystem::file_size(archive_path);
    std::cout << "Assay Type:        "
              << (!cp_reads.read_info.assay.empty() ? cp_reads.read_info.assay
                                                    : "auto");
    if (!cp_reads.read_info.assay_confidence.empty() &&
        cp_reads.read_info.assay_confidence != "N/A") {
      std::cout << " (" << cp_reads.read_info.assay_confidence << ")";
    }
    std::cout << "\n";
    if (cp_reads.encoding.cb_prefix_stripped) {
      std::cout << "CB Prefix:         Extracted ("
                << cp_reads.encoding.cb_prefix_len
                << " bp from R1 single-cell prefix)\n";
    }
    if (manifest.has_index && cp_index.encoding.index_id_suffix_reconstructed) {
      std::cout << "Index IDs:         Reconstructed trailing I1/I2 token "
                   "from index reads\n";
    }
    if (cp_reads.encoding.atac_adapter_stripped) {
      std::cout << "ATAC Adapters:     Stripped terminal Tn5/Nextera "
                   "read-through\n";
    }
    if (cp_reads.encoding.barcode_sort) {
      std::cout << "Barcode Sort:      Yes (CB: R1 prefix or I1 lane, "
                << cp_reads.encoding.cb_len << " bp) [Legacy]\n";
    }
    const uint32_t input_1_total_reads = cp_reads.encoding.paired_end
                                             ? cp_reads.read_info.num_reads / 2
                                             : cp_reads.read_info.num_reads;
    const uint32_t input_2_total_reads =
        cp_reads.encoding.paired_end ? cp_reads.read_info.num_reads / 2 : 0;
    const uint32_t input_1_non_clean_reads =
        input_1_total_reads >= cp_reads.read_info.num_reads_clean[0]
            ? input_1_total_reads - cp_reads.read_info.num_reads_clean[0]
            : 0;
    const uint32_t input_2_non_clean_reads =
        input_2_total_reads >= cp_reads.read_info.num_reads_clean[1]
            ? input_2_total_reads - cp_reads.read_info.num_reads_clean[1]
            : 0;
    std::cout << "Mode:              Grouped (R + I lanes), "
              << (cp_reads.encoding.paired_end ? "Paired-end" : "Single-end")
              << "\n";
    std::cout << "Total Read Records:" << std::setw(4) << " "
              << cp_reads.read_info.num_reads << "\n";
    if (cp_reads.encoding.paired_end) {
      std::cout << "Clean Reads:       Input 1: "
                << cp_reads.read_info.num_reads_clean[0];
      if (input_1_non_clean_reads > 0) {
        std::cout << " (+ " << input_1_non_clean_reads << " non-clean)";
      }
      std::cout << "\n";
      std::cout << "                   Input 2: "
                << cp_reads.read_info.num_reads_clean[1];
      if (input_2_non_clean_reads > 0) {
        std::cout << " (+ " << input_2_non_clean_reads << " non-clean)";
      }
      std::cout << "\n";
    }

    uint64_t total_orig_compressed_size =
        cp_reads.gzip.streams[0].compressed_size;
    if (cp_reads.encoding.paired_end) {
      total_orig_compressed_size += cp_reads.gzip.streams[1].compressed_size;
    }
    if (manifest.has_r3 && manifest.read3_alias_source.empty()) {
      total_orig_compressed_size += cp_r3.gzip.streams[0].compressed_size;
    }
    if (manifest.has_index) {
      total_orig_compressed_size += cp_index.gzip.streams[0].compressed_size;
      if (manifest.has_i2 && cp_index.encoding.paired_end) {
        total_orig_compressed_size += cp_index.gzip.streams[1].compressed_size;
      }
    }
    if (total_orig_compressed_size > 0) {
      double to_mb_factor = 1024.0 * 1024.0;
      double overall_ratio = (double)total_orig_compressed_size / archive_size;
      std::cout << "Compression Ratio: " << std::fixed << std::setprecision(2)
                << overall_ratio << "x ("
                << (uint64_t)(total_orig_compressed_size / to_mb_factor)
                << " / " << (uint64_t)(archive_size / to_mb_factor) << " MB)\n";
    }
    std::cout << "Max Read Length:   " << cp_reads.read_info.max_readlen
              << " (using " << (cp_reads.encoding.long_flag ? "long" : "short")
              << "-read encoder)\n";
    std::cout << "Preserve Order:    "
              << (cp_reads.encoding.preserve_order ? "Yes" : "No") << "\n";
    std::cout << "Preserve IDs:      "
              << (cp_reads.encoding.preserve_id ? "Yes" : "No") << "\n";
    std::cout << "Preserve Quality:  "
              << (cp_reads.encoding.preserve_quality ? "Yes" : "No") << "\n";
    if (cp_reads.encoding.preserve_quality) {
      std::cout << "Quality Mode:      ";
      if (cp_reads.quality.ill_bin_flag)
        std::cout << "Illumina 8-level binning";
      else if (cp_reads.quality.bin_thr_flag)
        std::cout << "Binary binning (thr: " << cp_reads.quality.bin_thr_thr
                  << ")";
      else
        std::cout << "Lossless";
      std::cout << "\n";
    }
    std::cout << "Compression Level: " << cp_reads.encoding.compression_level
              << "\n";
    std::cout << "Use CRLF:          "
              << (cp_reads.encoding.use_crlf ? "Yes" : "No") << "\n";

    print_gzip_compression_info(
        manifest.r1_name, cp_reads.gzip.streams[0].was_gzipped,
        cp_reads.gzip.streams[0].flg, cp_reads.gzip.streams[0].mtime,
        cp_reads.gzip.streams[0].xfl, cp_reads.gzip.streams[0].os,
        cp_reads.gzip.streams[0].name, cp_reads.gzip.streams[0].is_bgzf,
        cp_reads.gzip.streams[0].bgzf_block_size,
        cp_reads.gzip.streams[0].uncompressed_size,
        cp_reads.gzip.streams[0].compressed_size,
        cp_reads.gzip.streams[0].member_count);

    if (cp_reads.encoding.paired_end) {
      print_gzip_compression_info(
          manifest.r2_name, cp_reads.gzip.streams[1].was_gzipped,
          cp_reads.gzip.streams[1].flg, cp_reads.gzip.streams[1].mtime,
          cp_reads.gzip.streams[1].xfl, cp_reads.gzip.streams[1].os,
          cp_reads.gzip.streams[1].name, cp_reads.gzip.streams[1].is_bgzf,
          cp_reads.gzip.streams[1].bgzf_block_size,
          cp_reads.gzip.streams[1].uncompressed_size,
          cp_reads.gzip.streams[1].compressed_size,
          cp_reads.gzip.streams[1].member_count);
    }

    if (manifest.has_r3) {
      if (!manifest.read3_alias_source.empty()) {
        std::cout << "--------------------------------\n";
        std::cout << manifest.r3_name << " (aliased to "
                  << manifest.read3_alias_source << ", no extra payload)\n";
      } else {
        print_gzip_compression_info(
            manifest.r3_name, cp_r3.gzip.streams[0].was_gzipped,
            cp_r3.gzip.streams[0].flg, cp_r3.gzip.streams[0].mtime,
            cp_r3.gzip.streams[0].xfl, cp_r3.gzip.streams[0].os,
            cp_r3.gzip.streams[0].name, cp_r3.gzip.streams[0].is_bgzf,
            cp_r3.gzip.streams[0].bgzf_block_size,
            cp_r3.gzip.streams[0].uncompressed_size,
            cp_r3.gzip.streams[0].compressed_size,
            cp_r3.gzip.streams[0].member_count);
      }
    }

    if (manifest.has_index) {
      print_gzip_compression_info(
          manifest.i1_name, cp_index.gzip.streams[0].was_gzipped,
          cp_index.gzip.streams[0].flg, cp_index.gzip.streams[0].mtime,
          cp_index.gzip.streams[0].xfl, cp_index.gzip.streams[0].os,
          cp_index.gzip.streams[0].name, cp_index.gzip.streams[0].is_bgzf,
          cp_index.gzip.streams[0].bgzf_block_size,
          cp_index.gzip.streams[0].uncompressed_size,
          cp_index.gzip.streams[0].compressed_size,
          cp_index.gzip.streams[0].member_count);

      if (manifest.has_i2 && cp_index.encoding.paired_end) {
        print_gzip_compression_info(
            manifest.i2_name, cp_index.gzip.streams[1].was_gzipped,
            cp_index.gzip.streams[1].flg, cp_index.gzip.streams[1].mtime,
            cp_index.gzip.streams[1].xfl, cp_index.gzip.streams[1].os,
            cp_index.gzip.streams[1].name, cp_index.gzip.streams[1].is_bgzf,
            cp_index.gzip.streams[1].bgzf_block_size,
            cp_index.gzip.streams[1].uncompressed_size,
            cp_index.gzip.streams[1].compressed_size,
            cp_index.gzip.streams[1].member_count);
      }
    }
    return;
  }

  preview_single(archive_path, audit_only);
}

} // namespace spring
