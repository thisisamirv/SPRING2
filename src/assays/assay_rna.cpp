// Implements RNA-specific preprocessing and reconstruction helpers such as
// poly-A/T tail stripping and restoration.

#include "assay_rna.h"

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
  if (stats.total_reads == 0) {
    return result;
  }

  const double poly_a_frac =
      static_cast<double>(stats.poly_a_tails_found) / stats.total_reads;
  if (!suppress_poly_a_signal) {
    if (poly_a_frac > 0.10) {
      result.score += 80.0;
      result.evidence.emplace_back("poly-A/T tails (>10%)");
    } else if (poly_a_frac > 0.03) {
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

bool has_poly_a_tail(const std::string &seq) {
  if (seq.length() < 20)
    return false;
  int a_count = 0;
  int t_count = 0;
  for (size_t i = seq.length() - 20; i < seq.length(); ++i) {
    if (seq[i] == 'A')
      a_count++;
    if (seq[i] == 'T')
      t_count++;
  }
  return a_count >= 15 || t_count >= 15;
}

uint16_t detect_and_strip_rna_tail(std::string &read_str,
                                   uint32_t &read_length) {
  if (read_length <= POLY_AT_TAIL_MIN_LEN)
    return 0;
  const char last = read_str[read_length - 1];
  if (last != 'A' && last != 'T')
    return 0;
  uint32_t run = 0;
  while (run < read_length && read_str[read_length - 1 - run] == last)
    ++run;
  if (run <= POLY_AT_TAIL_MIN_LEN)
    return 0;
  read_length -= run;
  read_str.resize(read_length);
  return static_cast<uint16_t>((run << 1) | (last == 'T' ? 1 : 0));
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
