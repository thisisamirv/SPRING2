// Implements bisulfite-specific policy helpers so ternary handling is tracked
// in a dedicated assay file.

#include "assay_bisulfite.h"

#include <algorithm>
#include <string>

namespace spring {

BisulfiteDetectionScore
score_bisulfite_assay(const AssayDetectionStats &stats) {
  BisulfiteDetectionScore result;
  // No fragment-count floor (unlike a previous >=500 gate): ATAC/RNA have none,
  // and the per-stream base-count checks below already require enough C/T or
  // G/A observations for a reliable depletion ratio.

  const double r1_c_ratio =
      static_cast<double>(stats.r1_C) / (stats.r1_C + stats.r1_T + 1);
  const double r2_c_ratio =
      static_cast<double>(stats.r2_C) / (stats.r2_C + stats.r2_T + 1);
  const double r1_g_ratio =
      static_cast<double>(stats.r1_G) / (stats.r1_G + stats.r1_A + 1);
  const double r2_g_ratio =
      static_cast<double>(stats.r2_G) / (stats.r2_G + stats.r2_A + 1);

  const bool r1_c_strong =
      (stats.r1_C + stats.r1_T > 100) && (r1_c_ratio <= 0.10);
  const bool r1_g_strong =
      (stats.r1_G + stats.r1_A > 100) && (r1_g_ratio <= 0.10);
  const bool r2_c_strong =
      (stats.r2_C + stats.r2_T > 100) && (r2_c_ratio <= 0.10);
  const bool r2_g_strong =
      (stats.r2_G + stats.r2_A > 100) && (r2_g_ratio <= 0.10);

  const bool r1_c_moderate =
      (stats.r1_C + stats.r1_T > 100) && (r1_c_ratio <= 0.15);
  const bool r1_g_moderate =
      (stats.r1_G + stats.r1_A > 100) && (r1_g_ratio <= 0.15);
  const bool r2_c_moderate =
      (stats.r2_C + stats.r2_T > 100) && (r2_c_ratio <= 0.15);
  const bool r2_g_moderate =
      (stats.r2_G + stats.r2_A > 100) && (r2_g_ratio <= 0.15);

  // When C and G both qualify on one stream, keep the more depleted base
  // (lower ratio). Exact ties fall back to C for a stable choice.
  auto choose_depleted = [](bool c_hit, bool g_hit, double c_ratio,
                            double g_ratio) -> char {
    if (c_hit && g_hit) {
      return (c_ratio <= g_ratio) ? 'C' : 'G';
    }
    return c_hit ? 'C' : 'G';
  };

  if (r1_c_strong || r1_g_strong) {
    const char depleted =
        choose_depleted(r1_c_strong, r1_g_strong, r1_c_ratio, r1_g_ratio);
    result.breakdown.score += 80.0;
    result.breakdown.evidence.push_back("R1 " + std::string(1, depleted) +
                                         "-depletion");
    result.depleted_base_by_stream[0] = depleted;
  } else if (r1_c_moderate || r1_g_moderate) {
    const char depleted = choose_depleted(r1_c_moderate, r1_g_moderate,
                                          r1_c_ratio, r1_g_ratio);
    result.breakdown.score += 40.0;
    result.breakdown.evidence.push_back("R1 moderate " +
                                         std::string(1, depleted) +
                                         "-depletion");
    result.depleted_base_by_stream[0] = depleted;
  }

  if (r2_c_strong || r2_g_strong) {
    const char depleted =
        choose_depleted(r2_c_strong, r2_g_strong, r2_c_ratio, r2_g_ratio);
    result.breakdown.score += 80.0;
    result.breakdown.evidence.push_back("R2 " + std::string(1, depleted) +
                                         "-depletion");
    result.depleted_base_by_stream[1] = depleted;
  } else if (r2_c_moderate || r2_g_moderate) {
    const char depleted = choose_depleted(r2_c_moderate, r2_g_moderate,
                                          r2_c_ratio, r2_g_ratio);
    result.breakdown.score += 40.0;
    result.breakdown.evidence.push_back("R2 moderate " +
                                         std::string(1, depleted) +
                                         "-depletion");
    result.depleted_base_by_stream[1] = depleted;
  }

  // Single-end (no R2 bases sampled): mirror stream 0 into stream 1 so
  // downstream PE-shaped paths see a consistent pair. For true PE, leave an
  // un-signaled mate as 'N' so it is not forced onto the other mate's alphabet.
  const bool r2_absent =
      (stats.r2_C + stats.r2_T + stats.r2_G + stats.r2_A) == 0;
  if (r2_absent) {
    result.depleted_base_by_stream[1] = result.depleted_base_by_stream[0];
  }

  result.c_ratio = std::min(r1_c_ratio, r2_c_ratio);
  result.g_ratio = std::min(r1_g_ratio, r2_g_ratio);
  return result;
}

bool is_bisulfite_assay(const compression_params &cp) {
  return cp.read_info.assay == "bisulfite";
}

void apply_bisulfite_auto_config(
    compression_params &cp,
    const AssayDetector::DetectionResult &detection_result) {
  if (detection_result.assay != "bisulfite") {
    return;
  }
  if (detection_result.c_ratio < 0.05 || detection_result.g_ratio < 0.05) {
    cp.encoding.bisulfite_ternary = true;
    cp.encoding.depleted_base_by_stream[0] =
        detection_result.depleted_base_by_stream[0];
    cp.encoding.depleted_base_by_stream[1] =
        detection_result.depleted_base_by_stream[1];
  }
}

} // namespace spring
