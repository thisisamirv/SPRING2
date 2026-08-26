// Implements the templated read reordering stage that builds aligned and
// singleton artifacts for downstream encoding.

#ifndef SPRING_READ_REORDERING_IMPL_H_
#define SPRING_READ_REORDERING_IMPL_H_

#include "bitset_dictionary.h"
#include "core_utils.h"
#include "dna_utils.h"
#include "params.h"
#include "progress.h"
#include "raii.h"
#include "read_reordering.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <memory>
#include <numeric>
#include <omp.h>
#include <sstream>
#include <utility>
#include <vector>

namespace spring {

namespace {

bool deterministic_reorder_enabled() {
  const char *value = std::getenv("SPRING_DETERMINISTIC_REORDER");
  if (value == nullptr)
    return false;
  const std::string normalized(value);
  return !(normalized.empty() || normalized == "0" || normalized == "false" ||
           normalized == "FALSE" || normalized == "off" || normalized == "OFF");
}

} // namespace

template <size_t bitset_size> struct reorder_global {
  uint32_t numreads;
  uint32_t numreads_array[2];

  int maxshift, shift_step, num_thr, max_readlen;
  int numdict;

  bool paired_end;
  std::bitset<bitset_size> mask64;
  std::bitset<bitset_size> mask_lsb;
  char depleted_base;
  std::vector<std::array<std::bitset<bitset_size>, 128>> basemask;
  std::vector<std::bitset<bitset_size> *> basemask_ptrs;
  reorder_global(int max_readlen_param)
      : numreads(0), numreads_array{0, 0}, maxshift(0), shift_step(1),
        num_thr(0), max_readlen(max_readlen_param), numdict(NUM_DICT_REORDER),
        paired_end(false), depleted_base('N') {
    mask_lsb.reset();
    for (int i = 0; i < max_readlen_param * 2; i += 2)
      mask_lsb[i] = 1;
    basemask.resize(static_cast<size_t>(max_readlen_param));
    basemask_ptrs.resize(static_cast<size_t>(max_readlen_param));
    for (int i = 0; i < max_readlen_param; i++)
      basemask_ptrs[i] = basemask[static_cast<size_t>(i)].data();
  }
  reorder_global(const reorder_global &) = delete;
  reorder_global &operator=(const reorder_global &) = delete;
  ~reorder_global() = default;
};

namespace detail {

template <typename T>
inline void append_binary(std::string &buffer, const T &value) {
  buffer.append(reinterpret_cast<const char *>(&value), sizeof(T));
}

inline void append_encoded_read(std::string &buffer, const char *read_chars,
                                const uint32_t read_length) {
  append_binary(buffer, read_length);
  buffer.append(read_chars, read_length);
}

} // namespace detail

inline void
initialize_reorder_dict_ranges(std::array<bbhashdict, NUM_DICT_REORDER> &dict,
                               const int max_readlen) {
  dict[0].start = max_readlen > 100 ? max_readlen / 2 - 32
                                    : max_readlen / 2 - max_readlen * 32 / 100;
  dict[0].end = max_readlen / 2 - 1;
  dict[1].start = max_readlen / 2;
  dict[1].end = max_readlen > 100
                    ? max_readlen / 2 - 1 + 32
                    : max_readlen / 2 - 1 + max_readlen * 32 / 100;
}

template <size_t bitset_size>
void bitsettostring(std::bitset<bitset_size> encoded_bases, char *read_chars,
                    const uint16_t readlen,
                    const reorder_global<bitset_size> &rg) {
  static const char reverse_base_lookup[4] = {'A', 'G', 'C', 'T'};
  unsigned long long packed_bases;
  for (int block_index = 0; block_index < 2 * readlen / 64 + 1; block_index++) {
    packed_bases = (encoded_bases & rg.mask64).to_ullong();
    encoded_bases >>= 64;
    for (int read_index = 32 * block_index;
         read_index < 32 * block_index + 32 && read_index < readlen;
         read_index++) {
      read_chars[read_index] = reverse_base_lookup[packed_bases % 4];
      packed_bases /= 4;
    }
  }
  read_chars[readlen] = '\0';
  return;
}

template <size_t bitset_size>
void setglobalarrays(reorder_global<bitset_size> &rg) {
  for (int i = 0; i < 64; i++)
    rg.mask64[i] = 1;
  for (int i = 0; i < rg.max_readlen; i++) {
    rg.basemask[i][(uint8_t)'A'][2 * i] = 0;
    rg.basemask[i][(uint8_t)'A'][2 * i + 1] = 0;
    rg.basemask[i][(uint8_t)'C'][2 * i] = 0;
    rg.basemask[i][(uint8_t)'C'][2 * i + 1] = 1;
    rg.basemask[i][(uint8_t)'G'][2 * i] = 1;
    rg.basemask[i][(uint8_t)'G'][2 * i + 1] = 0;
    rg.basemask[i][(uint8_t)'T'][2 * i] = 1;
    rg.basemask[i][(uint8_t)'T'][2 * i + 1] = 1;
  }
  return;
}

template <size_t bitset_size>
void updaterefcount(std::bitset<bitset_size> &current_read,
                    std::bitset<bitset_size> &reference_read,
                    std::bitset<bitset_size> &reverse_reference_read,
                    int **base_counts, const bool reset_counts,
                    const bool use_reverse_orientation, const int shift,
                    const uint16_t current_read_length, int &reference_length,
                    const reorder_global<bitset_size> &rg) {
  static const char base_lookup[4] = {'A', 'C', 'T', 'G'};
  auto base_to_index = [](uint8_t encoded_base) {
    return (encoded_base & 0x06) >> 1;
  }; // inverse of above
  char read_chars[MAX_READ_LEN + 1];
  char reverse_chars[MAX_READ_LEN + 1];
  char *oriented_read;
  bitsettostring<bitset_size>(current_read, read_chars, current_read_length,
                              rg);
  if (!use_reverse_orientation)
    oriented_read = read_chars;
  else {
    reverse_complement(read_chars, reverse_chars, current_read_length);
    oriented_read = reverse_chars;
  }

  if (reset_counts == true) {
    std::fill(base_counts[0], base_counts[0] + rg.max_readlen, 0);
    std::fill(base_counts[1], base_counts[1] + rg.max_readlen, 0);
    std::fill(base_counts[2], base_counts[2] + rg.max_readlen, 0);
    std::fill(base_counts[3], base_counts[3] + rg.max_readlen, 0);
    for (int read_index = 0; read_index < current_read_length; read_index++) {
      base_counts[base_to_index((uint8_t)oriented_read[read_index])]
                 [read_index] = 1;
    }
    reference_length = current_read_length;
  } else {
    if (!use_reverse_orientation) {
      // shift count
      for (int ref_index = 0; ref_index < reference_length - shift;
           ref_index++) {
        for (int base_index = 0; base_index < 4; base_index++)
          base_counts[base_index][ref_index] =
              base_counts[base_index][ref_index + shift];
        if (ref_index < current_read_length)
          base_counts[base_to_index((uint8_t)oriented_read[ref_index])]
                     [ref_index] += 1;
      }

      // for the new positions set count to 1
      for (int ref_index = reference_length - shift;
           ref_index < current_read_length; ref_index++) {
        for (int base_index = 0; base_index < 4; base_index++)
          base_counts[base_index][ref_index] = 0;
        base_counts[base_to_index((uint8_t)oriented_read[ref_index])]
                   [ref_index] = 1;
      }
      reference_length =
          std::max<int>(reference_length - shift, current_read_length);
    } else {
      if (current_read_length - shift >= reference_length) {
        for (int ref_index = current_read_length - shift - reference_length;
             ref_index < current_read_length - shift; ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] =
                base_counts[base_index][ref_index - (current_read_length -
                                                     shift - reference_length)];
          base_counts[base_to_index((uint8_t)oriented_read[ref_index])]
                     [ref_index] += 1;
        }
        for (int ref_index = 0;
             ref_index < current_read_length - shift - reference_length;
             ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] = 0;
          base_counts[base_to_index((uint8_t)oriented_read[ref_index])]
                     [ref_index] = 1;
        }
        for (int ref_index = current_read_length - shift;
             ref_index < current_read_length; ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] = 0;
          base_counts[base_to_index((uint8_t)oriented_read[ref_index])]
                     [ref_index] = 1;
        }
        reference_length = current_read_length;
      } else if (reference_length + shift <= rg.max_readlen) {
        for (int ref_index = reference_length - current_read_length + shift;
             ref_index < reference_length; ref_index++)
          base_counts[base_to_index((
              uint8_t)oriented_read[ref_index - (reference_length -
                                                 current_read_length + shift)])]
                     [ref_index] += 1;
        for (int ref_index = reference_length;
             ref_index < reference_length + shift; ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] = 0;
          base_counts[base_to_index((
              uint8_t)oriented_read[ref_index - (reference_length -
                                                 current_read_length + shift)])]
                     [ref_index] = 1;
        }
        reference_length = reference_length + shift;
      } else {
        for (int ref_index = 0; ref_index < rg.max_readlen - shift;
             ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] =
                base_counts[base_index][ref_index + (reference_length + shift -
                                                     rg.max_readlen)];
        }
        for (int ref_index = rg.max_readlen - current_read_length;
             ref_index < rg.max_readlen - shift; ref_index++)
          base_counts[base_to_index(
              (uint8_t)oriented_read[ref_index -
                                     (rg.max_readlen - current_read_length)])]
                     [ref_index] += 1;
        for (int ref_index = rg.max_readlen - shift; ref_index < rg.max_readlen;
             ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] = 0;
          base_counts[base_to_index(
              (uint8_t)oriented_read[ref_index -
                                     (rg.max_readlen - current_read_length)])]
                     [ref_index] = 1;
        }
        reference_length = rg.max_readlen;
      }
    }
    for (int ref_index = 0; ref_index < reference_length; ref_index++) {
      int best_base_count = 0;
      int best_base_index = 0;
      for (int base_index = 0; base_index < 4; base_index++)
        if (base_counts[base_index][ref_index] > best_base_count) {
          best_base_count = base_counts[base_index][ref_index];
          best_base_index = base_index;
        }
      oriented_read[ref_index] = base_lookup[best_base_index];
    }
  }
  chartobitset<bitset_size>(
      oriented_read, reference_length, reference_read,
      const_cast<std::bitset<bitset_size> **>(rg.basemask_ptrs.data()));
  char reverse_oriented_read[MAX_READ_LEN + 1];
  reverse_complement(oriented_read, reverse_oriented_read, reference_length);
  chartobitset<bitset_size>(
      reverse_oriented_read, reference_length, reverse_reference_read,
      const_cast<std::bitset<bitset_size> **>(rg.basemask_ptrs.data()));

  return;
}

