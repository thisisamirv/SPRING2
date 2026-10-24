// Declares ATAC-specific preprocessing and reconstruction helpers such as
// terminal adapter stripping and restoration.

#ifndef SPRING_ASSAY_ATAC_H_
#define SPRING_ASSAY_ATAC_H_

#include "assay_detector.h"
#include "params.h"

#include <cstdint>
#include <string>

namespace spring {

[[nodiscard]] bool
grouped_archive_allows_atac_adapter_stripping(const std::string &archive_note);

[[nodiscard]] AssayScoreBreakdown
score_atac_assay(const AssayDetectionStats &stats, bool reference_loaded);

[[nodiscard]] bool has_atac_adapter(const std::string &seq);

[[nodiscard]] bool
should_consider_atac_adapter_stripping(const compression_params &cp,
                                       bool fasta_input,
                                       bool grouped_archive_allowed);

[[nodiscard]] bool
should_activate_atac_adapter_stripping(uint32_t eligible_reads,
                                       uint64_t stripped_bases,
                                       double min_bases_per_read = 1.0);

// Minimum non-N reads to sample before locking enable/disable, so a single
// early preprocess step (or an all-N first step) cannot permanently miss or
// falsely enable terminal Tn5 stripping.
inline constexpr uint32_t kAtacAdapterStripMinSampleReads = 1024;

struct AtacAdapterStripSample {
  uint32_t eligible_reads = 0;
  uint64_t stripped_bases = 0;
  bool decided = false;
  bool active = false;
};

// Accumulate one step's detection totals. Decides once eligible_reads reaches
// min_sample_reads, or immediately when end_of_input is true. Returns true
// when a decision is newly made.
[[nodiscard]] bool update_atac_adapter_strip_sample(
    AtacAdapterStripSample &sample, uint32_t step_eligible_reads,
    uint64_t step_stripped_bases, bool end_of_input,
    uint32_t min_sample_reads = kAtacAdapterStripMinSampleReads,
    double min_bases_per_read = 1.0);

uint8_t detect_atac_adapter_tail_info(const std::string &read_str,
                                      uint32_t read_length);

void strip_atac_adapter_tail(std::string &read_str, uint32_t &read_length,
                             uint8_t adapter_info);

void restore_atac_adapter_tail(std::string &read_str, std::string *quality_str,
                               uint8_t adapter_info,
                               const std::string *tail_qual);

} // namespace spring

#endif // SPRING_ASSAY_ATAC_H_
