#include "smoke_test_support.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <zlib.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace smoke_test_support {

namespace fs = std::filesystem;

namespace {

std::string get_env(const char *name) {
  const char *value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

fs::path default_output_root() {
  if (std::string_view(SMOKE_TEST_OUTPUT_ROOT).empty()) {
    return fs::path("out") / "tests" / "smoke";
  }
  return fs::path(SMOKE_TEST_OUTPUT_ROOT);
}

fs::path resolve_binary_path(const char *env_name, const char *compiled_path) {
  const std::string env_value = get_env(env_name);
  if (!env_value.empty()) {
    return fs::path(env_value);
  }

  if (!std::string_view(compiled_path).empty()) {
    return fs::path(compiled_path);
  }

  const std::string build_dir = get_env("BUILD_DIR");
  if (!build_dir.empty()) {
#ifdef _WIN32
    return fs::path(build_dir) / "spring2.exe";
#else
    return fs::path(build_dir) / "spring2";
#endif
  }

  return {};
}

fs::path spring_binary_path() {
  const fs::path path = resolve_binary_path("SPRING_BIN", SPRING2_EXECUTABLE);
  INFO("Unable to resolve SPRING smoke-test binary path");
  REQUIRE(!path.empty());
  const std::string missing_message =
      std::string("Missing SPRING binary: ") + path.string();
  INFO(missing_message);
  REQUIRE(fs::exists(path));
  return path;
}

fs::path preview_binary_path() {
  const fs::path path =
      resolve_binary_path("SPRING_PREVIEW_BIN", SPRING2_EXECUTABLE);
  INFO("Unable to resolve SPRING preview binary path");
  REQUIRE(!path.empty());
  const std::string missing_message =
      std::string("Missing SPRING preview binary: ") + path.string();
  INFO(missing_message);
  REQUIRE(fs::exists(path));
  return path;
}

#ifndef _WIN32
std::string quote_shell_arg(const std::string &value) {
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
      continue;
    }
    quoted.push_back(ch);
  }
  quoted.push_back('\'');
  return quoted;
}
#endif

std::vector<std::string> split_simple_args(const std::string &value) {
  std::istringstream stream(value);
  std::vector<std::string> tokens;
  for (std::string token; stream >> token;) {
    tokens.push_back(token);
  }
  return tokens;
}

bool has_flag(const std::vector<std::string> &args, std::string_view short_flag,
              std::string_view long_flag) {
  return std::any_of(args.begin(), args.end(), [&](const std::string &arg) {
    return arg == short_flag || arg == long_flag;
  });
}

void remove_existing_output_for_compress(const std::vector<std::string> &args) {
  if (!has_flag(args, "-c", "--compress")) {
    return;
  }

  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "-o" || args[i] == "--output") {
      std::error_code ec;
      fs::remove(fs::path(args[i + 1]), ec);
      return;
    }
  }
}

std::vector<std::string> prepare_command_args(std::vector<std::string> args) {
  const std::vector<std::string> extra_args =
      split_simple_args(get_env("SPRING_TEST_ARGS"));
  args.insert(args.begin(), extra_args.begin(), extra_args.end());

  const bool preview_command = has_flag(args, "-p", "--preview");
  if (!preview_command && !has_flag(args, "-v", "--verbose")) {
    args.insert(args.begin(), {"-v", "debug"});
  }

  remove_existing_output_for_compress(args);

  return args;
}

std::string build_command_string(const fs::path &executable,
                                 const std::vector<std::string> &args,
                                 const fs::path *capture_path) {
#ifdef _WIN32
  auto quote_for_log = [](const std::string &value) {
    return std::string("\"") + value + '"';
  };
#else
  auto quote_for_log = [](const std::string &value) {
    return quote_shell_arg(value);
  };
#endif

  std::ostringstream command;
  const std::string wrapper = get_env("SPRING_BIN_WRAPPER");
  if (!wrapper.empty()) {
    command << wrapper << ' ';
  }
  command << quote_for_log(executable.string());
  for (const std::string &arg : args) {
    command << ' ' << quote_for_log(arg);
  }
  if (capture_path != nullptr) {
    command << " > " << quote_for_log(capture_path->string()) << " 2>&1";
  }
  return command.str();
}

#ifdef _WIN32
std::string quote_windows_command_arg(const std::string &value) {
  if (value.find_first_of(" \t\"") == std::string::npos) {
    return value;
  }

  std::string quoted = "\"";
  size_t backslash_count = 0;
  for (const char ch : value) {
    if (ch == '\\') {
      ++backslash_count;
      continue;
    }

    if (ch == '"') {
      quoted.append(backslash_count * 2 + 1, '\\');
      quoted.push_back('"');
      backslash_count = 0;
      continue;
    }

    quoted.append(backslash_count, '\\');
    backslash_count = 0;
    quoted.push_back(ch);
  }

  quoted.append(backslash_count * 2, '\\');
  quoted.push_back('"');
  return quoted;
}

