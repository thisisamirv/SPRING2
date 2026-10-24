// Declares Spring's CLI-facing compression and decompression entry points and
// the shared string/range aliases used to drive those workflows.

#ifndef SPRING_SPRING_H_
#define SPRING_SPRING_H_

#include <cstdint>
#include <string>
#include <vector>

#include "params.h"
#include "progress.h"

namespace spring {

using string_list = std::vector<std::string>;
using read_range = std::vector<uint64_t>;

void preview(const std::string &archive_path, bool audit_only);

// Named options for compress(). Prefer setting fields by name (or C++20
// designated initializers) so strip/audit/assay arguments cannot be swapped.
struct CompressOptions {
  string_list input_paths;
  string_list output_paths;
  int num_threads = 1;
  bool strip_order = false;
  bool strip_quality = false;
  bool strip_ids = false;
  string_list quality_options;
  int compression_level = DEFAULT_COMPRESSION_LEVEL;
  std::string note;
  log_level verbosity = log_level::info;
  bool audit = false;
  std::string r3_path;
  std::string i1_path;
  std::string i2_path;
  std::string assay = "auto";
  std::string cb_source_path;
  uint32_t cb_len = 16;
  bool cb_len_was_explicit = false;
  double memory_cap_gb = 0.0;
};

void compress(const CompressOptions &options);

void decompress(const string_list &input_paths, const string_list &output_paths,
                const int num_thr = 1,
                const log_level verbosity_level = log_level::info,
                const bool unzip_flag = false);

void perform_audit(const std::string &archive_path);

} // namespace spring

#endif // SPRING_SPRING_H_
