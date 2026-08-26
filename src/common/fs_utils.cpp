// Implements filesystem and tar-archive helpers used for archive assembly,
// extraction, and in-memory member loading.

#include "fs_utils.h"
#include "io_utils.h"
#include "progress.h"
#include <archive.h>
#include <archive_entry.h>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <share.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

namespace spring {

namespace {

bool path_has_gzip_suffix(const std::string &path) {
  std::string normalized_path = path;
  for (char &character : normalized_path) {
    character =
        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return normalized_path.size() >= 3 &&
         normalized_path.substr(normalized_path.size() - 3) == ".gz";
}

void validate_archive_entry_name(const std::string &entry_name) {
  if (entry_name.empty()) {
    throw std::runtime_error("Archive contains an entry with an empty path.");
  }

  const std::filesystem::path entry_path(entry_name);
  if (entry_path.is_absolute() || entry_path.has_root_name() ||
      entry_path.has_root_directory()) {
    throw std::runtime_error("Archive entry path must be relative: " +
                             entry_path.generic_string());
  }

  for (const auto &part : entry_path) {
    if (part == "..") {
      throw std::runtime_error("Archive entry path escapes root: " +
                               entry_path.generic_string());
    }
  }
}

void write_archive_memory_entry(struct archive *archive_writer,
                                const std::string &entry_path,
                                const std::string &contents) {
  validate_archive_entry_name(entry_path);

  struct archive_entry *entry = archive_entry_new();
  if (entry == nullptr) {
    throw std::runtime_error("Failed to allocate archive entry for: " +
                             entry_path);
  }

  archive_entry_set_pathname(entry, entry_path.c_str());
  archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
  archive_entry_set_filetype(entry, AE_IFREG);
  archive_entry_set_perm(entry, 0644);
  if (archive_write_header(archive_writer, entry) != ARCHIVE_OK) {
    const char *message = archive_error_string(archive_writer);
    archive_entry_free(entry);
    throw std::runtime_error(
        "Failed to write archive header for '" + entry_path + "'" +
        (message ? ": " + std::string(message) : std::string()));
  }

  if (!contents.empty()) {
    const la_ssize_t written =
        archive_write_data(archive_writer, contents.data(), contents.size());
    if (written < 0 || written != static_cast<la_ssize_t>(contents.size())) {
      const char *message = archive_error_string(archive_writer);
      archive_entry_free(entry);
      throw std::runtime_error(
          "Failed to write archive data for '" + entry_path + "'" +
          (message ? ": " + std::string(message) : std::string()));
    }
  }

  archive_entry_free(entry);
}

void write_archive_file_entry(struct archive *archive_writer,
                              const std::string &entry_path,
                              const std::string &disk_path) {
  validate_archive_entry_name(entry_path);

  std::error_code file_ec;
  const uint64_t file_size = std::filesystem::file_size(disk_path, file_ec);
  if (file_ec) {
    throw std::runtime_error("Failed to stat archive input '" + disk_path +
                             "': " + file_ec.message());
  }

  std::ifstream input(disk_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open archive input '" + disk_path +
                             "'.");
  }

  struct archive_entry *entry = archive_entry_new();
  if (entry == nullptr) {
    throw std::runtime_error("Failed to allocate archive entry for: " +
                             entry_path);
  }

  archive_entry_set_pathname(entry, entry_path.c_str());
  archive_entry_set_size(entry, static_cast<la_int64_t>(file_size));
  archive_entry_set_filetype(entry, AE_IFREG);
  archive_entry_set_perm(entry, 0644);
  if (archive_write_header(archive_writer, entry) != ARCHIVE_OK) {
    const char *message = archive_error_string(archive_writer);
    archive_entry_free(entry);
    throw std::runtime_error(
        "Failed to write archive header for '" + entry_path + "'" +
        (message ? ": " + std::string(message) : std::string()));
  }

  std::vector<char> buffer(1 << 20);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize bytes_read = input.gcount();
    if (bytes_read <= 0) {
      break;
    }

    const la_ssize_t written = archive_write_data(
        archive_writer, buffer.data(), static_cast<size_t>(bytes_read));
    if (written < 0 || written != static_cast<la_ssize_t>(bytes_read)) {
      const char *message = archive_error_string(archive_writer);
      archive_entry_free(entry);
      throw std::runtime_error(
          "Failed to write archive data for '" + entry_path + "'" +
          (message ? ": " + std::string(message) : std::string()));
    }
  }

