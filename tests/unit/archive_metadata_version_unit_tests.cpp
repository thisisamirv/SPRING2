#include "../support/doctest.h"

#include "../../src/common/params.h"
#include "../../src/workflow/workflow_internal.h"

#include <cstring>
#include <sstream>

namespace {

constexpr char kExpectedLegacyArchiveVersion[] = "1.0.0-rc.1";

spring::compression_params make_sample_params() {
  spring::compression_params cp{};
  cp.encoding.paired_end = true;
  cp.encoding.preserve_order = true;
  cp.encoding.preserve_quality = true;
  cp.encoding.preserve_id = true;
  cp.encoding.long_flag = false;
  cp.encoding.num_thr = 4;
  cp.encoding.compression_level = 6;
  cp.encoding.num_reads_per_block = 123;
  cp.encoding.num_reads_per_block_long = 45;
  cp.encoding.fasta_mode = false;
  cp.encoding.use_crlf = false;
  cp.encoding.use_crlf_by_stream[0] = false;
  cp.encoding.use_crlf_by_stream[1] = true;
  cp.read_info.num_reads = 100;
  cp.read_info.num_reads_clean[0] = 50;
  cp.read_info.num_reads_clean[1] = 50;
  cp.read_info.max_readlen = 151;
  cp.read_info.paired_id_code = 7;
  cp.read_info.paired_id_match = true;
  cp.read_info.input_filename_1 = "reads_1.fastq.gz";
  cp.read_info.input_filename_2 = "reads_2.fastq.gz";
  cp.read_info.note = "note";
  cp.read_info.assay = "rna";
  cp.read_info.assay_confidence = "high";
  cp.read_info.compressor_version = "1.0.0-rc.2";
  cp.read_info.sequence_crc[0] = 11;
  cp.read_info.sequence_crc[1] = 22;
  cp.read_info.quality_crc[0] = 33;
  cp.read_info.quality_crc[1] = 44;
  cp.read_info.id_crc[0] = 55;
  cp.read_info.id_crc[1] = 66;
  cp.read_info.quality_header_has_id_by_stream[0] = false;
  cp.read_info.quality_header_has_id_by_stream[1] = true;
  cp.read_info.digest_length_framed = true;
  cp.gzip.streams[0].was_gzipped = true;
  cp.gzip.streams[1].was_gzipped = true;
  cp.gzip.streams[0].name = "reads_1.fastq";
  cp.gzip.streams[1].name = "reads_2.fastq";
  return cp;
}

std::string
serialize_current_archive_metadata(const spring::compression_params &cp) {
  std::ostringstream output(std::ios::binary);
  spring::write_compression_params(output, cp);
  return output.str();
}

std::string
serialize_legacy_archive_metadata(const spring::compression_params &cp) {
  std::ostringstream output(std::ios::binary);

  spring::write_bool(output, cp.encoding.paired_end);
  spring::write_bool(output, cp.encoding.preserve_order);
  spring::write_bool(output, cp.encoding.preserve_quality);
  spring::write_bool(output, cp.encoding.preserve_id);
  spring::write_bool(output, cp.encoding.long_flag);
  const bool reserved_qvz_flag = false;
  spring::write_bool(output, reserved_qvz_flag); // reserved (was qvz_flag)
  spring::write_bool(output, cp.quality.ill_bin_flag);
  spring::write_bool(output, cp.quality.bin_thr_flag);
  const double reserved_qvz_ratio = 0.0;
  output.write(reinterpret_cast<const char *>(&reserved_qvz_ratio),
               sizeof(double)); // reserved (was qvz_ratio)
  output.write(reinterpret_cast<const char *>(&cp.quality.bin_thr_thr),
               sizeof(unsigned int));
  output.write(reinterpret_cast<const char *>(&cp.quality.bin_thr_high),
               sizeof(unsigned int));
  output.write(reinterpret_cast<const char *>(&cp.quality.bin_thr_low),
               sizeof(unsigned int));
  output.write(reinterpret_cast<const char *>(&cp.read_info.num_reads),
               sizeof(uint32_t));
  output.write(reinterpret_cast<const char *>(&cp.read_info.num_reads_clean[0]),
               sizeof(uint32_t));
  output.write(reinterpret_cast<const char *>(&cp.read_info.num_reads_clean[1]),
               sizeof(uint32_t));
  output.write(reinterpret_cast<const char *>(&cp.read_info.max_readlen),
               sizeof(uint32_t));
  output.write(reinterpret_cast<const char *>(&cp.read_info.paired_id_code),
               sizeof(uint8_t));
  spring::write_bool(output, cp.read_info.paired_id_match);
  output.write(reinterpret_cast<const char *>(&cp.encoding.num_reads_per_block),
               sizeof(int));
  output.write(
      reinterpret_cast<const char *>(&cp.encoding.num_reads_per_block_long),
      sizeof(int));
  output.write(reinterpret_cast<const char *>(&cp.encoding.num_thr),
               sizeof(int));
  output.write(reinterpret_cast<const char *>(&cp.encoding.compression_level),
               sizeof(int));
  output.write(reinterpret_cast<const char *>(cp.read_info.file_len_seq_thr),
               sizeof(uint64_t) *
                   spring::compression_params::ReadMetadata::kFileLenThrSize);
  output.write(reinterpret_cast<const char *>(cp.read_info.file_len_id_thr),
               sizeof(uint64_t) *
                   spring::compression_params::ReadMetadata::kFileLenThrSize);
  spring::write_bool(output, cp.encoding.use_crlf);
  spring::write_string(output, cp.read_info.input_filename_1);
  spring::write_string(output, cp.read_info.input_filename_2);
  spring::write_string(output, cp.read_info.note);
  spring::write_bool(output, cp.encoding.fasta_mode);

  for (int i = 0; i < 2; ++i) {
    spring::write_bool(output, cp.gzip.streams[i].was_gzipped);
  }
  for (int i = 0; i < 2; ++i) {
    output.write(reinterpret_cast<const char *>(&cp.gzip.streams[i].flg),
                 sizeof(uint8_t));
  }
  for (int i = 0; i < 2; ++i) {
    output.write(reinterpret_cast<const char *>(&cp.gzip.streams[i].mtime),
                 sizeof(uint32_t));
  }
  for (int i = 0; i < 2; ++i) {
    output.write(reinterpret_cast<const char *>(&cp.gzip.streams[i].xfl),
                 sizeof(uint8_t));
  }
  for (int i = 0; i < 2; ++i) {
    output.write(reinterpret_cast<const char *>(&cp.gzip.streams[i].os),
                 sizeof(uint8_t));
  }
  for (int i = 0; i < 2; ++i) {
    spring::write_string(output, cp.gzip.streams[i].name);
  }
  for (int i = 0; i < 2; ++i) {
    spring::write_bool(output, cp.gzip.streams[i].is_bgzf);
  }
  for (int i = 0; i < 2; ++i) {
    output.write(
        reinterpret_cast<const char *>(&cp.gzip.streams[i].bgzf_block_size),
        sizeof(uint16_t));
  }
  for (int i = 0; i < 2; ++i) {
    output.write(
        reinterpret_cast<const char *>(&cp.gzip.streams[i].uncompressed_size),
        sizeof(uint64_t));
  }
  for (int i = 0; i < 2; ++i) {
    output.write(
        reinterpret_cast<const char *>(&cp.gzip.streams[i].compressed_size),
        sizeof(uint64_t));
  }
  for (int i = 0; i < 2; ++i) {
    output.write(
        reinterpret_cast<const char *>(&cp.gzip.streams[i].member_count),
        sizeof(uint32_t));
  }

  return output.str();
}

std::string serialize_legacy_spring_raw_cp() {
  std::string bytes(64, '\0');

  auto write_byte = [&](size_t offset, uint8_t value) {
    bytes[offset] = static_cast<char>(value);
  };
  auto write_trivial = [&](size_t offset, const auto &value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  };

  write_byte(0, 1);
  write_byte(1, 1);
  write_byte(2, 1);
  write_byte(3, 1);
  write_byte(4, 0);
  write_byte(5, 0);
  write_byte(6, 0);
  write_byte(7, 0);

  const double reserved_qvz_ratio = 1.0;
  write_trivial(8, reserved_qvz_ratio); // reserved (was qvz_ratio)

  const unsigned int bin_thr_thr = 0;
  const unsigned int bin_thr_high = 0;
  const unsigned int bin_thr_low = 0;
  write_trivial(16, bin_thr_thr);
  write_trivial(20, bin_thr_high);
  write_trivial(24, bin_thr_low);

  const uint32_t num_reads = 12;
  const uint32_t num_reads_clean_0 = 12;
  const uint32_t num_reads_clean_1 = 0;
  const uint32_t max_readlen = 101;
  write_trivial(28, num_reads);
  write_trivial(32, num_reads_clean_0);
  write_trivial(36, num_reads_clean_1);
  write_trivial(40, max_readlen);

  write_byte(44, 0);
  write_byte(45, 1);

  const int num_reads_per_block = 4;
  const int num_reads_per_block_long = 2;
  const int num_thr = 3;
  write_trivial(48, num_reads_per_block);
  write_trivial(52, num_reads_per_block_long);
  write_trivial(56, num_thr);

  return bytes;
}

} // namespace

