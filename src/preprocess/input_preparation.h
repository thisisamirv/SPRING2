// Declares the preprocessing stage that converts raw input reads into the
// temporary files consumed by Spring's reorder and encode passes.

#ifndef SPRING_INPUT_PREPARATION_H_
#define SPRING_INPUT_PREPARATION_H_

#include "params.h"
#include "quality_id_reordering.h"
#include "read_reordering.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace spring {

class ProgressBar;
struct compression_params;

struct input_detection_summary {
  uint32_t max_read_length = 0;
  std::array<bool, 2> use_crlf_by_stream = {false, false};
  bool contains_non_acgtn_symbols = false;
  uint64_t sampled_fragments = 0;

  [[nodiscard]] bool use_crlf() const {
    return use_crlf_by_stream[0] || use_crlf_by_stream[1];
  }

  [[nodiscard]] bool requires_long_mode() const {
    return max_read_length > MAX_READ_LEN || contains_non_acgtn_symbols;
  }
};

class preprocess_retry_exception : public std::runtime_error {
public:
  explicit preprocess_retry_exception(const input_detection_summary &summary);

  [[nodiscard]] const input_detection_summary &updated_summary() const {
    return updated_summary_;
  }

private:
  input_detection_summary updated_summary_;
};

struct preprocess_artifact {
  reorder_input_artifact reorder_inputs;
  post_encode_side_stream_artifact post_encode_side_streams;
  std::unordered_map<std::string, std::string> archive_members;
};

// Normalize input reads into Spring's temporary block files and side streams.
preprocess_artifact
preprocess(const std::string &infile_1, const std::string &infile_2,
           compression_params &cp, const bool &fasta_input,
           ProgressBar *progress = nullptr,
           const input_detection_summary *expected_summary = nullptr);

input_detection_summary detect_input_properties(const std::string &infile_1,
                                                const std::string &infile_2,
                                                bool paired_end,
                                                bool fasta_input);

} // namespace spring

#endif // SPRING_INPUT_PREPARATION_H_
