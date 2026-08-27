// Implements single-cell RNA-specific helpers used for barcode-prefix handling
// and grouped index identifier reconstruction.

#include "assay_sc_rna.h"

#include <stdexcept>
#include <string_view>

namespace spring {

bool is_grouped_index_archive_note(const std::string &note) {
  return note.find("index-group") != std::string::npos;
}

bool is_grouped_read3_archive_note(const std::string &note) {
  return note.find("read3-group") != std::string::npos;
}

bool should_enable_sc_rna_cb_prefix_stripping(const compression_params &cp) {
  return !cp.encoding.long_flag && cp.encoding.preserve_order &&
         !cp.encoding.cb_prefix_source_external && cp.encoding.cb_len > 0 &&
         cp.read_info.assay == "sc-rna" &&
         !is_grouped_index_archive_note(cp.read_info.note);
}

bool should_enable_grouped_sc_rna_index_suffix_stripping(
    const compression_params &cp) {
  return !cp.encoding.long_flag && cp.encoding.preserve_order &&
         cp.encoding.preserve_id && cp.read_info.assay == "sc-rna" &&
         is_grouped_index_archive_note(cp.read_info.note);
}

bool strip_grouped_sc_rna_index_suffix_from_id(std::string &id,
                                               const std::string &stream_1_seq,
                                               bool paired_index) {
  const size_t last_colon = id.rfind(':');
  if (last_colon == std::string::npos || last_colon + 1 >= id.size()) {
    return false;
  }

  std::string_view suffix{id};
  suffix.remove_prefix(last_colon + 1);
  if (paired_index) {
    const size_t plus = suffix.find('+');
    if (plus == std::string::npos || plus == 0) {
      return false;
    }
    if (suffix.substr(0, plus) != stream_1_seq) {
      return false;
    }
  } else if (suffix != stream_1_seq) {
    return false;
  }

  id.resize(last_colon + 1);
  return true;
}

void append_grouped_sc_rna_index_suffix_to_id(std::string &id,
                                              const std::string &index_read_1,
                                              const std::string *index_read_2) {
  if (id.empty() || id.back() != ':') {
    throw std::runtime_error("Corrupt grouped sc-RNA index metadata: stripped "
                             "ID prefix missing ':'");
  }

  id.append(index_read_1);
  if (index_read_2) {
    id.push_back('+');
    id.append(*index_read_2);
  }
}

} // namespace spring