template <size_t bitset_size>
void readDnaFile(std::bitset<bitset_size> *read, uint16_t *read_lengths,
                 const reorder_input_artifact &input_artifact,
                 const reorder_global<bitset_size> &rg) {
  size_t cursor = 0;
  for (uint32_t i = 0; i < rg.numreads_array[0]; i++) {
    if (cursor + sizeof(uint16_t) >
        input_artifact.clean_read_streams[0].size()) {
      throw std::runtime_error("Truncated clean read stream for mate 1.");
    }
    std::memcpy(&read_lengths[i],
                input_artifact.clean_read_streams[0].data() + cursor,
                sizeof(uint16_t));
    cursor += sizeof(uint16_t);
    uint16_t num_bytes_to_read = ((uint32_t)read_lengths[i] + 4 - 1) / 4;
    if (cursor + num_bytes_to_read >
        input_artifact.clean_read_streams[0].size()) {
      throw std::runtime_error(
          "Truncated encoded clean read payload for mate 1.");
    }
    std::memcpy(byte_ptr(&read[i]),
                input_artifact.clean_read_streams[0].data() + cursor,
                num_bytes_to_read);
    cursor += num_bytes_to_read;
  }
  if (rg.paired_end) {
    cursor = 0;
    for (uint32_t i = rg.numreads_array[0];
         i < rg.numreads_array[0] + rg.numreads_array[1]; i++) {
      if (cursor + sizeof(uint16_t) >
          input_artifact.clean_read_streams[1].size()) {
        throw std::runtime_error("Truncated clean read stream for mate 2.");
      }
      std::memcpy(&read_lengths[i],
                  input_artifact.clean_read_streams[1].data() + cursor,
                  sizeof(uint16_t));
      cursor += sizeof(uint16_t);
      uint16_t num_bytes_to_read = ((uint32_t)read_lengths[i] + 4 - 1) / 4;
      if (cursor + num_bytes_to_read >
          input_artifact.clean_read_streams[1].size()) {
        throw std::runtime_error(
            "Truncated encoded clean read payload for mate 2.");
      }
      std::memcpy(byte_ptr(&read[i]),
                  input_artifact.clean_read_streams[1].data() + cursor,
                  num_bytes_to_read);
      cursor += num_bytes_to_read;
    }
  }
  return;
}

// Reads exactly the number of reads specified by rg.numreads_array from the
// clean_read_streams, starting at the byte offsets given by cursor1/cursor2.
// Both cursors are advanced past the consumed bytes on return.
template <size_t bitset_size>
void readDnaFileChunk(std::bitset<bitset_size> *read, uint16_t *read_lengths,
                      const reorder_input_artifact &input_artifact,
                      size_t &cursor1, size_t &cursor2,
                      const reorder_global<bitset_size> &rg) {
  for (uint32_t i = 0; i < rg.numreads_array[0]; i++) {
    if (cursor1 + sizeof(uint16_t) >
        input_artifact.clean_read_streams[0].size())
      throw std::runtime_error("Truncated chunk read stream for mate 1.");
    std::memcpy(&read_lengths[i],
                input_artifact.clean_read_streams[0].data() + cursor1,
                sizeof(uint16_t));
    cursor1 += sizeof(uint16_t);
    uint16_t nbytes = static_cast<uint16_t>(
        (static_cast<uint32_t>(read_lengths[i]) + 4 - 1) / 4);
    if (cursor1 + nbytes > input_artifact.clean_read_streams[0].size())
      throw std::runtime_error("Truncated chunk read payload for mate 1.");
    std::memcpy(byte_ptr(&read[i]),
                input_artifact.clean_read_streams[0].data() + cursor1, nbytes);
    cursor1 += nbytes;
  }
  for (uint32_t i = rg.numreads_array[0]; i < rg.numreads; i++) {
    if (cursor2 + sizeof(uint16_t) >
        input_artifact.clean_read_streams[1].size())
      throw std::runtime_error("Truncated chunk read stream for mate 2.");
    std::memcpy(&read_lengths[i],
                input_artifact.clean_read_streams[1].data() + cursor2,
                sizeof(uint16_t));
    cursor2 += sizeof(uint16_t);
    uint16_t nbytes = static_cast<uint16_t>(
        (static_cast<uint32_t>(read_lengths[i]) + 4 - 1) / 4);
    if (cursor2 + nbytes > input_artifact.clean_read_streams[1].size())
      throw std::runtime_error("Truncated chunk read payload for mate 2.");
    std::memcpy(byte_ptr(&read[i]),
                input_artifact.clean_read_streams[1].data() + cursor2, nbytes);
    cursor2 += nbytes;
  }
}

// Reads exactly the number of reads specified by rg.numreads_array from FILE*
// handles positioned at the start of the next unread bytes (no seeking needed).
template <size_t bitset_size>
void readDnaFileChunkFromFile(std::bitset<bitset_size> *read,
                              uint16_t *read_lengths, std::FILE *file0,
                              std::FILE *file1,
                              const reorder_global<bitset_size> &rg) {
  auto read_from = [](std::FILE *f, std::bitset<bitset_size> *reads,
                      uint16_t *lens, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
      if (std::fread(&lens[i], sizeof(uint16_t), 1, f) != 1)
        throw std::runtime_error("Truncated spilled stream at read length.");
      uint16_t nbytes =
          static_cast<uint16_t>((static_cast<uint32_t>(lens[i]) + 4 - 1) / 4);
      if (std::fread(byte_ptr(&reads[i]), nbytes, 1, f) != 1)
        throw std::runtime_error("Truncated spilled stream at read payload.");
    }
  };
  read_from(file0, read, read_lengths, rg.numreads_array[0]);
  read_from(file1, read + rg.numreads_array[0],
            read_lengths + rg.numreads_array[0], rg.numreads_array[1]);
}

