#include "legacy_id_codec.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace spring {
namespace {

constexpr uint8_t kLegacyIdAlpha = 0;
constexpr uint8_t kLegacyIdDigit = 1;
constexpr uint8_t kLegacyIdChar = 2;
constexpr uint8_t kLegacyIdMatch = 3;
constexpr uint8_t kLegacyIdZeros = 4;
constexpr uint8_t kLegacyIdDelta = 5;
constexpr uint8_t kLegacyIdEnd = 6;

constexpr uint32_t kLegacyArithmeticWordLength = 26;
constexpr size_t kMaxLegacyIdTokens = 1024;

class bit_reader {
public:
  explicit bit_reader(std::string_view bytes) : bytes_(bytes) {}

  uint8_t read_bit() {
    if (byte_offset_ >= bytes_.size()) {
      throw std::runtime_error("Truncated legacy Spring ID stream.");
    }
    const uint8_t byte = static_cast<uint8_t>(bytes_[byte_offset_]);
    const uint8_t bit = static_cast<uint8_t>((byte >> (7 - bit_offset_)) & 1U);
    ++bit_offset_;
    if (bit_offset_ == 8) {
      bit_offset_ = 0;
      ++byte_offset_;
    }
    return bit;
  }

  uint32_t read_bits(const uint8_t bit_count) {
    uint32_t value = 0;
    for (int bit = bit_count - 1; bit >= 0; --bit) {
      value |= static_cast<uint32_t>(read_bit()) << bit;
    }
    return value;
  }

private:
  std::string_view bytes_;
  size_t byte_offset_ = 0;
  uint8_t bit_offset_ = 0;
};

class arithmetic_decoder {
public:
  explicit arithmetic_decoder(std::string_view bytes)
      : reader_(bytes), l_(0), u_((1U << kLegacyArithmeticWordLength) - 1),
        t_(reader_.read_bits(kLegacyArithmeticWordLength)) {}

  uint32_t symbol_range(const uint32_t n) const {
    const uint64_t range = static_cast<uint64_t>(u_) - l_ + 1;
    const uint64_t tag_gap = static_cast<uint64_t>(t_) - l_ + 1;
    return static_cast<uint32_t>((tag_gap * n - 1) / range);
  }

  void advance(const uint32_t cum_count_x_1, const uint32_t cum_count_x,
               const uint32_t n) {
    const uint64_t range = static_cast<uint64_t>(u_) - l_ + 1;
    const uint32_t msb_shift = kLegacyArithmeticWordLength - 1;
    const uint32_t smsb_shift = kLegacyArithmeticWordLength - 2;
    const uint32_t msb_clear_mask = (1U << msb_shift) - 1;

    u_ = l_ + static_cast<uint32_t>((range * cum_count_x) / n) - 1;
    l_ = l_ + static_cast<uint32_t>((range * cum_count_x_1) / n);

    uint8_t msb_l = static_cast<uint8_t>(l_ >> msb_shift);
    uint8_t msb_u = static_cast<uint8_t>(u_ >> msb_shift);
    bool e1_e2 = msb_l == msb_u;
    bool e3 = false;
    if (!e1_e2) {
      const uint8_t smsb_l = static_cast<uint8_t>(l_ >> smsb_shift);
      const uint8_t smsb_u = static_cast<uint8_t>(u_ >> smsb_shift);
      e3 = smsb_l == 0x01 && smsb_u == 0x02;
    }

    while (e1_e2 || e3) {
      if (e1_e2) {
        l_ = (l_ & msb_clear_mask) << 1;
        u_ = ((u_ & msb_clear_mask) << 1) + 1;
        t_ = ((t_ & msb_clear_mask) << 1) + reader_.read_bit();
      } else {
        l_ = (l_ << 1) & msb_clear_mask;
        u_ = (((u_ << 1) & msb_clear_mask) | (1U << msb_shift)) + 1;
        t_ = (((t_ & msb_clear_mask) << 1) ^ (1U << msb_shift)) +
             reader_.read_bit();
      }

      msb_l = static_cast<uint8_t>(l_ >> msb_shift);
      msb_u = static_cast<uint8_t>(u_ >> msb_shift);
      e1_e2 = msb_l == msb_u;
      e3 = false;
      if (!e1_e2) {
        const uint8_t smsb_l = static_cast<uint8_t>(l_ >> smsb_shift);
        const uint8_t smsb_u = static_cast<uint8_t>(u_ >> smsb_shift);
        e3 = smsb_l == 0x01 && smsb_u == 0x02;
      }
    }
  }

private:
  bit_reader reader_;
  uint32_t l_;
  uint32_t u_;
  uint32_t t_;
};

struct stream_model {
  explicit stream_model(const uint32_t alphabet_card,
                        const uint32_t rescale_limit, const uint32_t step_size)
      : counts(alphabet_card, 1), alphabet_card(alphabet_card), step(step_size),
        n(alphabet_card), rescale(rescale_limit) {}

