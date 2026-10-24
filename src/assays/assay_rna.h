// Declares RNA-specific preprocessing and reconstruction helpers such as
// poly-A/T tail stripping and restoration.

#ifndef SPRING_ASSAY_RNA_H_
#define SPRING_ASSAY_RNA_H_

#include "assay_detector.h"
#include "params.h"

#include <cstdint>
#include <string>

namespace spring {

[[nodiscard]] bool
should_enable_rna_tail_stripping(const compression_params &cp);

[[nodiscard]] AssayScoreBreakdown
score_rna_assay(const AssayDetectionStats &stats, bool reference_loaded,
                bool suppress_poly_a_signal);

// True when the read ends with a contiguous A or T run longer than
// POLY_AT_TAIL_MIN_LEN (same rule as terminal poly-A/T stripping).
[[nodiscard]] bool has_poly_a_tail(const std::string &seq);

// Returns packed tail info (run << 1 | is_T) without mutating the read, or 0.
[[nodiscard]] uint16_t detect_rna_tail_info(const std::string &read_str,
                                            uint32_t read_length);

uint16_t detect_and_strip_rna_tail(std::string &read_str,
                                   uint32_t &read_length);

void restore_rna_tail(std::string &read_str, std::string *quality_str,
                      uint16_t tail_info, const std::string *tail_qual);

} // namespace spring

#endif // SPRING_ASSAY_RNA_H_