  if (!input.eof() && input.fail()) {
    archive_entry_free(entry);
    throw std::runtime_error("Failed reading archive input '" + disk_path +
                             "'.");
  }

  archive_entry_free(entry);
}

int archive_memory_open_callback(struct archive * /*archive_writer*/,
                                 void * /*client_data*/) {
  return ARCHIVE_OK;
}

la_ssize_t archive_memory_write_callback(struct archive * /*archive_writer*/,
                                         void *client_data, const void *buffer,
                                         size_t length) {
  auto *archive_bytes = static_cast<std::string *>(client_data);
  archive_bytes->append(static_cast<const char *>(buffer), length);
  return static_cast<la_ssize_t>(length);
}

int archive_memory_close_callback(struct archive * /*archive_writer*/,
                                  void * /*client_data*/) {
  return ARCHIVE_OK;
}

std::string create_tar_archive_from_sources_impl(
    const std::vector<tar_archive_source> &sources, const bool to_memory,
    const std::string *archive_path) {
  struct archive *archive_writer = archive_write_new();
  uint64_t archived_file_count = 0;
  uint64_t archived_total_bytes = 0;
  std::string archive_bytes;

  auto close_archive = [&]() noexcept {
    if (archive_writer != nullptr) {
      archive_write_close(archive_writer);
      archive_write_free(archive_writer);
      archive_writer = nullptr;
    }
  };

  try {
    archive_write_set_format_pax_restricted(archive_writer);
    if (to_memory) {
      for (const tar_archive_source &source : sources) {
        if (source.from_memory) {
          archived_total_bytes += static_cast<uint64_t>(source.contents.size());
          continue;
        }

        std::error_code file_ec;
        const uint64_t file_size =
            std::filesystem::file_size(source.disk_path, file_ec);
        if (file_ec) {
          throw std::runtime_error("Failed to stat archive input '" +
                                   source.disk_path +
                                   "': " + file_ec.message());
        }
        archived_total_bytes += file_size;
      }
      archive_bytes.reserve(static_cast<size_t>(archived_total_bytes));
      if (archive_write_open(archive_writer, &archive_bytes,
                             archive_memory_open_callback,
                             archive_memory_write_callback,
                             archive_memory_close_callback) != ARCHIVE_OK) {
        const char *message = archive_error_string(archive_writer);
        throw std::runtime_error(
            "Failed to open archive in memory" +
            (message ? ": " + std::string(message) : std::string()));
      }
    } else {
      if (archive_write_open_filename(archive_writer, archive_path->c_str()) !=
          ARCHIVE_OK) {
        const char *message = archive_error_string(archive_writer);
        throw std::runtime_error(
            "Failed to open archive for writing" +
            (message ? ": " + std::string(message) : std::string()));
      }
    }

    archived_total_bytes = 0;
    for (const tar_archive_source &source : sources) {
      if (source.from_memory) {
        write_archive_memory_entry(archive_writer, source.archive_path,
                                   source.contents);
        archived_file_count++;
        archived_total_bytes += static_cast<uint64_t>(source.contents.size());
        continue;
      }

      write_archive_file_entry(archive_writer, source.archive_path,
                               source.disk_path);
      archived_file_count++;

      std::error_code file_ec;
      const uint64_t file_size =
          std::filesystem::file_size(source.disk_path, file_ec);
      if (file_ec) {
        throw std::runtime_error("Failed to stat archive input '" +
                                 source.disk_path + "': " + file_ec.message());
      }
      archived_total_bytes += file_size;
    }

    if (archive_write_close(archive_writer) != ARCHIVE_OK) {
      const char *message = archive_error_string(archive_writer);
      throw std::runtime_error(
          "Failed to finalize archive" +
          (message ? ": " + std::string(message) : std::string()));
    }
    archive_write_free(archive_writer);
    archive_writer = nullptr;
  } catch (...) {
    close_archive();
    throw;
  }

  SPRING_LOG_DEBUG(
      std::string(to_memory ? "create_tar_archive_from_sources_bytes"
                            : "create_tar_archive_from_sources") +
      " complete: files=" + std::to_string(archived_file_count) +
      ", total_input_bytes=" + std::to_string(archived_total_bytes));

  if (!to_memory) {
    return {};
  }

  return archive_bytes;
}

bool path_is_within_directory(const std::filesystem::path &root,
                              const std::filesystem::path &candidate) {
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  while (root_it != root.end() && candidate_it != candidate.end()) {
    if (*root_it != *candidate_it) {
      return false;
    }
    ++root_it;
    ++candidate_it;
  }
  return root_it == root.end();
}

std::filesystem::path
validated_archive_entry_destination(const std::filesystem::path &target_root,
                                    const char *entry_name) {
  if (entry_name == nullptr || entry_name[0] == '\0') {
    throw std::runtime_error("Archive contains an entry with an empty path.");
  }

  const std::filesystem::path entry_path(entry_name);
  if (entry_path.is_absolute() || entry_path.has_root_name() ||
      entry_path.has_root_directory()) {
    throw std::runtime_error("Archive contains an absolute extraction path: " +
                             entry_path.generic_string());
  }

  const std::filesystem::path destination =
      (target_root / entry_path).lexically_normal();
  if (!path_is_within_directory(target_root, destination)) {
    throw std::runtime_error(
        "Archive entry escapes the extraction directory: " +
        entry_path.generic_string());
  }

  return destination;
}

std::string normalize_archive_entry_name(std::string entry_name) {
  while (entry_name.starts_with("./")) {
    entry_name.erase(0, 2);
  }
  return entry_name;
}

// State passed to the nested-archive read callback.  Wraps the outer
// archive reader and provides a buffer so the inner reader can get data
// from the current outer entry without buffering the whole entry in RAM.
struct NestedTarReadState {
  struct archive *outer;
  std::vector<char> buf;
  explicit NestedTarReadState(struct archive *outer_reader)
      : outer(outer_reader), buf(65536) {}
};

la_ssize_t nested_tar_read_callback(struct archive * /*inner*/,
                                    void *client_data, const void **buffer) {
  auto *state = static_cast<NestedTarReadState *>(client_data);
  la_ssize_t n =
      archive_read_data(state->outer, state->buf.data(), state->buf.size());
  if (n > 0)
    *buffer = state->buf.data();
  return n; // 0 = EOF, <0 = error
}

// Reads and discards all remaining data for the current archive entry.
// Used instead of archive_read_data_skip because archive_read_data_skip is
// implemented via lseek/seek on file-backed archives and can crash on certain
// platforms (Windows ARM64) and with certain archive types (legacy spring v1
// archives opened via archive_read_open_memory with large preceding entries).
void drain_archive_entry(struct archive *a) {
  char buf[65536];
  while (archive_read_data(a, buf, sizeof(buf)) > 0) {
  }
}

std::unordered_map<std::string, std::string>
read_files_from_tar_impl(struct archive *archive_reader,
                         const std::vector<std::string> *target_filenames) {
  struct archive_entry *entry = nullptr;
  std::unordered_map<std::string, std::string> results;
  std::unordered_set<std::string> targets;
  const bool read_all = target_filenames == nullptr;

  if (!read_all) {
    targets.insert(target_filenames->begin(), target_filenames->end());
    results.reserve(targets.size());
  }

  for (;;) {
    const int header_status = archive_read_next_header(archive_reader, &entry);
    if (header_status == ARCHIVE_EOF) {
      break;
    }
    if (header_status != ARCHIVE_OK) {
      throw std::runtime_error(std::string("Failed to read archive header: ") +
                               archive_error_string(archive_reader));
    }

    const char *pathname = archive_entry_pathname(entry);
    if (pathname == nullptr) {
      throw std::runtime_error("Archive contains an entry with no path.");
    }

    const std::string entry_name = normalize_archive_entry_name(pathname);
    if (entry_name.empty()) {
      archive_read_data_skip(archive_reader);
      continue;
    }
    validate_archive_entry_name(entry_name);
    if (!read_all && !targets.contains(entry_name)) {
      archive_read_data_skip(archive_reader);
      continue;
    }

    std::string contents;
    constexpr size_t kBufferSize = 64U * 1024U;
    std::vector<char> buffer(kBufferSize);
    for (;;) {
      const la_ssize_t bytes_read = archive_read_data(
          archive_reader, buffer.data(), static_cast<size_t>(buffer.size()));
      if (bytes_read == 0) {
        break;
      }
      if (bytes_read < 0) {
        throw std::runtime_error(std::string("Failed to read archive entry '") +
                                 entry_name +
                                 "': " + archive_error_string(archive_reader));
      }
      contents.append(buffer.data(), static_cast<size_t>(bytes_read));
    }

    results.emplace(entry_name, std::move(contents));
  }

  return results;
}

} // namespace

