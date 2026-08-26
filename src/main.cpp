// Implements the Spring command-line entrypoint, including option parsing
// and dispatch to compress/decompress modes.

#include "params.h"
#include "parse_utils.h"
#include "progress.h"
#include "version.h"
#include "workflow_api.h"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// omp.h (in pch.h) may pull in <windows.h> without NOMINMAX, defining
// min/max as macros.  Undefine them here so std::min/std::max work normally.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace {

constexpr double kApproxMemoryCapPerThreadGiB = 1.0;

int default_num_threads() {
  const unsigned int hardware_threads = std::thread::hardware_concurrency();
  if (hardware_threads == 0)
    return 8;

  const int preferred_threads = static_cast<int>(hardware_threads) - 1;
  return std::min(std::max(1, preferred_threads), 16);
}

struct command_line_options {
  bool help_flag = false;
  bool version_flag = false;
  bool preview_flag = false;
  bool compress_flag = false;
  bool decompress_flag = false;
  bool pairing_only_flag = false;
  bool no_quality_flag = false;
  bool no_ids_flag = false;
  bool audit_flag = false;
  std::string r1_path;
  std::string r2_path;
  std::string r3_path;
  std::string i1_path;
  std::string i2_path;
  std::vector<std::string> input_paths;
  std::vector<std::string> output_paths;
  std::vector<std::string> quality_options;
  int num_threads = default_num_threads();
  bool num_threads_was_explicit = false;
  double memory_cap_gb = 0.0;
  int compression_level = spring::DEFAULT_COMPRESSION_LEVEL;
  std::string note;
  std::string assay = "auto";
  uint32_t cb_len = 16;
  spring::log_level log_level = spring::log_level::quiet;
  bool unzip_flag = false;
};

int print_invalid_mode_and_exit(const std::string &options_description) {
  std::cout << "Exactly one of compress, decompress, or preview needs to be "
               "specified \n";
  std::cout << options_description << "\n";
  return 1;
}

