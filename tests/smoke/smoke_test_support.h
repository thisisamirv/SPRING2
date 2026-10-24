#pragma once

#include "../support/doctest.h"

#include <string>
#include <vector>

#ifndef SPRING2_EXECUTABLE
#define SPRING2_EXECUTABLE ""
#endif

#ifndef SMOKE_TEST_ASSET_DIR
#define SMOKE_TEST_ASSET_DIR ""
#endif

#ifndef SMOKE_TEST_OUTPUT_ROOT
#define SMOKE_TEST_OUTPUT_ROOT ""
#endif

namespace smoke_test_support {

struct SmokeWorkspace {
  explicit SmokeWorkspace(const std::string &name);
  ~SmokeWorkspace();

  std::string path(const std::string &name) const;

  std::string dir;
};

std::string asset_path(const std::string &name);
void run_spring(const std::vector<std::string> &args);
std::string run_preview_capture(const std::string &archive_path);

void expect_text_match(const std::string &actual_path,
                       const std::string &expected_path);
void expect_gzip_output_matches(const std::string &actual_gzip_path,
                                const std::string &expected_text_path);
void expect_line_count_match(const std::string &actual_path,
                             const std::string &expected_path);
void expect_sorted_match(const std::string &actual_path,
                         const std::string &expected_path);
void expect_files_identical(const std::string &left_path,
                            const std::string &right_path);

// Temporarily set or clear an environment variable; restores prior value on
// destruction (including when the variable was previously unset).
struct ScopedEnvVar {
  ScopedEnvVar(const char *name, const char *value);
  ~ScopedEnvVar();

  ScopedEnvVar(const ScopedEnvVar &) = delete;
  ScopedEnvVar &operator=(const ScopedEnvVar &) = delete;

  const char *name;
  bool had_prior;
  std::string prior;
};

unsigned smoke_compress_threads();
unsigned smoke_decompress_threads();

} // namespace smoke_test_support