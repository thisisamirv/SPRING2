// Declares Spring's CLI-facing compression and decompression entry points and
// the shared string/range aliases used to drive those workflows.

#ifndef SPRING_SPRING_H_
#define SPRING_SPRING_H_

#include <cstdint>
#include <string>
#include <vector>

#include "progress.h"

namespace spring {

using string_list = std::vector<std::string>;
using read_range = std::vector<uint64_t>;

void preview(const std::string &archive_path, bool audit_only);

// Top-level compression and decompression entry points used by the CLI.
void compress(const string_list &input_paths, const string_list &output_paths,
              const int num_thr, const bool pairing_only_flag,
              const bool no_quality_flag, const bool no_ids_flag,
              const string_list &quality_options, const int compression_level,
              const std::string &note,
              const log_level verbosity_level = log_level::info,
              const bool audit_flag = false,
              const std::string &r3_path = std::string(),
              const std::string &i1_path = std::string(),
              const std::string &i2_path = std::string(),
              const std::string &assay_type = "auto",
              const std::string &cb_source_path = std::string(),
              uint32_t cb_len = 16, double memory_cap_gb = 0.0);

void decompress(const string_list &input_paths, const string_list &output_paths,
                const log_level verbosity_level = log_level::info,
                const bool unzip_flag = false);

void perform_audit(const std::string &archive_path);

} // namespace spring

#endif // SPRING_SPRING_H_
