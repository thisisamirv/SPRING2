// Declares single-cell RNA-specific helpers used for barcode-prefix handling
// and grouped index identifier reconstruction.

#ifndef SPRING_ASSAY_SC_RNA_H_
#define SPRING_ASSAY_SC_RNA_H_

#include "params.h"

#include <string>

namespace spring {

[[nodiscard]] bool is_grouped_index_archive_note(const std::string &note);
[[nodiscard]] bool is_grouped_read3_archive_note(const std::string &note);

[[nodiscard]] bool
should_enable_sc_rna_cb_prefix_stripping(const compression_params &cp);

[[nodiscard]] bool should_enable_grouped_sc_rna_index_suffix_stripping(
    const compression_params &cp);

bool strip_grouped_sc_rna_index_suffix_from_id(std::string &id,
                                               const std::string &stream_1_seq,
                                               bool paired_index);

void append_grouped_sc_rna_index_suffix_to_id(std::string &id,
                                              const std::string &index_read_1,
                                              const std::string *index_read_2);

} // namespace spring

#endif // SPRING_ASSAY_SC_RNA_H_