TEST_CASE("Current archive metadata stores format header") {
  const spring::compression_params cp = make_sample_params();
  const std::string bytes = serialize_current_archive_metadata(cp);
  std::istringstream input(bytes, std::ios::binary);

  spring::compression_params parsed{};
  spring::read_compression_params(input, parsed);

  CHECK(parsed.read_info.archive_format_version ==
        spring::CURRENT_ARCHIVE_FORMAT_VERSION);
  CHECK(parsed.read_info.compressor_version == cp.read_info.compressor_version);
  CHECK(parsed.read_info.digest_length_framed == true);

  const spring::archive_decompression_plan plan =
      spring::build_archive_decompression_plan(parsed);
  CHECK(plan.is_v1_0_0_rc1 == false);
  CHECK(plan.archive_version.valid);
  CHECK(plan.archive_version.major == 1);
  CHECK(plan.archive_version.minor == 0);
}

TEST_CASE("Legacy archive metadata remains readable") {
  const spring::compression_params cp = make_sample_params();
  const std::string bytes = serialize_legacy_archive_metadata(cp);
  std::istringstream input(bytes, std::ios::binary);

  spring::compression_params parsed{};
  spring::read_compression_params(input, parsed);

  CHECK(parsed.read_info.archive_format_version ==
        spring::LEGACY_ARCHIVE_FORMAT_VERSION);
  CHECK(parsed.read_info.compressor_version == kExpectedLegacyArchiveVersion);
  CHECK(parsed.read_info.digest_length_framed == false);
  CHECK(parsed.read_info.input_filename_1 == cp.read_info.input_filename_1);
  CHECK(parsed.read_info.input_filename_2 == cp.read_info.input_filename_2);

  const spring::archive_decompression_plan plan =
      spring::build_archive_decompression_plan(parsed);
  CHECK(plan.is_v1_0_0_rc1);
  CHECK(plan.compressor_version == kExpectedLegacyArchiveVersion);
}