std::string build_options_description() {
  std::ostringstream options;
  options
      << "Allowed options:\n\n"
      << "* General Options:\n"
      << "  -h [ --help ]                   produce help message\n"
      << "  -V [ --version ]                produce version information\n"
      << "  -p [ --preview ]                inspect archive metadata without\n"
      << "                                  decompression\n"
      << "  -o [ --output ] arg             output file name\n"
      << "                                    - if not specified, it uses "
         "original input\n"
      << "                                      filenames (swapping extension "
         "to .sp during\n"
      << "                                      compression)\n"
      << "                                    - for paired end decompression, "
         "if only one file\n"
      << "                                      is specified, two output files "
         "will be created\n"
      << "                                      by suffixing .1 and .2\n"
      << "  -t [ --threads ] arg            number of threads (default:\n"
      << "                                  min(max(1, hw_threads - 1), 16))\n"
      << "  -m [ --memory ] arg (=0)        approximate memory budget in GB; "
         "reduces\n"
      << "                                  effective thread count using about "
         "1 GB per\n"
      << "                                  worker thread (0 disables)\n"
      << "  -v [ --verbose ] [arg (=info)]  logging level: info or debug "
         "(default\n"
      << "                                  without -v: progress bar)\n"
      << "---------------------------------------------------------------------"
         "-----------\n"
      << "* Compression Options:\n"
      << "  -c [ --compress ]               compress\n"
      << "  -R1 [ --R1 ] arg                input read-1 file (required)\n"
      << "  -R2 [ --R2 ] arg                input read-2 file (optional; "
         "enables paired-end mode)\n"
      << "  -R3 [ --R3 ] arg                input read-3 file (optional; "
         "requires --R2)\n"
      << "  -I1 [ --I1 ] arg                input index-read-1 file (optional; "
         "requires --R2)\n"
      << "  -I2 [ --I2 ] arg                input index-read-2 file (optional; "
         "requires --I1)\n"
      << "  -l [ --level ] arg (=6)         compression level (1-9) to use for "
         "output\n"
      << "                                  (.gz) formatting (passed to gzip "
         "unchanged\n"
      << "                                  and scaled to Zstd 1-22 "
         "internally)\n"
      << "  -s [ --strip ] arg              discard data: i (ids), o (order), "
         "q (quality)\n"
      << "                                  Example: --strip io to drop ids "
         "and order.\n"
      << "  -q [ --qmod ] arg               quality mode: possible modes are\n"
      << "                                    1. -q lossless (default)\n"
      << "                                    2. -q ill_bin (Illumina 8-level "
         "binning)\n"
      << "                                    3. -q binary thr high low "
         "(binary (2-level)\n"
      << "                                      thresholding, quality binned "
         "to high if >=\n"
      << "                                      thr and to low if < thr)\n"
      << "  -n [ --note ] arg               add a custom note to the archive\n"
      << "  -y [ --assay ] arg (=auto)      specify assay type. Valid "
         "choices:\n"
      << "                                  auto, dna, rna, atac, bisulfite,\n"
      << "                                  sc-rna, sc-atac, sc-bisulfite\n"
      << "  -b [ --cb-len ] arg (=16)       cellular barcode length in bases.\n"
      << "                                  Used when --assay is sc-rna,\n"
      << "                                  sc-atac, or sc-bisulfite and no "
         "I1\n"
      << "                                  lane is provided. Ignored when I1\n"
      << "                                  is present (auto-detected).\n"
      << "  -a [ --audit ]                  enable integrity verification; "
         "with --preview,\n"
      << "                                  perform full archive audit\n"
      << "---------------------------------------------------------------------"
         "-----------\n"
      << "* Decompression Options:\n"
      << "  -d [ --decompress ]             decompress\n"
      << "  -i [ --input ] arg              input archive file (.sp)\n"
      << "  -u [ --unzip ]                  during decompression, force\n"
      << "                                  output to be uncompressed (even "
         "if\n"
      << "                                  original was .gz)\n"
      << "---------------------------------------------------------------------"
         "-----------\n"
      << "* Preview Options:\n"
      << "  -p [ --preview ]                inspect archive metadata without\n"
      << "                                  decompression\n"
      << "  -i [ --input ] arg              input archive file (.sp)\n"
      << "                                  or pass <archive.sp> positionally\n"
      << "  -a [ --audit ]                  perform full archive integrity\n"
      << "                                  check without decompression\n\n"
      << "For full documentation and examples see: "
         "https://spring2.readthedocs.io/";
  return options.str();
}

bool is_option_token(const std::string &token) {
  return !token.empty() && token[0] == '-';
}

std::string strip_quotes(const std::string &value) {
  if (value.size() >= 2) {
    if ((value.front() == '"' && value.back() == '"') ||
        (value.front() == '\'' && value.back() == '\'')) {
      return value.substr(1, value.size() - 2);
    }
  }
  return value;
}

void require_value(const std::vector<std::string> &args, size_t index,
                   const char *option_name) {
  if (index >= args.size())
    throw std::runtime_error(std::string("Missing value for ") + option_name);
}

std::vector<std::string>
collect_option_values(const std::vector<std::string> &args, size_t &index) {
  std::vector<std::string> values;
  while (index < args.size() && !is_option_token(args[index])) {
    values.push_back(args[index]);
    index++;
  }
  return values;
}

spring::log_level parse_log_level(const std::string &value) {
  if (value == "info")
    return spring::log_level::info;
  if (value == "debug")
    return spring::log_level::debug;
  throw std::runtime_error("Invalid --verbose level: " + value +
                           ". Valid values are: info, debug.");
}

