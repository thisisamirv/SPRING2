#include "integration_test_support.h"

#include "decompress/archive_stream_reader.h"
#include "workflow/archive_preview.h"
#include "workflow/workflow_api.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace spring;
using namespace integration_test_support;

namespace {

struct CompatFixture {
  std::string id;
  std::string kind;
  std::string archive;
  std::string expected_version;
  std::string ref_r1;
  std::string ref_r2;
  bool refs_gzipped = false;
  std::vector<std::string> checks;
};

std::string data_root() {
#ifdef INTEGRATION_TEST_ASSET_DIR
  return fs::path(INTEGRATION_TEST_ASSET_DIR).generic_string();
#else
  return (fs::path("..") / ".." / "data").generic_string();
#endif
}

std::string asset_path(const std::string &relative) {
  return (fs::path(data_root()) / relative).generic_string();
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

std::string trim(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::vector<std::string> split(const std::string &value, char delim) {
  std::vector<std::string> parts;
  std::string current;
  for (const char ch : value) {
    if (ch == delim) {
      parts.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  parts.push_back(current);
  return parts;
}

bool has_check(const CompatFixture &fixture, const char *name) {
  return std::find(fixture.checks.begin(), fixture.checks.end(), name) !=
         fixture.checks.end();
}

std::string load_reference_bytes(const std::string &relative_path,
                                 const bool gzipped) {
  const std::string path = asset_path(relative_path);
  REQUIRE(fs::exists(path));
  if (gzipped) {
    return read_gzip_file_binary(path);
  }
  return normalize_newlines(read_file_binary(path));
}

std::vector<CompatFixture> load_compat_manifest() {
  const fs::path manifest_path =
      fs::path(data_root()) / "compat" / "manifest.tsv";
  INFO("compat manifest=" << manifest_path.generic_string());
  REQUIRE(fs::exists(manifest_path));

  std::ifstream input(manifest_path);
  REQUIRE(input.is_open());

  std::vector<CompatFixture> fixtures;
  std::string line;
  bool saw_header = false;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (!saw_header) {
      saw_header = true;
      if (line.rfind("id\t", 0) == 0) {
        continue;
      }
    }
    const auto cols = split(line, '\t');
    REQUIRE_MESSAGE(cols.size() >= 8, "Bad manifest row: " << line);
    CompatFixture fixture;
    fixture.id = trim(cols[0]);
    fixture.kind = trim(cols[1]);
    fixture.archive = trim(cols[2]);
    fixture.expected_version = trim(cols[3]);
    fixture.ref_r1 = trim(cols[4]);
    fixture.ref_r2 = trim(cols[5]);
    fixture.refs_gzipped = trim(cols[6]) == "1";
    for (std::string check : split(cols[7], ',')) {
      check = trim(check);
      if (!check.empty()) {
        fixture.checks.push_back(check);
      }
    }
    fixtures.push_back(std::move(fixture));
  }
  REQUIRE_FALSE(fixtures.empty());
  return fixtures;
}

void check_decompress(const CompatFixture &fixture) {
  const std::string archive_path = asset_path(fixture.archive);
  REQUIRE(fs::exists(archive_path));

  const fs::path work = fs::path("compat_decompress_tmp") / fixture.id;
  fs::remove_all(work);
  fs::create_directories(work);

  if (fixture.ref_r2.empty()) {
    const fs::path output = work / "restored";
    CHECK_NOTHROW(decompress({archive_path}, {output.generic_string()}));
    const std::string restored =
        normalize_newlines(read_file_binary(output.generic_string()));
    const std::string reference =
        load_reference_bytes(fixture.ref_r1, fixture.refs_gzipped);
    check_bytes_equal(restored, reference, fixture.id.c_str());
  } else {
    const fs::path output_r1 = work / "restored.R1";
    const fs::path output_r2 = work / "restored.R2";
    CHECK_NOTHROW(decompress({archive_path}, {output_r1.generic_string(),
                                              output_r2.generic_string()}));
    check_bytes_equal(
        normalize_newlines(read_file_binary(output_r1.generic_string())),
        load_reference_bytes(fixture.ref_r1, fixture.refs_gzipped),
        (fixture.id + " R1").c_str());
    check_bytes_equal(
        normalize_newlines(read_file_binary(output_r2.generic_string())),
        load_reference_bytes(fixture.ref_r2, fixture.refs_gzipped),
        (fixture.id + " R2").c_str());
  }

  fs::remove_all(work);
}

void check_preview(const CompatFixture &fixture) {
  const std::string archive_path = asset_path(fixture.archive);
  REQUIRE(fs::exists(archive_path));

  std::ostringstream preview_output;
  auto *original_buffer = std::cout.rdbuf(preview_output.rdbuf());
  CHECK_NOTHROW(preview(archive_path, false));
  std::cout.rdbuf(original_buffer);

  const std::string output = preview_output.str();
  const std::string needle = "Archive Version:   " + fixture.expected_version;
  CHECK_MESSAGE(output.find(needle) != std::string::npos,
                "Preview missing '" << needle << "' for " << fixture.id
                                    << "\n--- preview ---\n"
                                    << output);
  if (fixture.kind == "legacy_spring") {
    CHECK(output.find(
              "Original Inputs:   Unavailable in legacy spring archives") !=
          std::string::npos);
    CHECK(output.find(
              "Assay Type:        Unavailable in legacy spring archives") !=
          std::string::npos);
  }
}

void check_audit(const CompatFixture &fixture) {
  const std::string archive_path = asset_path(fixture.archive);
  REQUIRE(fs::exists(archive_path));
  // Library preview with audit enabled exercises the same integrity path as
  // `spring2 -p -a` for current archives.
  CHECK_NOTHROW(preview(archive_path, true));
}

bool next_fastq_record(std::istringstream &stream, ReadRecord &record) {
  std::string id_line;
  std::string plus_line;
  if (!std::getline(stream, id_line)) {
    return false;
  }
  REQUIRE(std::getline(stream, record.sequence));
  REQUIRE(std::getline(stream, plus_line));
  REQUIRE(std::getline(stream, record.quality));
  REQUIRE_FALSE(id_line.empty());
  record.id = id_line.substr(1);
  return true;
}

void check_reader(const CompatFixture &fixture) {
  const std::string archive_path = asset_path(fixture.archive);
  REQUIRE(fs::exists(archive_path));
  SpringReader reader(archive_path, 1);

  if (!fixture.ref_r2.empty()) {
    std::istringstream r1_stream(
        load_reference_bytes(fixture.ref_r1, fixture.refs_gzipped));
    std::istringstream r2_stream(
        load_reference_bytes(fixture.ref_r2, fixture.refs_gzipped));
    ReadRecord actual_r1;
    ReadRecord actual_r2;
    ReadRecord expected_r1;
    ReadRecord expected_r2;
    int count = 0;
    while (reader.next(actual_r1, actual_r2)) {
      REQUIRE(next_fastq_record(r1_stream, expected_r1));
      REQUIRE(next_fastq_record(r2_stream, expected_r2));
      CHECK(normalize_record_id(actual_r1.id) ==
            normalize_record_id(expected_r1.id));
      CHECK(actual_r1.sequence == expected_r1.sequence);
      CHECK(actual_r1.quality == expected_r1.quality);
      CHECK(normalize_record_id(actual_r2.id) ==
            normalize_record_id(expected_r2.id));
      CHECK(actual_r2.sequence == expected_r2.sequence);
      CHECK(actual_r2.quality == expected_r2.quality);
      ++count;
    }
    CHECK_MESSAGE(count > 0,
                  "SpringReader produced no paired records for " << fixture.id);
    return;
  }

  ReadRecord record;
  int record_count = 0;
  while (reader.next(record)) {
    const std::string normalized_id = normalize_record_id(record.id);
    if (normalized_id.empty() || record.sequence.empty()) {
      continue;
    }
    if (!record.quality.empty()) {
      CHECK(record.quality.size() == record.sequence.size());
    }
    ++record_count;
  }
  CHECK_MESSAGE(record_count > 0,
                "SpringReader produced no records for " << fixture.id);
}

} // namespace

TEST_CASE("Compat manifest fixtures decompress under current SPRING2") {
  const auto fixtures = load_compat_manifest();
  for (const CompatFixture &fixture : fixtures) {
    INFO("fixture id=" << fixture.id << " archive=" << fixture.archive);
    REQUIRE(fs::exists(asset_path(fixture.archive)));

    if (has_check(fixture, "decompress")) {
      check_decompress(fixture);
    }
    if (has_check(fixture, "preview")) {
      check_preview(fixture);
    }
    if (has_check(fixture, "audit")) {
      check_audit(fixture);
    }
    if (has_check(fixture, "reader")) {
      check_reader(fixture);
    }
  }
}
