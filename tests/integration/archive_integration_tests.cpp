#include "integration_test_support.h"

#include "common/fs_utils.h"
#include "decompress/archive_stream_reader.h"
#include "workflow/archive_preview.h"
#include "workflow/workflow_api.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using namespace spring;
using namespace integration_test_support;

namespace {

std::string sample_asset_path(const std::string &name) {
#ifdef INTEGRATION_TEST_ASSET_DIR
  return (fs::path(INTEGRATION_TEST_ASSET_DIR) / name).generic_string();
#else
  return (fs::path("..") / ".." / "data" / "samples" / name).generic_string();
#endif
}

std::string normalize_newlines(std::string text) {
  std::erase(text, '\r');
  return text;
}

std::string normalize_record_id(std::string text) {
  if (!text.empty() && (text.front() == '@' || text.front() == '>')) {
    text.erase(text.begin());
  }
  return text;
}

void check_legacy_single_stream_fixture(const char *archive_name,
                                        const char *reference_name,
                                        const char *output_name) {
  const std::string test_dir = "legacy_spring_single_asset_decompress_test_tmp";
  fs::create_directories(test_dir);

  const std::string archive_path = sample_asset_path(archive_name);
  const std::string reference_path = sample_asset_path(reference_name);
  const std::string output_path =
      (fs::path(test_dir) / output_name).generic_string();

  INFO("archive=" << archive_path << ", reference=" << reference_path);
  REQUIRE(fs::exists(archive_path));
  REQUIRE(fs::exists(reference_path));

  CHECK_NOTHROW(decompress({archive_path}, {output_path}));

  const std::string restored = read_file_binary(output_path);
  const std::string reference = read_file_binary(reference_path);
  const std::string normalized_restored = normalize_newlines(restored);
  const std::string normalized_reference = normalize_newlines(reference);
  const bool content_ok = (normalized_restored == normalized_reference);
  CHECK_MESSAGE(content_ok, "Normalized decompressed output mismatch for "
                                << archive_name << " vs " << reference_name
                                << " (restored bytes=" << restored.size()
                                << ", reference bytes=" << reference.size()
                                << ", normalized restored bytes="
                                << normalized_restored.size()
                                << ", normalized reference bytes="
                                << normalized_reference.size() << ")");

  fs::remove_all(test_dir);
}

TEST_CASE("Archive Integrity Verification Test") {
  std::string test_dir = "integrity_test_tmp";
  fs::create_directories(test_dir);

  std::string input_fastq = test_dir + "/input.fastq";
  std::string archive_sp = test_dir + "/test.sp";

  create_dummy_fastq(input_fastq, 100);

  std::string compress_cmd = std::string(SPRING2_EXECUTABLE) + " -c -a --R1 " +
                             input_fastq + " -o " + archive_sp + " -t 1";
  run_spring(compress_cmd);

  std::string spring2_path = SPRING2_EXECUTABLE;
  std::string audit_cmd = spring2_path + " -p -a " + archive_sp;
  run_spring(audit_cmd);

  std::string corrupt_dir = test_dir + "/corrupt_work";
  fs::create_directories(corrupt_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_sp + " -C " + corrupt_dir).c_str()) == 0);

  const fs::path cp_path = fs::path(corrupt_dir) / "cp.bin";
  REQUIRE(fs::exists(cp_path));
  const auto cp_size = fs::file_size(cp_path);
  REQUIRE(cp_size > 16);
  fs::resize_file(cp_path, cp_size / 2);

  std::string corrupted_sp = test_dir + "/corrupted.sp";
  std::string retar_cmd =
      "cd " + corrupt_dir + " && tar -cf ../../" + corrupted_sp + " *";
  REQUIRE(std::system(retar_cmd.c_str()) == 0);

  std::string audit_log = test_dir + "/corrupt_audit.log";
  std::string audit_corrupt_cmd =
      spring2_path + " -p -a " + corrupted_sp + " > " + audit_log + " 2>&1";
  int ret = std::system(audit_corrupt_cmd.c_str());

  bool audit_detected_corruption = (ret != 0);
  if (!audit_detected_corruption) {
    std::ifstream ifs(audit_log, std::ios::binary);
    std::string output((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
    audit_detected_corruption =
        output.find("Original Input 1:  input.fastq") == std::string::npos;
  }
  CHECK(audit_detected_corruption);

  fs::remove_all(test_dir);
}

TEST_CASE("Archive extraction rejects absolute paths") {
  const std::string test_dir = "archive_path_escape_test_tmp";
  fs::create_directories(test_dir);

  const std::string archive_path = test_dir + "/malicious.tar";
  const std::string target_dir = test_dir + "/extract";
  const std::string outside_path =
      fs::absolute(fs::path(test_dir) / "outside.txt").generic_string();

  create_tar_with_entry(archive_path, outside_path, "blocked");

  CHECK_THROWS_AS(extract_tar_archive(archive_path, target_dir),
                  std::runtime_error);
  CHECK_FALSE(fs::exists(outside_path));

  fs::remove_all(test_dir);
}

TEST_CASE("Corrupt long-read archive reports a normal decompression error") {
  const std::string test_dir = "long_read_error_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_path = test_dir + "/long_reads.sp";

  const std::string corrupt_dir = test_dir + "/corrupt_extract";
  const std::string corrupted_archive = test_dir + "/corrupted.sp";
  const std::string output_fastq = test_dir + "/restored.fastq";
  const std::string decompress_log = test_dir + "/decompress.log";

  create_custom_fastq(input_fastq, 128, false, false, 700);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + input_fastq + " -o " +
                                   archive_path + " -t 1";
  run_spring(compress_cmd);

  fs::create_directories(corrupt_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + corrupt_dir).c_str()) == 0);

  const fs::path read_length_block =
      fs::path(corrupt_dir) / "readlength_1.0.bsc";
  REQUIRE(fs::exists(read_length_block));
  const auto block_size = fs::file_size(read_length_block);
  REQUIRE(block_size > 8);
  fs::resize_file(read_length_block, block_size / 2);

  fs::remove(fs::path(corrupt_dir) / "_repack_tmp.tar");
  REQUIRE(
      std::system(("cd " + corrupt_dir +
                   " && tar -cf _repack_tmp.tar --exclude _repack_tmp.tar *")
                      .c_str()) == 0);
  fs::rename(fs::path(corrupt_dir) / "_repack_tmp.tar", corrupted_archive);

  const std::string decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + corrupted_archive + " -o " +
      output_fastq + " > " + decompress_log + " 2>&1";
  CHECK(std::system(decompress_cmd.c_str()) != 0);

  const std::string output = read_file_binary(decompress_log);
  CHECK(output.find("Program terminated unexpectedly") != std::string::npos);

  fs::remove_all(test_dir);
}

