// Builds and verifies per-member SHA-256 integrity manifests.

#include "archive_integrity.h"

#include "progress.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace spring {
namespace {

std::string trim_ws(const std::string &input) {
  size_t begin = 0;
  while (begin < input.size() &&
         (input[begin] == ' ' || input[begin] == '\t' || input[begin] == '\r' ||
          input[begin] == '\n')) {
    ++begin;
  }
  size_t end = input.size();
  while (end > begin && (input[end - 1] == ' ' || input[end - 1] == '\t' ||
                         input[end - 1] == '\r' || input[end - 1] == '\n')) {
    --end;
  }
  return input.substr(begin, end - begin);
}

sha256_digest hash_tar_source(const tar_archive_source &source) {
  if (source.from_memory) {
    return sha256_bytes(source.contents);
  }
  if (source.disk_path.empty()) {
    throw std::runtime_error(
        "Cannot hash archive member without memory contents or disk path: " +
        source.archive_path);
  }
  return sha256_file(source.disk_path);
}

} // namespace

sha256_digest compute_integrity_root(
    const std::vector<archive_member_digest> &sorted_members) {
  std::string canonical;
  canonical.reserve(sorted_members.size() * 96);
  for (const archive_member_digest &member : sorted_members) {
    canonical.append(member.path);
    canonical.push_back('\n');
    canonical.append(sha256_to_hex(member.digest));
    canonical.push_back('\n');
  }
  return sha256_bytes(canonical);
}

std::string serialize_integrity_manifest(
    const archive_integrity_manifest &manifest) {
  std::ostringstream output;
  output << "version=" << manifest.version << "\n";
  output << "root=" << sha256_to_hex(manifest.root) << "\n";
  for (const archive_member_digest &member : manifest.members) {
    output << "member=" << member.path << "\t" << sha256_to_hex(member.digest)
           << "\n";
  }
  return output.str();
}

archive_integrity_manifest
parse_integrity_manifest(const std::string &content) {
  archive_integrity_manifest manifest;
  bool saw_root = false;
  std::istringstream input(content);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = trim_ws(line.substr(0, eq));
    const std::string value = trim_ws(line.substr(eq + 1));
    if (key == "version") {
      manifest.version = value;
    } else if (key == "root") {
      if (!sha256_from_hex(value, manifest.root)) {
        throw std::runtime_error(
            "Invalid integrity.meta root digest (expected 64 hex chars).");
      }
      saw_root = true;
    } else if (key == "member") {
      const size_t tab = value.find('\t');
      if (tab == std::string::npos) {
        throw std::runtime_error(
            "Invalid integrity.meta member line (expected path<TAB>hex).");
      }
      archive_member_digest entry;
      entry.path = trim_ws(value.substr(0, tab));
      const std::string hex = trim_ws(value.substr(tab + 1));
      if (entry.path.empty() || entry.path == kIntegrityManifestName ||
          !sha256_from_hex(hex, entry.digest)) {
        throw std::runtime_error(
            "Invalid integrity.meta member digest for path: " + entry.path);
      }
      manifest.members.push_back(std::move(entry));
    }
  }

  if (manifest.version != kIntegrityManifestVersion) {
    throw std::runtime_error("Unsupported integrity.meta version: " +
                             manifest.version);
  }
  if (!saw_root) {
    throw std::runtime_error("integrity.meta is missing root= digest.");
  }
  if (manifest.members.empty()) {
    throw std::runtime_error("integrity.meta contains no member digests.");
  }

  std::sort(manifest.members.begin(), manifest.members.end(),
            [](const archive_member_digest &a, const archive_member_digest &b) {
              return a.path < b.path;
            });
  for (size_t i = 1; i < manifest.members.size(); ++i) {
    if (manifest.members[i].path == manifest.members[i - 1].path) {
      throw std::runtime_error(
          "integrity.meta lists duplicate member path: " +
          manifest.members[i].path);
    }
  }

  const sha256_digest expected_root = compute_integrity_root(manifest.members);
  if (expected_root != manifest.root) {
    throw std::runtime_error(
        "integrity.meta root digest does not match sorted member digest list.");
  }
  return manifest;
}

archive_integrity_manifest build_integrity_manifest_from_memory(
    const std::unordered_map<std::string, std::string> &members) {
  archive_integrity_manifest manifest;
  manifest.members.reserve(members.size());
  for (const auto &[path, contents] : members) {
    if (path == kIntegrityManifestName) {
      continue;
    }
    manifest.members.push_back(
        {.path = path, .digest = sha256_bytes(contents)});
  }
  std::sort(manifest.members.begin(), manifest.members.end(),
            [](const archive_member_digest &a, const archive_member_digest &b) {
              return a.path < b.path;
            });
  if (manifest.members.empty()) {
    throw std::runtime_error(
        "Cannot build integrity.meta: archive has no members.");
  }
  manifest.root = compute_integrity_root(manifest.members);
  return manifest;
}