void parse_command_line(int argc, char **argv, command_line_options &options) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  size_t index = 0;

  while (index < args.size()) {
    const std::string &arg = args[index++];

    if (arg == "-h" || arg == "--help") {
      options.help_flag = true;
    } else if (arg == "-V" || arg == "--version") {
      options.version_flag = true;
    } else if (arg == "-p" || arg == "--preview") {
      options.preview_flag = true;
    } else if (arg == "-c" || arg == "--compress") {
      options.compress_flag = true;
    } else if (arg == "-d" || arg == "--decompress") {
      options.decompress_flag = true;
    } else if (arg == "-s" || arg == "--strip") {
      require_value(args, index, "--strip");
      const std::string strip_options = args[index++];
      for (const char c : strip_options) {
        switch (c) {
        case 'i':
          options.no_ids_flag = true;
          break;
        case 'o':
          options.pairing_only_flag = true;
          break;
        case 'q':
          options.no_quality_flag = true;
          break;
        default:
          throw std::runtime_error("Invalid character '" + std::string(1, c) +
                                   "' in --strip. Valid are: i, o, q.");
        }
      }
    } else if (arg == "-t" || arg == "--threads") {
      require_value(args, index, "--threads");
      options.num_threads = spring::parse_int_or_throw(
          args[index++], "Invalid number of threads.");
      options.num_threads_was_explicit = true;
    } else if (arg == "-m" || arg == "--memory") {
      require_value(args, index, "--memory");
      options.memory_cap_gb =
          spring::parse_double_or_throw(args[index++], "Invalid memory cap.");
    } else if (arg == "-l" || arg == "--level") {
      require_value(args, index, "--level");
      options.compression_level = spring::parse_int_or_throw(
          args[index++], "Invalid compression level.");
      if (options.compression_level < 1 || options.compression_level > 9)
        throw std::runtime_error("Compression level must be between 1 and 9.");
    } else if (arg == "-R1" || arg == "--R1") {
      require_value(args, index, "--R1");
      if (!options.r1_path.empty())
        throw std::runtime_error("--R1 can only be specified once.");
      options.r1_path = args[index++];
    } else if (arg == "-R2" || arg == "--R2") {
      require_value(args, index, "--R2");
      if (!options.r2_path.empty())
        throw std::runtime_error("--R2 can only be specified once.");
      options.r2_path = args[index++];
    } else if (arg == "-R3" || arg == "--R3") {
      require_value(args, index, "--R3");
      if (!options.r3_path.empty())
        throw std::runtime_error("--R3 can only be specified once.");
      options.r3_path = args[index++];
    } else if (arg == "-I1" || arg == "--I1") {
      require_value(args, index, "--I1");
      if (!options.i1_path.empty())
        throw std::runtime_error("--I1 can only be specified once.");
      options.i1_path = args[index++];
    } else if (arg == "-I2" || arg == "--I2") {
      require_value(args, index, "--I2");
      if (!options.i2_path.empty())
        throw std::runtime_error("--I2 can only be specified once.");
      options.i2_path = args[index++];
    } else if (arg == "-i" || arg == "--input") {
      require_value(args, index, "--input");
      const std::vector<std::string> values =
          collect_option_values(args, index);
      if (values.empty())
        throw std::runtime_error("--input requires at least 1 value.");
      options.input_paths.insert(options.input_paths.end(), values.begin(),
                                 values.end());
    } else if (arg == "-o" || arg == "--output") {
      require_value(args, index, "--output");
      const std::vector<std::string> values =
          collect_option_values(args, index);
      if (values.empty())
        throw std::runtime_error("--output requires at least 1 value.");
      options.output_paths.insert(options.output_paths.end(), values.begin(),
                                  values.end());
    } else if (arg == "-q" || arg == "--qmod") {
      require_value(args, index, "--qmod");
      options.quality_options = collect_option_values(args, index);
      if (options.quality_options.empty())
        throw std::runtime_error("--qmod requires at least 1 value.");
    } else if (arg == "-n" || arg == "--note") {
      require_value(args, index, "--note");
      options.note = strip_quotes(args[index++]);
    } else if (arg == "-y" || arg == "--assay") {
      require_value(args, index, "--assay");
      options.assay = strip_quotes(args[index++]);
      const std::vector<std::string> valid_assays = {
          "auto",      "dna",    "rna",     "atac",
          "bisulfite", "sc-rna", "sc-atac", "sc-bisulfite"};
      if (std::ranges::find(valid_assays, options.assay) ==
          valid_assays.end()) {
        throw std::runtime_error("Invalid --assay value: " + options.assay +
                                 ". Valid choices: auto, dna, rna, atac, "
                                 "bisulfite, sc-rna, sc-atac, sc-bisulfite.");
      }
    } else if (arg == "-b" || arg == "--cb-len") {
      require_value(args, index, "--cb-len");
      const int raw = std::stoi(strip_quotes(args[index++]));
      if (raw < 1 || raw > 64)
        throw std::runtime_error("--cb-len must be between 1 and 64.");
      options.cb_len = static_cast<uint32_t>(raw);
    } else if (arg == "-v" || arg == "--verbose") {
      options.log_level = spring::log_level::info;
      if (index < args.size() && !is_option_token(args[index])) {
        options.log_level = parse_log_level(args[index++]);
      }
    } else if (arg == "-a" || arg == "--audit") {
      options.audit_flag = true;
    } else if (arg == "-u" || arg == "--unzip") {
      options.unzip_flag = true;
    } else if (!arg.empty() && arg[0] != '-') {
      options.input_paths.push_back(arg);
    } else {
      throw std::runtime_error(std::string("Unknown option: ") + arg);
    }
  }

  if (options.decompress_flag && !options.compress_flag &&
      options.output_paths.size() > 5) {
    throw std::runtime_error("Decompression accepts at most 5 output files.");
  }
}