TEST_CASE("Preview and SpringReader reject truncated metadata") {
  const std::string test_dir = "truncated_metadata_preview_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_path = test_dir + "/test.sp";
  const std::string corrupt_dir = test_dir + "/extract";
  const std::string corrupted_archive = test_dir + "/corrupted.sp";
  const std::string preview_log = test_dir + "/preview.log";

  create_dummy_fastq(input_fastq, 200);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + input_fastq + " -o " +
                                   archive_path + " -t 1";
  run_spring(compress_cmd);

  fs::create_directories(corrupt_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + corrupt_dir).c_str()) == 0);

  const fs::path cp_path = fs::path(corrupt_dir) / "cp.bin";
  REQUIRE(fs::exists(cp_path));
  const auto cp_size = fs::file_size(cp_path);
  REQUIRE(cp_size > 16);
  fs::resize_file(cp_path, cp_size / 2);

  fs::remove(fs::path(corrupt_dir) / "_repack_tmp.tar");
  REQUIRE(
      std::system(("cd " + corrupt_dir +
                   " && tar -cf _repack_tmp.tar --exclude _repack_tmp.tar *")
                      .c_str()) == 0);
  fs::rename(fs::path(corrupt_dir) / "_repack_tmp.tar", corrupted_archive);

  const std::string preview_cmd = std::string(SPRING2_EXECUTABLE) + " -p " +
                                  corrupted_archive + " > " + preview_log +
                                  " 2>&1";
  CHECK(std::system(preview_cmd.c_str()) != 0);
  CHECK_THROWS_AS(SpringReader(corrupted_archive, 1), std::runtime_error);

  fs::remove_all(test_dir);
}

TEST_CASE("Preview identifies legacy Spring archives") {
  const std::string test_dir = "legacy_spring_preview_test_tmp";
  fs::create_directories(test_dir);

  for (const char *archive_name :
       {"sample.spring_v1", "test_1_fastq.spring_v1"}) {
    const std::string archive_path = sample_asset_path(archive_name);
    INFO("archive=" << archive_path);
    REQUIRE(fs::exists(archive_path));

    std::ostringstream preview_output;
    auto *original_buffer = std::cout.rdbuf(preview_output.rdbuf());
    preview(archive_path, false);
    std::cout.rdbuf(original_buffer);

    const std::string output = preview_output.str();
    CHECK(output.find("Archive Version:   legacy spring") != std::string::npos);
    CHECK(output.find(
              "Original Inputs:   Unavailable in legacy spring archives") !=
          std::string::npos);
    CHECK(output.find(
              "Assay Type:        Unavailable in legacy spring archives") !=
          std::string::npos);
  }

  fs::remove_all(test_dir);
}

