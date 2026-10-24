#include "smoke_test_support.h"

using namespace smoke_test_support;

namespace {

void compress_twice_expect_identical(
    const std::vector<std::string> &compress_args, const std::string &archive_a,
    const std::string &archive_b) {
  auto args_a = compress_args;
  args_a.insert(args_a.end(), {"-o", archive_a});
  auto args_b = compress_args;
  args_b.insert(args_b.end(), {"-o", archive_b});
  run_spring(args_a);
  run_spring(args_b);
  expect_files_identical(archive_a, archive_b);
}

TEST_CASE("Smoke single-thread compress is bit-identical (paired bisulfite)") {
  // test_3 exercises the short-read reorder/encode path where OpenMP
  // try-lock races otherwise produce divergent archives under -t > 1.
  SmokeWorkspace workspace("determinism_paired_t1");
  compress_twice_expect_identical(
      {"-c", "--R1", asset_path("test_3_R1.fastq.gz"), "--R2",
       asset_path("test_3_R2.fastq.gz"), "-t", "1"},
      workspace.path("a.sp"), workspace.path("b.sp"));
}

TEST_CASE("Smoke single-thread compress is bit-identical (single-end)") {
  SmokeWorkspace workspace("determinism_single_t1");
  compress_twice_expect_identical(
      {"-c", "--R1", asset_path("test_1.fastq"), "-t", "1"},
      workspace.path("a.sp"), workspace.path("b.sp"));
}

TEST_CASE("Smoke deterministic-reorder multi-thread compress is bit-identical") {
  ScopedEnvVar deterministic("SPRING_DETERMINISTIC_REORDER", "1");
  SmokeWorkspace workspace("determinism_paired_det_mt");
  const std::string threads = std::to_string(smoke_compress_threads());
  compress_twice_expect_identical(
      {"-c", "--R1", asset_path("test_3_R1.fastq.gz"), "--R2",
       asset_path("test_3_R2.fastq.gz"), "-t", threads},
      workspace.path("a.sp"), workspace.path("b.sp"));
}

TEST_CASE(
    "Smoke deterministic-reorder multi-thread round-trips like single-thread") {
  // Archive packaging still carries the requested thread count (shard layout),
  // so DET+MT bytes need not match -t 1. Decompressed mates must match.
  SmokeWorkspace workspace("determinism_det_mt_roundtrip_t1");
  const auto archive_t1 = workspace.path("t1.sp");
  const auto archive_det = workspace.path("det.sp");
  const auto out_t1 = workspace.path("t1_out");
  const auto out_det = workspace.path("det_out");
  const std::string threads = std::to_string(smoke_compress_threads());

  run_spring({"-c", "--R1", asset_path("test_3_R1.fastq.gz"), "--R2",
              asset_path("test_3_R2.fastq.gz"), "-o", archive_t1, "-t", "1"});
  {
    ScopedEnvVar deterministic("SPRING_DETERMINISTIC_REORDER", "1");
    run_spring({"-c", "--R1", asset_path("test_3_R1.fastq.gz"), "--R2",
                asset_path("test_3_R2.fastq.gz"), "-o", archive_det, "-t",
                threads});
  }

  run_spring({"-d", "-i", archive_t1, "-o", out_t1, "-u"});
  run_spring({"-d", "-i", archive_det, "-o", out_det, "-u"});
  expect_files_identical(workspace.path("t1_out.R1"),
                         workspace.path("det_out.R1"));
  expect_files_identical(workspace.path("t1_out.R2"),
                         workspace.path("det_out.R2"));
}

} // namespace
