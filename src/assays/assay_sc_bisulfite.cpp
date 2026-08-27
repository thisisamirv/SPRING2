// Implements sc-bisulfite-specific policy helpers so single-cell bisulfite
// handling is tracked in its own assay file.

#include "assay_sc_bisulfite.h"

namespace spring {

bool is_sc_bisulfite_assay(const compression_params &cp) {
  return cp.read_info.assay == "sc-bisulfite";
}

// TODO: sc-bisulfite CB-prefix stripping is not yet implemented (read
// structure varies by protocol); wire this up once a reliable detection rule
// exists, mirroring should_enable_sc_rna_cb_prefix_stripping.
bool should_enable_sc_bisulfite_cb_prefix_stripping(
    const compression_params &cp) {
  (void)cp;
  return false;
}

void apply_sc_bisulfite_auto_config(
    compression_params &cp,
    const AssayDetector::DetectionResult &detection_result) {
  if (detection_result.assay != "sc-bisulfite") {
    return;
  }
  if (detection_result.c_ratio < 0.05 || detection_result.g_ratio < 0.05) {
    cp.encoding.bisulfite_ternary = true;
    cp.encoding.depleted_base = detection_result.depleted_base;
  }
}

} // namespace spring
