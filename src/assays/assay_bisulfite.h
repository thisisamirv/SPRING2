// Declares bisulfite-specific policy helpers so ternary handling is tracked in
// a dedicated assay file.

#ifndef SPRING_ASSAY_BISULFITE_H_
#define SPRING_ASSAY_BISULFITE_H_

#include "assay_detector.h"
#include "params.h"

namespace spring {

[[nodiscard]] BisulfiteDetectionScore
score_bisulfite_assay(const AssayDetectionStats &stats);

[[nodiscard]] bool is_bisulfite_assay(const compression_params &cp);

// Shared by bulk and sc-bisulfite auto-config: arm depleted-base reorder
// masks and ternary packing only for a uniform C- or G-depleted alphabet.
void apply_bisulfite_encoding_from_detection(
    compression_params &cp,
    const AssayDetector::DetectionResult &detection_result);

void apply_bisulfite_auto_config(
    compression_params &cp,
    const AssayDetector::DetectionResult &detection_result);

} // namespace spring

#endif // SPRING_ASSAY_BISULFITE_H_
