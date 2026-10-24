#include "smoke_test_support.h"

using namespace smoke_test_support;

namespace {

TEST_CASE("Smoke multi-threaded single-end round-trip") {
  SmokeWorkspace workspace("threaded_single_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "-o", archive, "-t",
              std::to_string(smoke_compress_threads())});
  run_spring({"-d", "-i", archive, "-o", output, "-t",
              std::to_string(smoke_decompress_threads())});
  expect_text_match(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke multi-threaded paired-end round-trip") {
  SmokeWorkspace workspace("threaded_paired_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "--R2",
              asset_path("test_2.fastq"), "-o", archive, "-t",
              std::to_string(smoke_compress_threads())});
  run_spring({"-d", "-i", archive, "-o", output_prefix, "-t",
              std::to_string(smoke_decompress_threads())});
  expect_text_match(workspace.path("tmp.1"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.2"), asset_path("test_2.fastq"));
}

TEST_CASE("Smoke sorted output single-end round-trip") {
  SmokeWorkspace workspace("sorted_single_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring(
      {"-c", "--R1", asset_path("test_1.fastq"), "-o", archive, "-s", "o"});
  run_spring({"-d", "-i", archive, "-o", output});
  expect_sorted_match(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke sorted output paired-end round-trip") {
  SmokeWorkspace workspace("sorted_paired_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "--R2",
              asset_path("test_2.fastq"), "-o", archive, "-s", "o", "-t",
              std::to_string(smoke_compress_threads())});
  run_spring({"-d", "-i", archive, "-o", output_prefix, "-t",
              std::to_string(smoke_decompress_threads())});
  expect_sorted_match(workspace.path("tmp.1"), asset_path("test_1.fastq"));
  expect_sorted_match(workspace.path("tmp.2"), asset_path("test_2.fastq"));
}

TEST_CASE("Smoke memory capping round-trip") {
  SmokeWorkspace workspace("memory_capping_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring(
      {"-m", "0.1", "-c", "--R1", asset_path("test_1.fastq"), "-o", archive});
  run_spring({"-m", "0.1", "-d", "-i", archive, "-o", output});
  expect_text_match(output, asset_path("test_1.fastq"));
}

TEST_CASE(
    "Smoke disk-path paired round-trip (external MPHF / thread capping)") {
  // Forces disk-backed compression path via -m 0.1 on paired-end input,
  // exercising the external-MPHF / thread-capping decision block for both R1
  // and R2 streams.
  SmokeWorkspace workspace("disk_path_paired_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-m", "0.1", "-c", "--R1", asset_path("test_1.fastq"), "--R2",
              asset_path("test_2.fastq"), "-o", archive});
  run_spring({"-m", "0.1", "-d", "-i", archive, "-o", output_prefix});
  expect_text_match(workspace.path("tmp.1"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.2"), asset_path("test_2.fastq"));
}

TEST_CASE("Smoke disk-path quality/ID reorder round-trip (order-stripped "
          "streaming)") {
  // Forces disk-backed compression with -m 0.1 and strips read order (-s o)
  // so that quality and ID streams must be reordered via the disk-backed
  // streaming path (reorder_compress_quality_id from file).
  SmokeWorkspace workspace("disk_path_sorted_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring({"-m", "0.1", "-c", "--R1", asset_path("test_1.fastq"), "-s", "o",
              "-o", archive});
  run_spring({"-m", "0.1", "-d", "-i", archive, "-o", output});
  expect_sorted_match(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke preview preserves archive notes and filenames") {
  SmokeWorkspace workspace("preview_validation");
  const auto archive = workspace.path("archive.sp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "-n", "SMOKE_TEST_NOTE",
              "-o", archive});
  const std::string preview = run_preview_capture(archive);
  CHECK(preview.find("SMOKE_TEST_NOTE") != std::string::npos);
  CHECK(preview.find("test_1.fastq") != std::string::npos);
}

TEST_CASE("Smoke lossy ill_bin preserves record count") {
  SmokeWorkspace workspace("lossy_ill_bin");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring({"-c", "-q", "ill_bin", "--R1", asset_path("test_1.fastq"), "-o",
              archive});
  run_spring({"-d", "-i", archive, "-o", output});
  expect_line_count_match(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke lossy binary preserves record count") {
  SmokeWorkspace workspace("lossy_binary");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring({"-c", "-q", "binary", "30", "40", "10", "--R1",
              asset_path("test_1.fastq"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output});
  expect_line_count_match(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke stripping ids preserves record count") {
  SmokeWorkspace workspace("strip_ids");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring(
      {"-c", "-s", "i", "--R1", asset_path("test_1.fastq"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output});
  expect_line_count_match(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke stripping quality preserves record count") {
  SmokeWorkspace workspace("strip_quality");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring(
      {"-c", "-s", "q", "--R1", asset_path("test_1.fastq"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output});
  expect_line_count_match(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke decompress with --audit runs audit then restores") {
  SmokeWorkspace workspace("decompress_with_audit");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "-o", archive});
  run_spring({"-d", "-a", "-i", archive, "-o", output});
  expect_text_match(output, asset_path("test_1.fastq"));
}

} // namespace