// Declares sc-bisulfite-specific policy helpers so single-cell bisulfite
// handling is tracked in its own assay file.

#ifndef SPRING_ASSAY_SC_BISULFITE_H_
#define SPRING_ASSAY_SC_BISULFITE_H_

#include "assay_detector.h"
#include "params.h"

namespace spring {

[[nodiscard]] bool is_sc_bisulfite_assay(const compression_params &cp);

// Intentionally no CB-prefix strip helper: sc-bisulfite read layouts vary by
// protocol, so R1 barcode extraction is not implemented (unlike sc-rna).

void apply_sc_bisulfite_auto_config(
    compression_params &cp,
    const AssayDetector::DetectionResult &detection_result);

} // namespace spring

#endif // SPRING_ASSAY_SC_BISULFITE_H_