std::string shell_quote(const std::string &value) {
#ifdef _WIN32
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted += character;
    }
  }
  quoted += '"';
  return quoted;
#else
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\"'\"'";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
#endif
}

std::string shell_path(const std::string &value) {
  return std::filesystem::path(value).generic_string();
}

uint64_t detect_available_memory_bytes() noexcept {
#ifdef _WIN32
  MEMORYSTATUSEX memory_status{};
  memory_status.dwLength = sizeof(memory_status);
  if (GlobalMemoryStatusEx(&memory_status)) {
    return static_cast<uint64_t>(memory_status.ullAvailPhys);
  }
  return 0;
#elif defined(__linux__)
  std::ifstream meminfo("/proc/meminfo");
  if (meminfo.is_open()) {
    std::string label;
    uint64_t value_kib = 0;
    std::string unit;
    while (meminfo >> label >> value_kib >> unit) {
      if (label == "MemAvailable:") {
        return value_kib * 1024ULL;
      }
    }
  }

  struct sysinfo info{};
  if (sysinfo(&info) == 0) {
    return static_cast<uint64_t>(info.freeram) *
           static_cast<uint64_t>(info.mem_unit);
  }
  return 0;
#elif defined(__APPLE__)
  mach_port_t host_port = mach_host_self();
  vm_size_t page_size = 0;
  if (host_page_size(host_port, &page_size) != KERN_SUCCESS) {
    return 0;
  }

  vm_statistics64_data_t vm_stats{};
  mach_msg_type_number_t host_size = HOST_VM_INFO64_COUNT;
  if (host_statistics64(host_port, HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&vm_stats),
                        &host_size) != KERN_SUCCESS) {
    return 0;
  }

  const uint64_t available_pages =
      static_cast<uint64_t>(vm_stats.free_count) +
      static_cast<uint64_t>(vm_stats.inactive_count);
  return available_pages * static_cast<uint64_t>(page_size);
