// Declares grouped bundle manifest structures and serialization helpers used
// to describe multi-member SPRING2 archives.

#ifndef SPRING_BUNDLE_MANIFEST_H_
#define SPRING_BUNDLE_MANIFEST_H_

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace spring {

inline constexpr const char *kBundleManifestName = "bundle.meta";
inline constexpr const char *kBundleVersion = "SPRING2_BUNDLE_V1";

struct bundle_manifest {
  std::string version;
  std::string read_archive_name;
  bool has_r3 = false;
  std::string read3_archive_name;
  std::string read3_alias_source;
  // When R3 aliases R1/R2, bind the label to that mate's stored CRCs so a
  // flipped alias source cannot audit/decompress as the wrong mate.
  bool read3_alias_digests_present = false;
  uint32_t read3_alias_sequence_crc = 0;
  uint32_t read3_alias_quality_crc = 0;
  uint32_t read3_alias_id_crc = 0;
  bool has_index = false;
  std::string index_archive_name;
  bool has_i2 = false;
  std::string r1_name;
  std::string r2_name;
  std::string r3_name;
  std::string i1_name;
  std::string i2_name;
};

namespace detail {

inline std::string trim_bundle_manifest_whitespace(const std::string &input) {
  size_t begin = 0;
  while (begin < input.size() &&
         (input[begin] == ' ' || input[begin] == '\t' || input[begin] == '\r' ||
          input[begin] == '\n')) {
    begin++;
  }
  size_t end = input.size();
  while (end > begin && (input[end - 1] == ' ' || input[end - 1] == '\t' ||
                         input[end - 1] == '\r' || input[end - 1] == '\n')) {
    end--;
  }
  return input.substr(begin, end - begin);
}

inline bundle_manifest parse_bundle_manifest_map(
    const std::unordered_map<std::string, std::string> &kv) {
  bundle_manifest manifest;
  auto get = [&](const char *key) -> std::string {
    auto it = kv.find(key);
    return it == kv.end() ? std::string() : it->second;
  };

  manifest.version = get("version");
  manifest.read_archive_name = get("read_archive");
  manifest.has_r3 = get("has_r3") == "1";
  manifest.read3_archive_name = get("read3_archive");
  manifest.read3_alias_source = get("read3_alias_source");
  const std::string alias_digests_flag = get("read3_alias_digests");
  const std::string alias_seq = get("read3_alias_sequence_crc");
  const std::string alias_qual = get("read3_alias_quality_crc");
  const std::string alias_id = get("read3_alias_id_crc");
  if (alias_digests_flag == "1" || !alias_seq.empty() || !alias_qual.empty() ||
      !alias_id.empty()) {
    if (alias_seq.empty() || alias_qual.empty() || alias_id.empty()) {
      throw std::runtime_error(
          "Invalid bundle manifest: aliased R3 digests require "
          "read3_alias_sequence_crc, read3_alias_quality_crc, and "
          "read3_alias_id_crc.");
    }
    try {
      manifest.read3_alias_sequence_crc =
          static_cast<uint32_t>(std::stoul(alias_seq));
      manifest.read3_alias_quality_crc =
          static_cast<uint32_t>(std::stoul(alias_qual));
      manifest.read3_alias_id_crc = static_cast<uint32_t>(std::stoul(alias_id));
    } catch (const std::exception &) {
      throw std::runtime_error(
          "Invalid bundle manifest: aliased R3 digest values must be "
          "unsigned integers.");
    }
    manifest.read3_alias_digests_present = true;
  }
  manifest.has_index = get("has_index") == "1";
  manifest.index_archive_name = get("index_archive");
  manifest.has_i2 = get("has_i2") == "1";
  manifest.r1_name = get("r1_name");
  manifest.r2_name = get("r2_name");
  manifest.r3_name = get("r3_name");
  manifest.i1_name = get("i1_name");
  manifest.i2_name = get("i2_name");

  if (manifest.version != kBundleVersion ||
      manifest.read_archive_name.empty() || manifest.r1_name.empty()) {
    throw std::runtime_error("Invalid or unsupported bundle manifest format.");
  }
  if (manifest.has_r3 && manifest.r3_name.empty()) {
    throw std::runtime_error(
        "Invalid bundle manifest: has_r3=1 but r3_name is missing.");
  }
  if (manifest.has_r3 && manifest.read3_archive_name.empty() &&
      manifest.read3_alias_source.empty()) {
    throw std::runtime_error("Invalid bundle manifest: has_r3=1 but no read3 "
                             "archive or alias source.");
  }
  if (!manifest.read3_alias_source.empty() &&
      manifest.read3_alias_source != "R1" &&
      manifest.read3_alias_source != "R2") {
    throw std::runtime_error(
        "Invalid bundle manifest: read3 alias source must be R1 or R2.");
  }
  if (manifest.has_r3 && !manifest.read3_archive_name.empty() &&
      !manifest.read3_alias_source.empty()) {
    throw std::runtime_error("Invalid bundle manifest: read3 cannot specify "
                             "both an archive and an alias source.");
  }
  if (manifest.read3_alias_digests_present &&
      manifest.read3_alias_source.empty()) {
    throw std::runtime_error(
        "Invalid bundle manifest: aliased R3 digests require "
        "read3_alias_source.");
  }
  if (manifest.has_i2 && !manifest.has_index) {
    throw std::runtime_error(
        "Invalid bundle manifest: has_i2=1 requires has_index=1.");
  }
  if (manifest.has_index && manifest.index_archive_name.empty()) {
    throw std::runtime_error(
        "Invalid bundle manifest: has_index=1 but index archive is missing.");
  }
  if (manifest.has_index && manifest.i1_name.empty()) {
    throw std::runtime_error(
        "Invalid bundle manifest: has_index=1 but i1_name is missing.");
  }
  if (manifest.has_i2 && manifest.i2_name.empty()) {
    throw std::runtime_error(
        "Invalid bundle manifest: has_i2=1 but i2_name is missing.");
  }

  return manifest;
}

} // namespace detail

inline bundle_manifest
read_bundle_manifest_from_string(const std::string &content) {
  std::unordered_map<std::string, std::string> kv;
  std::istringstream input(content);
  std::string line;
  while (std::getline(input, line)) {
    const size_t delimiter = line.find('=');
    if (delimiter == std::string::npos) {
      continue;
    }
    const std::string key =
        detail::trim_bundle_manifest_whitespace(line.substr(0, delimiter));
    const std::string value =
        detail::trim_bundle_manifest_whitespace(line.substr(delimiter + 1));
    if (!key.empty()) {
      kv[key] = value;
    }
  }
  return detail::parse_bundle_manifest_map(kv);
}

inline std::string serialize_bundle_manifest(const bundle_manifest &manifest) {
  std::ostringstream output;
  output << "version=" << manifest.version << "\n";
  output << "read_archive=" << manifest.read_archive_name << "\n";
  output << "has_r3=" << (manifest.has_r3 ? "1" : "0") << "\n";
  output << "read3_archive=" << manifest.read3_archive_name << "\n";
  output << "read3_alias_source=" << manifest.read3_alias_source << "\n";
  if (manifest.read3_alias_digests_present) {
    output << "read3_alias_digests=1\n";
    output << "read3_alias_sequence_crc=" << manifest.read3_alias_sequence_crc
           << "\n";
    output << "read3_alias_quality_crc=" << manifest.read3_alias_quality_crc
           << "\n";
    output << "read3_alias_id_crc=" << manifest.read3_alias_id_crc << "\n";
  }
  output << "has_index=" << (manifest.has_index ? "1" : "0") << "\n";
  output << "index_archive=" << manifest.index_archive_name << "\n";
  output << "has_i2=" << (manifest.has_i2 ? "1" : "0") << "\n";
  output << "r1_name=" << manifest.r1_name << "\n";
  output << "r2_name=" << manifest.r2_name << "\n";
  output << "r3_name=" << manifest.r3_name << "\n";
  output << "i1_name=" << manifest.i1_name << "\n";
  output << "i2_name=" << manifest.i2_name << "\n";
  return output.str();
}

} // namespace spring

#endif // SPRING_BUNDLE_MANIFEST_H_