bool has_exactly_one_mode(const command_line_options &options) {
  const int mode_count = static_cast<int>(options.compress_flag) +
                         static_cast<int>(options.decompress_flag) +
                         static_cast<int>(options.preview_flag);
  return mode_count == 1;
}

bool has_valid_thread_count(const command_line_options &options) {
  return options.num_threads > 0;
}

bool has_valid_memory_cap(const command_line_options &options) {
  return options.memory_cap_gb >= 0.0;
}

void normalize_mode_specific_inputs(command_line_options &options) {
  if (options.preview_flag) {
    if (!options.r1_path.empty() || !options.r2_path.empty() ||
        !options.r3_path.empty() || !options.i1_path.empty() ||
        !options.i2_path.empty()) {
      throw std::runtime_error(
          "Preview mode does not use --R1/--R2/--R3/--I1/--I2. Use --input "
          "<archive.sp> or pass the archive path positionally.");
    }
    if (!options.output_paths.empty()) {
      throw std::runtime_error(
          "Preview mode does not produce output files. Omit --output.");
    }
    return;
  }

  if (options.compress_flag) {
    if (!options.input_paths.empty()) {
      throw std::runtime_error(
          "Compression no longer accepts --input. Use --R1 (required) and "
          "--R2 (optional).");
    }
    if (options.r1_path.empty()) {
      throw std::runtime_error(
          "Compression requires --R1 <file>. Optionally provide --R2 <file> "
          "for paired-end mode and --R3/--I1/--I2 for extra lanes.");
    }
    if (!options.r3_path.empty() && options.r2_path.empty()) {
      throw std::runtime_error("--R3 requires --R2.");
    }
    if (!options.i2_path.empty() && options.i1_path.empty()) {
      throw std::runtime_error("--I2 requires --I1.");
    }
    if (!options.i1_path.empty() && options.r2_path.empty()) {
      throw std::runtime_error(
          "--I1/--I2 currently require paired-end reads (--R2).");
    }
    options.input_paths.push_back(options.r1_path);
    if (!options.r2_path.empty()) {
      options.input_paths.push_back(options.r2_path);
    }
    if (!options.r3_path.empty()) {
      options.input_paths.push_back(options.r3_path);
    }
    if (!options.i1_path.empty()) {
      options.input_paths.push_back(options.i1_path);
    }
    if (!options.i2_path.empty()) {
      options.input_paths.push_back(options.i2_path);
    }
    return;
  }

  if (!options.r1_path.empty() || !options.r2_path.empty() ||
      !options.r3_path.empty() || !options.i1_path.empty() ||
      !options.i2_path.empty()) {
    throw std::runtime_error(
        "Decompression does not use --R1/--R2/--R3/--I1/--I2. Use --input "
        "<archive.sp>.");
  }
}

