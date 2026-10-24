#include "../support/doctest.h"

#include "../../src/common/bundle_manifest.h"
#include "../../src/workflow/workflow_internal.h"

#include <stdexcept>
#include <string>

using spring::bundle_manifest;

TEST_CASE("bundle_manifest serializes aliased R3 mate digests") {
  bundle_manifest manifest{};
  manifest.version = spring::kBundleVersion;
  manifest.read_archive_name = "reads.sp";
  manifest.has_r3 = true;
  manifest.read3_alias_source = "R1";
  manifest.read3_alias_digests_present = true;
  manifest.read3_alias_sequence_crc = 11;
  manifest.read3_alias_quality_crc = 22;
  manifest.read3_alias_id_crc = 33;
  manifest.r1_name = "r1.fastq";
  manifest.r2_name = "r2.fastq";
  manifest.r3_name = "r3.fastq";

  const std::string text = spring::serialize_bundle_manifest(manifest);
  CHECK(text.find("read3_alias_digests=1") != std::string::npos);
  CHECK(text.find("read3_alias_sequence_crc=11") != std::string::npos);

  const bundle_manifest parsed = spring::read_bundle_manifest_from_string(text);
  CHECK(parsed.read3_alias_source == "R1");
  CHECK(parsed.read3_alias_digests_present);
  CHECK(parsed.read3_alias_sequence_crc == 11);
  CHECK(parsed.read3_alias_quality_crc == 22);
  CHECK(parsed.read3_alias_id_crc == 33);
}

TEST_CASE("bundle_manifest rejects incomplete aliased R3 digests") {
  const std::string text =
      "version=SPRING2_BUNDLE_V1\n"
      "read_archive=reads.sp\n"
      "has_r3=1\n"
      "read3_archive=\n"
      "read3_alias_source=R1\n"
      "read3_alias_digests=1\n"
      "read3_alias_sequence_crc=1\n"
      "has_index=0\n"
      "index_archive=\n"
      "has_i2=0\n"
      "r1_name=r1.fastq\n"
      "r2_name=r2.fastq\n"
      "r3_name=r3.fastq\n"
      "i1_name=\n"
      "i2_name=\n";
  CHECK_THROWS_AS(spring::read_bundle_manifest_from_string(text),
                  std::runtime_error);
}

TEST_CASE("verify_read3_alias_binding fails audit when mate digests absent") {
  bundle_manifest manifest{};
  manifest.version = spring::kBundleVersion;
  manifest.read_archive_name = "reads.sp";
  manifest.has_r3 = true;
  manifest.read3_alias_source = "R1";
  manifest.read3_alias_digests_present = false;
  manifest.r1_name = "r1.fastq";
  manifest.r2_name = "r2.fastq";
  manifest.r3_name = "r3.fastq";

  CHECK_THROWS_AS(spring::verify_read3_alias_binding(manifest, "not-a-tar",
                                                     /*require_digests=*/true),
                  std::runtime_error);
}