int run_process_windows(const fs::path &executable,
                        const std::vector<std::string> &args,
                        const fs::path *capture_path) {
  std::string command_line = quote_windows_command_arg(executable.string());
  for (const std::string &arg : args) {
    command_line += ' ';
    command_line += quote_windows_command_arg(arg);
  }

  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  SECURITY_ATTRIBUTES security_attributes{};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;

  HANDLE capture_handle = INVALID_HANDLE_VALUE;
  if (capture_path != nullptr) {
    capture_handle =
        CreateFileA(capture_path->string().c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, &security_attributes,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (capture_handle == INVALID_HANDLE_VALUE) {
      return -1;
    }
  }

  STARTUPINFOA startup_info{};
  startup_info.cb = sizeof(startup_info);
  if (capture_handle != INVALID_HANDLE_VALUE) {
    startup_info.dwFlags |= STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = capture_handle;
    startup_info.hStdError = capture_handle;
  }

  PROCESS_INFORMATION process_info{};
  const BOOL created =
      CreateProcessA(executable.string().c_str(), mutable_command.data(),
                     nullptr, nullptr, capture_handle != INVALID_HANDLE_VALUE,
                     0, nullptr, nullptr, &startup_info, &process_info);

  if (!created) {
    if (capture_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(capture_handle);
    }
    return static_cast<int>(GetLastError());
  }

  WaitForSingleObject(process_info.hProcess, INFINITE);

  DWORD exit_code = 1;
  GetExitCodeProcess(process_info.hProcess, &exit_code);
  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  if (capture_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(capture_handle);
  }
  return static_cast<int>(exit_code);
}
#endif

void run_command(const fs::path &executable, std::vector<std::string> args,
                 const fs::path *capture_path = nullptr) {
  args = prepare_command_args(std::move(args));

  // When no external capture path is provided, redirect subprocess
  // stdout/stderr to a temp file so it does not flood the console.
  // The captured text is attached via INFO() so doctest surfaces it only
  // when the test fails.
  static std::atomic<unsigned> tmp_counter{0};
  fs::path own_capture_path;
  const bool using_own_capture = (capture_path == nullptr);
  if (using_own_capture) {
    own_capture_path =
        fs::temp_directory_path() /
        ("spring2_smoke_" + std::to_string(tmp_counter.fetch_add(1)) + ".log");
    capture_path = &own_capture_path;
  }

  const std::string command =
      build_command_string(executable, args, capture_path);
  INFO(command);
  int status = 0;
#ifdef _WIN32
  if (get_env("SPRING_BIN_WRAPPER").empty()) {
    status = run_process_windows(executable, args, capture_path);
  } else {
    status = std::system(command.c_str());
  }
#else
  status = std::system(command.c_str());
#endif

  if (using_own_capture) {
    std::string captured;
    {
      std::ifstream log_file(own_capture_path);
      if (log_file.is_open()) {
        captured.assign(std::istreambuf_iterator<char>(log_file),
                        std::istreambuf_iterator<char>());
      }
    }
    std::error_code ec;
    fs::remove(own_capture_path, ec);
    INFO(captured);
  }

  const std::string failure_message =
      std::string("Smoke command failed: ") + command;
  INFO(failure_message);
  REQUIRE(status == 0);
}

std::string read_file_binary(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  const std::string message =
      std::string("Unable to read file: ") + path.string();
  INFO(message);
  REQUIRE(input.is_open());
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

std::string normalize_text(std::string text) {
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::vector<std::string> split_lines(const std::string &text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  for (std::string line; std::getline(stream, line);) {
    lines.push_back(line);
  }
  return lines;
}

std::string read_gzip_binary(const fs::path &path) {
  const std::string compressed = read_file_binary(path);
  z_stream stream{};
  stream.next_in =
      reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());

  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
    throw std::runtime_error("Failed to initialize gzip inflater.");
  }

  std::string output;
  char buffer[1 << 15];
  while (true) {
    stream.next_out = reinterpret_cast<Bytef *>(buffer);
    stream.avail_out = sizeof(buffer);

    const int status = inflate(&stream, Z_NO_FLUSH);
    const size_t produced = sizeof(buffer) - stream.avail_out;
    if (produced != 0) {
      output.append(buffer, produced);
    }

    if (status == Z_STREAM_END) {
      Bytef *next_in = stream.next_in;
      uInt avail_in = stream.avail_in;
      inflateEnd(&stream);

      while (avail_in > 0 && *next_in == 0) {
        ++next_in;
        --avail_in;
      }

      if (avail_in == 0) {
        break;
      }

      stream = {};
      stream.next_in = next_in;
      stream.avail_in = avail_in;
      if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        throw std::runtime_error("Failed to initialize gzip inflater.");
      }
      continue;
    }

    if (status != Z_OK) {
      inflateEnd(&stream);
      throw std::runtime_error("Failed to inflate gzip data.");
    }
  }

  return output;
}

unsigned detect_cpu_threads() {
  const unsigned detected = std::thread::hardware_concurrency();
  return detected == 0 ? 1u : detected;
}

unsigned capped_threads(const char *env_name, unsigned fallback) {
  const std::string requested_text = get_env(env_name);
  unsigned requested = fallback;
  if (!requested_text.empty()) {
    requested = static_cast<unsigned>(std::max(1, std::stoi(requested_text)));
  }
  return std::min(requested, detect_cpu_threads());
}

