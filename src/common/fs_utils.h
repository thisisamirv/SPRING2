// Declares filesystem and tar-archive helpers used by compression,
// decompression, preview, and audit flows.

#ifndef SPRING_FS_UTILS_H_
#define SPRING_FS_UTILS_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace spring {

struct tar_archive_source {
  std::string archive_path;
  std::string disk_path;
  std::string contents;
  bool from_memory = false;
};

uint64_t detect_available_memory_bytes() noexcept;
uint64_t estimate_input_file_size_bytes(const std::string &input_path);
uint64_t
estimate_total_input_size_bytes(const std::vector<std::string> &input_paths);
std::string resolve_archive_entry_disk_path(const std::string &root_dir,
                                            const std::string &entry_name);
void write_archive_member_file(const std::string &root_dir,
                               const std::string &entry_name,
                               const std::string &contents);
void create_tar_archive_from_sources(
    const std::string &archive_path,
    const std::vector<tar_archive_source> &sources);
std::string create_tar_archive_from_sources_bytes(
    const std::vector<tar_archive_source> &sources);
void extract_tar_archive(const std::string &archive_path,
                         const std::string &target_dir);

std::unordered_map<std::string, std::string>
read_files_from_tar_memory(const std::string &archive_path,
                           const std::vector<std::string> &target_filenames);
std::unordered_map<std::string, std::string>
read_files_from_tar_bytes(const std::string &archive_contents,
                          const std::vector<std::string> &target_filenames);
std::unordered_map<std::string, std::string>
read_all_files_from_tar_memory(const std::string &archive_path);
std::unordered_map<std::string, std::string>
read_all_files_from_tar_bytes(const std::string &archive_contents);

// Stream through `archive_path` once from disk (no full-file buffering).
// Extracts files in `outer_direct_targets` directly, and for each entry in
// `nested_targets` opens the tar entry as a nested tar archive via a
// libarchive read callback, extracting only the listed inner files without
// buffering the outer entry in RAM.
// Result keys: outer direct targets as-is; nested files as "outer/inner".
std::unordered_map<std::string, std::string> read_files_from_nested_tars(
    const std::string &archive_path,
    const std::vector<std::string> &outer_direct_targets,
    const std::unordered_map<std::string, std::vector<std::string>>
        &nested_targets);

} // namespace spring

#endif // SPRING_FS_UTILS_H_
