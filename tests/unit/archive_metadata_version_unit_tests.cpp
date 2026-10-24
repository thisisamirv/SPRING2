#include "../support/doctest.h"

#include "../../src/common/params.h"
#include "../../src/workflow/workflow_internal.h"

#include <cstring>
#include <sstream>
#include <string>

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
  CHECK(parsed.read_info.digests_present == true);
  CHECK(parsed.read_info.sequence_crc[0] == cp.read_info.sequence_crc[0]);
  CHECK(parsed.read_info.quality_crc[1] == cp.read_info.quality_crc[1]);

  const spring::archive_decompression_plan plan =
      spring::build_archive_decompression_plan(parsed);
  CHECK(plan.is_v1_0_0_rc1 == false);
  CHECK(plan.archive_version.valid);
  CHECK(plan.archive_version.major == 1);
  CHECK(plan.archive_version.minor == 0);
}

bool find_v2_trailer(const std::string &v2_bytes, size_t &trailer_len_pos,
                     uint32_t &trailer_len) {
  // Core is large (threshold tables); the extension trailer is small and starts
  // with digests_present=1. Prefer the smallest valid trailer length.
  bool found = false;
  uint32_t best_len = 0;
  size_t best_pos = 0;
  for (size_t pos = 8; pos + 4 < v2_bytes.size(); ++pos) {
    uint32_t candidate = 0;
    std::memcpy(&candidate, v2_bytes.data() + pos, sizeof(uint32_t));
    if (pos + 4 + candidate != v2_bytes.size()) {
      continue;
    }
    if (candidate < 25 || candidate > 1024 * 1024) {
      continue;
    }
    if (static_cast<unsigned char>(v2_bytes[pos + 4]) != 1) {
      continue;
    }
    if (!found || candidate < best_len) {
      found = true;
      best_len = candidate;
      best_pos = pos;
    }
  }
  if (found) {
    trailer_len_pos = best_pos;
    trailer_len = best_len;
  }
  return found;
}

std::string serialize_v1_peek_archive_metadata(
    const spring::compression_params &cp) {
  // Format v1 layout: core + digests + extensions without a length prefix or
  // digests_present flag (presence inferred via peek).
  std::ostringstream v2_stream(std::ios::binary);
  spring::write_compression_params(v2_stream, cp);
  const std::string v2_bytes = v2_stream.str();

  size_t trailer_len_pos = 0;
  uint32_t trailer_len = 0;
  REQUIRE(find_v2_trailer(v2_bytes, trailer_len_pos, trailer_len));
  const std::string core = v2_bytes.substr(8, trailer_len_pos - 8);
  const std::string trailer =
      v2_bytes.substr(trailer_len_pos + 4, trailer_len);
  REQUIRE(!trailer.empty());
  // Drop leading digests_present=true byte; v1 starts with the raw digest block.
  REQUIRE(static_cast<unsigned char>(trailer[0]) == 1);
  const std::string v1_trailer = trailer.substr(1);

  std::ostringstream output(std::ios::binary);
  const uint32_t magic = spring::ARCHIVE_METADATA_MAGIC;
  const uint32_t version = spring::ARCHIVE_FORMAT_VERSION_V1;
  output.write(reinterpret_cast<const char *>(&magic), sizeof(uint32_t));
  output.write(reinterpret_cast<const char *>(&version), sizeof(uint32_t));
  output.write(core.data(), static_cast<std::streamsize>(core.size()));
  output.write(v1_trailer.data(),
               static_cast<std::streamsize>(v1_trailer.size()));
  return output.str();
}

TEST_CASE("Format v1 peek-based digest trailer remains readable") {
  const spring::compression_params cp = make_sample_params();
  const std::string bytes = serialize_v1_peek_archive_metadata(cp);
  std::istringstream input(bytes, std::ios::binary);

  spring::compression_params parsed{};
  spring::read_compression_params(input, parsed);

  CHECK(parsed.read_info.archive_format_version ==
        spring::ARCHIVE_FORMAT_VERSION_V1);
  CHECK(parsed.read_info.digests_present == true);
  CHECK(parsed.read_info.sequence_crc[0] == cp.read_info.sequence_crc[0]);
  CHECK(parsed.read_info.id_crc[1] == cp.read_info.id_crc[1]);
  CHECK(parsed.read_info.digest_length_framed == true);
}

TEST_CASE("Format v2 rejects truncated extension trailer") {
  const spring::compression_params cp = make_sample_params();
  std::string bytes = serialize_current_archive_metadata(cp);
  REQUIRE(bytes.size() > 20);
  // Chop the last few trailer bytes while leaving a still-valid length prefix
  // that claims more data than remains.
  bytes.resize(bytes.size() - 8);

  std::istringstream input(bytes, std::ios::binary);
  spring::compression_params parsed{};
  CHECK_THROWS_AS(spring::read_compression_params(input, parsed),
                  std::runtime_error);
}

TEST_CASE("Format v2 rejects truncated digest block inside trailer") {
  const spring::compression_params cp = make_sample_params();
  std::string bytes = serialize_current_archive_metadata(cp);

  size_t trailer_len_pos = 0;
  uint32_t found_len = 0;
  REQUIRE(find_v2_trailer(bytes, trailer_len_pos, found_len));
  REQUIRE(found_len > 8);
  const uint32_t short_len = 5; // bool + 4 bytes != full digest block
  std::memcpy(bytes.data() + trailer_len_pos, &short_len, sizeof(uint32_t));
  bytes.resize(trailer_len_pos + 4 + short_len);
  bytes[trailer_len_pos + 4] = 1; // digests_present

  std::istringstream input(bytes, std::ios::binary);
  spring::compression_params parsed{};
  CHECK_THROWS_AS(spring::read_compression_params(input, parsed),
                  std::runtime_error);
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