std::string sanitize_name(std::string name) {
  std::replace_if(
      name.begin(), name.end(),
      [](unsigned char ch) {
        return !std::isalnum(ch) && ch != '-' && ch != '_';
      },
      '_');
  return name;
}

} // namespace

SmokeWorkspace::SmokeWorkspace(const std::string &name) {
  static std::atomic<unsigned> counter{0};

  const fs::path root = default_output_root();
  fs::create_directories(root);
  dir = (root /
         (sanitize_name(name) + "." + std::to_string(counter.fetch_add(1))))
            .string();

  std::error_code ec;
  fs::remove_all(fs::path(dir), ec);
  fs::create_directories(fs::path(dir));
}

SmokeWorkspace::~SmokeWorkspace() {
  std::error_code ec;
  fs::remove_all(fs::path(dir), ec);
}

std::string SmokeWorkspace::path(const std::string &name) const {
  return (fs::path(dir) / name).string();
}

std::string asset_path(const std::string &name) {
  const fs::path path = fs::path(SMOKE_TEST_ASSET_DIR) / name;
  const std::string message =
      std::string("Missing smoke asset: ") + path.string();
  INFO(message);
  REQUIRE(fs::exists(path));
  return path.string();
}

void run_spring(const std::vector<std::string> &args) {
  run_command(spring_binary_path(), args);
}

std::string run_preview_capture(const std::string &archive_path) {
  const fs::path archive_fs_path(archive_path);
  const fs::path capture_path =
      archive_fs_path.parent_path() /
      (archive_fs_path.filename().string() + ".preview.out");
  run_command(preview_binary_path(), {"-p", archive_path}, &capture_path);
  return read_file_binary(capture_path);
}

void expect_text_match(const std::string &actual_path,
                       const std::string &expected_path) {
  const std::string actual = normalize_text(read_file_binary(actual_path));
  const std::string expected = normalize_text(read_file_binary(expected_path));
  CHECK(actual == expected);
}

void expect_gzip_output_matches(const std::string &actual_gzip_path,
                                const std::string &expected_text_path) {
  const std::string actual = normalize_text(read_gzip_binary(actual_gzip_path));
  const std::string expected =
      normalize_text(read_file_binary(expected_text_path));
  CHECK(actual == expected);
}

void expect_line_count_match(const std::string &actual_path,
                             const std::string &expected_path) {
  const auto actual_lines =
      split_lines(normalize_text(read_file_binary(actual_path)));
  const auto expected_lines =
      split_lines(normalize_text(read_file_binary(expected_path)));
  CHECK(actual_lines.size() == expected_lines.size());
}

void expect_sorted_match(const std::string &actual_path,
                         const std::string &expected_path) {
  auto actual_lines =
      split_lines(normalize_text(read_file_binary(actual_path)));
  auto expected_lines =
      split_lines(normalize_text(read_file_binary(expected_path)));
  std::sort(actual_lines.begin(), actual_lines.end());
  std::sort(expected_lines.begin(), expected_lines.end());
  CHECK(actual_lines == expected_lines);
}

void expect_files_identical(const std::string &left_path,
                            const std::string &right_path) {
  const std::string left = read_file_binary(left_path);
  const std::string right = read_file_binary(right_path);
  const bool match = left == right;
  CHECK_MESSAGE(match, "Binary mismatch: "
                           << left_path << " (" << left.size() << " bytes) vs "
                           << right_path << " (" << right.size() << " bytes)");
}

#ifdef _WIN32
void set_env_var(const char *name, const char *value) {
  if (value == nullptr) {
    // Empty value via _putenv_s leaves a defined empty string; use NAME= to
    // remove it so getenv returns nullptr (matches unsetenv on POSIX).
    _putenv((std::string(name) + "=").c_str());
    SetEnvironmentVariableA(name, nullptr);
    return;
  }
  _putenv_s(name, value);
  SetEnvironmentVariableA(name, value);
}
#endif

ScopedEnvVar::ScopedEnvVar(const char *name, const char *value)
    : name(name), had_prior(false) {
  const char *existing = std::getenv(name);
  if (existing != nullptr) {
    had_prior = true;
    prior = existing;
  }
#ifdef _WIN32
  set_env_var(name, value);
#else
  if (value == nullptr) {
    unsetenv(name);
  } else {
    setenv(name, value, 1);
  }
#endif
}

ScopedEnvVar::~ScopedEnvVar() {
#ifdef _WIN32
  if (had_prior) {
    set_env_var(name, prior.c_str());
  } else {
    set_env_var(name, nullptr);
  }
#else
  if (had_prior) {
    setenv(name, prior.c_str(), 1);
  } else {
    unsetenv(name);
  }
#endif
}

unsigned smoke_compress_threads() {
  return capped_threads("SMOKE_THREADS_COMPRESS", 8u);
}

unsigned smoke_decompress_threads() {
  return capped_threads("SMOKE_THREADS_DECOMPRESS", 5u);
}

} // namespace smoke_test_support