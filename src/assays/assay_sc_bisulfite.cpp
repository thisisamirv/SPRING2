// Implements sc-bisulfite-specific policy helpers so single-cell bisulfite
// handling is tracked in its own assay file.

#include "assay_sc_bisulfite.h"

#include "assay_bisulfite.h"

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
  apply_bisulfite_encoding_from_detection(cp, detection_result);
}

} // namespace spring