void validate_io_parameters(const command_line_options &options) {
  // Validate input files: must exist and be readable
  if (options.input_paths.empty()) {
    throw std::runtime_error("No input files specified.");
  }

  if (options.preview_flag && options.input_paths.size() != 1) {
    throw std::runtime_error("Preview requires exactly 1 input archive, but " +
                             std::to_string(options.input_paths.size()) +
                             " provided.");
  }

  for (const auto &path : options.input_paths) {
    if (path.empty()) {
      throw std::runtime_error("Input path cannot be empty.");
    }
    if (!std::filesystem::exists(path)) {
      throw std::runtime_error("Input file does not exist: " + path);
    }
    if (!std::filesystem::is_regular_file(path)) {
      throw std::runtime_error("Input path is not a regular file: " + path);
    }
  }

  // Validate output paths: directories must exist or be creatable
  if (!options.output_paths.empty()) {
    for (const auto &path : options.output_paths) {
      if (path.empty()) {
        throw std::runtime_error("Output path cannot be empty.");
      }
      const std::filesystem::path output_path(path);
      const std::filesystem::path parent_dir = output_path.parent_path();
      if (!parent_dir.empty() && !std::filesystem::exists(parent_dir)) {
        throw std::runtime_error("Output directory does not exist: " +
                                 parent_dir.generic_string());
      }
    }
  }

  // Validate compression input count: supports 1 to 5 files depending
  // on whether index reads are provided.
  if (options.compress_flag &&
      (options.input_paths.size() < 1 || options.input_paths.size() > 5)) {
    throw std::runtime_error(
        "Compression accepts between 1 and 5 input files, but " +
        std::to_string(options.input_paths.size()) + " provided.");
  }

  if (options.decompress_flag && options.input_paths.size() != 1) {
    throw std::runtime_error(
        "Decompression requires exactly 1 input archive, but " +
        std::to_string(options.input_paths.size()) + " provided.");
  }

  // Validate decompression output count: grouped archives may require up to 5
  // outputs (R1, R2, R3, I1, I2).
  if (options.decompress_flag && options.output_paths.size() > 5) {
    throw std::runtime_error(
        "Decompression accepts at most 5 output files, but " +
        std::to_string(options.output_paths.size()) + " specified.");
  }
}

int max_threads_for_memory_cap_gb(const double memory_cap_gb) {
  if (memory_cap_gb <= 0.0)
    return 0;

  const int capped_threads = static_cast<int>(
      std::floor(memory_cap_gb / kApproxMemoryCapPerThreadGiB));
  return std::max(1, capped_threads);
}

void apply_memory_cap(command_line_options &options) {
  const int memory_capped_threads =
      max_threads_for_memory_cap_gb(options.memory_cap_gb);
  if (memory_capped_threads == 0 ||
      options.num_threads <= memory_capped_threads)
    return;

  if (options.num_threads_was_explicit) {
    std::cout << "Memory cap detected; reducing requested thread count from "
              << options.num_threads << " to " << memory_capped_threads
              << ".\n";
  } else {
    std::cout << "Memory cap detected; reducing default thread count from "
              << options.num_threads << " to " << memory_capped_threads
              << ".\n";
  }
  options.num_threads = memory_capped_threads;
}

int print_unexpected_error_and_exit(const std::string &error_message) {
  std::cout << error_message << "\n";
  return 1;
}

void run_requested_mode(const command_line_options &options) {
  if (options.preview_flag) {
    spring::preview(options.input_paths.front(), options.audit_flag);
    return;
  }

  if (options.compress_flag) {
    spring::compress(
        options.input_paths, options.output_paths, options.num_threads,
        options.pairing_only_flag, options.no_quality_flag, options.no_ids_flag,
        options.quality_options, options.compression_level, options.note,
        options.log_level, options.audit_flag, options.r3_path, options.i1_path,
        options.i2_path, options.assay,
        options
            .i1_path, // cb_source_path: use I1 lane for SC barcode extraction
        options.cb_len, options.memory_cap_gb);
    return;
  }

  spring::decompress(options.input_paths, options.output_paths,
                     options.log_level, options.unzip_flag);
}