namespace detail {
template <size_t bitset_size>
bool search_match(const std::bitset<bitset_size> &ref,
                  std::bitset<bitset_size> *index_masks, OmpLock *dict_locks,
                  OmpLock *read_locks, std::bitset<bitset_size> **length_masks,
                  uint16_t *read_lengths, bool *remaining_reads,
                  std::bitset<bitset_size> *reads, bbhashdict *dict,
                  uint32_t &matched_read_id, const bool use_reverse_match,
                  const int shift, const int &ref_len,
                  const reorder_global<bitset_size> &rg) {
  static const unsigned int thresh = THRESH_REORDER;
  const int maxsearch = MAX_SEARCH_REORDER;
  std::bitset<bitset_size> masked_ref_bits;
  uint64_t lookup_key;
  int64_t bucket_range[2];
  uint64_t bucket_start_index;
  std::array<uint32_t, MAX_SEARCH_REORDER> candidate_ids{};
  bool found_match = 0;
  for (int dictionary_index = 0; dictionary_index < rg.numdict;
       dictionary_index++) {
    if (!use_reverse_match) {
      if (dict[dictionary_index].end + shift >= ref_len)
        continue;
    } else {
      if (dict[dictionary_index].end >= ref_len + shift ||
          dict[dictionary_index].start <= shift)
        continue;
    }
    masked_ref_bits = ref & index_masks[dictionary_index];
    lookup_key =
        (masked_ref_bits >> 2 * dict[dictionary_index].start).to_ullong();
    if (rg.depleted_base == 'C') {
      lookup_key &= ~((lookup_key >> 1) & 0x5555555555555555ULL);
    } else if (rg.depleted_base == 'G') {
      lookup_key &= ~((~lookup_key >> 1) & 0x5555555555555555ULL);
    }
    bucket_start_index = (*dict[dictionary_index].bphf)(lookup_key);
    if (bucket_start_index >= dict[dictionary_index].numkeys)
      continue;
    if (!omp_test_lock(
            dict_locks[detail::lock_shard(bucket_start_index)].get()))
      continue;

    size_t candidate_count = 0;
    bool bucket_matches_lookup = false;
    dict[dictionary_index].findpos(bucket_range, bucket_start_index);
    if (dict[dictionary_index].empty_bin[bucket_start_index]) {
      omp_unset_lock(dict_locks[detail::lock_shard(bucket_start_index)].get());
      continue;
    }
    if (!detail::valid_bucket_range(dict[dictionary_index], bucket_range)) {
      dict[dictionary_index].empty_bin[bucket_start_index] = true;
      omp_unset_lock(dict_locks[detail::lock_shard(bucket_start_index)].get());
      continue;
    }
    uint64_t candidate_key =
        ((reads[dict[dictionary_index].read_id[bucket_range[0]]] &
          index_masks[dictionary_index]) >>
         2 * dict[dictionary_index].start)
            .to_ullong();
    if (rg.depleted_base == 'C') {
      candidate_key &= ~((candidate_key >> 1) & 0x5555555555555555ULL);
    } else if (rg.depleted_base == 'G') {
      candidate_key &= ~((~candidate_key >> 1) & 0x5555555555555555ULL);
    }
    if (lookup_key == candidate_key) {
      bucket_matches_lookup = true;
      for (int64_t bucket_index = bucket_range[1] - 1;
           bucket_index >= bucket_range[0] &&
           bucket_index >= bucket_range[1] - maxsearch;
           bucket_index--) {
        if (candidate_count >= candidate_ids.size())
          break;
        candidate_ids[candidate_count++] =
            dict[dictionary_index].read_id[bucket_index];
      }
    }
    omp_unset_lock(dict_locks[detail::lock_shard(bucket_start_index)].get());

    if (!bucket_matches_lookup)
      continue;

    for (size_t candidate_index = 0; candidate_index < candidate_count;
         candidate_index++) {
      const auto read_id = candidate_ids[candidate_index];
      size_t hamming;
      std::bitset<bitset_size> diff = ref ^ reads[read_id];
      if (rg.depleted_base == 'C') {
        diff &= ~((ref >> 1) & (reads[read_id] >> 1) & rg.mask_lsb);
      } else if (rg.depleted_base == 'G') {
        diff &= ~((~ref >> 1) & (~reads[read_id] >> 1) & rg.mask_lsb);
      }

      if (!use_reverse_match)
        hamming = (diff & length_masks[0][rg.max_readlen -
                                          std::min<int>(ref_len - shift,
                                                        read_lengths[read_id])])
                      .count();
      else
        hamming =
            (diff & length_masks[shift][rg.max_readlen -
                                        std::min<int>(ref_len + shift,
                                                      read_lengths[read_id])])
                .count();
      if (hamming > thresh)
        continue;

      if (!omp_test_lock(read_locks[detail::lock_shard(read_id)].get()))
        continue;
      if (remaining_reads[read_id]) {
        remaining_reads[read_id] = 0;
        matched_read_id = read_id;
        found_match = 1;
      }
      omp_unset_lock(read_locks[detail::lock_shard(read_id)].get());
      if (found_match == 1)
        break;
    }

    if (found_match == 1)
      break;
  }
  return found_match;
}
} // namespace detail

