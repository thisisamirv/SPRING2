// Declares sc-ATAC-specific preprocessing and reconstruction helpers so the
// single-cell ATAC path can be tracked independently from bulk ATAC.

#ifndef SPRING_ASSAY_SC_ATAC_H_
#define SPRING_ASSAY_SC_ATAC_H_

#include "params.h"

#include <cstdint>
#include <string>

namespace spring {

[[nodiscard]] bool
should_consider_sc_atac_adapter_stripping(const compression_params &cp,
                                          bool fasta_input);

[[nodiscard]] bool
should_activate_sc_atac_adapter_stripping(uint32_t eligible_reads,
                                          uint64_t stripped_bases,
                                          double min_bases_per_read = 1.0);

uint8_t detect_sc_atac_adapter_tail_info(const std::string &read_str,
                                         uint32_t read_length);

void strip_sc_atac_adapter_tail(std::string &read_str, uint32_t &read_length,
                                uint8_t adapter_info);

void restore_sc_atac_adapter_tail(std::string &read_str,
                                  std::string *quality_str,
                                  uint8_t adapter_info,
                                  const std::string *tail_qual);

} // namespace spring

#endif // SPRING_ASSAY_SC_ATAC_H_
