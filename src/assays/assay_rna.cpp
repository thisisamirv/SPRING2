// Implements RNA-specific preprocessing and reconstruction helpers such as
// poly-A/T tail stripping and restoration.

#include "assay_rna.h"
#include "dna_utils.h"

#include <limits>
#include <string>

namespace spring {

bool should_enable_rna_tail_stripping(const compression_params &cp) {
  // Grouped index/read-3 members may inherit the primary assay label; do not
  // apply genomic poly-A/T stripping to those lanes.
  if (cp.read_info.note.find("index-group") != std::string::npos ||
      cp.read_info.note.find("read3-group") != std::string::npos) {
    return false;
  }
  return !cp.encoding.long_flag && !cp.encoding.preserve_order &&
         (cp.read_info.assay == "rna" || cp.read_info.assay == "sc-rna") &&
         (cp.read_info.assay_confidence == "N/A" ||
          cp.read_info.assay_confidence.starts_with("high"));
}

AssayScoreBreakdown score_rna_assay(const AssayDetectionStats &stats,
                                    bool reference_loaded,
                                    bool suppress_poly_a_signal) {
  AssayScoreBreakdown result;
  if (stats.r1_lengths.empty() && stats.r2_lengths.empty() &&
      stats.r3_lengths.empty()) {
    return result;
  }

  // Max per-lane rate: PE both-mates hits no longer score as ~2x SE; a signal
  // on only one mate (typical sc-RNA poly-A on R2) or R3 is preserved.
  // Thresholds are calibrated to contiguous-run rates (same as strip).
  const double poly_a_frac = max_lane_signal_rate(
      stats.poly_a_tails_r1, stats.poly_a_tails_r2, stats.poly_a_tails_r3,
      stats);
  if (!suppress_poly_a_signal) {
    if (poly_a_frac > 0.05) {
      result.score += 80.0;
      result.evidence.emplace_back("poly-A/T tails (>5%)");
    } else if (poly_a_frac > 0.015) {
      result.score += 60.0;
      result.evidence.emplace_back("poly-A/T tails");
    }
  }

  if (reference_loaded && stats.total_sampled_kmers > 0) {
    const double rna_ref_score =
        static_cast<double>(stats.rna_hits) / (stats.intron_hits + 1);
    if (rna_ref_score > 10.0) {
      result.score += 60.0;
      result.evidence.emplace_back("exon alignment");
    } else if (rna_ref_score > 5.0) {
      result.score += 30.0;
    }
  }

  return result;
}

uint16_t detect_rna_tail_info(const std::string &read_str,
                              uint32_t read_length) {
  if (read_length <= POLY_AT_TAIL_MIN_LEN ||
      read_length > read_str.size()) {
    return 0;
  }
  const char last = dna_ascii_upper(read_str[read_length - 1]);
  if (last != 'A' && last != 'T') {
    return 0;
  }
  uint32_t run = 0;
  while (run < read_length &&
         dna_ascii_upper(read_str[read_length - 1 - run]) == last) {
    ++run;
  }
  if (run <= POLY_AT_TAIL_MIN_LEN) {
    return 0;
  }
  return static_cast<uint16_t>((run << 1) | (last == 'T' ? 1 : 0));
}

bool has_poly_a_tail(const std::string &seq) {
  // Same criterion as strip: contiguous terminal A/T run > POLY_AT_TAIL_MIN_LEN.
  // Composition counts in a fixed window can label RNA from non-strippable tails.
  if (seq.size() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  return detect_rna_tail_info(seq, static_cast<uint32_t>(seq.size())) != 0;
}

uint16_t detect_and_strip_rna_tail(std::string &read_str,
                                   uint32_t &read_length) {
  const uint16_t tail_info = detect_rna_tail_info(read_str, read_length);
  if (tail_info == 0) {
    return 0;
  }
  const uint32_t run = static_cast<uint32_t>(tail_info) >> 1;
  read_length -= run;
  read_str.resize(read_length);
  return tail_info;
}

void restore_rna_tail(std::string &read_str, std::string *quality_str,
                      uint16_t tail_info, const std::string *tail_qual) {
  if (tail_info == 0)
    return;
  const char base = (tail_info & 1) ? 'T' : 'A';
  const uint32_t run = tail_info >> 1;
  read_str.append(run, base);
  if (quality_str) {
    if (tail_qual) {
      quality_str->append(*tail_qual);
    } else {
      quality_str->append(run, 'I');
    }
  }
}

} // namespace spring