archive_integrity_manifest build_integrity_manifest_from_disk(
    const std::unordered_map<std::string, std::string> &member_disk_paths) {
  archive_integrity_manifest manifest;
  manifest.members.reserve(member_disk_paths.size());
  for (const auto &[path, disk_path] : member_disk_paths) {
    if (path == kIntegrityManifestName) {
      continue;
    }
    manifest.members.push_back(
        {.path = path, .digest = sha256_file(disk_path)});
  }
  std::sort(manifest.members.begin(), manifest.members.end(),
            [](const archive_member_digest &a, const archive_member_digest &b) {
              return a.path < b.path;
            });
  if (manifest.members.empty()) {
    throw std::runtime_error(
        "Cannot build integrity.meta: archive has no members.");
  }
  manifest.root = compute_integrity_root(manifest.members);
  return manifest;
}

void append_integrity_manifest_to_sources(
    std::vector<tar_archive_source> &sources) {
  sources.erase(std::remove_if(sources.begin(), sources.end(),
                               [](const tar_archive_source &source) {
                                 return source.archive_path ==
                                        kIntegrityManifestName;
                               }),
                sources.end());

  archive_integrity_manifest manifest;
  manifest.members.reserve(sources.size());
  for (const tar_archive_source &source : sources) {
    manifest.members.push_back(
        {.path = source.archive_path, .digest = hash_tar_source(source)});
  }
  std::sort(manifest.members.begin(), manifest.members.end(),
            [](const archive_member_digest &a, const archive_member_digest &b) {
              return a.path < b.path;
            });
  if (manifest.members.empty()) {
    throw std::runtime_error(
        "Cannot append integrity.meta: no archive sources.");
  }
  for (size_t i = 1; i < manifest.members.size(); ++i) {
    if (manifest.members[i].path == manifest.members[i - 1].path) {
      throw std::runtime_error(
          "Duplicate archive member path when building integrity.meta: " +
          manifest.members[i].path);
    }
  }
  manifest.root = compute_integrity_root(manifest.members);

  sources.push_back({.archive_path = kIntegrityManifestName,
                     .disk_path = std::string(),
                     .contents = serialize_integrity_manifest(manifest),
                     .from_memory = true});
}

archive_integrity_result verify_archive_integrity(
    const std::unordered_map<std::string, std::string> &files) {
  archive_integrity_result result;
  auto manifest_it = files.find(kIntegrityManifestName);
  if (manifest_it == files.end()) {
    result.status = archive_integrity_status::skipped_absent;
    result.detail = "integrity.meta absent";
    return result;
  }

  try {
    const archive_integrity_manifest manifest =
        parse_integrity_manifest(manifest_it->second);

    for (const archive_member_digest &expected : manifest.members) {
      auto file_it = files.find(expected.path);
      if (file_it == files.end()) {
        result.status = archive_integrity_status::mismatch;
        result.detail = "missing member listed in integrity.meta: " +
                        expected.path;
        return result;
      }
      const sha256_digest actual = sha256_bytes(file_it->second);
      if (actual != expected.digest) {
        result.status = archive_integrity_status::mismatch;
        result.detail = "SHA-256 mismatch for member: " + expected.path +
                        " (expected " + sha256_to_hex(expected.digest) +
                        ", actual " + sha256_to_hex(actual) + ")";
        return result;
      }
    }

    // Reject undeclared payload members (tamper / incomplete manifest).
    for (const auto &[path, contents] : files) {
      (void)contents;
      if (path == kIntegrityManifestName) {
        continue;
      }
      const auto found = std::lower_bound(
          manifest.members.begin(), manifest.members.end(), path,
          [](const archive_member_digest &entry, const std::string &name) {
            return entry.path < name;
          });
      if (found == manifest.members.end() || found->path != path) {
        result.status = archive_integrity_status::mismatch;
        result.detail =
            "archive contains member not listed in integrity.meta: " + path;
        return result;
      }
    }

    result.status = archive_integrity_status::verified;
    result.detail = "root=" + sha256_to_hex(manifest.root) + ", members=" +
                    std::to_string(manifest.members.size());
    return result;
  } catch (const std::exception &ex) {
    result.status = archive_integrity_status::mismatch;
    result.detail = ex.what();
    return result;
  }
}

void require_archive_integrity(
    const std::unordered_map<std::string, std::string> &files,
    const std::string &archive_label, bool warn_if_absent) {
  const archive_integrity_result result = verify_archive_integrity(files);
  switch (result.status) {
  case archive_integrity_status::verified:
    SPRING_LOG_DEBUG("Container integrity verified for " + archive_label +
                     " (" + result.detail + ")");
    return;
  case archive_integrity_status::skipped_absent:
    if (warn_if_absent) {
      Logger::log_warning(
          "Archive has no integrity.meta; container SHA-256 was not verified (" +
          archive_label + ").");
    }
    return;
  case archive_integrity_status::mismatch:
    Logger::log_error("Container integrity check failed for " + archive_label +
                      ": " + result.detail);
    throw std::runtime_error(
        "ARCHIVE CONTAINER INTEGRITY CHECK FAILED: " + result.detail + " (" +
        archive_label + ")");
  }
}

} // namespace spring
