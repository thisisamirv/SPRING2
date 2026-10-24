#include "integration_test_support.h"

#include "common/io_utils.h"
#include "decompress/archive_stream_reader.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace spring;
using namespace integration_test_support;

namespace {

TEST_CASE(
    "Grouped preview audit detects corruption and preserves archive note") {
  const std::string test_dir = "grouped_preview_audit_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string r3_fastq = test_dir + "/input_R3.fastq";
  const std::string i1_fastq = test_dir + "/input_I1.fastq";
  const std::string archive_path = test_dir + "/grouped_note.sp";
  const std::string preview_log = test_dir + "/preview.log";
  const std::string corrupt_dir = test_dir + "/corrupt_extract";
  const std::string nested_read_dir = test_dir + "/nested_read_extract";
  const std::string corrupt_log = test_dir + "/corrupt_audit.log";
  const std::string corrupt_preview_log = test_dir + "/corrupt_preview.log";
  const fs::path corrupted_archive_path =
      fs::absolute(fs::path(test_dir) / "grouped_corrupted.sp");

  create_dummy_fastq(r1_fastq, 500);
  create_dummy_fastq(r2_fastq, 500);
  create_dummy_fastq(r3_fastq, 500);
  create_dummy_fastq(i1_fastq, 500);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r3_fastq + " --I1 " + i1_fastq +
      " -n GROUPED_TEST_NOTE -o " + archive_path + " -t 1 -y auto";
  const std::string preview_cmd = std::string(SPRING2_EXECUTABLE) + " -p " +
                                  archive_path + " > " + preview_log + " 2>&1";
  const std::string audit_cmd =
      std::string(SPRING2_EXECUTABLE) + " -p -a " + archive_path;

  run_spring(compress_cmd);
  REQUIRE(std::system(preview_cmd.c_str()) == 0);
  run_spring(audit_cmd);

  {
    std::ifstream preview_in(preview_log, std::ios::binary);
    REQUIRE(preview_in.is_open());
    const std::string preview_output(
        (std::istreambuf_iterator<char>(preview_in)),
        std::istreambuf_iterator<char>());
    CHECK(preview_output.find("Note:              GROUPED_TEST_NOTE") !=
          std::string::npos);
  }

  fs::create_directories(corrupt_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + corrupt_dir).c_str()) == 0);

  fs::create_directories(nested_read_dir);
  const std::string read_archive_name =
      read_manifest_value(corrupt_dir + "/bundle.meta", "read_archive");
  REQUIRE(!read_archive_name.empty());
  const fs::path read_archive = fs::path(corrupt_dir) / read_archive_name;
  const std::string corrupted_archive_tar_path =
      corrupted_archive_path.generic_string();
  REQUIRE(fs::exists(read_archive));
  REQUIRE(std::system(
              ("tar -xf " + read_archive.string() + " -C " + nested_read_dir)
                  .c_str()) == 0);

  const fs::path cp_path = fs::path(nested_read_dir) / "cp.bin";
  REQUIRE(fs::exists(cp_path));
  const auto cp_size = fs::file_size(cp_path);
  REQUIRE(cp_size > 16);
  fs::resize_file(cp_path, cp_size / 2);

  fs::remove(fs::path(nested_read_dir) / "_repack_tmp.tar");
  REQUIRE(
      std::system(("cd " + nested_read_dir +
                   " && tar -cf _repack_tmp.tar --exclude _repack_tmp.tar *")
                      .c_str()) == 0);
  fs::rename(fs::path(nested_read_dir) / "_repack_tmp.tar", read_archive);
  fs::remove(fs::path(corrupt_dir) / "_repack_tmp.tar");
  REQUIRE(
      std::system(("cd " + corrupt_dir +
                   " && tar -cf _repack_tmp.tar --exclude _repack_tmp.tar *")
                      .c_str()) == 0);
  fs::rename(fs::path(corrupt_dir) / "_repack_tmp.tar", corrupted_archive_path);

  const std::string corrupt_audit_cmd = std::string(SPRING2_EXECUTABLE) +
                                        " -p -a " + corrupted_archive_tar_path +
                                        " > " + corrupt_log + " 2>&1";
  const std::string corrupt_preview_cmd = std::string(SPRING2_EXECUTABLE) +
                                          " -p " + corrupted_archive_tar_path +
                                          " > " + corrupt_preview_log + " 2>&1";
  CHECK(std::system(corrupt_audit_cmd.c_str()) != 0);
  CHECK(std::system(corrupt_preview_cmd.c_str()) != 0);

  fs::remove_all(test_dir);
}

