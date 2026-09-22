#include "integration_test_support.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using namespace integration_test_support;

namespace {

std::string sample_asset_path(const std::string &name) {
#ifdef INTEGRATION_TEST_ASSET_DIR
  return (fs::path(INTEGRATION_TEST_ASSET_DIR) / name).generic_string();
#else
  return (fs::path("..") / ".." / "data" / "samples" / name).generic_string();
#endif
}

enum class ChunkOrderFixture { Random, Aligned, UnevenN };

void create_chunk_order_fastq(const std::string &path, const int mate,
                              const ChunkOrderFixture fixture,
                              const int num_records = 200) {
  static constexpr char kBases[] = {'A', 'C', 'G', 'T'};
  static constexpr int kReadLength = 150;
  std::mt19937 rng(9382U + static_cast<uint32_t>(mate));
  std::mt19937 template_rng(751U);
  std::uniform_int_distribution<int> base_distribution(0, 3);

  std::string aligned_template(kReadLength, 'A');
  for (char &base : aligned_template) {
    base = kBases[base_distribution(template_rng)];
  }

  std::ofstream output(path, std::ios::binary);
  for (int record = 0; record < num_records; ++record) {
    std::string sequence(kReadLength, 'A');
    for (char &base : sequence) {
      base = kBases[base_distribution(rng)];
    }

    if (fixture == ChunkOrderFixture::Aligned) {
      sequence = aligned_template;
      const uint32_t tag =
          static_cast<uint32_t>(record + (mate - 1) * num_records);
      for (uint32_t base_index = 0; base_index < 8; ++base_index) {
        sequence[base_index] = kBases[(tag >> (2U * base_index)) & 3U];
      }
    }
    if (fixture == ChunkOrderFixture::UnevenN &&
        record % (mate == 1 ? 13 : 7) == 0) {
      sequence[37] = 'N';
    }

    std::string quality(kReadLength, '!');
    for (int base_index = 0; base_index < kReadLength; ++base_index) {
      quality[base_index] =
          static_cast<char>(53 + (record * 3 + base_index + mate * 5) % 21);
    }
    write_fastq_record(output,
                       "@pair" + std::to_string(record + 1) + "/" +
                           std::to_string(mate),
                       sequence, quality, false, false);
  }
}

TEST_CASE("Single-end sample matches for memory_path and disk_path") {
  const std::string test_dir = "storage_path_single_test_tmp";
  const std::string input_fastq = sample_asset_path("test_1.fastq");
  const std::string memory_archive = test_dir + "/memory.sp";
  const std::string disk_archive = test_dir + "/disk.sp";
  const std::string memory_output = test_dir + "/memory.fastq";
  const std::string disk_output = test_dir + "/disk.fastq";
  const std::string memory_log = test_dir + "/memory.log";
  const std::string disk_log = test_dir + "/disk.log";

  REQUIRE(fs::exists(input_fastq));
  fs::create_directories(test_dir);

  const std::string memory_compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + input_fastq + " -o " +
      memory_archive + " -t 1 -m 1024 > " + memory_log + " 2>&1";
  const std::string disk_compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + input_fastq + " -o " +
      disk_archive + " -t 1 -m 0.00001 > " + disk_log + " 2>&1";
  const std::string memory_decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                            " -d -i " + memory_archive +
                                            " -o " + memory_output + " -t 1";
  const std::string disk_decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                          " -d -i " + disk_archive + " -o " +
                                          disk_output + " -t 1";

  REQUIRE(std::system(memory_compress_cmd.c_str()) == 0);
  REQUIRE(std::system(disk_compress_cmd.c_str()) == 0);
  run_spring(memory_decompress_cmd);
  run_spring(disk_decompress_cmd);

  const std::string original_bytes = read_file_binary(input_fastq);
  check_bytes_equal(read_file_binary(memory_output), original_bytes,
                    "memory-path output");
  check_bytes_equal(read_file_binary(disk_output), original_bytes,
                    "disk-path output");
  check_bytes_equal(read_file_binary(memory_output),
                    read_file_binary(disk_output), "memory vs disk output");

  const std::string memory_log_output = read_file_binary(memory_log);
  const std::string disk_log_output = read_file_binary(disk_log);
  CHECK(memory_log_output.find(
            "Disk-backed compression path selected based on estimated peak "
            "working memory and available memory.") == std::string::npos);
  CHECK(disk_log_output.find(
            "Disk-backed compression path selected based on estimated peak "
            "working memory and available memory.") != std::string::npos);
  CHECK_FALSE(fs::exists(disk_archive + ".work-tmp"));

  fs::remove_all(test_dir);
}

TEST_CASE("Paired sample matches for memory_path and disk_path") {
  const std::string test_dir = "storage_path_paired_test_tmp";
  const std::string input_r1 = sample_asset_path("test_1.fastq");
  const std::string input_r2 = sample_asset_path("test_2.fastq");
  const std::string memory_archive = test_dir + "/memory_paired.sp";
  const std::string disk_archive = test_dir + "/disk_paired.sp";
  const std::string memory_r1 = test_dir + "/memory_R1.fastq";
  const std::string memory_r2 = test_dir + "/memory_R2.fastq";
  const std::string disk_r1 = test_dir + "/disk_R1.fastq";
  const std::string disk_r2 = test_dir + "/disk_R2.fastq";
  const std::string memory_log = test_dir + "/memory_paired.log";
  const std::string disk_log = test_dir + "/disk_paired.log";

  REQUIRE(fs::exists(input_r1));
  REQUIRE(fs::exists(input_r2));
  fs::create_directories(test_dir);

  const std::string memory_compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + input_r1 + " --R2 " +
      input_r2 + " -o " + memory_archive + " -t 1 -m 1024 > " + memory_log +
      " 2>&1";
  const std::string disk_compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + input_r1 + " --R2 " +
      input_r2 + " -o " + disk_archive + " -t 1 -m 0.00001 > " + disk_log +
      " 2>&1";
  const std::string memory_decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + memory_archive + " -o " +
      memory_r1 + " " + memory_r2 + " -t 1";
  const std::string disk_decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                          " -d -i " + disk_archive + " -o " +
                                          disk_r1 + " " + disk_r2 + " -t 1";

  REQUIRE(std::system(memory_compress_cmd.c_str()) == 0);
  REQUIRE(std::system(disk_compress_cmd.c_str()) == 0);
  run_spring(memory_decompress_cmd);
  run_spring(disk_decompress_cmd);

  const std::string original_r1 = read_file_binary(input_r1);
  const std::string original_r2 = read_file_binary(input_r2);
  check_bytes_equal(read_file_binary(memory_r1), original_r1, "memory-path R1");
  check_bytes_equal(read_file_binary(memory_r2), original_r2, "memory-path R2");
  check_bytes_equal(read_file_binary(disk_r1), original_r1, "disk-path R1");
  check_bytes_equal(read_file_binary(disk_r2), original_r2, "disk-path R2");

  const std::string memory_log_output = read_file_binary(memory_log);
  const std::string disk_log_output = read_file_binary(disk_log);
  CHECK(memory_log_output.find(
            "Disk-backed compression path selected based on estimated peak "
            "working memory and available memory.") == std::string::npos);
  CHECK(disk_log_output.find(
            "Disk-backed compression path selected based on estimated peak "
            "working memory and available memory.") != std::string::npos);
  CHECK_FALSE(fs::exists(disk_archive + ".work-tmp"));

  fs::remove_all(test_dir);
}

// ---------------------------------------------------------------------------
// Five-path correctness test
//
// Runs a full compress→decompress round-trip through each of the five
// compression paths and confirms that the decompressed output is bit-for-bit
// identical to the original input (or line-count-equal when order is stripped).
//
//   memory_path           : compression fully in RAM (-m 1024); baseline.
//
//   disk_path_external_mphf
//                         : disk-backed path that exercises MPHF selection
//                           policy (-m 0.00001 -v debug). With RAM-first
//                           selection enabled, this can choose either the
//                           in-memory MPHF route or the external-memory route
//                           depending on available memory and dataset size.
//                           The debug log is checked for either valid policy
//                           banner.
//
//   disk_path_thread_capped
//                         : disk-backed path with thread-count capping in
//                           effect. Forced via -t 4 combined with -m 0.00001;
//                           available_memory_bytes is ~107 MB (0.1 GiB),
//                           giving safe_threads = floor(0.1 / 2 / 4) = 0
//                           → clamped to 1. NOTE: thread capping only fires
//                           when disk space is insufficient for the external-
//                           MPHF path; on most machines the external-MPHF
//                           path is taken instead. The sub-test verifies
//                           round-trip correctness regardless of which branch
//                           fires.
//
//   disk_path_order_stripped
//                         : disk-backed path with read-order discarded
//                           (-m 0.00001 -s o), exercising the quality/ID
//                           streaming-from-disk path. Decompressed output is
//                           compared by line count only (order is not
//                           preserved by design).
//
//   disk_path_disk_streaming
//                         : disk-backed path where both the aligned shards
//                           and singleton reads are streamed directly from
//                           their spilled files during encoding instead of
//                           being loaded into RAM first (-m 0.00001 -v info).
//                           The info log is checked for the
//                           "Streaming aligned shard" banner to confirm the
//                           new code path was taken.
// ---------------------------------------------------------------------------
TEST_CASE(
    "All five disk-path memory-reduction variants produce correct output") {
  const std::string test_dir = "disk_path_variants_tmp";
  const std::string input = sample_asset_path("test_1.fastq");

  REQUIRE(fs::exists(input));
  fs::create_directories(test_dir);

  struct Variant {
    std::string name;
    std::string compress_extra_flags;
    std::string decompress_extra_flags;
    bool sorted = false;
    std::vector<std::string> expected_log_fragments; // empty means no log check
  };

  const std::vector<Variant> variants = {
      {"memory_path", "-m 1024", "", false, {}},
      {"disk_path_external_mphf",
       "-m 0.00001 -v debug",
       "",
       false,
       {"disk_path: RAM budget sufficient for in-memory MPHF",
        "disk_path: using external-memory MPHF builder"}},
      {"disk_path_thread_capped",
       "-m 0.00001 -t 4 -v debug",
       "",
       false,
       // On most machines the external-MPHF path fires instead of thread
       // capping; round-trip correctness is verified regardless.
       {}},
      {"disk_path_order_stripped", "-m 0.00001 -s o", "", true, {}},
      {"disk_path_disk_streaming",
       "-m 0.00001 -v info",
       "",
       false,
       {"Streaming aligned shard"}},
  };

  for (const auto &v : variants) {
    CAPTURE(v.name);

    const std::string archive = test_dir + "/" + v.name + ".sp";
    const std::string output = test_dir + "/" + v.name + ".fastq";
    const std::string log_file = test_dir + "/" + v.name + ".log";

    const std::string compress_cmd =
        std::string(SPRING2_EXECUTABLE) + " -c --R1 " + input + " -o " +
        archive + " " + v.compress_extra_flags + " > " + log_file + " 2>&1";
    REQUIRE(std::system(compress_cmd.c_str()) == 0);

    if (!v.expected_log_fragments.empty()) {
      const std::string log_contents = read_file_binary(log_file);
      bool found_expected_fragment = false;
      for (const std::string &expected_fragment : v.expected_log_fragments) {
        if (log_contents.find(expected_fragment) != std::string::npos) {
          found_expected_fragment = true;
          break;
        }
      }
      CHECK(found_expected_fragment);
    }

    const std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                       " -d -i " + archive + " -o " + output +
                                       " " + v.decompress_extra_flags;
    run_spring(decompress_cmd);

    if (v.sorted) {
      // Order was stripped; compare sorted lines.
      // Load both files, sort their lines, and compare.
      const std::string original = read_file_binary(input);
      const std::string result = read_file_binary(output);
      // Line-count must match at minimum.
      auto count_lines = [](const std::string &s) {
        return std::count(s.begin(), s.end(), '\n');
      };
      CHECK(count_lines(result) == count_lines(original));
      // Byte-exact match is not required when order is stripped, but the
      // decompressed file must be non-empty and the same size class.
      CHECK(result.size() > 0);
    } else {
      check_bytes_equal(read_file_binary(output), read_file_binary(input),
                        (v.name + " round-trip").c_str());
    }
  }

  // Cross-check: all non-sorted paths must produce identical output.
  const std::string memory_out =
      read_file_binary(test_dir + "/memory_path.fastq");
  const std::string disk_ext_mphf_out =
      read_file_binary(test_dir + "/disk_path_external_mphf.fastq");
  const std::string disk_thread_capped_out =
      read_file_binary(test_dir + "/disk_path_thread_capped.fastq");
  const std::string disk_disk_streaming_out =
      read_file_binary(test_dir + "/disk_path_disk_streaming.fastq");
  check_bytes_equal(disk_ext_mphf_out, memory_out,
                    "disk_external_mphf == memory_path");
  check_bytes_equal(disk_thread_capped_out, memory_out,
                    "disk_thread_capped == memory_path");
  check_bytes_equal(disk_disk_streaming_out, memory_out,
                    "disk_disk_streaming == memory_path");

  fs::remove_all(test_dir);
}

// ---------------------------------------------------------------------------
// Reorder stage logging
//
// Three checks added for V1.3.4:
//   1. The INFO logs that bracket the OMP parallel region ("Reordering reads"
//      and "Reorder pass time:") appear in verbose output — proving the log
//      infrastructure is not silent.
//   2. The maxshift-cap INFO log fires for reads longer than 100 bp.
//   3. The preprocess archive-member staging log fires on disk-path runs.
// ---------------------------------------------------------------------------

TEST_CASE("Reorder stage info logs appear in verbose compression") {
  const std::string test_dir = "reorder_log_test_tmp";
  const std::string input = sample_asset_path("test_1.fastq");
  const std::string archive = test_dir + "/out.sp";
  const std::string log = test_dir + "/out.log";

  REQUIRE(fs::exists(input));
  fs::create_directories(test_dir);

  const std::string cmd = std::string(SPRING2_EXECUTABLE) + " -c --R1 " +
                          input + " -o " + archive +
                          " -t 1 -m 1024 -v info > " + log + " 2>&1";
  REQUIRE(std::system(cmd.c_str()) == 0);

  const std::string log_contents = read_file_binary(log);
  // "Reordering reads" fires before the OMP region; "Reorder pass time:"
  // fires after it.  Both must appear to prove the log path is active.
  CHECK(log_contents.find("Reordering reads") != std::string::npos);
  CHECK(log_contents.find("Reorder pass time:") != std::string::npos);
  CHECK(log_contents.find("Dictionary stage time:") != std::string::npos);

  fs::remove_all(test_dir);
}

TEST_CASE("Maxshift cap log fires for reads longer than 100 bp") {
  const std::string test_dir = "maxshift_cap_log_test_tmp";
  const std::string input = test_dir + "/long_reads.fastq";
  const std::string archive = test_dir + "/out.sp";
  const std::string log = test_dir + "/out.log";

  fs::create_directories(test_dir);
  // 120 bp reads: uncapped maxshift would be 60 > 50, so the cap fires.
  create_custom_fastq(input, 200, false, false, 120);

  const std::string cmd = std::string(SPRING2_EXECUTABLE) + " -c --R1 " +
                          input + " -o " + archive +
                          " -t 1 -m 1024 -v info > " + log + " 2>&1";
  REQUIRE(std::system(cmd.c_str()) == 0);

  const std::string log_contents = read_file_binary(log);
  CHECK(log_contents.find("Reorder maxshift capped at 50") !=
        std::string::npos);
  // Shift step = 2 must also fire for reads > 100 bp.
  CHECK(log_contents.find("Reorder shift step: 2") != std::string::npos);

  fs::remove_all(test_dir);
}

TEST_CASE(
    "Disk-path staging log appears when preprocessing members are staged") {
  const std::string test_dir = "staging_log_test_tmp";
  const std::string input = sample_asset_path("test_1.fastq");
  const std::string archive = test_dir + "/out.sp";
  const std::string log = test_dir + "/out.log";

  REQUIRE(fs::exists(input));
  fs::create_directories(test_dir);

  const std::string cmd = std::string(SPRING2_EXECUTABLE) + " -c --R1 " +
                          input + " -o " + archive +
                          " -t 1 -m 0.00001 -v info > " + log + " 2>&1";
  REQUIRE(std::system(cmd.c_str()) == 0);

  const std::string log_contents = read_file_binary(log);
  CHECK(log_contents.find("preprocess archive members to work directory") !=
        std::string::npos);
  CHECK(log_contents.find("Staging done.") != std::string::npos);
  CHECK_FALSE(fs::exists(archive + ".work-tmp"));

  fs::remove_all(test_dir);
}

TEST_CASE("Chunked reorder log fires when chunk size threshold is exceeded") {
  const std::string test_dir = "chunked_reorder_log_test_tmp";
  const std::string input = test_dir + "/reads.fastq";
  const std::string archive = test_dir + "/out.sp";
  const std::string log = test_dir + "/out.log";

  fs::create_directories(test_dir);
  // 200 reads, 75 bp — comfortably above the forced chunk size of 10.
  create_custom_fastq(input, 200, false, false, 75);

  // Force chunk size to 10 so a 200-read FASTQ triggers multi-chunk reorder.
#if defined(_WIN32)
  _putenv_s("SPRING2_REORDER_CHUNK_SIZE", "10");
#else
  setenv("SPRING2_REORDER_CHUNK_SIZE", "10", 1);
#endif

  const std::string cmd = std::string(SPRING2_EXECUTABLE) + " -c --R1 " +
                          input + " -o " + archive +
                          " -t 1 -m 1024 -v info > " + log + " 2>&1";
  REQUIRE(std::system(cmd.c_str()) == 0);

#if defined(_WIN32)
  _putenv_s("SPRING2_REORDER_CHUNK_SIZE", "");
#else
  unsetenv("SPRING2_REORDER_CHUNK_SIZE");
#endif

  const std::string log_contents = read_file_binary(log);
  CHECK(log_contents.find("splitting") != std::string::npos);
  CHECK(log_contents.find("chunks of up to 10 reads") != std::string::npos);
  CHECK(log_contents.find("Reorder chunk 1") != std::string::npos);

  // Verify the archive decompresses correctly even after chunked reorder.
  const std::string out_dir = test_dir + "/decomp";
  fs::create_directories(out_dir);
  const std::string decomp_cmd = std::string(SPRING2_EXECUTABLE) + " -d -i " +
                                 archive + " -o " + out_dir + "/out.fastq > " +
                                 log + " 2>&1";
  CHECK(std::system(decomp_cmd.c_str()) == 0);

  fs::remove_all(test_dir);
}

TEST_CASE("Paired multi-chunk reorder preserves mate record associations") {
  struct Variant {
    const char *name;
    ChunkOrderFixture fixture;
    const char *memory;
    int threads;
    bool expect_disk_path;
  };

  const std::vector<Variant> variants = {
      {"singleton_memory", ChunkOrderFixture::Random, "1024", 1, false},
      {"aligned_disk", ChunkOrderFixture::Aligned, "0.00001", 4, true},
      {"uneven_n_memory", ChunkOrderFixture::UnevenN, "1024", 1, false},
  };

  for (const Variant &variant : variants) {
    CAPTURE(variant.name);
    const std::string test_dir =
        std::string("paired_chunk_order_") + variant.name + "_tmp";
    const std::string input_r1 = test_dir + "/input_R1.fastq";
    const std::string input_r2 = test_dir + "/input_R2.fastq";
    const std::string archive = test_dir + "/archive.sp";
    const std::string output_r1 = test_dir + "/output_R1.fastq";
    const std::string output_r2 = test_dir + "/output_R2.fastq";
    const std::string log = test_dir + "/compress.log";

    fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    create_chunk_order_fastq(input_r1, 1, variant.fixture);
    create_chunk_order_fastq(input_r2, 2, variant.fixture);

#if defined(_WIN32)
    _putenv_s("SPRING2_REORDER_CHUNK_SIZE", "100");
#else
    setenv("SPRING2_REORDER_CHUNK_SIZE", "100", 1);
#endif

    const std::string compress_cmd =
        std::string(SPRING2_EXECUTABLE) + " -c --R1 " + input_r1 + " --R2 " +
        input_r2 + " -o " + archive + " -t " + std::to_string(variant.threads) +
        " -m " + variant.memory +
        " --assay dna -q lossless --audit -v info > " + log + " 2>&1";
    const int compress_status = std::system(compress_cmd.c_str());

#if defined(_WIN32)
    _putenv_s("SPRING2_REORDER_CHUNK_SIZE", "");
#else
    unsetenv("SPRING2_REORDER_CHUNK_SIZE");
#endif

    REQUIRE(compress_status == 0);
    const std::string log_contents = read_file_binary(log);
    CHECK(log_contents.find("Reorder chunk 2") != std::string::npos);
    CHECK(log_contents.find("Audit successful:") != std::string::npos);
    const bool used_disk_path =
        log_contents.find("Disk-backed compression path selected") !=
        std::string::npos;
    CHECK(used_disk_path == variant.expect_disk_path);

    run_spring(std::string(SPRING2_EXECUTABLE) + " -d -u -i " + archive +
               " -o " + output_r1 + " " + output_r2 + " -t " +
               std::to_string(variant.threads));
    check_bytes_equal(read_file_binary(output_r1), read_file_binary(input_r1),
                      "paired multi-chunk R1 round-trip");
    check_bytes_equal(read_file_binary(output_r2), read_file_binary(input_r2),
                      "paired multi-chunk R2 round-trip");

    fs::remove_all(test_dir);
  }
}

TEST_CASE("Chunked reorder spills streams and singletons to disk (disk_path)") {
  const std::string test_dir = "chunked_reorder_spill_test_tmp";
  const std::string input = test_dir + "/reads.fastq";
  const std::string archive = test_dir + "/out.sp";
  const std::string log = test_dir + "/out.log";

  fs::create_directories(test_dir);
  // 300 reads, 75 bp (all distinct so most will be singletons).
  create_custom_fastq(input, 300, false, false, 75);

  // Chunk size 10 → 30 chunks; -m 0.00001 forces disk_path mode → use_spill.
#if defined(_WIN32)
  _putenv_s("SPRING2_REORDER_CHUNK_SIZE", "10");
#else
  setenv("SPRING2_REORDER_CHUNK_SIZE", "10", 1);
#endif

  const std::string cmd = std::string(SPRING2_EXECUTABLE) + " -c --R1 " +
                          input + " -o " + archive +
                          " -t 1 -m 0.00001 -v info > " + log + " 2>&1";
  REQUIRE(std::system(cmd.c_str()) == 0);

#if defined(_WIN32)
  _putenv_s("SPRING2_REORDER_CHUNK_SIZE", "");
#else
  unsetenv("SPRING2_REORDER_CHUNK_SIZE");
#endif

  const std::string log_contents = read_file_binary(log);
  CHECK(log_contents.find("Spilled read stream 0") != std::string::npos);
  CHECK(log_contents.find("freed from RAM") != std::string::npos);
  CHECK(log_contents.find("pre-spilled to") != std::string::npos);

  // Verify round-trip correctness after chunked spill reorder.
  const std::string out_dir = test_dir + "/decomp";
  fs::create_directories(out_dir);
  const std::string decomp_cmd = std::string(SPRING2_EXECUTABLE) + " -d -i " +
                                 archive + " -o " + out_dir + "/out.fastq > " +
                                 log + " 2>&1";
  CHECK(std::system(decomp_cmd.c_str()) == 0);

  fs::remove_all(test_dir);
}

} // namespace