#else
  const long available_pages = sysconf(_SC_AVPHYS_PAGES);
  const long page_size = sysconf(_SC_PAGESIZE);
  if (available_pages > 0 && page_size > 0) {
    return static_cast<uint64_t>(available_pages) *
           static_cast<uint64_t>(page_size);
  }
  return 0;
#endif
}

uint64_t estimate_input_file_size_bytes(const std::string &input_path) {
  std::error_code file_ec;
  const uint64_t disk_size = std::filesystem::file_size(input_path, file_ec);
  if (file_ec) {
    throw std::runtime_error("Failed to stat input file '" + input_path +
                             "': " + file_ec.message());
  }

  if (!path_has_gzip_suffix(input_path)) {
    return disk_size;
  }

  bool is_gzipped = false;
  uint8_t flg = 0;
  uint32_t mtime = 0;
  uint8_t xfl = 0;
  uint8_t os = 0;
  std::string name;
  bool is_bgzf = false;
  uint16_t bgzf_block_size = 0;
  uint64_t uncompressed_size = 0;
  uint64_t compressed_size = 0;
  uint32_t member_count = 0;
  extract_gzip_detailed_info(input_path, is_gzipped, flg, mtime, xfl, os, name,
                             is_bgzf, bgzf_block_size, uncompressed_size,
                             compressed_size, member_count);

  if (is_gzipped) {
    return uncompressed_size;
  }

  return disk_size;
}

uint64_t
estimate_total_input_size_bytes(const std::vector<std::string> &input_paths) {
  uint64_t total_size = 0;
  for (const std::string &input_path : input_paths) {
    total_size += estimate_input_file_size_bytes(input_path);
  }
  return total_size;
}

