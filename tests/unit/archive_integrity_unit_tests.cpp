#include "../support/doctest.h"

#include "../../src/common/archive_integrity.h"
#include "../../src/common/sha256.h"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using spring::archive_integrity_status;
using spring::sha256_digest;
using spring::tar_archive_source;

TEST_CASE("SHA-256 matches NIST empty and abc digests") {
  CHECK(spring::sha256_to_hex(spring::sha256_bytes("")) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(spring::sha256_to_hex(spring::sha256_bytes("abc")) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("SHA-256 hex round-trip") {
  const sha256_digest digest = spring::sha256_bytes("spring2");
  const std::string hex = spring::sha256_to_hex(digest);
  sha256_digest parsed{};
  REQUIRE(spring::sha256_from_hex(hex, parsed));
  CHECK(parsed == digest);
  CHECK_FALSE(spring::sha256_from_hex("zz", parsed));
  CHECK_FALSE(spring::sha256_from_hex(hex.substr(0, 63), parsed));
}

TEST_CASE("integrity.meta serialize/parse and root of sorted digests") {
  std::unordered_map<std::string, std::string> members = {
      {"z.bin", "zzz"},
      {"a.bin", "aaa"},
      {"m.bin", "mmm"},
  };
  const spring::archive_integrity_manifest built =
      spring::build_integrity_manifest_from_memory(members);
  REQUIRE(built.members.size() == 3);
  CHECK(built.members[0].path == "a.bin");
  CHECK(built.members[1].path == "m.bin");
  CHECK(built.members[2].path == "z.bin");
  CHECK(built.root == spring::compute_integrity_root(built.members));

  const std::string text = spring::serialize_integrity_manifest(built);
  const spring::archive_integrity_manifest parsed =
      spring::parse_integrity_manifest(text);
  CHECK(parsed.version == spring::kIntegrityManifestVersion);
  CHECK(parsed.root == built.root);
  REQUIRE(parsed.members.size() == built.members.size());
  for (size_t i = 0; i < parsed.members.size(); ++i) {
    CHECK(parsed.members[i].path == built.members[i].path);
    CHECK(parsed.members[i].digest == built.members[i].digest);
  }
}

TEST_CASE("append_integrity_manifest_to_sources then verify") {
  std::vector<tar_archive_source> sources = {
      {.archive_path = "cp.bin",
       .disk_path = std::string(),
       .contents = "params",
       .from_memory = true},
      {.archive_path = "read_seq.bin",
       .disk_path = std::string(),
       .contents = "ACGT",
       .from_memory = true},
  };
  spring::append_integrity_manifest_to_sources(sources);
  REQUIRE(sources.size() == 3);
  CHECK(sources.back().archive_path == spring::kIntegrityManifestName);
  CHECK(sources.back().from_memory);

  std::unordered_map<std::string, std::string> files;
  for (const tar_archive_source &source : sources) {
    files[source.archive_path] = source.contents;
  }

  const spring::archive_integrity_result ok =
      spring::verify_archive_integrity(files);
  CHECK(ok.status == archive_integrity_status::verified);
  CHECK(ok.detail.find("members=2") != std::string::npos);

  files["read_seq.bin"] = "ACGG";
  const spring::archive_integrity_result bad =
      spring::verify_archive_integrity(files);
  CHECK(bad.status == archive_integrity_status::mismatch);
  CHECK(bad.detail.find("read_seq.bin") != std::string::npos);
}

TEST_CASE("verify_archive_integrity skips legacy archives without manifest") {
  std::unordered_map<std::string, std::string> files = {
      {"cp.bin", "x"},
  };
  const spring::archive_integrity_result result =
      spring::verify_archive_integrity(files);
  CHECK(result.status == archive_integrity_status::skipped_absent);
}

TEST_CASE("verify_archive_integrity rejects undeclared members") {
  std::unordered_map<std::string, std::string> members = {
      {"a.bin", "a"},
  };
  const spring::archive_integrity_manifest manifest =
      spring::build_integrity_manifest_from_memory(members);
  std::unordered_map<std::string, std::string> files = members;
  files[spring::kIntegrityManifestName] =
      spring::serialize_integrity_manifest(manifest);
  files["extra.bin"] = "sneaky";

  const spring::archive_integrity_result result =
      spring::verify_archive_integrity(files);
  CHECK(result.status == archive_integrity_status::mismatch);
  CHECK(result.detail.find("extra.bin") != std::string::npos);
}

TEST_CASE("parse_integrity_manifest rejects bad root") {
  const std::string bad =
      "version=SPRING2_INTEGRITY_V1\n"
      "root=0000000000000000000000000000000000000000000000000000000000000000\n"
      "member=a.bin\t" +
      spring::sha256_to_hex(spring::sha256_bytes("a")) + "\n";
  CHECK_THROWS_AS(spring::parse_integrity_manifest(bad), std::runtime_error);
}
