#include "smoke_test_support.h"

using namespace smoke_test_support;

namespace {

TEST_CASE("Smoke fastq round-trip") {
  SmokeWorkspace workspace("fastq_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output});
  expect_text_match(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke fasta round-trip") {
  SmokeWorkspace workspace("fasta_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fasta");

  run_spring({"-c", "--R1", asset_path("test_1.fasta"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output});
  expect_text_match(output, asset_path("test_1.fasta"));
}

TEST_CASE("Smoke paired fastq round-trip") {
  SmokeWorkspace workspace("paired_fastq_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "--R2",
              asset_path("test_2.fastq"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output_prefix});
  expect_text_match(workspace.path("tmp.R1"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.R2"), asset_path("test_2.fastq"));
}

TEST_CASE("Smoke paired fasta round-trip") {
  SmokeWorkspace workspace("paired_fasta_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fasta"), "--R2",
              asset_path("test_2.fasta"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output_prefix});
  expect_text_match(workspace.path("tmp.R1"), asset_path("test_1.fasta"));
  expect_text_match(workspace.path("tmp.R2"), asset_path("test_2.fasta"));
}

TEST_CASE("Smoke long-read round-trip") {
  SmokeWorkspace workspace("long_read_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring({"-c", "--R1", asset_path("sample.fastq"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output});
  expect_text_match(output, asset_path("sample.fastq"));
}

} // namespace