std::string resolve_archive_entry_disk_path(const std::string &root_dir,
                                            const std::string &entry_name) {
  const std::filesystem::path root_path(root_dir);
  std::error_code create_ec;
  std::filesystem::create_directories(root_path, create_ec);
  if (create_ec) {
    throw std::runtime_error("Failed to create archive work directory '" +
                             root_dir + "': " + create_ec.message());
  }

  const std::filesystem::path canonical_root =
      std::filesystem::weakly_canonical(root_path);
  return validated_archive_entry_destination(canonical_root, entry_name.c_str())
      .string();
}

void write_archive_member_file(const std::string &root_dir,
                               const std::string &entry_name,
                               const std::string &contents) {
  const std::filesystem::path output_path =
      resolve_archive_entry_disk_path(root_dir, entry_name);
  std::error_code create_ec;
  std::filesystem::create_directories(output_path.parent_path(), create_ec);
  if (create_ec) {
    throw std::runtime_error("Failed to create archive member directory for '" +
                             output_path.string() +
                             "': " + create_ec.message());
  }

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("Failed to open archive work file '" +
                             output_path.string() + "'.");
  }

  if (!contents.empty()) {
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    if (!output) {
      throw std::runtime_error("Failed to write archive work file '" +
                               output_path.string() + "'.");
    }
  }
}

void create_tar_archive_from_sources(
    const std::string &archive_path,
    const std::vector<tar_archive_source> &sources) {
  create_tar_archive_from_sources_impl(sources, false, &archive_path);
}

std::string create_tar_archive_from_sources_bytes(
    const std::vector<tar_archive_source> &sources) {
  return create_tar_archive_from_sources_impl(sources, true, nullptr);
}

void extract_tar_archive(const std::string &archive_path,
                         const std::string &target_dir) {
  struct archive *a;
  struct archive *ext;
  struct archive_entry *entry;
  int flags;
  int r;
  uint64_t extracted_entry_count = 0;
  uint64_t extracted_data_bytes = 0;

  SPRING_LOG_DEBUG("extract_tar_archive start: archive_path=" + archive_path +
                   ", target_dir=" + target_dir);

  flags = ARCHIVE_EXTRACT_PERM;
  flags |= ARCHIVE_EXTRACT_SECURE_NODOTDOT;
  flags |= ARCHIVE_EXTRACT_SECURE_SYMLINKS;

  a = archive_read_new();
  archive_read_support_filter_gzip(a);
  archive_read_support_filter_xz(a);
  archive_read_support_filter_zstd(a);
  archive_read_support_filter_none(a);
  archive_read_support_format_tar(a);
  archive_read_support_format_empty(a);

  ext = archive_write_disk_new();
  archive_write_disk_set_options(ext, flags);

  auto close_archives = [&]() noexcept {
    if (a != nullptr) {
      archive_read_close(a);
      archive_read_free(a);
      a = nullptr;
    }
    if (ext != nullptr) {
      archive_write_close(ext);
      archive_write_free(ext);
      ext = nullptr;
    }
  };

  try {
    r = archive_read_open_filename(a, archive_path.c_str(), 10240);
    if (r != ARCHIVE_OK) {
      throw std::runtime_error("Failed to open archive for reading: " +
                               std::string(archive_error_string(a)));
    }

    std::filesystem::create_directories(target_dir);
    const std::filesystem::path target_root =
        std::filesystem::weakly_canonical(std::filesystem::path(target_dir));

    for (;;) {
      r = archive_read_next_header(a, &entry);
      if (r == ARCHIVE_EOF)
        break;
      if (r < ARCHIVE_WARN) {
        throw std::runtime_error("Error reading archive header: " +
                                 std::string(archive_error_string(a)));
      }

      const std::filesystem::path dest_path =
          validated_archive_entry_destination(target_root,
                                              archive_entry_pathname(entry));
      archive_entry_set_pathname(entry, dest_path.string().c_str());

      r = archive_write_header(ext, entry);
      extracted_entry_count++;
      if (r >= ARCHIVE_OK && archive_entry_size(entry) > 0) {
        const void *buff;
        size_t size;
        la_int64_t offset;
        while (true) {
          r = archive_read_data_block(a, &buff, &size, &offset);
          if (r == ARCHIVE_EOF)
            break;
          if (r < ARCHIVE_OK)
            throw std::runtime_error("Error reading archive data: " +
                                     std::string(archive_error_string(a)));
          const la_ssize_t write_result =
              archive_write_data_block(ext, buff, size, offset);
          extracted_data_bytes += static_cast<uint64_t>(size);
          if (write_result < ARCHIVE_OK)
            throw std::runtime_error("Error writing disk data: " +
                                     std::string(archive_error_string(ext)));
        }
      }
      r = archive_write_finish_entry(ext);
      if (r < ARCHIVE_OK)
        throw std::runtime_error("Error finishing disk entry: " +
                                 std::string(archive_error_string(ext)));
    }

    close_archives();
  } catch (...) {
    close_archives();
    throw;
  }
  SPRING_LOG_DEBUG("extract_tar_archive complete: entries=" +
                   std::to_string(extracted_entry_count) +
                   ", extracted_bytes=" + std::to_string(extracted_data_bytes));
}