TEST_CASE("Grouped decompression accepts five explicit output files") {
  const std::string test_dir = "grouped_five_output_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string r3_fastq = test_dir + "/input_R3.fastq";
  const std::string i1_fastq = test_dir + "/input_I1.fastq";
  const std::string i2_fastq = test_dir + "/input_I2.fastq";
  const std::string archive_path = test_dir + "/grouped_five_out.sp";
  const std::string out_r1 = test_dir + "/out_R1.fastq";
  const std::string out_r2 = test_dir + "/out_R2.fastq";
  const std::string out_r3 = test_dir + "/out_R3.fastq";
  const std::string out_i1 = test_dir + "/out_I1.fastq";
  const std::string out_i2 = test_dir + "/out_I2.fastq";

  create_grouped_sc_rna_like_fastqs(r1_fastq, r2_fastq, i1_fastq, i2_fastq,
                                    1000);
  create_dummy_fastq(r3_fastq, 1000);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r3_fastq + " --I1 " + i1_fastq + " --I2 " +
      i2_fastq + " -o " + archive_path + " -t 1 -y sc-rna";
  const std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                     " -d -i " + archive_path + " -o " +
                                     out_r1 + " " + out_r2 + " " + out_r3 +
                                     " " + out_i1 + " " + out_i2 + " -t 1";

  run_spring(compress_cmd);
  run_spring(decompress_cmd);

  for (const auto &[original_path, restored_path] :
       {std::pair{r1_fastq, out_r1}, std::pair{r2_fastq, out_r2},
        std::pair{r3_fastq, out_r3}, std::pair{i1_fastq, out_i1},
        std::pair{i2_fastq, out_i2}}) {
    check_bytes_equal(read_file_binary(restored_path),
                      read_file_binary(original_path),
                      "grouped lane round-trip");
  }

  fs::remove_all(test_dir);
}

TEST_CASE("Grouped decompression derives unique default output names") {
  const std::string test_dir = "grouped_default_name_test_tmp";
  fs::create_directories(test_dir + "/r1");
  fs::create_directories(test_dir + "/r2");
  fs::create_directories(test_dir + "/r3");
  fs::create_directories(test_dir + "/i1");

  const std::string r1_fastq = test_dir + "/r1/same.fastq";
  const std::string r2_fastq = test_dir + "/r2/same.fastq";
  const std::string r3_fastq = test_dir + "/r3/same.fastq";
  const std::string i1_fastq = test_dir + "/i1/same.fastq";
  const std::string archive_path =
      fs::absolute(fs::path(test_dir) / "grouped.sp").generic_string();

  create_dummy_fastq(r1_fastq, 120);
  create_dummy_fastq(r2_fastq, 120);
  create_dummy_fastq(r3_fastq, 120);
  create_dummy_fastq(i1_fastq, 120);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + r1_fastq + " --R2 " +
                                   r2_fastq + " --R3 " + r3_fastq + " --I1 " +
                                   i1_fastq + " -o " + archive_path + " -t 1";
  run_spring(compress_cmd);

  {
    ScopedCurrentPath cwd_guard(test_dir);
    const std::string decompress_cmd =
        std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_path + " -t 1";
    run_spring(decompress_cmd);
  }

  check_bytes_equal(read_file_binary(test_dir + "/same.R1.fastq"),
                    read_file_binary(r1_fastq), "R1 default name");
  check_bytes_equal(read_file_binary(test_dir + "/same.R2.fastq"),
                    read_file_binary(r2_fastq), "R2 default name");
  check_bytes_equal(read_file_binary(test_dir + "/same.R3.fastq"),
                    read_file_binary(r3_fastq), "R3 default name");
  check_bytes_equal(read_file_binary(test_dir + "/same.I1.fastq"),
                    read_file_binary(i1_fastq), "I1 default name");

  fs::remove_all(test_dir);
}