template <size_t bitset_size>
void reorder(std::bitset<bitset_size> *read, bbhashdict *dict,
             uint16_t *read_lengths, const reorder_global<bitset_size> &rg,
             reorder_encoder_artifact &artifact,
             const bool deterministic_mode) {
  const uint32_t num_locks = NUM_LOCKS_REORDER;
  std::vector<OmpLock> dict_locks(num_locks);
  std::vector<OmpLock> read_locks(num_locks);
  std::vector<OmpLock> remaining_read_lock(num_locks);
  std::vector<std::vector<std::bitset<bitset_size>>> length_masks;
  length_masks.assign(static_cast<size_t>(rg.max_readlen),
                      std::vector<std::bitset<bitset_size>>(rg.max_readlen));
  std::vector<std::bitset<bitset_size> *> length_masks_ptrs;
  length_masks_ptrs.reserve(static_cast<size_t>(rg.max_readlen));
  for (int i = 0; i < rg.max_readlen; ++i)
    length_masks_ptrs.push_back(length_masks[static_cast<size_t>(i)].data());
  generatemasks<bitset_size>(length_masks_ptrs.data(), rg.max_readlen, 2);
  std::vector<std::bitset<bitset_size>> index_masks(
      static_cast<size_t>(rg.numdict));
  SPRING_LOG_INFO("Constructing dictionaries");
  generateindexmasks<bitset_size>(index_masks.data(), dict, rg.numdict, 2);
  auto remaining_reads_storage = std::make_unique<bool[]>(rg.numreads);
  bool *remaining_reads = remaining_reads_storage.get();
  std::fill(remaining_reads, remaining_reads + rg.numreads, true);

  uint32_t first_read = 0;
  SPRING_LOG_INFO("Reordering reads");
  std::vector<uint32_t> unmatched_counts(static_cast<size_t>(rg.num_thr));
  std::vector<std::string> singleton_order_buffers(
      static_cast<size_t>(rg.num_thr));
  artifact.aligned_shards.assign(static_cast<size_t>(rg.num_thr), {});
  // Extract raw data pointers from all vector shared vars before the OMP
  // parallel region.  Clang's OMP outlined-function closure mishandles
  // complex template types (e.g. vector<bitset<N>>), returning a garbage
  // data() pointer inside worker threads.  Plain raw pointers are safe.
  auto *const index_masks_data = index_masks.data();
  auto *const length_masks_data = length_masks_ptrs.data();
  auto *const dict_locks_data = dict_locks.data();
  auto *const read_locks_data = read_locks.data();
  auto *const remaining_read_lock_data = remaining_read_lock.data();
  auto *const unmatched_counts_data = unmatched_counts.data();
  auto *const singleton_order_buffers_data = singleton_order_buffers.data();
  auto reorder_pass_start = std::chrono::steady_clock::now();
  auto last_progress_log_ts = reorder_pass_start;
#pragma omp parallel default(none) shared(                                     \
        rg, read, read_lengths, dict, remaining_reads, artifact, first_read,   \
            deterministic_mode, index_masks_data, length_masks_data,           \
            dict_locks_data, read_locks_data, remaining_read_lock_data,        \
            unmatched_counts_data, singleton_order_buffers_data,               \
            reorder_pass_start, last_progress_log_ts)
  {
    bool done = false;
    int thread_id = omp_get_thread_num();
    const int64_t scan_stride = std::max<int64_t>(1, omp_get_num_threads());
    int64_t remaining_read_scan = rg.numreads - 1 - thread_id;
    if (remaining_read_scan < 0)
      remaining_read_scan = -1;
    std::string orientation_output;
    std::string flag_output;
    std::string position_output;
    std::string order_output;
    std::string singleton_order_output;
    std::string read_length_output;

    // Deterministic mode serializes match selection to thread 0 while still
    // materializing empty per-thread shard files for downstream stages.
    if (deterministic_mode && thread_id != 0) {
      done = true;
    }

    unmatched_counts_data[thread_id] = 0;
    std::bitset<bitset_size> reference_read, reverse_reference_read,
        masked_read_bits;

    int64_t seed_read_id = 0;

    std::array<std::list<std::pair<uint32_t, uint64_t>>, NUM_DICT_REORDER>
        pending_bin_deletions;

    bool stop_searching = false;
    uint32_t thread_read_count = 0;
    uint32_t unmatched_reads_in_window = 0;

    std::array<std::unique_ptr<int[]>, 4> base_counts_storage;
    std::array<int *, 4> base_counts;
    for (int base_index = 0; base_index < 4; base_index++) {
      base_counts_storage[static_cast<size_t>(base_index)] =
          std::make_unique<int[]>(static_cast<size_t>(rg.max_readlen));
      base_counts[static_cast<size_t>(base_index)] =
          base_counts_storage[static_cast<size_t>(base_index)].get();
    }
    int64_t bucket_range[2];
    uint64_t bucket_start_index;
    bool found_match = 0;
    bool previous_read_unmatched = false;
    bool left_search_start = false;
    bool left_search = false;
    int64_t current_read_id = 0;
    int64_t previous_read_id = 0;
    int64_t scan_slot;
    uint64_t lookup_key;
    int reference_length;
    int64_t reference_position = 0;
    int64_t current_read_position;

    // Claim an initial seed read from a shared cursor. Keep trying until we
    // either reserve an unclaimed read or exhaust the input.
    while (!done) {
#pragma omp critical(spring_reorder_seed_claim)
      {
        scan_slot = first_read;
        first_read += 1;
      }
      if (rg.numreads == 0 || scan_slot >= static_cast<int64_t>(rg.numreads)) {
        done = true;
        break;
      }
      current_read_id = scan_slot;

      omp_set_lock(
          remaining_read_lock_data[detail::lock_shard(current_read_id)].get());
      omp_set_lock(read_locks_data[detail::lock_shard(current_read_id)].get());
      if (remaining_reads[current_read_id]) {
        remaining_reads[current_read_id] = 0;
        unmatched_counts_data[thread_id]++;
        omp_unset_lock(
            read_locks_data[detail::lock_shard(current_read_id)].get());
        omp_unset_lock(
            remaining_read_lock_data[detail::lock_shard(current_read_id)]
                .get());
        break;
      }
      omp_unset_lock(
          read_locks_data[detail::lock_shard(current_read_id)].get());
      omp_unset_lock(
          remaining_read_lock_data[detail::lock_shard(current_read_id)].get());
    }
    if (!done) {
      updaterefcount<bitset_size>(read[current_read_id], reference_read,
                                  reverse_reference_read, base_counts.data(),
                                  true, false, 0, read_lengths[current_read_id],
                                  reference_length, rg);
      current_read_position = 0;
      reference_position = 0;
      seed_read_id = current_read_id;
      previous_read_unmatched = true;
      previous_read_id = current_read_id;
    }
    while (!done) {
      if (thread_read_count % 100000 == 0) {
        if (thread_id == 0) {
          if (auto *progress = ProgressBar::GlobalInstance()) {
            // Estimate progress based on current scan position
            // This is a rough estimate but avoids atomic overhead in the hot
            // loop
            progress->update(1.0f - (float)remaining_read_scan / rg.numreads);
          }
          const auto now = std::chrono::steady_clock::now();
          if (now - last_progress_log_ts >= std::chrono::seconds(60)) {
            last_progress_log_ts = now;
            const auto elapsed_s =
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - reorder_pass_start)
                    .count();
            const auto approx_placed = std::min(first_read, rg.numreads);
            SPRING_LOG_INFO("Reorder pass: " + std::to_string(approx_placed) +
                            "/" + std::to_string(rg.numreads) + " seeds, " +
                            std::to_string(elapsed_s) + " s elapsed");
          }
        }
        if (unmatched_reads_in_window > STOP_CRITERIA_REORDER * 100000) {
          stop_searching = true;
        }
        unmatched_reads_in_window = 0;
      }
      thread_read_count++;
      for (int dictionary_index = 0; dictionary_index < rg.numdict;
           dictionary_index++) {
        for (auto pending_delete_it =
                 pending_bin_deletions[dictionary_index].begin();
             pending_delete_it !=
             pending_bin_deletions[dictionary_index].end();) {
          uint32_t read_id = (*pending_delete_it).first;
          uint64_t pending_bucket_start = (*pending_delete_it).second;
          if (!omp_test_lock(
                  dict_locks_data[detail::lock_shard(pending_bucket_start)]
                      .get())) {
            ++pending_delete_it;
            continue;
          }
          dict[dictionary_index].findpos(bucket_range, pending_bucket_start);
          if (!detail::valid_bucket_range(dict[dictionary_index],
                                          bucket_range)) {
            dict[dictionary_index].empty_bin[pending_bucket_start] = true;
            pending_delete_it = pending_bin_deletions[dictionary_index].erase(
                pending_delete_it);
            omp_unset_lock(
                dict_locks_data[detail::lock_shard(pending_bucket_start)]
                    .get());
            continue;
          }
          dict[dictionary_index].remove(bucket_range, pending_bucket_start,
                                        read_id);
          pending_delete_it =
              pending_bin_deletions[dictionary_index].erase(pending_delete_it);
          omp_unset_lock(
              dict_locks_data[detail::lock_shard(pending_bucket_start)].get());
        }
      }

      // Fast-path: once no more matches can be found, drain remaining
      // singletons with a contiguous sweep to eliminate false-sharing on
      // remaining_reads.
      if (stop_searching) {
        if (previous_read_unmatched) {
          detail::append_binary(singleton_order_output,
                                static_cast<uint32_t>(previous_read_id));
        }
        const int64_t num_thr =
            scan_stride; // scan_stride == omp_get_num_threads()
        const int64_t reads_per_thr =
            (static_cast<int64_t>(rg.numreads) + num_thr - 1) / num_thr;
        const int64_t sweep_start = thread_id * reads_per_thr;
        const int64_t sweep_end = std::min(sweep_start + reads_per_thr,
                                           static_cast<int64_t>(rg.numreads));
        for (int64_t read_id = sweep_start; read_id < sweep_end; read_id++) {
          if (!remaining_reads[read_id])
            continue; // already claimed; cheap sequential check
          if (!omp_test_lock(
                  read_locks_data[detail::lock_shard(read_id)].get()))
            continue; // contested; safety net will recover
          const bool still_remaining = remaining_reads[read_id] != 0;
          if (still_remaining) {
            remaining_reads[read_id] = 0;
            unmatched_counts_data[thread_id]++;
          }
          omp_unset_lock(read_locks_data[detail::lock_shard(read_id)].get());
          if (still_remaining) {
            detail::append_binary(singleton_order_output,
                                  static_cast<uint32_t>(read_id));
          }
        }
        done = true;
        break;
      }

      if (!left_search_start) {
        for (int dictionary_index = 0; dictionary_index < rg.numdict;
             dictionary_index++) {
          if (read_lengths[current_read_id] <= dict[dictionary_index].end)
            continue;
          masked_read_bits =
              read[current_read_id] & index_masks_data[dictionary_index];
          lookup_key = (masked_read_bits >> 2 * dict[dictionary_index].start)
                           .to_ullong();
          if (rg.depleted_base == 'C') {
            lookup_key &= ~((lookup_key >> 1) & 0x5555555555555555ULL);
          } else if (rg.depleted_base == 'G') {
            lookup_key &= ~((~lookup_key >> 1) & 0x5555555555555555ULL);
          }
          bucket_start_index = (*dict[dictionary_index].bphf)(lookup_key);
          if (bucket_start_index >= dict[dictionary_index].numkeys ||
              dict[dictionary_index].empty_bin[bucket_start_index])
            continue;
          if (!omp_test_lock(
                  dict_locks_data[detail::lock_shard(bucket_start_index)]
                      .get())) {
            pending_bin_deletions[dictionary_index].push_back(
                std::pair<uint32_t, uint64_t>{
                    static_cast<uint32_t>(current_read_id),
                    bucket_start_index});
            continue;
          }
          dict[dictionary_index].findpos(bucket_range, bucket_start_index);
          if (!detail::valid_bucket_range(dict[dictionary_index],
                                          bucket_range)) {
            dict[dictionary_index].empty_bin[bucket_start_index] = true;
            omp_unset_lock(
                dict_locks_data[detail::lock_shard(bucket_start_index)].get());
            continue;
          }
          dict[dictionary_index].remove(bucket_range, bucket_start_index,
                                        current_read_id);
          omp_unset_lock(
              dict_locks_data[detail::lock_shard(bucket_start_index)].get());
        }
      } else {
        left_search_start = false;
      }
      found_match = 0;
      uint32_t matched_read_id;
      if (!stop_searching &&
          (reference_position < MAX_CONTIG_GROWTH - 2 * rg.max_readlen))
        for (int shift = 0; shift < rg.maxshift; shift += rg.shift_step) {
          found_match = detail::search_match<bitset_size>(
              reference_read, index_masks_data, dict_locks_data,
              read_locks_data, length_masks_data, read_lengths, remaining_reads,
              read, dict, matched_read_id, false, shift, reference_length, rg);
          if (found_match == 1) {
            current_read_id = matched_read_id;
            int previous_reference_length = reference_length;
            updaterefcount<bitset_size>(
                read[current_read_id], reference_read, reverse_reference_read,
                base_counts.data(), false, false, shift,
                read_lengths[current_read_id], reference_length, rg);
            if (!left_search) {
              current_read_position = reference_position + shift;
              reference_position = current_read_position;
            } else {
              current_read_position = reference_position +
                                      previous_reference_length - shift -
                                      read_lengths[current_read_id];
              reference_position = reference_position +
                                   previous_reference_length - shift -
                                   reference_length;
            }
            if (previous_read_unmatched == true) {
              orientation_output.push_back('d');
              detail::append_binary(order_output,
                                    static_cast<uint32_t>(previous_read_id));
              flag_output.push_back('0');
              int64_t zero = 0;
              detail::append_binary(position_output, zero);
              detail::append_binary(read_length_output,
                                    read_lengths[previous_read_id]);
            }
            orientation_output.push_back(left_search ? 'r' : 'd');
            detail::append_binary(order_output,
                                  static_cast<uint32_t>(current_read_id));
            flag_output.push_back('1');
            detail::append_binary(position_output, current_read_position);
            detail::append_binary(read_length_output,
                                  read_lengths[current_read_id]);

            previous_read_unmatched = false;
            break;
          }

          // find reverse match
          found_match = detail::search_match<bitset_size>(
              reverse_reference_read, index_masks_data, dict_locks_data,
              read_locks_data, length_masks_data, read_lengths, remaining_reads,
              read, dict, matched_read_id, true, shift, reference_length, rg);
          if (found_match == 1) {
            current_read_id = matched_read_id;
            int previous_reference_length = reference_length;
            updaterefcount<bitset_size>(
                read[current_read_id], reference_read, reverse_reference_read,
                base_counts.data(), false, true, shift,
                read_lengths[current_read_id], reference_length, rg);
            if (!left_search) {
              current_read_position = reference_position +
                                      previous_reference_length + shift -
                                      read_lengths[current_read_id];
              reference_position = reference_position +
                                   previous_reference_length + shift -
                                   reference_length;
            } else {
              current_read_position = reference_position - shift;
              reference_position = current_read_position;
            }
            if (previous_read_unmatched ==
                true) // prev read not singleton, write it now
            {
              orientation_output.push_back('d');
              detail::append_binary(order_output,
                                    static_cast<uint32_t>(previous_read_id));
              flag_output.push_back('0');
              int64_t zero = 0;
              detail::append_binary(position_output, zero);
              detail::append_binary(read_length_output,
                                    read_lengths[previous_read_id]);
            }
            orientation_output.push_back(left_search ? 'd' : 'r');
            detail::append_binary(order_output,
                                  static_cast<uint32_t>(current_read_id));
            flag_output.push_back('1');
            detail::append_binary(position_output, current_read_position);
            detail::append_binary(read_length_output,
                                  read_lengths[current_read_id]);

            previous_read_unmatched = false;
            break;
          }

          reverse_reference_read <<= 2 * rg.shift_step;
          reference_read >>= 2 * rg.shift_step;
        }
      if (found_match == 0) {
        unmatched_reads_in_window++;
        if (!left_search) {
          // Retry around the contig seed in reverse-complement space once.
          left_search = true;
          left_search_start = true;
          updaterefcount<bitset_size>(
              read[seed_read_id], reference_read, reverse_reference_read,
              base_counts.data(), true, true, 0, read_lengths[seed_read_id],
              reference_length, rg);
          reference_position = 0;
          current_read_position = 0;
        } else {
          left_search = false;
          for (int64_t read_id = remaining_read_scan; read_id >= 0;
               read_id -= scan_stride) {
            omp_set_lock(
                remaining_read_lock_data[detail::lock_shard(read_id)].get());
            omp_set_lock(read_locks_data[detail::lock_shard(read_id)].get());
            if (remaining_reads[read_id]) {
              current_read_id = read_id;
              remaining_read_scan = read_id - scan_stride;
              remaining_reads[read_id] = 0;
              found_match = 1;
              unmatched_counts_data[thread_id]++;
            }
            omp_unset_lock(read_locks_data[detail::lock_shard(read_id)].get());
            omp_unset_lock(
                remaining_read_lock_data[detail::lock_shard(read_id)].get());
            if (found_match == 1)
              break;
          }
          if (found_match == 0) {
            if (previous_read_unmatched == true) {
              detail::append_binary(singleton_order_output,
                                    static_cast<uint32_t>(previous_read_id));
            }
            done = 1;
          } else {
            updaterefcount<bitset_size>(
                read[current_read_id], reference_read, reverse_reference_read,
                base_counts.data(), true, false, 0,
                read_lengths[current_read_id], reference_length, rg);
            reference_position = 0;
            current_read_position = 0;
            if (previous_read_unmatched == true) {
              detail::append_binary(singleton_order_output,
                                    static_cast<uint32_t>(previous_read_id));
            }
            previous_read_unmatched = true;
            seed_read_id = current_read_id;
            previous_read_id = current_read_id;
          }
        }
      }
    }
    artifact.aligned_shards[static_cast<size_t>(thread_id)].orientation_bytes =
        std::move(orientation_output);
    artifact.aligned_shards[static_cast<size_t>(thread_id)].flag_bytes =
        std::move(flag_output);
    artifact.aligned_shards[static_cast<size_t>(thread_id)].position_bytes =
        std::move(position_output);
    artifact.aligned_shards[static_cast<size_t>(thread_id)].order_bytes =
        std::move(order_output);
    artifact.aligned_shards[static_cast<size_t>(thread_id)].read_length_bytes =
        std::move(read_length_output);
    singleton_order_buffers_data[static_cast<size_t>(thread_id)] =
        std::move(singleton_order_output);
    // base_counts_storage RAII will free the per-thread buffers
  }
  // Safety net: any reads still marked remaining were never emitted by worker
  // threads. Append them as singletons so downstream stages see all reads.
  uint32_t recovered_singletons = 0;
  {
    for (uint32_t read_id = 0; read_id < rg.numreads; read_id++) {
      if (!remaining_reads[read_id])
        continue;
      detail::append_binary(singleton_order_buffers_data[0], read_id);
      remaining_reads[read_id] = false;
      recovered_singletons++;
    }
  }

  if (recovered_singletons > 0) {
    unmatched_counts_data[0] += recovered_singletons;
    SPRING_LOG_DEBUG("Recovered leftover reorder reads as singletons: " +
                     std::to_string(recovered_singletons));
  }

  artifact.singleton_order_bytes.clear();
  for (std::string &singleton_orders : singleton_order_buffers) {
    artifact.singleton_order_bytes.append(singleton_orders);
  }
  artifact.singleton_count = static_cast<uint32_t>(
      artifact.singleton_order_bytes.size() / sizeof(uint32_t));
  // remaining_reads_storage RAII will free the remaining_reads buffer
  SPRING_LOG_INFO("Reordering done, " +
                  std::to_string(std::accumulate(unmatched_counts.begin(),
                                                 unmatched_counts.end(), 0U)) +
                  " were unmatched");
  // length_masks and index_masks are now RAII-managed containers
  return;
}