  std::vector<uint32_t> counts;
  uint32_t alphabet_card;
  uint32_t step;
  uint32_t n;
  uint32_t rescale;
};

using model_array = std::vector<stream_model>;

model_array make_model_array(const uint32_t context_size,
                             const uint32_t alphabet_card,
                             const uint32_t step_size,
                             const uint32_t rescale_limit) {
  model_array models;
  models.reserve(context_size);
  for (uint32_t i = 0; i < context_size; ++i) {
    models.emplace_back(alphabet_card, rescale_limit, step_size);
  }
  return models;
}

void update_model(stream_model &model, const uint32_t symbol) {
  model.counts[symbol] += model.step;
  model.n += model.step;
  if (model.n < model.rescale) {
    return;
  }

  model.n = 0;
  for (uint32_t &count : model.counts) {
    count = (count >> 1U) + 1U;
    model.n += count;
  }
}

uint32_t read_value(arithmetic_decoder &decoder, stream_model &model) {
  const uint32_t target = decoder.symbol_range(model.n);
  uint32_t cumulative = 0;
  uint32_t symbol = 0;
  for (; symbol < model.alphabet_card; ++symbol) {
    cumulative += model.counts[symbol];
    if (target < cumulative) {
      break;
    }
  }
  if (symbol >= model.alphabet_card) {
    throw std::runtime_error("Corrupt legacy Spring ID stream.");
  }

  uint32_t lower = 0;
  for (uint32_t i = 0; i < symbol; ++i) {
    lower += model.counts[i];
  }
  decoder.advance(lower, cumulative, model.n);
  update_model(model, symbol);
  return symbol;
}

struct legacy_id_models {
  explicit legacy_id_models(const uint32_t rescale_limit)
      : alpha_len(make_model_array(kMaxLegacyIdTokens, 256, 10, rescale_limit)),
        alpha_value(
            make_model_array(kMaxLegacyIdTokens, 256, 10, rescale_limit)),
        chars(make_model_array(kMaxLegacyIdTokens, 256, 10, rescale_limit)),
        integer(
            make_model_array(kMaxLegacyIdTokens * 4, 256, 10, rescale_limit)),
        delta(make_model_array(kMaxLegacyIdTokens, 256, 10, rescale_limit)),
        zero_run(make_model_array(kMaxLegacyIdTokens, 256, 10, rescale_limit)),
        token_type(
            make_model_array(kMaxLegacyIdTokens, 10, 10, rescale_limit)) {}

  model_array alpha_len;
  model_array alpha_value;
  model_array chars;
  model_array integer;
  model_array delta;
  model_array zero_run;
  model_array token_type;
};

uint32_t parse_decimal_token(const std::string &id, const uint32_t offset,
                             const uint32_t length) {
  if (offset + length > id.size()) {
    throw std::runtime_error("Corrupt legacy Spring ID token reference.");
  }
  uint32_t value = 0;
  for (uint32_t i = 0; i < length; ++i) {
    const char ch = id[offset + i];
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      throw std::runtime_error("Corrupt legacy Spring digit token.");
    }
    value = value * 10U + static_cast<uint32_t>(ch - '0');
  }
  return value;
}