std::unordered_map<std::string, std::string>
read_files_from_tar_bytes(const std::string &archive_contents,
                          const std::vector<std::string> &target_filenames) {
  struct archive *archive_reader = archive_read_new();
  std::unordered_map<std::string, std::string> results;

  auto close_archive = [&]() noexcept {
    if (archive_reader != nullptr) {
      archive_read_close(archive_reader);
      archive_read_free(archive_reader);
      archive_reader = nullptr;
    }
  };

  archive_read_support_filter_gzip(archive_reader);
  archive_read_support_filter_xz(archive_reader);
  archive_read_support_filter_zstd(archive_reader);
  archive_read_support_filter_none(archive_reader);
  archive_read_support_format_tar(archive_reader);
  archive_read_support_format_empty(archive_reader);

  try {
    const int open_status = archive_read_open_memory(
        archive_reader, archive_contents.data(), archive_contents.size());
    if (open_status != ARCHIVE_OK) {
      throw std::runtime_error(
          std::string("Failed to open archive from memory: ") +
          archive_error_string(archive_reader));
    }

    results = read_files_from_tar_impl(archive_reader, &target_filenames);
  } catch (...) {
    close_archive();
    throw;
  }

  close_archive();
  return results;
}

std::unordered_map<std::string, std::string>
read_files_from_tar_memory(const std::string &archive_path,
                           const std::vector<std::string> &target_filenames) {
  std::ifstream archive_input(archive_path, std::ios::binary);
  if (!archive_input.is_open()) {
    throw std::runtime_error("Failed to open archive: " + archive_path);
  }

  std::ostringstream contents;
  contents << archive_input.rdbuf();
  if (!archive_input.good() && !archive_input.eof()) {
    throw std::runtime_error("Failed to read archive: " + archive_path);
  }

  return read_files_from_tar_bytes(contents.str(), target_filenames);
}

std::unordered_map<std::string, std::string>
read_all_files_from_tar_bytes(const std::string &archive_contents) {
  struct archive *archive_reader = archive_read_new();
  std::unordered_map<std::string, std::string> results;

  auto close_archive = [&]() noexcept {
    if (archive_reader != nullptr) {
      archive_read_close(archive_reader);
      archive_read_free(archive_reader);
      archive_reader = nullptr;
    }
  };

  archive_read_support_filter_gzip(archive_reader);
  archive_read_support_filter_xz(archive_reader);
  archive_read_support_filter_zstd(archive_reader);
  archive_read_support_filter_none(archive_reader);
  archive_read_support_format_tar(archive_reader);
  archive_read_support_format_empty(archive_reader);

  try {
    const int open_status = archive_read_open_memory(
        archive_reader, archive_contents.data(), archive_contents.size());
    if (open_status != ARCHIVE_OK) {
      throw std::runtime_error(
          std::string("Failed to open archive from memory: ") +
          archive_error_string(archive_reader));
    }

    results = read_files_from_tar_impl(archive_reader, nullptr);
  } catch (...) {
    close_archive();
    throw;
  }

  close_archive();
  return results;
}

