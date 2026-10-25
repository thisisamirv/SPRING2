#include "integration_test_support.h"

#include "common/fs_utils.h"
#include "common/params.h"
#include "decompress/archive_stream_reader.h"

#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;
using namespace spring;
using namespace integration_test_support;

namespace {

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

TEST_CASE("SpringReader Integration Test") {
  std::string test_dir = "reader_test_tmp";
  fs::create_directories(test_dir);

  std::string input_fastq = test_dir + "/input.fastq";
  std::string archive_spring = test_dir + "/test.spring";

  const int num_records = 100;
  create_dummy_fastq(input_fastq, num_records);

  std::ostringstream compress_cmd;
  compress_cmd << SPRING2_EXECUTABLE << " -c --R1 " << input_fastq << " -o "
               << archive_spring << " -t 1";
  run_spring(compress_cmd.str());

  SUBCASE("Stream decompression (Single End)") {
    SpringReader reader(archive_spring, 1);

    CHECK_THROWS_WITH(
        [&]() -> lossless_digest_check {
          return reader.verify_digests();
        }(),
        "SpringReader digests are only available after the archive has "
        "been fully consumed.");

    ReadRecord rec;
    int count = 0;
    while (reader.next(rec)) {
      CHECK(rec.id == std::string("@read_") + std::to_string(count));
      count++;
    }
    CHECK(count == num_records);

    auto contents = read_files_from_tar_memory(archive_spring, {"cp.bin"});
    REQUIRE(contents.contains("cp.bin"));
    compression_params cp{};
    std::istringstream cp_input(contents["cp.bin"], std::ios::binary);
    read_compression_params(cp_input, cp);
    REQUIRE(cp_input.good());

    uint32_t seq_crc[2] = {0, 0};
    uint32_t qual_crc[2] = {0, 0};
    uint32_t id_crc[2] = {0, 0};
    reader.get_digests(seq_crc, qual_crc, id_crc);
    CHECK(seq_crc[0] == cp.read_info.sequence_crc[0]);
    CHECK(qual_crc[0] == cp.read_info.quality_crc[0]);
    CHECK(id_crc[0] == cp.read_info.id_crc[0]);

    const lossless_digest_check check = reader.verify_digests();
    CHECK_FALSE(check.mismatch);
    CHECK(check.stream_count == 1);
  }

  fs::remove_all(test_dir);
}

TEST_CASE("SpringReader streams grouped archives via primary read member") {
  const std::string test_dir = "reader_grouped_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string r3_fastq = test_dir + "/input_R3.fastq";
  const std::string i1_fastq = test_dir + "/input_I1.fastq";
  const std::string archive_path = test_dir + "/grouped_reader.sp";

  create_dummy_fastq(r1_fastq, 120);
  create_dummy_fastq(r2_fastq, 120);
  create_dummy_fastq(r3_fastq, 120);
  create_dummy_fastq(i1_fastq, 120);

  std::ostringstream compress_cmd;
  compress_cmd << SPRING2_EXECUTABLE << " -c --R1 " << r1_fastq << " --R2 "
               << r2_fastq << " --R3 " << r3_fastq << " --I1 " << i1_fastq
               << " -o " << archive_path << " -t 1";
  run_spring(compress_cmd.str());

  SpringReader reader(archive_path, 1);
  CHECK(reader.lane() == SpringReaderLane::Reads);
  ReadRecord mate1;
  ReadRecord mate2;
  int count = 0;
  while (reader.next(mate1, mate2)) {
    CHECK(mate1.id == std::string("@read_") + std::to_string(count));
    CHECK(mate2.id == std::string("@read_") + std::to_string(count));
    count++;
  }
  CHECK(count == 120);

  SpringReader r3_reader(archive_path, SpringReaderLane::Read3, 1);
  CHECK(r3_reader.lane() == SpringReaderLane::Read3);
  ReadRecord r3;
  int r3_count = 0;
  while (r3_reader.next(r3)) {
    CHECK(r3.id == std::string("@read_") + std::to_string(r3_count));
    r3_count++;
  }
  CHECK(r3_count == 120);

  SpringReader i1_reader(archive_path, SpringReaderLane::Index, 1);
  CHECK(i1_reader.lane() == SpringReaderLane::Index);
  ReadRecord i1;
  int i1_count = 0;
  while (i1_reader.next(i1)) {
    CHECK(i1.id == std::string("@read_") + std::to_string(i1_count));
    i1_count++;
  }
  CHECK(i1_count == 120);
  CHECK_THROWS_AS(i1_reader.next(mate1, mate2), std::runtime_error);

  fs::remove_all(test_dir);
}

TEST_CASE("SpringReader rejects missing grouped lanes") {
  const std::string test_dir = "reader_grouped_missing_lane_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string archive_path = test_dir + "/paired_only.sp";

  create_dummy_fastq(r1_fastq, 40);
  create_dummy_fastq(r2_fastq, 40);

  std::ostringstream compress_cmd;
  compress_cmd << SPRING2_EXECUTABLE << " -c --R1 " << r1_fastq << " --R2 "
               << r2_fastq << " -o " << archive_path << " -t 1";
  run_spring(compress_cmd.str());

  // Non-grouped paired archive: Read3/Index are unavailable.
  CHECK_THROWS_AS(
      SpringReader(archive_path, SpringReaderLane::Read3, 1),
      std::runtime_error);
  CHECK_THROWS_AS(
      SpringReader(archive_path, SpringReaderLane::Index, 1),
      std::runtime_error);

  fs::remove_all(test_dir);
}

} // namespace