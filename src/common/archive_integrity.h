// Per-member SHA-256 integrity manifests for SPRING2 tar archives.
//
// New archives store integrity.meta listing SHA-256 of every member payload
// plus a root hash over the sorted digest list. Legacy archives without the
// file skip container-hash verification.

#ifndef SPRING_ARCHIVE_INTEGRITY_H_
#define SPRING_ARCHIVE_INTEGRITY_H_

#include "fs_utils.h"
#include "sha256.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace spring {

inline constexpr const char *kIntegrityManifestName = "integrity.meta";
inline constexpr const char *kIntegrityManifestVersion = "SPRING2_INTEGRITY_V1";

struct archive_member_digest {
  std::string path;
  sha256_digest digest{};
};

struct archive_integrity_manifest {
  std::string version = kIntegrityManifestVersion;
  sha256_digest root{};
  std::vector<archive_member_digest> members; // sorted by path
};

enum class archive_integrity_status {
  verified,        // Manifest present and all digests match
  skipped_absent,  // No integrity.meta (legacy archive)
  mismatch,        // Manifest present but digests/root failed
};

struct archive_integrity_result {
  archive_integrity_status status = archive_integrity_status::skipped_absent;
  std::string detail;
};

sha256_digest compute_integrity_root(
    const std::vector<archive_member_digest> &sorted_members);

std::string serialize_integrity_manifest(
    const archive_integrity_manifest &manifest);

archive_integrity_manifest
parse_integrity_manifest(const std::string &content);

archive_integrity_manifest build_integrity_manifest_from_memory(
    const std::unordered_map<std::string, std::string> &members);

archive_integrity_manifest build_integrity_manifest_from_disk(
    const std::unordered_map<std::string, std::string> &member_disk_paths);

// Hash each source payload, append integrity.meta as an in-memory tar source.
// Existing integrity.meta entries in sources are ignored/replaced.
void append_integrity_manifest_to_sources(
    std::vector<tar_archive_source> &sources);

// Verify members in an already-extracted tar map against integrity.meta.
archive_integrity_result verify_archive_integrity(
    const std::unordered_map<std::string, std::string> &files);

// Throws on mismatch. Warns and returns on skipped_absent when warn_if_absent.
void require_archive_integrity(
    const std::unordered_map<std::string, std::string> &files,
    const std::string &archive_label, bool warn_if_absent = true);

} // namespace spring

#endif // SPRING_ARCHIVE_INTEGRITY_H_