std::unordered_map<std::string, std::string>
read_all_files_from_tar_memory(const std::string &archive_path) {
  std::ifstream archive_input(archive_path, std::ios::binary);
  if (!archive_input.is_open()) {
    throw std::runtime_error("Failed to open archive: " + archive_path);
  }

  std::ostringstream contents;
  contents << archive_input.rdbuf();
  if (!archive_input.good() && !archive_input.eof()) {
    throw std::runtime_error("Failed to read archive: " + archive_path);
  }

  return read_all_files_from_tar_bytes(contents.str());
}

std::unordered_map<std::string, std::string> read_files_from_nested_tars(
    const std::string &archive_path,
    const std::vector<std::string> &outer_direct_targets,
    const std::unordered_map<std::string, std::vector<std::string>>
        &nested_targets) {
  std::unordered_map<std::string, std::string> results;
  const std::unordered_set<std::string> direct_set(outer_direct_targets.begin(),
                                                   outer_direct_targets.end());
  const size_t total_needed = direct_set.size() + nested_targets.size();

  struct archive *outer = archive_read_new();
  auto close_outer = [&]() noexcept {
    if (outer != nullptr) {
      archive_read_close(outer);
      archive_read_free(outer);
      outer = nullptr;
    }
  };

  archive_read_support_filter_gzip(outer);
  archive_read_support_filter_xz(outer);
  archive_read_support_filter_zstd(outer);
  archive_read_support_filter_none(outer);
  archive_read_support_format_tar(outer);
  archive_read_support_format_empty(outer);

  try {
    if (archive_read_open_filename(outer, archive_path.c_str(), 65536) !=
        ARCHIVE_OK) {
      throw std::runtime_error(std::string("Failed to open archive: ") +
                               archive_error_string(outer));
    }

    struct archive_entry *entry = nullptr;
    size_t found = 0;

    for (;;) {
      int r = archive_read_next_header(outer, &entry);
      if (r == ARCHIVE_EOF)
        break;
      if (r != ARCHIVE_OK)
        throw std::runtime_error(std::string("Archive header error: ") +
                                 archive_error_string(outer));

      const char *pathname = archive_entry_pathname(entry);
      if (pathname == nullptr) {
        drain_archive_entry(outer);
        continue;
      }

      std::string entry_name = normalize_archive_entry_name(pathname);
      if (entry_name.empty()) {
        drain_archive_entry(outer);
        continue;
      }
      validate_archive_entry_name(entry_name);

      if (direct_set.count(entry_name)) {
        std::string file_contents;
        constexpr size_t kBufSize = 64U * 1024U;
        std::vector<char> buf(kBufSize);
        for (;;) {
          la_ssize_t n = archive_read_data(outer, buf.data(), buf.size());
          if (n == 0)
            break;
          if (n < 0)
            throw std::runtime_error("Error reading archive entry '" +
                                     entry_name + "'");
          file_contents.append(buf.data(), static_cast<size_t>(n));
        }
        results.emplace(entry_name, std::move(file_contents));
        if (++found == total_needed)
          break;
        continue;
      }

      auto nested_it = nested_targets.find(entry_name);
      if (nested_it == nested_targets.end()) {
        drain_archive_entry(outer);
        continue;
      }

      // Open the outer entry as a nested tar via callback — no RAM buffer
      // for the outer entry; the inner reader pulls data 64 KB at a time.
      struct archive *inner = archive_read_new();
      auto close_inner = [&]() noexcept {
        if (inner != nullptr) {
          archive_read_close(inner);
          archive_read_free(inner);
          inner = nullptr;
        }
      };

      archive_read_support_filter_none(inner);
      archive_read_support_format_tar(inner);
      archive_read_support_format_empty(inner);

      NestedTarReadState state(outer);
      try {
        if (archive_read_open(inner, &state, nullptr, nested_tar_read_callback,
                              nullptr) != ARCHIVE_OK) {
          throw std::runtime_error(
              std::string("Failed to open nested archive '") + entry_name +
              "': " + archive_error_string(inner));
        }
        auto inner_results =
            read_files_from_tar_impl(inner, &nested_it->second);
        for (auto &[inner_name, inner_content] : inner_results) {
          results.emplace(entry_name + "/" + inner_name,
                          std::move(inner_content));
        }
      } catch (...) {
        close_inner();
        throw;
      }
      close_inner();

      if (++found == total_needed)
        break;
    }
  } catch (...) {
    close_outer();
    throw;
  }

  close_outer();
  return results;
}

} // namespace spring
