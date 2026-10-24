// Implements ATAC-specific preprocessing and reconstruction helpers such as
// terminal adapter stripping and restoration.

#include "assay_atac.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace spring {

namespace {
constexpr std::array<std::string_view, 2> kAtacAdapters = {
    "CTGTCTCTTATACACATCT", "AGATGTGTATAAGAGACAG"};
}

bool grouped_archive_allows_atac_adapter_stripping(
    const std::string &archive_note) {
  return archive_note.find("index-group") == std::string::npos &&
         archive_note.find("read3-group") == std::string::npos;
}

AssayScoreBreakdown score_atac_assay(const AssayDetectionStats &stats,
                                     bool reference_loaded) {
  AssayScoreBreakdown result;
  if (stats.total_reads == 0) {
    return result;
  }

  const double atac_adapter_frac =
      static_cast<double>(stats.atac_adapters_found) / stats.total_reads;
  if (atac_adapter_frac > 0.05) {
    result.score += 90.0;
    result.evidence.emplace_back("Tn5 adapters (>5%)");
  } else if (atac_adapter_frac > 0.01) {
    result.score += 70.0;
    result.evidence.emplace_back("Tn5 adapters");
  } else if (atac_adapter_frac > 0.003) {
    result.score += 35.0;
    result.evidence.emplace_back("Tn5 adapters (weak)");
  }

  if (reference_loaded && stats.total_sampled_kmers > 0) {
    const double atac_ref_score =
        static_cast<double>(stats.atac_hits) / (stats.intron_hits + 1);
    if (atac_ref_score > 5.0) {
      result.score += 50.0;
      result.evidence.emplace_back("promoter alignment");
    } else if (atac_ref_score > 2.0) {
      result.score += 25.0;
    }
  }

  return result;
}

bool has_atac_adapter(const std::string &seq) {
  // Match a prefix kmer from each Tn5 mosaic-end orientation so reverse-
  // adapter reads are counted. The previous reverse kmer (TATACACATCTC) was
  // not a substring of either adapter and missed AGATGTGTATAAGAGACAG.
  constexpr size_t kKmerLen = 12;
  for (const std::string_view adapter : kAtacAdapters) {
    if (adapter.size() < kKmerLen) {
      continue;
    }
    if (seq.find(adapter.substr(0, kKmerLen)) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool should_consider_atac_adapter_stripping(const compression_params &cp,
                                            bool fasta_input,
                                            bool grouped_archive_allowed) {
  return !fasta_input && !cp.encoding.long_flag &&
         (cp.read_info.assay == "atac" || cp.read_info.assay == "sc-atac") &&
         (cp.read_info.assay_confidence == "N/A" ||
          cp.read_info.assay_confidence.starts_with("high") ||
          cp.read_info.assay_confidence.starts_with("medium")) &&
         grouped_archive_allowed;
}

bool should_activate_atac_adapter_stripping(uint32_t eligible_reads,
                                            uint64_t stripped_bases,
                                            double min_bases_per_read) {
  const double avg_stripped_bases_per_read =
      eligible_reads == 0 ? 0.0
                          : static_cast<double>(stripped_bases) /
                                static_cast<double>(eligible_reads);
  return avg_stripped_bases_per_read >= min_bases_per_read;
}

uint8_t detect_atac_adapter_tail_info(const std::string &read_str,
                                      uint32_t read_length) {
  if (read_length < ATAC_ADAPTER_MIN_MATCH)
    return 0;

  for (uint16_t adapter_id = 0; adapter_id < kAtacAdapters.size();
       ++adapter_id) {
    const std::string_view adapter = kAtacAdapters[adapter_id];
    const uint32_t max_overlap =
        std::min<uint32_t>(read_length, static_cast<uint32_t>(adapter.size()));
    for (uint32_t overlap = max_overlap; overlap >= ATAC_ADAPTER_MIN_MATCH;
         --overlap) {
      const size_t read_offset = static_cast<size_t>(read_length - overlap);
      if (read_str.compare(read_offset, overlap,
                           adapter.substr(0, overlap).data(), overlap) == 0) {
        return static_cast<uint8_t>((overlap << 1) | adapter_id);
      }
      if (overlap == ATAC_ADAPTER_MIN_MATCH)
        break;
    }
  }

  return 0;
}

void strip_atac_adapter_tail(std::string &read_str, uint32_t &read_length,
                             uint8_t adapter_info) {
  const uint32_t strip_len = adapter_info >> 1;
  if (strip_len == 0)
    return;
  read_length -= strip_len;
  read_str.resize(read_length);
}

void restore_atac_adapter_tail(std::string &read_str, std::string *quality_str,
                               uint8_t adapter_info,
                               const std::string *tail_qual) {
  if (adapter_info == 0)
    return;

  const uint8_t adapter_id = adapter_info & 1;
  const uint32_t overlap = adapter_info >> 1;
  const std::string_view adapter = kAtacAdapters[adapter_id];
  if (overlap > adapter.size()) {
    throw std::runtime_error("Corrupt ATAC adapter metadata: overlap exceeds "
                             "adapter length");
  }

  read_str.append(adapter.substr(0, overlap));
  if (quality_str) {
    if (tail_qual) {
      quality_str->append(*tail_qual);
    } else {
      quality_str->append(overlap, 'I');
    }
  }
}

} // namespace spring