template <size_t bitset_size>
void writetofile(std::bitset<bitset_size> *read, uint16_t *read_lengths,
                 reorder_global<bitset_size> &rg,
                 reorder_encoder_artifact &artifact) {
  std::vector<std::string> write_errors(static_cast<size_t>(rg.num_thr));
  artifact.singleton_read_bytes.clear();

#pragma omp parallel for schedule(static)
  for (int tid = 0; tid < rg.num_thr; tid++) {
    reorder_encoder_shard &shard =
        artifact.aligned_shards[static_cast<size_t>(tid)];
    const size_t aligned_count = shard.orientation_bytes.size();
    if (shard.order_bytes.size() != aligned_count * sizeof(uint32_t)) {
      write_errors[static_cast<size_t>(tid)] =
          "Reorder shard order/orientation size mismatch.";
      continue;
    }

    std::string encoded_reads;
    char s[MAX_READ_LEN + 1], s1[MAX_READ_LEN + 1];
    for (size_t read_index = 0; read_index < aligned_count; ++read_index) {
      uint32_t current = 0;
      std::memcpy(&current,
                  shard.order_bytes.data() + read_index * sizeof(uint32_t),
                  sizeof(uint32_t));
      if (current >= rg.numreads) {
        write_errors[static_cast<size_t>(tid)] =
            "Reorder shard read id out of bounds.";
        break;
      }
      bitsettostring<bitset_size>(read[current], s, read_lengths[current], rg);
      if (shard.orientation_bytes[read_index] == 'd') {
        detail::append_encoded_read(encoded_reads, s, read_lengths[current]);
      } else {
        reverse_complement(s, s1, read_lengths[current]);
        detail::append_encoded_read(encoded_reads, s1, read_lengths[current]);
      }
    }
    shard.read_bytes = std::move(encoded_reads);
  }

  for (const std::string &write_error : write_errors) {
    if (!write_error.empty()) {
      throw std::runtime_error(write_error);
    }
  }

  for (size_t read_index = 0;
       read_index < artifact.singleton_order_bytes.size();
       read_index += sizeof(uint32_t)) {
    uint32_t current = 0;
    std::memcpy(&current, artifact.singleton_order_bytes.data() + read_index,
                sizeof(uint32_t));
    if (current >= rg.numreads) {
      throw std::runtime_error("Reorder singleton read id out of bounds.");
    }
    char s[MAX_READ_LEN + 1];
    bitsettostring<bitset_size>(read[current], s, read_lengths[current], rg);
    detail::append_encoded_read(artifact.singleton_read_bytes, s,
                                read_lengths[current]);
  }
}