TEST_CASE("Grouped aliased R3 output honors requested target format") {
  const std::string test_dir = "grouped_alias_format_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string archive_path = test_dir + "/grouped_alias.sp";
  const std::string out_r1 = test_dir + "/out_R1.fastq";
  const std::string out_r2 = test_dir + "/out_R2.fastq";
  const std::string out_r3 = test_dir + "/out_R3.fastq.gz";

  create_dummy_fastq(r1_fastq, 250);
  create_dummy_fastq(r2_fastq, 250);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r1_fastq + " -o " + archive_path + " -t 1";
  const std::string decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_path + " -o " +
      out_r1 + " " + out_r2 + " " + out_r3 + " -t 1";

  run_spring(compress_cmd);
  run_spring(decompress_cmd);

  uint8_t flg = 0;
  uint32_t mtime = 0;
  uint8_t xfl = 0;
  uint8_t os = 0;
  std::string name;
  bool is_gzipped = false;
  bool is_bgzf = false;
  uint16_t bgzf_block_size = 0;
  uint64_t uncompressed_size = 0;
  uint64_t compressed_size = 0;
  uint32_t member_count = 0;
  extract_gzip_detailed_info(out_r3, is_gzipped, flg, mtime, xfl, os, name,
                             is_bgzf, bgzf_block_size, uncompressed_size,
                             compressed_size, member_count);
  CHECK(is_gzipped);
  check_bytes_equal(read_gzip_file_binary(out_r3), read_file_binary(r1_fastq),
                    "R3 alias gzip content");

  fs::remove_all(test_dir);
}

TEST_CASE("Grouped decompression rejects invalid read3 alias metadata") {
  const std::string test_dir = "grouped_invalid_alias_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string archive_path = test_dir + "/grouped_alias.sp";
  const std::string extract_dir = test_dir + "/extract";
  const std::string corrupted_archive = test_dir + "/grouped_alias_corrupt.sp";
  const std::string corrupt_log = test_dir + "/corrupt.log";

  create_dummy_fastq(r1_fastq, 200);
  create_dummy_fastq(r2_fastq, 200);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r1_fastq + " -o " + archive_path + " -t 1";
  run_spring(compress_cmd);

  fs::create_directories(extract_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + extract_dir).c_str()) == 0);
  replace_exact_in_file(extract_dir + "/bundle.meta", "read3_alias_source=R1",
                        "read3_alias_source=BAD");
  fs::remove(fs::path(extract_dir) / "_repack_tmp.tar");
  REQUIRE(
      std::system(("cd " + extract_dir +
                   " && tar -cf _repack_tmp.tar --exclude _repack_tmp.tar *")
                      .c_str()) == 0);
  fs::rename(fs::path(extract_dir) / "_repack_tmp.tar", corrupted_archive);

  const std::string decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + corrupted_archive + " -o " +
      test_dir + "/out_R1.fastq " + test_dir + "/out_R2.fastq " + test_dir +
      "/out_R3.fastq -t 1 > " + corrupt_log + " 2>&1";
  CHECK(std::system(decompress_cmd.c_str()) != 0);

  fs::remove_all(test_dir);
}