void decompress_one_legacy_id(
    arithmetic_decoder &decoder, legacy_id_models &models, std::string &id,
    std::string &prev_id,
    std::array<uint32_t, kMaxLegacyIdTokens> &prev_token_offsets,
    std::array<uint32_t, kMaxLegacyIdTokens> &prev_token_lengths) {
  id.clear();

  uint32_t token_index = 0;
  for (;;) {
    if (token_index >= kMaxLegacyIdTokens) {
      throw std::runtime_error("Legacy Spring ID exceeds token limit.");
    }

    const uint32_t token = read_value(decoder, models.token_type[token_index]);
    if (token == kLegacyIdEnd) {
      break;
    }

    const uint32_t token_offset = static_cast<uint32_t>(id.size());
    uint32_t token_length = 0;
    switch (token) {
    case kLegacyIdMatch: {
      token_length = prev_token_lengths[token_index];
      const uint32_t prev_offset = prev_token_offsets[token_index];
      if (prev_offset + token_length > prev_id.size()) {
        throw std::runtime_error("Corrupt legacy Spring match token.");
      }
      id.append(prev_id, prev_offset, token_length);
      break;
    }
    case kLegacyIdAlpha: {
      token_length = read_value(decoder, models.alpha_len[token_index]);
      id.reserve(id.size() + token_length);
      for (uint32_t i = 0; i < token_length; ++i) {
        id.push_back(static_cast<char>(
            read_value(decoder, models.alpha_value[token_index])));
      }
      break;
    }
    case kLegacyIdDigit: {
      uint32_t value = 0;
      const uint32_t base_index = token_index << 2U;
      value |= read_value(decoder, models.integer[base_index]);
      value |= read_value(decoder, models.integer[base_index + 1]) << 8U;
      value |= read_value(decoder, models.integer[base_index + 2]) << 16U;
      value |= read_value(decoder, models.integer[base_index + 3]) << 24U;
      const std::string digits = std::to_string(value);
      token_length = static_cast<uint32_t>(digits.size());
      id.append(digits);
      break;
    }
    case kLegacyIdDelta: {
      const uint32_t prev_offset = prev_token_offsets[token_index];
      const uint32_t prev_length = prev_token_lengths[token_index];
      const uint32_t delta = read_value(decoder, models.delta[token_index]);
      const uint32_t value =
          parse_decimal_token(prev_id, prev_offset, prev_length) + delta;
      const std::string digits = std::to_string(value);
      token_length = static_cast<uint32_t>(digits.size());
      id.append(digits);
      break;
    }
    case kLegacyIdZeros: {
      token_length = read_value(decoder, models.zero_run[token_index]);
      id.append(token_length, '0');
      break;
    }
    case kLegacyIdChar: {
      id.push_back(
          static_cast<char>(read_value(decoder, models.chars[token_index])));
      token_length = 1;
      break;
    }
    default:
      throw std::runtime_error("Corrupt legacy Spring token type.");
    }

    prev_token_offsets[token_index] = token_offset;
    prev_token_lengths[token_index] = token_length;
    ++token_index;
  }

  if (token_index == 0) {
    prev_token_offsets.fill(0);
    prev_token_lengths.fill(0);
  }
  prev_id = id;
}

} // namespace

void decompress_legacy_id_block_bytes(std::string_view input_bytes,
                                      std::string *id_array,
                                      const uint32_t num_ids) {
  if (num_ids == 0) {
    return;
  }

  arithmetic_decoder decoder(input_bytes);
  legacy_id_models models(1U << 20U);
  std::string prev_id;
  std::array<uint32_t, kMaxLegacyIdTokens> prev_token_offsets{};
  std::array<uint32_t, kMaxLegacyIdTokens> prev_token_lengths{};

  for (uint32_t i = 0; i < num_ids; ++i) {
    decompress_one_legacy_id(decoder, models, id_array[i], prev_id,
                             prev_token_offsets, prev_token_lengths);
  }
}

} // namespace spring