#include "smoke_test_support.h"

#include <cstdio>

using namespace smoke_test_support;

namespace {

TEST_CASE("Smoke plain fastq decompresses to gzip output") {
  SmokeWorkspace workspace("plain_fastq_to_gzip_output");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq.gz");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output});
  expect_gzip_output_matches(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke paired fastq gzipped input round-trip") {
  SmokeWorkspace workspace("paired_fastq_gzip_input_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq.gz"), "--R2",
              asset_path("test_2.fastq.gz"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output_prefix, "-u"});
  expect_text_match(workspace.path("tmp.R1"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.R2"), asset_path("test_2.fastq"));
}

TEST_CASE("Smoke paired fasta gzipped input round-trip") {
  SmokeWorkspace workspace("paired_fasta_gzip_input_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fasta.gz"), "--R2",
              asset_path("test_2.fasta.gz"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output_prefix, "-u"});
  expect_text_match(workspace.path("tmp.R1"), asset_path("test_1.fasta"));
  expect_text_match(workspace.path("tmp.R2"), asset_path("test_2.fasta"));
}

TEST_CASE("Smoke single gzipped fastq round-trip") {
  SmokeWorkspace workspace("single_fastq_gzip_input_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq");

  run_spring({"-c", "--R1", asset_path("test_1.fastq.gz"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output, "-u"});
  expect_text_match(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke single gzipped fastq input to gzipped output") {
  SmokeWorkspace workspace("single_fastq_gzip_to_gzip_output");
  const auto archive = workspace.path("archive.sp");
  const auto output = workspace.path("tmp.fastq.gz");

  run_spring({"-c", "--R1", asset_path("test_1.fastq.gz"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output});
  expect_gzip_output_matches(output, asset_path("test_1.fastq"));
}

TEST_CASE("Smoke paired gzipped fastq input to gzipped outputs") {
  SmokeWorkspace workspace("paired_fastq_gzip_to_gzip_outputs");
  const auto archive = workspace.path("archive.sp");
  const auto output_1 = workspace.path("tmp.R1.fastq.gz");
  const auto output_2 = workspace.path("tmp.R2.fastq.gz");

  run_spring({"-c", "--R1", asset_path("test_1.fastq.gz"), "--R2",
              asset_path("test_2.fastq.gz"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output_1, output_2});
  expect_gzip_output_matches(output_1, asset_path("test_1.fastq"));
  expect_gzip_output_matches(output_2, asset_path("test_2.fastq"));
}

TEST_CASE("Smoke paired gzipped input restores .gz on generated prefix names") {
  SmokeWorkspace workspace("paired_gzip_prefix_restores_gz");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq.gz"), "--R2",
              asset_path("test_2.fastq.gz"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output_prefix});
  expect_gzip_output_matches(workspace.path("tmp.R1.gz"),
                             asset_path("test_1.fastq"));
  expect_gzip_output_matches(workspace.path("tmp.R2.gz"),
                             asset_path("test_2.fastq"));
}

TEST_CASE("Smoke single gzipped input restores default basename with .gz") {
  SmokeWorkspace workspace("single_gzip_default_basename_gz");
  const auto archive = workspace.path("archive.sp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq.gz"), "-o", archive});
  run_spring({"-d", "-i", archive});
  // Stored input basename is test_1.fastq.gz; with no -o the restore lands in
  // the process cwd under that name and is written gzipped.
  const auto restored = "test_1.fastq.gz";
  expect_gzip_output_matches(restored, asset_path("test_1.fastq"));
  std::remove(restored);
}

} // namespace