TEST_CASE("Decompression restores legacy Spring long-read archives") {
  check_legacy_single_stream_fixture("test_1_fastq.spring_v1", "test_1.fastq",
                                     "test_1.fastq");
}

TEST_CASE(
    "Decompression restores checked-in legacy Spring single-stream archives") {
  check_legacy_single_stream_fixture("sample.spring_v1", "sample.fastq",
                                     "sample.fastq");
  check_legacy_single_stream_fixture("test_1_fasta.spring_v1", "test_1.fasta",
                                     "test_1.fasta");
}

TEST_CASE("Decompression restores legacy Spring short-read archives") {
  const std::string test_dir = "legacy_spring_short_decompress_test_tmp";
  fs::create_directories(test_dir);

  const std::string paired_archive = sample_asset_path("test_3.spring_v1");
  const std::string output_r1 = test_dir + "/restored_R1.fastq";
  const std::string output_r2 = test_dir + "/restored_R2.fastq";
  const std::string reference_r1 = sample_asset_path("test_3_R1.fastq.gz");
  const std::string reference_r2 = sample_asset_path("test_3_R2.fastq.gz");
  REQUIRE(fs::exists(paired_archive));
  REQUIRE(fs::exists(reference_r1));
  REQUIRE(fs::exists(reference_r2));

  CHECK_NOTHROW(decompress({paired_archive}, {output_r1, output_r2}));

  check_bytes_equal(read_file_binary(output_r1),
                    read_gzip_file_binary(reference_r1), "R1 round-trip");
  check_bytes_equal(read_file_binary(output_r2),
                    read_gzip_file_binary(reference_r2), "R2 round-trip");

  fs::remove_all(test_dir);
}

TEST_CASE("SpringReader streams legacy Spring archives") {
  const std::string archive_path = sample_asset_path("test_1_fastq.spring_v1");
  REQUIRE(fs::exists(archive_path));

  SpringReader reader(archive_path, 1);

  ReadRecord record;
  int record_count = 0;
  std::string first_id;
  while (reader.next(record)) {
    const std::string normalized_id = normalize_record_id(record.id);
    if (normalized_id.empty() || record.sequence.empty()) {
      continue;
    }
    CHECK(record.quality.size() == record.sequence.size());
    if (record_count == 0) {
      first_id = normalized_id;
    }
    ++record_count;
  }

  CHECK(record_count >= 98);
  CHECK(first_id == "SRR554369.1 1/1");
}

TEST_CASE("Decompression rejects colliding output paths") {
  const std::string test_dir = "decompress_output_collision_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string paired_archive = test_dir + "/paired.sp";
  const std::string single_archive = test_dir + "/single.sp";
  const std::string duplicate_output = test_dir + "/duplicate.fastq";
  const std::string duplicate_log = test_dir + "/duplicate.log";
  const std::string overwrite_log = test_dir + "/overwrite.log";

  create_dummy_fastq(r1_fastq, 180);
  create_dummy_fastq(r2_fastq, 180);

  run_spring(std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq +
             " --R2 " + r2_fastq + " -o " + paired_archive + " -t 1");
  run_spring(std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " -o " +
             single_archive + " -t 1");

  const std::string duplicate_cmd = std::string(SPRING2_EXECUTABLE) +
                                    " -d -i " + paired_archive + " -o " +
                                    duplicate_output + " " + duplicate_output +
                                    " -t 1 > " + duplicate_log + " 2>&1";
  const std::string overwrite_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + single_archive + " -o " +
      single_archive + " -t 1 > " + overwrite_log + " 2>&1";

  CHECK(std::system(duplicate_cmd.c_str()) != 0);
  CHECK(std::system(overwrite_cmd.c_str()) != 0);

  fs::remove_all(test_dir);
}

TEST_CASE("Compression rejects archive paths that overwrite inputs") {
  const std::string test_dir = "compression_output_collision_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string collision_log = test_dir + "/collision.log";

  create_dummy_fastq(input_fastq, 160);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + input_fastq + " -o " +
      input_fastq + " -t 1 > " + collision_log + " 2>&1";
  CHECK(std::system(compress_cmd.c_str()) != 0);

  const std::string log_output = read_file_binary(collision_log);
  CHECK(log_output.find("must not overwrite an input file") !=
        std::string::npos);

  fs::remove_all(test_dir);
}

} // namespace