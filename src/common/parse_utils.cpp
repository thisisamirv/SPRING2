// Implements small parsing and string-conversion helpers used by CLI,
// FASTQ parsing, and metadata handling.

#include "parse_utils.h"
#include <stdexcept>
#include <string>

namespace spring {

void remove_CR_from_end(std::string &str) {
  if (!str.empty() && str.back() == '\r')
    str.resize(str.size() - 1);
}

bool has_suffix(const std::string &value, const std::string &suffix) {
  if (suffix.size() > value.size())
    return false;
  return value.ends_with(suffix);
}

int parse_int_or_throw(const std::string &value, const char *error_message) {
  try {
    size_t parsed_chars = 0;
    int parsed_value = std::stoi(value, &parsed_chars);
    if (parsed_chars != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return parsed_value;
  } catch (const std::exception &) {
    throw std::runtime_error(error_message);
  }
}

double parse_double_or_throw(const std::string &value,
                             const char *error_message) {
  try {
    size_t parsed_chars = 0;
    double parsed_value = std::stod(value, &parsed_chars);
    if (parsed_chars != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return parsed_value;
  } catch (const std::exception &) {
    throw std::runtime_error(error_message);
  }
}

uint64_t parse_uint64_or_throw(const std::string &value,
                               const char *error_message) {
  try {
    if (value.empty() || value[0] == '-') {
      throw std::invalid_argument("negative or empty");
    }
    size_t parsed_chars = 0;
    uint64_t parsed_value = std::stoull(value, &parsed_chars);
    if (parsed_chars != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return parsed_value;
  } catch (const std::exception &) {
    throw std::runtime_error(error_message);
  }
}

namespace {
// Index of the mate digit ('1') for paired-id code 3, chosen deterministically
// from a single ID so matching and modify_id agree. Prefer the first Illumina
// CASAVA token (" 1:") so names like "@sample 1 1:N:0:AAAA" flip the mate
// field rather than an earlier " 1". Fall back to the first legacy " 1".
size_t find_paired_id_code3_digit_index(const std::string &id) {
  size_t legacy_digit_index = std::string::npos;
  for (size_t i = 0; i + 1 < id.size(); ++i) {
    if (id[i] != ' ' || id[i + 1] != '1') {
      continue;
    }
    if (i + 2 < id.size() && id[i + 2] == ':') {
      return i + 1;
    }
    if (legacy_digit_index == std::string::npos) {
      legacy_digit_index = i + 1;
    }
  }
  return legacy_digit_index;
}

bool matches_paired_id_code(const std::string &id_1, const std::string &id_2,
                            const uint8_t paired_id_code) {
  if (id_1.length() != id_2.length())
    return false;

  const size_t len = id_1.length();
  switch (paired_id_code) {
  case 1:
    if (id_1[len - 1] != '1' || id_2[len - 1] != '2')
      return false;
    for (size_t index = 0; index + 1 < len; ++index)
      if (id_1[index] != id_2[index])
        return false;
    return true;
  case 2:
    return id_1 == id_2;
  case 3: {
    const size_t digit_index = find_paired_id_code3_digit_index(id_1);
    if (digit_index == std::string::npos || digit_index >= len ||
        id_2[digit_index] != '2') {
      return false;
    }
    // If id_1 used the Illumina " 1:" form, require the same colon on id_2.
    if (digit_index + 1 < len && id_1[digit_index + 1] == ':' &&
        id_2[digit_index + 1] != ':') {
      return false;
    }
    for (size_t j = 0; j < len; ++j) {
      if (j == digit_index) {
        continue;
      }
      if (id_1[j] != id_2[j]) {
        return false;
      }
    }
    return true;
  }
  default:
    return false;
  }
}
} // namespace

uint8_t find_id_pattern(const std::string &id_1, const std::string &id_2) {
  for (uint8_t code = 1; code <= 3; ++code)
    if (matches_paired_id_code(id_1, id_2, code))
      return code;
  return 0;
}

bool check_id_pattern(const std::string &id_1, const std::string &id_2,
                      uint8_t paired_id_code) {
  return matches_paired_id_code(id_1, id_2, paired_id_code);
}

void modify_id(std::string &id, uint8_t paired_id_code) {
  if (id.empty())
    return;
  if (paired_id_code == 1) {
    if (id.back() == '1')
      id.back() = '2';
  } else if (paired_id_code == 3) {
    const size_t digit_index = find_paired_id_code3_digit_index(id);
    if (digit_index != std::string::npos) {
      id[digit_index] = '2';
    }
  }
}

} // namespace spring
