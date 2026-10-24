#include "integration_test_support.h"

#include "common/archive_integrity.h"
#include "common/fs_utils.h"
#include "common/params.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace spring;
using namespace integration_test_support;

namespace {

TEST_CASE("Multi-thread compression compatibility") {
  std::string test_dir = "thread_test_tmp";
  fs::create_directories(test_dir);

  std::string input_fastq = test_dir + "/input.fastq";
  create_dummy_fastq(input_fastq, 1000);

  for (int compress_threads : {1, 4, 8}) {
    std::string archive_sp =
        test_dir + "/test_t" + std::to_string(compress_threads) + ".sp";
    std::string output_fastq =
        test_dir + "/output_t" + std::to_string(compress_threads) + ".fastq";

    std::string compress_cmd = std::string(SPRING2_EXECUTABLE) + " -c --R1 " +
                               input_fastq + " -o " + archive_sp + " -t " +
                               std::to_string(compress_threads);
    run_spring(compress_cmd);

    std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) + " -d -i " +
                                 archive_sp + " -o " + output_fastq + " -t 1";
    run_spring(decompress_cmd);
  }

  fs::remove_all(test_dir);
}

TEST_CASE("Paired FASTQ preserves per-stream plus lines and line endings") {
  const std::string test_dir = "paired_fastq_metadata_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string archive_path = test_dir + "/paired_metadata.sp";
  const std::string out_r1 = test_dir + "/roundtrip_R1.fastq";
  const std::string out_r2 = test_dir + "/roundtrip_R2.fastq";

  create_custom_paired_fastqs(r1_fastq, r2_fastq, 1500, false, true, false,
                              true);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + r1_fastq + " --R2 " +
                                   r2_fastq + " -o " + archive_path + " -t 1";
  const std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                     " -d -i " + archive_path + " -o " +
                                     out_r1 + " " + out_r2 + " -t 1";

  run_spring(compress_cmd);
  run_spring(decompress_cmd);

  check_bytes_equal(read_file_binary(out_r1), read_file_binary(r1_fastq),
                    "R1 round-trip");
  check_bytes_equal(read_file_binary(out_r2), read_file_binary(r2_fastq),
                    "R2 round-trip");

  fs::remove_all(test_dir);
}

TEST_CASE("Late overlength read escalates sampled short input into long mode") {
  const std::string test_dir = "late_long_retry_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_path = test_dir + "/late_long.sp";
  const std::string output_fastq = test_dir + "/restored.fastq";

  create_late_long_fastq(input_fastq, 10000, 10032, 80, 700);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + input_fastq + " -o " +
                                   archive_path + " -t 1";
  const std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                     " -d -i " + archive_path + " -o " +
                                     output_fastq + " -t 1";

  run_spring(compress_cmd);
  run_spring(decompress_cmd);
  check_bytes_equal(read_file_binary(output_fastq),
                    read_file_binary(input_fastq), "long-mode round-trip");

  auto contents = read_files_from_tar_memory(archive_path, {"cp.bin"});
  REQUIRE(contents.contains("cp.bin"));
  compression_params cp{};
  std::istringstream cp_input(contents["cp.bin"], std::ios::binary);
  read_compression_params(cp_input, cp);
  REQUIRE(cp_input.good());
  CHECK(cp.encoding.long_flag);
  CHECK(cp.read_info.max_readlen == 700);

  fs::remove_all(test_dir);
}

TEST_CASE("Late CRLF updates metadata after startup sample") {
  const std::string test_dir = "late_crlf_retry_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_path = test_dir + "/late_crlf.sp";

  create_delayed_crlf_fastq(input_fastq, 10040, 10000);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + input_fastq + " -o " +
                                   archive_path + " -t 1";
  run_spring(compress_cmd);

  auto contents = read_files_from_tar_memory(archive_path, {"cp.bin"});
  REQUIRE(contents.contains("cp.bin"));
  compression_params cp{};
  std::istringstream cp_input(contents["cp.bin"], std::ios::binary);
  read_compression_params(cp_input, cp);
  REQUIRE(cp_input.good());
  CHECK(cp.encoding.use_crlf_by_stream[0]);
  CHECK(cp.encoding.use_crlf);

  fs::remove_all(test_dir);
}

TEST_CASE("Paired gzip outputs preserve per-stream compression profile") {
  const std::string test_dir = "paired_gzip_profile_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string archive_path = test_dir + "/paired_gzip_profile.sp";
  const std::string extract_dir = test_dir + "/archive_extract";
  const std::string baseline_r1 = test_dir + "/baseline_R1.fastq.gz";
  const std::string baseline_r2 = test_dir + "/baseline_R2.fastq.gz";
  const std::string out_r1 = test_dir + "/roundtrip_R1.fastq.gz";
  const std::string out_r2 = test_dir + "/roundtrip_R2.fastq.gz";

  create_dummy_fastq(r1_fastq, 600);
  create_dummy_fastq(r2_fastq, 600);
  create_gzip_copy(r1_fastq, baseline_r1, 1);
  create_gzip_copy(r2_fastq, baseline_r2, 9);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + r1_fastq + " --R2 " +
                                   r2_fastq + " -o " + archive_path + " -t 1";
  const std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                     " -d -i " + archive_path + " -o " +
                                     out_r1 + " " + out_r2 + " -t 1";

  run_spring(compress_cmd);

  fs::create_directories(extract_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + extract_dir).c_str()) == 0);

  compression_params cp{};
  {
    std::ifstream cp_input(extract_dir + "/cp.bin", std::ios::binary);
    REQUIRE(cp_input.is_open());
    read_compression_params(cp_input, cp);
  }
  cp.gzip.streams[0].xfl = 4;
  cp.gzip.streams[1].xfl = 2;
  {
    std::ofstream cp_output(extract_dir + "/cp.bin",
                            std::ios::binary | std::ios::trunc);
    REQUIRE(cp_output.is_open());
    write_compression_params(cp_output, cp);
  }

  // Mutating cp.bin invalidates the stored member digest; rebuild
  // integrity.meta so decompress still passes container SHA-256 checks.
  {
    std::unordered_map<std::string, std::string> members;
    for (const auto &entry : fs::directory_iterator(extract_dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      if (name == kIntegrityManifestName || name == "_repack_tmp.tar") {
        continue;
      }
      members[name] = read_file_binary(entry.path().string());
    }
    const archive_integrity_manifest manifest =
        build_integrity_manifest_from_memory(members);
    std::ofstream meta_output(extract_dir + "/" + kIntegrityManifestName,
                              std::ios::binary | std::ios::trunc);
    REQUIRE(meta_output.is_open());
    meta_output << serialize_integrity_manifest(manifest);
    REQUIRE(meta_output.good());
  }

  fs::remove(fs::path(extract_dir) / "_repack_tmp.tar");
  REQUIRE(
      std::system(("cd " + extract_dir +
                   " && tar -cf _repack_tmp.tar --exclude _repack_tmp.tar *")
                      .c_str()) == 0);
  fs::rename(fs::path(extract_dir) / "_repack_tmp.tar", archive_path);

  run_spring(decompress_cmd);

  CHECK(fs::file_size(baseline_r2) < fs::file_size(baseline_r1));
  CHECK(fs::file_size(out_r2) < fs::file_size(out_r1));

  const std::string restored_r1 = read_gzip_file_binary(out_r1);
  const std::string restored_r2 = read_gzip_file_binary(out_r2);
  CHECK(restored_r1 == read_file_binary(r1_fastq));
  CHECK(restored_r2 == read_file_binary(r2_fastq));

  fs::remove_all(test_dir);
}

} // namespace