// If the average number of reads per dictionary key exceeds this threshold the
// greedy chain algorithm degenerates (essentially every candidate matches every
// other candidate in the same bucket).  Use a direct bucket-order fast path
// instead: emit all reads in each bucket as a single chain in O(N) time.
constexpr uint32_t kLowDiversityReadsPerKeyThreshold = 10000;

// Fast-path reorder for archives with very few distinct sequences (e.g.
// sc-ATAC I1 barcode reads).  Reads in the same dict[0] bucket share the same
// k-mer key and are emitted as one chain (seed + aligned at shift 0).  Reads
// absent from dict[0] because they are shorter than dict[0].end are collected
// as singletons.  The output shard format is identical to reorder(), so all
// downstream stages (writetofile, stream_reordering, quality_id_reordering)
// consume it without any changes.
template <size_t bitset_size>
void reorder_fast_low_diversity(bbhashdict *dict, uint16_t *read_lengths,
                                const reorder_global<bitset_size> &rg,
                                reorder_encoder_artifact &artifact) {
  SPRING_LOG_INFO(
      "Low-diversity fast path (keys=" + std::to_string(dict[0].numkeys) +
      ", reads=" + std::to_string(rg.numreads) + ")");

  // aligned_shards must be sized before the parallel region accesses it by
  // thread index.  reorder() normally does this, but the fast path bypasses
  // reorder(), so we initialise it here.
  artifact.aligned_shards.assign(static_cast<size_t>(rg.num_thr), {});

  // Each read appears in exactly one bucket in dict[0] (or none if too
  // short).  Track placement to accumulate singletons afterwards.
  auto placed = std::make_unique<bool[]>(rg.numreads);
  std::fill(placed.get(), placed.get() + rg.numreads, false);

#pragma omp parallel num_threads(rg.num_thr)
  {
    const int tid = omp_get_thread_num();
    reorder_encoder_shard &shard =
        artifact.aligned_shards[static_cast<size_t>(tid)];

    // Distribute buckets across threads; each bucket forms one contiguous
    // chain so we use dynamic scheduling to balance unequal bucket sizes.
#pragma omp for schedule(dynamic, 1)
    for (uint32_t bucket_idx = 0; bucket_idx < dict[0].numkeys; ++bucket_idx) {
      int64_t bucket_range[2];
      dict[0].findpos(bucket_range, static_cast<uint64_t>(bucket_idx));
      if (!detail::valid_bucket_range(dict[0], bucket_range))
        continue;

      // First read in bucket starts a new chain (flag '0'); subsequent reads
      // align at position 0 / shift 0 (flag '1').
      bool first_in_bucket = true;
      for (int64_t pos = bucket_range[0]; pos < bucket_range[1]; ++pos) {
        const uint32_t read_id = dict[0].read_id[static_cast<size_t>(pos)];
        placed[read_id] = true;

        shard.orientation_bytes.push_back('d');
        detail::append_binary(shard.order_bytes, read_id);
        shard.flag_bytes.push_back(first_in_bucket ? '0' : '1');
        const int64_t p = 0;
        detail::append_binary(shard.position_bytes, p);
        detail::append_binary(shard.read_length_bytes, read_lengths[read_id]);
        first_in_bucket = false;
      }
    }
  } // implicit barrier: placed[] writes visible to the main thread below

  // Reads absent from dict[0] (too short for its key window) become
  // singletons so no reads are silently dropped.
  artifact.singleton_count = 0;
  artifact.singleton_order_bytes.clear();
  for (uint32_t read_id = 0; read_id < rg.numreads; ++read_id) {
    if (!placed[read_id]) {
      detail::append_binary(artifact.singleton_order_bytes, read_id);
      ++artifact.singleton_count;
    }
  }

  SPRING_LOG_INFO("Reordering done, " +
                  std::to_string(artifact.singleton_count) + " were unmatched");
}