TEST_CASE("Archive decompression plan preserves exact current version") {
  spring::compression_params cp = make_sample_params();
  cp.read_info.compressor_version = "1.0.7";

  const spring::archive_decompression_plan plan =
      spring::build_archive_decompression_plan(cp);
  CHECK(plan.archive_version.valid);
  CHECK(plan.archive_version.major == 1);
  CHECK(plan.archive_version.minor == 0);
  CHECK(plan.archive_version.patch == 7);
  CHECK(spring::archive_decompression_route_name(plan) == "1.0.7");
}

TEST_CASE("Unsupported archive compressor versions fail explicitly") {
  spring::compression_params cp = make_sample_params();
  cp.read_info.compressor_version = "2.0.0";

  CHECK_THROWS_AS(spring::build_archive_decompression_plan(cp),
                  std::runtime_error);
}

TEST_CASE("Version 1.2.x archives are accepted for decompression") {
  spring::compression_params cp = make_sample_params();
  cp.read_info.compressor_version = "1.2.0";

  const spring::archive_decompression_plan plan =
      spring::build_archive_decompression_plan(cp);
  CHECK(plan.archive_version.valid);
  CHECK(plan.archive_version.major == 1);
  CHECK(plan.archive_version.minor == 2);
  CHECK(plan.archive_version.patch == 0);
  CHECK(spring::archive_decompression_route_name(plan) == "1.2.0");
}

TEST_CASE("Version 1.3.x archives are accepted for decompression") {
  spring::compression_params cp = make_sample_params();
  cp.read_info.compressor_version = "1.3.0";

  const spring::archive_decompression_plan plan =
      spring::build_archive_decompression_plan(cp);
  CHECK(plan.archive_version.valid);
  CHECK(plan.archive_version.major == 1);
  CHECK(plan.archive_version.minor == 3);
  CHECK(plan.archive_version.patch == 0);
  CHECK(spring::archive_decompression_route_name(plan) == "1.3.0");
}

TEST_CASE("Legacy Spring raw metadata is detected") {
  spring::decompression_archive_artifact artifact;
  artifact.files["cp.bin"] = serialize_legacy_spring_raw_cp();
  artifact.files["read_1.0"] = "";

  spring::compression_params parsed{};
  spring::read_archive_compression_params(artifact, parsed);

  CHECK(parsed.read_info.legacy_spring);
  CHECK(parsed.read_info.compressor_version == "legacy spring");
  CHECK(parsed.read_info.num_reads == 12);
  CHECK(parsed.encoding.num_thr == 3);
  CHECK(parsed.encoding.long_flag == false);

  const spring::archive_decompression_plan plan =
      spring::build_archive_decompression_plan(parsed);
  CHECK(plan.is_legacy_spring);
  CHECK(plan.is_v1_0_0_rc1 == false);
  CHECK(spring::archive_decompression_route_name(plan) == "legacy spring");
}