void log_options_for_debugging(const command_line_options &options) {
  if (!spring::Logger::is_debug_enabled())
    return;

  SPRING_LOG_DEBUG("CLI mode: " + std::string(options.compress_flag ? "compress"
                                              : options.decompress_flag
                                                  ? "decompress"
                                                  : "preview"));
  SPRING_LOG_DEBUG(
      "CLI settings: threads=" + std::to_string(options.num_threads) +
      ", memory_cap_gb=" + std::to_string(options.memory_cap_gb) +
      ", level=" + std::to_string(options.compression_level) + ", log_level=" +
      std::string(options.log_level == spring::log_level::debug ? "debug"
                                                                : "info") +
      ", audit=" + std::string(options.audit_flag ? "true" : "false") +
      ", unzip=" + std::string(options.unzip_flag ? "true" : "false"));

  SPRING_LOG_DEBUG(
      "CLI strip flags: order=" +
      std::string(options.pairing_only_flag ? "true" : "false") +
      ", quality=" + std::string(options.no_quality_flag ? "true" : "false") +
      ", ids=" + std::string(options.no_ids_flag ? "true" : "false"));

  SPRING_LOG_DEBUG(
      "CLI paths: inputs=" + std::to_string(options.input_paths.size()) +
      ", outputs=" + std::to_string(options.output_paths.size()));
}

} // namespace

void signalHandler(int signum) {
  std::cout << "Interrupt signal (" << signum << ") received.\n";
  std::cout << "Program terminated unexpectedly\n";
  std::exit(signum);
}

int main(int argc, char **argv) {
  signal(SIGINT, signalHandler);

#ifdef _WIN32
  // Make stdout/stderr fully unbuffered so every write is immediately
  // committed to the file handle before any potential crash.
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  // Set the LLVM libomp worker-thread stack size via the Windows env API.
  // libomp reads env vars through GetEnvironmentVariableW (not the CRT's
  // getenv), so _putenv_s has no effect; SetEnvironmentVariableA must be
  // used.  64 MiB gives ample headroom when /Od enlarges stack frames.
  SetEnvironmentVariableA("KMP_STACKSIZE", "67108864");
#endif

  command_line_options options;
  const std::string options_description = build_options_description();

  try {
    parse_command_line(argc, argv, options);
    spring::Logger::set_level(options.log_level);
  } catch (const std::runtime_error &e) {
    std::cout << e.what() << "\n";
    std::cout << options_description << "\n";
    return 1;
  }

  if (options.help_flag) {
    std::cout << options_description << "\n";
    return 0;
  }
  if (options.version_flag) {
    std::cout << "spring2 version " << spring::VERSION << "\n";
    return 0;
  }

  if (!has_exactly_one_mode(options))
    return print_invalid_mode_and_exit(options_description);

  if (!has_valid_thread_count(options)) {
    std::cout << "Number of threads must be positive.\n";
    return 1;
  }

  if (!has_valid_memory_cap(options)) {
    std::cout << "Memory cap must be non-negative.\n";
    return 1;
  }

  try {
    normalize_mode_specific_inputs(options);
  } catch (const std::runtime_error &e) {
    std::cout << e.what() << "\n";
    std::cout << options_description << "\n";
    return 1;
  }

  // Validate I/O parameters early: input files exist, output paths are valid.
  // This ensures any error during run_requested_mode is a true runtime error,
  // not a parameter issue.
  try {
    validate_io_parameters(options);
  } catch (const std::runtime_error &e) {
    std::cout << e.what() << "\n";
    std::cout << options_description << "\n";
    return 1;
  }

  if (options.preview_flag) {
    try {
      run_requested_mode(options);
    } catch (const std::runtime_error &e) {
      return print_unexpected_error_and_exit(
          std::string("Program terminated unexpectedly with error: ") +
          e.what());
    } catch (const std::exception &e) {
      return print_unexpected_error_and_exit(
          std::string("Program terminated unexpectedly with std::exception: ") +
          e.what());
    } catch (...) {
      return print_unexpected_error_and_exit("Program terminated unexpectedly");
    }
    return 0;
  }

  apply_memory_cap(options);
  log_options_for_debugging(options);
  try {
    run_requested_mode(options);
  } catch (const std::runtime_error &e) {
    return print_unexpected_error_and_exit(
        std::string("Program terminated unexpectedly with error: ") + e.what());
  } catch (const std::exception &e) {
    return print_unexpected_error_and_exit(
        std::string("Program terminated unexpectedly with std::exception: ") +
        e.what());
  } catch (...) {
    return print_unexpected_error_and_exit("Program terminated unexpectedly");
  }
  return 0;
}