template <size_t bitset_size>
reorder_encoder_artifact reorder_main(reorder_input_artifact input_artifact,
                                      const compression_params &cp) {
  auto format_seconds = [](const std::chrono::steady_clock::duration &value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
           << std::chrono::duration_cast<std::chrono::milliseconds>(value)
                      .count() /
                  1000.0;
    return stream.str();
  };

  reorder_global<bitset_size> rg(cp.read_info.max_readlen);
  rg.paired_end = cp.encoding.paired_end;
  rg.depleted_base = cp.encoding.depleted_base;

  rg.max_readlen = cp.read_info.max_readlen;
  rg.num_thr = cp.encoding.num_thr;
  rg.paired_end = cp.encoding.paired_end;
  // Cap maxshift so 150+ bp datasets don't take proportionally longer.
  static constexpr int kMaxReorderShift = 50;
  // For long reads, try every other shift position — halves inner-loop
  // iterations with minimal compression loss at high coverage.
  static constexpr int kReorderShiftStep = 2;
  rg.maxshift = std::min(rg.max_readlen / 2, kMaxReorderShift);
  rg.shift_step = (rg.max_readlen > 100) ? kReorderShiftStep : 1;
  if (rg.max_readlen / 2 > kMaxReorderShift) {
    SPRING_LOG_INFO("Reorder maxshift capped at " +
                    std::to_string(kMaxReorderShift) +
                    " (uncapped: " + std::to_string(rg.max_readlen / 2) +
                    " for " + std::to_string(rg.max_readlen) + " bp reads)");
  }
  if (rg.shift_step > 1) {
    SPRING_LOG_INFO("Reorder shift step: " + std::to_string(rg.shift_step) +
                    " (read length " + std::to_string(rg.max_readlen) +
                    " bp, ~" + std::to_string(rg.maxshift / rg.shift_step + 1) +
                    " shift iterations instead of " +
                    std::to_string(rg.maxshift) + ")");
  }
  rg.numreads =
      cp.read_info.num_reads_clean[0] + cp.read_info.num_reads_clean[1];
  rg.numreads_array[0] = cp.read_info.num_reads_clean[0];
  rg.numreads_array[1] = cp.read_info.num_reads_clean[1];
  rg.numdict =
      (rg.numreads < DICT_SINGLE_STAGE_READ_THRESHOLD) ? 1 : NUM_DICT_REORDER;
  const bool deterministic_mode = deterministic_reorder_enabled();
  SPRING_LOG_DEBUG("Reorder dictionary configuration: active_dicts=" +
                   std::to_string(rg.numdict) +
                   ", clean_reads=" + std::to_string(rg.numreads));
  SPRING_LOG_INFO(std::string("Reorder mode: ") +
                  (deterministic_mode ? "deterministic" : "parallel"));

  omp_set_num_threads(rg.num_thr);
  setglobalarrays(rg);

  // Chunk large read pools so the bitset<N> read array stays ~2 GB per chunk.
  // At 40 bytes/read (bitset<320>), 50 M reads = 2 GB — well within the range
  // where random MPHF lookups hit L3 cache rather than saturating main memory.
  // SPRING2_REORDER_CHUNK_SIZE env var overrides the threshold (for testing).
  static constexpr uint32_t kDefaultReorderChunkReads = 50'000'000;
  const uint32_t kReorderChunkReads = []() -> uint32_t {
    const char *env = std::getenv("SPRING2_REORDER_CHUNK_SIZE");
    if (env) {
      const long v = std::strtol(env, nullptr, 10);
      if (v > 0)
        return static_cast<uint32_t>(v);
    }
    return kDefaultReorderChunkReads;
  }();
  const uint32_t total0 = rg.numreads_array[0];
  const uint32_t total1 = rg.numreads_array[1];
  const bool use_chunked = (rg.numreads > kReorderChunkReads);
  if (use_chunked) {
    const uint32_t num_chunks =
        (rg.numreads + kReorderChunkReads - 1) / kReorderChunkReads;
    SPRING_LOG_INFO("Large dataset: splitting " + std::to_string(rg.numreads) +
                    " reads into " + std::to_string(num_chunks) +
                    " chunks of up to " + std::to_string(kReorderChunkReads) +
                    " reads to reduce working-set pressure");
  }

  // In disk_path mode the reorder artifact directory is passed as spill_dir.
  // Spilling streams + singleton sequences per chunk prevents accumulating
  // ~29 GB (streams) + ~7 GB/chunk (singleton_read_bytes) in RAM.
  const std::string &spill_dir = cp.encoding.reorder_spill_dir;
  const bool use_spill = use_chunked && !spill_dir.empty();

  // Helpers for writing output files in spill mode (same layout as
  // spill_reorder_encoder_artifact so load_reorder_encoder_artifact can read
  // them without any additional conversion).
  auto spill_write = [&spill_dir](const std::string &name,
                                  const std::string &data) {
    namespace fs = std::filesystem;
    const fs::path p = fs::path(spill_dir) / name;
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f)
      throw std::runtime_error("Cannot write spill file: " + p.string());
    if (!data.empty())
      f.write(data.data(), static_cast<std::streamsize>(data.size()));
  };

  auto spill_append = [&spill_dir](const std::string &name,
                                   const std::string &data) {
    if (data.empty())
      return;
    const std::filesystem::path p = std::filesystem::path(spill_dir) / name;
    std::ofstream f(p, std::ios::binary | std::ios::app);
    if (!f)
      throw std::runtime_error("Cannot append to spill file: " + p.string());
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
  };

  // FILE* handles for reading spilled streams; non-null only in use_spill mode.
  std::FILE *stream_file0 = nullptr;
  std::FILE *stream_file1 = nullptr;

  if (use_spill) {
    namespace fs = std::filesystem;
    fs::create_directories(spill_dir);
    const std::string s0 = spill_dir + "/chunk_stream_0.bin";
    const std::string s1 = spill_dir + "/chunk_stream_1.bin";
    {
      auto write_stream = [](const std::string &path, const std::string &data) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
          throw std::runtime_error("Cannot spill read stream: " + path);
        if (!data.empty())
          f.write(data.data(), static_cast<std::streamsize>(data.size()));
      };
      write_stream(s0, input_artifact.clean_read_streams[0]);
      std::string().swap(input_artifact.clean_read_streams[0]);
      SPRING_LOG_INFO("Spilled read stream 0 (" +
                      std::to_string(fs::file_size(s0) >> 20) +
                      " MiB) to disk; freed from RAM");
      write_stream(s1, input_artifact.clean_read_streams[1]);
      std::string().swap(input_artifact.clean_read_streams[1]);
      SPRING_LOG_INFO("Spilled read stream 1 (" +
                      std::to_string(fs::file_size(s1) >> 20) +
                      " MiB) to disk; freed from RAM");
    }
    stream_file0 = std::fopen(s0.c_str(), "rb");
    stream_file1 = std::fopen(s1.c_str(), "rb");
    if (!stream_file0 || !stream_file1)
      throw std::runtime_error(
          "Failed to open spilled stream files for reading.");
    // Truncate so append mode starts fresh on re-runs.
    spill_write("singleton_read_bytes.bin", "");
  }

  reorder_encoder_artifact artifact;
  artifact.aligned_shards.assign(rg.num_thr, {});

  size_t cursor1 = 0, cursor2 = 0; // byte cursors into clean_read_streams
  uint32_t done0 = 0, done1 = 0;
  uint32_t global_offset = 0;
  uint32_t chunk_idx = 0;

  while (done0 < total0 || done1 < total1) {
    const uint32_t rem0 = total0 - done0;
    const uint32_t rem1 = total1 - done1;
    const uint32_t rem = rem0 + rem1;
    const uint32_t chunk_total =
        use_chunked ? std::min(rem, kReorderChunkReads) : rem;
    // Distribute chunk_total proportionally across both streams.
    const uint32_t chunk0 =
        (rem > 0)
            ? std::min(rem0,
                       static_cast<uint32_t>(
                           static_cast<uint64_t>(chunk_total) * rem0 / rem))
            : 0;
    const uint32_t chunk1 = std::min(rem1, chunk_total - chunk0);

    rg.numreads_array[0] = chunk0;
    rg.numreads_array[1] = chunk1;
    rg.numreads = chunk0 + chunk1;
    rg.numdict =
        (rg.numreads < DICT_SINGLE_STAGE_READ_THRESHOLD) ? 1 : NUM_DICT_REORDER;

    if (use_chunked) {
      SPRING_LOG_INFO("Reorder chunk " + std::to_string(chunk_idx + 1) + " (" +
                      std::to_string(rg.numreads) + " reads, global offset " +
                      std::to_string(global_offset) + ")");
    }

    std::vector<std::bitset<bitset_size>> chunk_read(rg.numreads);
    std::vector<uint16_t> chunk_read_lengths(rg.numreads);

    if (use_spill) {
      readDnaFileChunkFromFile<bitset_size>(chunk_read.data(),
                                            chunk_read_lengths.data(),
                                            stream_file0, stream_file1, rg);
    } else if (use_chunked) {
      readDnaFileChunk<bitset_size>(chunk_read.data(),
                                    chunk_read_lengths.data(), input_artifact,
                                    cursor1, cursor2, rg);
    } else {
      SPRING_LOG_INFO("Reading file");
      readDnaFile<bitset_size>(chunk_read.data(), chunk_read_lengths.data(),
                               input_artifact, rg);
      // Free streams now that all reads are decoded — not used again.
      std::string().swap(input_artifact.clean_read_streams[0]);
      std::string().swap(input_artifact.clean_read_streams[1]);
      SPRING_LOG_DEBUG("Freed raw clean-read byte streams after decode.");
    }

    std::array<bbhashdict, NUM_DICT_REORDER> chunk_dict;
    initialize_reorder_dict_ranges(chunk_dict, rg.max_readlen);

    if (rg.numreads > 0) {
      SPRING_LOG_INFO("Constructing dictionaries");
      const auto dictionary_stage_start = std::chrono::steady_clock::now();
      constructdictionary<bitset_size>(
          chunk_read.data(), chunk_dict.data(), chunk_read_lengths.data(),
          rg.numdict, rg.numreads, 2, rg.depleted_base,
          cp.encoding.use_external_mphf, cp.encoding.mphf_tmp_dir);
      const auto dictionary_stage_end = std::chrono::steady_clock::now();
      SPRING_LOG_INFO(
          "Dictionary stage time: " +
          format_seconds(dictionary_stage_end - dictionary_stage_start) + " s");
    }

    const bool use_low_diversity_fast_path =
        rg.numreads > 0 && chunk_dict[0].numkeys > 0 &&
        rg.numreads / chunk_dict[0].numkeys > kLowDiversityReadsPerKeyThreshold;

    SPRING_LOG_INFO("Reordering reads");
    const auto reorder_stage_start = std::chrono::steady_clock::now();
    reorder_encoder_artifact chunk_artifact;
    if (use_low_diversity_fast_path) {
      reorder_fast_low_diversity<bitset_size>(
          chunk_dict.data(), chunk_read_lengths.data(), rg, chunk_artifact);
    } else {
      reorder<bitset_size>(chunk_read.data(), chunk_dict.data(),
                           chunk_read_lengths.data(), rg, chunk_artifact,
                           deterministic_mode);
    }
    const auto reorder_stage_end = std::chrono::steady_clock::now();
    SPRING_LOG_INFO("Reorder pass time: " +
                    format_seconds(reorder_stage_end - reorder_stage_start) +
                    " s");

    SPRING_LOG_INFO("Writing to file");
    const auto write_stage_start = std::chrono::steady_clock::now();
    writetofile<bitset_size>(chunk_read.data(), chunk_read_lengths.data(), rg,
                             chunk_artifact);
    const auto write_stage_end = std::chrono::steady_clock::now();
    SPRING_LOG_INFO("Reorder write time: " +
                    format_seconds(write_stage_end - write_stage_start) + " s");

    // Merge chunk output into the combined artifact.
    // order_bytes (local chunk read IDs) are shifted by global_offset so
    // quality/id reordering can map them back to the original read stream.
    for (int tid = 0; tid < rg.num_thr; ++tid) {
      auto &src = chunk_artifact.aligned_shards[static_cast<size_t>(tid)];
      auto &dst = artifact.aligned_shards[static_cast<size_t>(tid)];
      dst.read_bytes += src.read_bytes;
      dst.orientation_bytes += src.orientation_bytes;
      dst.flag_bytes += src.flag_bytes;
      dst.position_bytes += src.position_bytes;
      dst.read_length_bytes += src.read_length_bytes;
      for (size_t k = 0; k < src.order_bytes.size(); k += sizeof(uint32_t)) {
        uint32_t id;
        std::memcpy(&id, src.order_bytes.data() + k, sizeof(uint32_t));
        id += global_offset;
        detail::append_binary(dst.order_bytes, id);
      }
    }
    artifact.singleton_count += chunk_artifact.singleton_count;
    if (use_spill) {
      // Flush singleton sequences to disk immediately (~7.4 GB per chunk for
      // sc-ATAC datasets) to keep RAM flat across all chunks.
      spill_append("singleton_read_bytes.bin",
                   chunk_artifact.singleton_read_bytes);
      std::string().swap(chunk_artifact.singleton_read_bytes);
    } else {
      artifact.singleton_read_bytes += chunk_artifact.singleton_read_bytes;
    }
    for (size_t k = 0; k < chunk_artifact.singleton_order_bytes.size();
         k += sizeof(uint32_t)) {
      uint32_t id;
      std::memcpy(&id, chunk_artifact.singleton_order_bytes.data() + k,
                  sizeof(uint32_t));
      id += global_offset;
      detail::append_binary(artifact.singleton_order_bytes, id);
    }

    done0 += chunk0;
    done1 += chunk1;
    global_offset += rg.numreads;
    ++chunk_idx;
  }

  if (use_spill) {
    if (stream_file0) {
      std::fclose(stream_file0);
    }
    if (stream_file1) {
      std::fclose(stream_file1);
    }
    // Remove the temp stream files now that all chunks are read.
    std::error_code ec;
    std::filesystem::remove(
        std::filesystem::path(spill_dir) / "chunk_stream_0.bin", ec);
    std::filesystem::remove(
        std::filesystem::path(spill_dir) / "chunk_stream_1.bin", ec);

    // Write all remaining in-memory buffers to their spill files.
    // This completes the same layout that spill_reorder_encoder_artifact
    // would produce; the workflow can then load via
    // load_reorder_encoder_artifact.
    spill_write("meta/shard_count.txt", std::to_string(rg.num_thr));
    spill_write("meta/singleton_count.txt",
                std::to_string(artifact.singleton_count));

    artifact.n_read_bytes = std::move(input_artifact.n_read_bytes);
    artifact.n_read_order_bytes = std::move(input_artifact.n_read_order_bytes);
    spill_write("singleton_order_bytes.bin", artifact.singleton_order_bytes);
    spill_write("n_read_bytes.bin", artifact.n_read_bytes);
    spill_write("n_read_order_bytes.bin", artifact.n_read_order_bytes);

    for (int tid = 0; tid < rg.num_thr; ++tid) {
      const std::string prefix = "aligned_shards/" + std::to_string(tid) + "/";
      const auto &shard = artifact.aligned_shards[static_cast<size_t>(tid)];
      spill_write(prefix + "read_bytes.bin", shard.read_bytes);
      spill_write(prefix + "orientation_bytes.bin", shard.orientation_bytes);
      spill_write(prefix + "flag_bytes.bin", shard.flag_bytes);
      spill_write(prefix + "position_bytes.bin", shard.position_bytes);
      spill_write(prefix + "order_bytes.bin", shard.order_bytes);
      spill_write(prefix + "read_length_bytes.bin", shard.read_length_bytes);
    }

    // Signal to the workflow that the full artifact is already on disk.
    artifact.singleton_read_file =
        (std::filesystem::path(spill_dir) / "singleton_read_bytes.bin")
            .string();
    SPRING_LOG_INFO("Done! Reorder artifact pre-spilled to " + spill_dir);
    return artifact;
  }

  if (use_chunked) {
    std::string().swap(input_artifact.clean_read_streams[0]);
    std::string().swap(input_artifact.clean_read_streams[1]);
  }

  artifact.n_read_bytes = std::move(input_artifact.n_read_bytes);
  artifact.n_read_order_bytes = std::move(input_artifact.n_read_order_bytes);
  SPRING_LOG_INFO("Done!");
  return artifact;
}

} // namespace spring

#endif // SPRING_READ_REORDERING_IMPL_H_
