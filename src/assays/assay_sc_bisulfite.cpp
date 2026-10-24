// Implements sc-bisulfite-specific policy helpers so single-cell bisulfite
// handling is tracked in its own assay file.

#include "assay_sc_bisulfite.h"

namespace spring {

bool is_sc_bisulfite_assay(const compression_params &cp) {
  return cp.read_info.assay == "sc-bisulfite";
}

void apply_sc_bisulfite_auto_config(
    compression_params &cp,
    const AssayDetector::DetectionResult &detection_result) {
  if (detection_result.assay != "sc-bisulfite") {
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