TEST_CASE("Grouped audit rejects flipped aliased R3 mate label") {
  const std::string test_dir = "grouped_flipped_alias_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string archive_path = test_dir + "/grouped_alias.sp";
  const std::string extract_dir = test_dir + "/extract";
  const std::string flipped_archive = test_dir + "/grouped_alias_flipped.sp";
  const std::string audit_log = test_dir + "/audit.log";
  const std::string decompress_log = test_dir + "/decompress.log";

  // Distinct mates so flipping the alias label changes the bound digests.
  create_custom_paired_fastqs(r1_fastq, r2_fastq, 200, false, false, false,
                              false);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r1_fastq + " -o " + archive_path + " -t 1";
  run_spring(compress_cmd);

  // Confirm a clean aliased archive audits successfully.
  run_spring(std::string(SPRING2_EXECUTABLE) + " -p -a -i " + archive_path);

  fs::create_directories(extract_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + extract_dir).c_str()) == 0);
  // Flip R1 -> R2 while leaving stored mate digests bound to R1. Drop
  // integrity.meta so the container hash does not fail first.
  replace_exact_in_file(extract_dir + "/bundle.meta", "read3_alias_source=R1",
                        "read3_alias_source=R2");
  fs::remove(fs::path(extract_dir) / "integrity.meta");
  fs::remove(fs::path(extract_dir) / "_repack_tmp.tar");
  REQUIRE(
      std::system(("cd " + extract_dir +
                   " && tar -cf _repack_tmp.tar --exclude _repack_tmp.tar *")
                      .c_str()) == 0);
  fs::rename(fs::path(extract_dir) / "_repack_tmp.tar", flipped_archive);

  const std::string audit_cmd = std::string(SPRING2_EXECUTABLE) +
                                " -p -a -i " + flipped_archive + " > " +
                                audit_log + " 2>&1";
  CHECK(std::system(audit_cmd.c_str()) != 0);

  const std::string decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + flipped_archive + " -o " +
      test_dir + "/out_R1.fastq " + test_dir + "/out_R2.fastq " + test_dir +
      "/out_R3.fastq -t 1 > " + decompress_log + " 2>&1";
  CHECK(std::system(decompress_cmd.c_str()) != 0);

  {
    std::ifstream in(audit_log, std::ios::binary);
    REQUIRE(in.is_open());
    const std::string output((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    CHECK(output.find("does not match stored mate digests") !=
          std::string::npos);
  }

  fs::remove_all(test_dir);
}

TEST_CASE(
    "Grouped preview and SpringReader reject inconsistent index metadata") {
  const std::string test_dir = "grouped_invalid_index_manifest_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string i1_fastq = test_dir + "/input_I1.fastq";
  const std::string archive_path = test_dir + "/grouped_index.sp";
  const std::string extract_dir = test_dir + "/extract";
  const std::string corrupted_archive = test_dir + "/grouped_index_corrupt.sp";
  const std::string preview_log = test_dir + "/preview.log";

  create_dummy_fastq(r1_fastq, 200);
  create_dummy_fastq(r2_fastq, 200);
  create_dummy_fastq(i1_fastq, 200);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --I1 " + i1_fastq + " -o " + archive_path + " -t 1";
  run_spring(compress_cmd);

  fs::create_directories(extract_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + extract_dir).c_str()) == 0);
  replace_exact_in_file(extract_dir + "/bundle.meta", "has_index=1",
                        "has_index=0");
  replace_exact_in_file(extract_dir + "/bundle.meta", "has_i2=0", "has_i2=1");
  fs::remove(fs::path(extract_dir) / "_repack_tmp.tar");
  REQUIRE(
      std::system(("cd " + extract_dir +
                   " && tar -cf _repack_tmp.tar --exclude _repack_tmp.tar *")
                      .c_str()) == 0);
  fs::rename(fs::path(extract_dir) / "_repack_tmp.tar", corrupted_archive);

  const std::string preview_cmd = std::string(SPRING2_EXECUTABLE) + " -p " +
                                  corrupted_archive + " > " + preview_log +
                                  " 2>&1";
  CHECK(std::system(preview_cmd.c_str()) != 0);
  CHECK_THROWS_AS(SpringReader(corrupted_archive, 1), std::runtime_error);

  fs::remove_all(test_dir);
}

} // namespace