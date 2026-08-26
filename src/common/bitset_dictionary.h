// Declares the BBHash-based dictionary structures and construction helpers used
// to index packed reads during reordering and encoding.

#ifndef SPRING_BITSET_DICTIONARY_H_
#define SPRING_BITSET_DICTIONARY_H_

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <omp.h>
#include <sstream>
#include <string>
#include <vector>

#include "bucketers.hpp"
#include "core_utils.h"
#include "encoders.hpp"
#include "hasher.hpp"
#include "progress.h"
#include "single_phf.hpp"

namespace spring {

using boophf_t = pthash::single_phf<pthash::xxhash_64, pthash::range_bucketer,
                                    pthash::compact, false>;

class bbhashdict {
public:
  std::unique_ptr<boophf_t> bphf;
  int start;
  int end;
  uint32_t numkeys;
  uint32_t dict_numreads; // Number of reads long enough to participate here.
  std::unique_ptr<uint32_t[]> startpos;
  std::unique_ptr<uint32_t[]> read_id;
  // std::vector<bool> packs bits into shared words which is not safe for
  // concurrent read/write on different elements (they share the same byte).
  // Use uint8_t so each element has its own byte and threads can safely
  // access adjacent entries with different dict_locks shards.
  std::vector<uint8_t> empty_bin;
  void findpos(int64_t *dictidx, const uint64_t &startposidx);
  void remove(const int64_t *dictidx, const uint64_t &startposidx,
              const int64_t read_id_to_remove);

  bbhashdict()
      : bphf(nullptr), start(0), end(0), numkeys(0), dict_numreads(0),
        startpos(nullptr), read_id(nullptr) {}
  bbhashdict(const bbhashdict &) = delete;
  bbhashdict &operator=(const bbhashdict &) = delete;
  bbhashdict(bbhashdict &&) noexcept = default;
  bbhashdict &operator=(bbhashdict &&) noexcept = default;
  ~bbhashdict() = default;
};

namespace detail {

struct thread_range {
  uint64_t begin;
  uint64_t end;
};

inline thread_range split_thread_range(const uint64_t item_count,
                                       const int thread_id,
                                       const int thread_count) {
  thread_range range;
  range.begin = uint64_t(thread_id) * item_count / thread_count;
  range.end = uint64_t(thread_id + 1) * item_count / thread_count;
  if (thread_id == thread_count - 1)
    range.end = item_count;
  return range;
}

inline bool valid_bucket_range(const bbhashdict &dictionary,
                               const int64_t *dictidx) {
  // dictidx[1] <= dictidx[0] covers both empty ranges (==) and inverted ones
  // (<).  An empty range means the bucket has no live elements; the tail
  // marker at dictidx[0] must not be used as a read index.
  if (dictidx[0] < 0 || dictidx[1] <= dictidx[0])
    return false;

  const uint64_t begin = static_cast<uint64_t>(dictidx[0]);
  const uint64_t end = static_cast<uint64_t>(dictidx[1]);
  return begin < dictionary.dict_numreads && end <= dictionary.dict_numreads;
}

template <size_t bitset_size>
void compute_dictionary_keys(std::bitset<bitset_size> *read_bits,
                             const std::bitset<bitset_size> &index_mask,
                             const int start_base, const uint32_t read_count,
                             const int bits_per_base, uint64_t *dictionary_keys,
                             const char depleted_base = 'N') {
  const std::bitset<bitset_size> local_index_mask = index_mask;
  const uint32_t local_read_count = read_count;
  const int local_bits_per_base = bits_per_base;
  const int local_start_base = start_base;
  std::bitset<bitset_size> *local_read_bits = read_bits;
  uint64_t *local_dictionary_keys = dictionary_keys;
#pragma omp parallel for default(none)                                         \
    shared(local_read_bits, local_dictionary_keys)                             \
    firstprivate(local_index_mask, local_read_count, local_bits_per_base,      \
                     local_start_base, depleted_base)
  for (int64_t read_index = 0;
       read_index < static_cast<int64_t>(local_read_count); read_index++) {
    std::bitset<bitset_size> masked_read_bits =
        local_read_bits[read_index] & local_index_mask;
    uint64_t key =
        (masked_read_bits >> (local_bits_per_base * local_start_base))
            .to_ullong();
    if (depleted_base == 'C') {
      key &= ~((key >> 1) & 0x5555555555555555ULL);
    } else if (depleted_base == 'G') {
      key &= ~((~key >> 1) & 0x5555555555555555ULL);
    }
    local_dictionary_keys[read_index] = key;
  }
}

inline uint32_t compact_dictionary_keys(uint64_t *dictionary_keys,
                                        const uint16_t *read_lengths,
                                        const uint32_t read_count,
                                        const int dictionary_end) {
  uint32_t eligible_read_count = 0;
  for (uint32_t read_index = 0; read_index < read_count; read_index++) {
    if (read_lengths[read_index] > dictionary_end) {
      dictionary_keys[eligible_read_count] = dictionary_keys[read_index];
      eligible_read_count++;
    }
  }

  return eligible_read_count;
}

inline void merge_sorted_key_ranges(const uint64_t *source_keys,
                                    uint64_t *destination_keys,
                                    const uint64_t left_begin,
                                    const uint64_t left_end,
                                    const uint64_t right_end) {
  std::merge(source_keys + left_begin, source_keys + left_end,
             source_keys + left_end, source_keys + right_end,
             destination_keys + left_begin);
}

inline uint32_t sort_and_deduplicate_keys(uint64_t *dictionary_keys,
                                          const uint32_t key_count) {
  if (key_count < 2) {
    return key_count;
  }

  const int chunk_count =
      std::max(1, std::min<int>(omp_get_max_threads(), key_count));
  if (chunk_count == 1) {
    std::sort(dictionary_keys, dictionary_keys + key_count);
  } else {
    std::vector<uint64_t> scratch(key_count);
    std::vector<uint64_t> chunk_boundaries(static_cast<size_t>(chunk_count) +
                                           1);
    for (int chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
      chunk_boundaries[static_cast<size_t>(chunk_index)] =
          split_thread_range(key_count, chunk_index, chunk_count).begin;
    }
    chunk_boundaries[static_cast<size_t>(chunk_count)] = key_count;

#pragma omp parallel for schedule(static)
    for (int chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
      const uint64_t begin = chunk_boundaries[static_cast<size_t>(chunk_index)];
      const uint64_t end =
          chunk_boundaries[static_cast<size_t>(chunk_index) + 1];
      if (begin < end) {
        std::sort(dictionary_keys + begin, dictionary_keys + end);
      }
    }

    uint64_t *source_keys = dictionary_keys;
    uint64_t *destination_keys = scratch.data();
    for (int merge_width = 1; merge_width < chunk_count; merge_width *= 2) {
      const int merge_count =
          (chunk_count + 2 * merge_width - 1) / (2 * merge_width);
#pragma omp parallel for schedule(static)
      for (int merge_index = 0; merge_index < merge_count; ++merge_index) {
        const int left_chunk = merge_index * 2 * merge_width;
        const int middle_chunk =
            std::min(left_chunk + merge_width, chunk_count);
        const int right_chunk =
            std::min(left_chunk + 2 * merge_width, chunk_count);
        const uint64_t left_begin =
            chunk_boundaries[static_cast<size_t>(left_chunk)];
        const uint64_t middle_end =
            chunk_boundaries[static_cast<size_t>(middle_chunk)];
        const uint64_t right_end =
            chunk_boundaries[static_cast<size_t>(right_chunk)];
        if (middle_chunk == right_chunk) {
          std::copy(source_keys + left_begin, source_keys + right_end,
                    destination_keys + left_begin);
        } else {
          merge_sorted_key_ranges(source_keys, destination_keys, left_begin,
                                  middle_end, right_end);
        }
      }
      std::swap(source_keys, destination_keys);
    }

    if (source_keys != dictionary_keys) {
      std::copy(source_keys, source_keys + key_count, dictionary_keys);
    }
  }

  uint32_t unique_key_index = 0;
  for (uint32_t key_index = 1; key_index < key_count; key_index++) {
    if (dictionary_keys[key_index] != dictionary_keys[unique_key_index])
      dictionary_keys[++unique_key_index] = dictionary_keys[key_index];
  }

  return unique_key_index + 1;
}

inline std::vector<uint64_t> compute_hash_values(const bbhashdict &dictionary,
                                                 const uint64_t *keys,
                                                 const uint32_t key_count,
                                                 const int dict_index) {
  std::vector<uint64_t> hash_values(static_cast<size_t>(key_count), 0);
  if (key_count == 0)
    return hash_values;

  const bbhashdict *local_dictionary = &dictionary;
  const uint64_t *local_keys = keys;
  uint64_t *local_hash_values = hash_values.data();
#pragma omp parallel for schedule(static) default(none)                        \
    shared(local_dictionary, local_keys, local_hash_values)                    \
    firstprivate(key_count)
  for (int64_t key_index = 0; key_index < static_cast<int64_t>(key_count);
       key_index++) {
    local_hash_values[key_index] =
        (*(local_dictionary->bphf))(local_keys[key_index]);
  }

  for (uint32_t key_index = 0; key_index < key_count; key_index++) {
    if (hash_values[key_index] >= dictionary.numkeys) {
      SPRING_LOG_DEBUG(
          "block_id=dict-hash-compute:" + std::to_string(dict_index) +
          ", hash out-of-range: expected_bytes=" +
          std::to_string(dictionary.numkeys) +
          ", actual_bytes=" + std::to_string(hash_values[key_index]) +
          ", index=" + std::to_string(key_index));
    }
  }
  return hash_values;
}

inline void count_bucket_sizes(bbhashdict &dictionary,
                               const std::vector<uint64_t> &hash_values,
                               const int dict_index) {
  bbhashdict *local_dictionary = &dictionary;
  const std::vector<uint64_t> *local_hash_values = &hash_values;
#pragma omp parallel for schedule(static) default(none)                        \
    shared(local_dictionary, local_hash_values) firstprivate(dict_index)
  for (int64_t hash_index = 0;
       hash_index < static_cast<int64_t>(local_hash_values->size());
       hash_index++) {
    const uint64_t current_hash = (*local_hash_values)[hash_index];
    if (current_hash >= local_dictionary->numkeys) {
      SPRING_LOG_DEBUG(
          "block_id=dict-bucket-count:" + std::to_string(dict_index) +
          ", hash out-of-range: expected_bytes=" +
          std::to_string(local_dictionary->numkeys) +
          ", actual_bytes=" + std::to_string(current_hash) +
          ", index=" + std::to_string(hash_index));
      continue;
    }
#pragma omp atomic update
    local_dictionary->startpos[current_hash + 1]++;
  }
}

inline void finalize_bucket_offsets(bbhashdict &dictionary) {
  dictionary.empty_bin.assign(dictionary.numkeys, 0);
  bbhashdict *local_dictionary = &dictionary;

#pragma omp parallel for schedule(static) default(none) shared(local_dictionary)
  for (int64_t key_index = 0;
       key_index < static_cast<int64_t>(local_dictionary->numkeys);
       key_index++) {
    if (local_dictionary->startpos[key_index + 1] == 0)
      local_dictionary->empty_bin[key_index] = 1;
  }

  const uint32_t scan_length =
      (local_dictionary->numkeys > 0) ? local_dictionary->numkeys - 1 : 0;
  if (scan_length == 0)
    return;

  const int thread_count =
      std::max(1, std::min<int>(omp_get_max_threads(), scan_length));
  std::vector<uint32_t> block_totals(static_cast<size_t>(thread_count), 0);
  std::vector<uint32_t> block_offsets(static_cast<size_t>(thread_count), 0);

#pragma omp parallel num_threads(thread_count) default(none)                   \
    shared(local_dictionary, block_totals, block_offsets, scan_length)         \
    firstprivate(thread_count)
  {
    const int thread_id = omp_get_thread_num();
    const uint32_t begin =
        static_cast<uint32_t>(thread_id) * scan_length / thread_count;
    const uint32_t end =
        static_cast<uint32_t>(thread_id + 1) * scan_length / thread_count;

    uint32_t running_sum = 0;
    for (uint32_t local_index = begin; local_index < end; local_index++) {
      const uint32_t startpos_index = local_index + 1;
      running_sum += local_dictionary->startpos[startpos_index];
      local_dictionary->startpos[startpos_index] = running_sum;
    }
    block_totals[static_cast<size_t>(thread_id)] = running_sum;

#pragma omp barrier

#pragma omp single
    {
      uint32_t prefix_sum = 0;
      for (int block_index = 0; block_index < thread_count; block_index++) {
        block_offsets[static_cast<size_t>(block_index)] = prefix_sum;
        prefix_sum += block_totals[static_cast<size_t>(block_index)];
      }
    }

    const uint32_t carry = block_offsets[static_cast<size_t>(thread_id)];
    if (carry != 0) {
      for (uint32_t local_index = begin; local_index < end; local_index++) {
        const uint32_t startpos_index = local_index + 1;
        local_dictionary->startpos[startpos_index] += carry;
      }
    }
  }
}

inline void populate_bucket_read_ids(bbhashdict &dictionary,
                                     uint16_t *read_lengths,
                                     const std::vector<uint64_t> &hash_values,
                                     const int dict_index,
                                     const uint32_t numreads) {
  dictionary.read_id = std::make_unique<uint32_t[]>(dictionary.dict_numreads);

  // Save bucket starts so we can sort each populated bucket range after
  // parallel placement and preserve ascending read-id order.
  std::vector<uint32_t> bucket_starts(static_cast<size_t>(dictionary.numkeys) +
                                      1);
  std::copy_n(dictionary.startpos.get(), dictionary.numkeys + 1,
              bucket_starts.begin());

  std::vector<uint32_t> eligible_read_ids;
  eligible_read_ids.reserve(dictionary.dict_numreads);
  for (uint32_t read_index = 0; read_index < numreads; read_index++) {
    if (read_lengths[read_index] > dictionary.end)
      eligible_read_ids.push_back(read_index);
  }

  const uint32_t usable_count =
      std::min<uint32_t>(static_cast<uint32_t>(hash_values.size()),
                         static_cast<uint32_t>(eligible_read_ids.size()));

  bbhashdict *local_dictionary = &dictionary;
  const std::vector<uint64_t> *local_hash_values = &hash_values;
  const std::vector<uint32_t> *local_eligible_read_ids = &eligible_read_ids;

#pragma omp parallel for schedule(static) default(none)                        \
    shared(local_dictionary, local_hash_values, local_eligible_read_ids)       \
    firstprivate(usable_count, dict_index)
  for (int64_t hash_index = 0; hash_index < static_cast<int64_t>(usable_count);
       hash_index++) {
    const uint64_t current_hash = (*local_hash_values)[hash_index];
    if (current_hash >= local_dictionary->numkeys) {
      SPRING_LOG_DEBUG(
          "block_id=dict-bucket-populate:" + std::to_string(dict_index) +
          ", hash out-of-range: expected_bytes=" +
          std::to_string(local_dictionary->numkeys) +
          ", actual_bytes=" + std::to_string(current_hash) +
          ", index=" + std::to_string(hash_index));
      continue;
    }

    uint32_t insert_index;
    // Use raw pointers for omp atomic capture: MSVC does not inline
    // unique_ptr::operator[] in debug builds, causing atomic to fail.
    uint32_t *startpos_raw = local_dictionary->startpos.get();
    uint32_t *read_id_raw = local_dictionary->read_id.get();
#pragma omp atomic capture
    insert_index = startpos_raw[current_hash]++;

    read_id_raw[insert_index] = (*local_eligible_read_ids)[hash_index];
  }

  if (usable_count < static_cast<uint32_t>(hash_values.size())) {
    SPRING_LOG_DEBUG(
        "block_id=dict-bucket-populate:" + std::to_string(dict_index) +
        ", exhausted source reads: expected_bytes=" +
        std::to_string(hash_values.size()) +
        ", actual_bytes=" + std::to_string(usable_count));
  }

#pragma omp parallel for schedule(static) default(none)                        \
    shared(local_dictionary, bucket_starts)
  for (int64_t bucket_index = 0;
       bucket_index < static_cast<int64_t>(local_dictionary->numkeys);
       bucket_index++) {
    const uint32_t begin = bucket_starts[static_cast<size_t>(bucket_index)];
    const uint32_t end = bucket_starts[static_cast<size_t>(bucket_index) + 1];
    if (end > begin + 1) {
      std::sort(&local_dictionary->read_id[begin],
                &local_dictionary->read_id[end]);
    }
  }
}

inline void restore_bucket_starts(bbhashdict &dictionary) {
  for (int64_t key_index = dictionary.numkeys; key_index >= 1; key_index--)
    dictionary.startpos[key_index] = dictionary.startpos[key_index - 1];
  dictionary.startpos[0] = 0;
}

} // namespace detail

template <size_t bitset_size>
void stringtobitset(const std::string &s, const uint16_t readlen,
                    std::bitset<bitset_size> &b,
                    std::bitset<bitset_size> **basemask) {
  for (int i = 0; i < readlen; i++)
    b |= basemask[i][(uint8_t)s[i]];
}

template <size_t bitset_size>
void generateindexmasks(std::bitset<bitset_size> *index_masks, bbhashdict *dict,
                        int numdict, int bpb) {
  for (int j = 0; j < numdict; j++)
    index_masks[j].reset();
  for (int j = 0; j < numdict; j++)
    for (int i = bpb * dict[j].start; i < bpb * (dict[j].end + 1); i++)
      index_masks[j][i] = 1;
  return;
}

template <size_t bitset_size>
void constructdictionary(std::bitset<bitset_size> *read, bbhashdict *dict,
                         uint16_t *read_lengths, const int numdict,
                         const uint32_t numreads, const int bpb,
                         const char depleted_base = 'N',
                         const bool use_external_mphf = false,
                         const std::string &mphf_tmp_dir = "") {
  auto format_seconds = [](const std::chrono::steady_clock::duration &value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
           << std::chrono::duration_cast<std::chrono::milliseconds>(value)
                      .count() /
                  1000.0;
    return stream.str();
  };

  auto index_masks = std::make_unique<std::bitset<bitset_size>[]>(
      static_cast<size_t>(numdict));
  generateindexmasks<bitset_size>(index_masks.get(), dict, numdict, bpb);

  for (int dict_index = 0; dict_index < numdict; dict_index++) {
    bbhashdict &current_dict = dict[dict_index];
    std::vector<uint64_t> dictionary_keys(numreads, 0);
    uint64_t *dictionary_keys_data = dictionary_keys.data();

    detail::compute_dictionary_keys<bitset_size>(
        read, index_masks[dict_index], current_dict.start, numreads, bpb,
        dictionary_keys_data, depleted_base);

    // Keep only reads that are long enough for this dictionary.
    current_dict.dict_numreads = detail::compact_dictionary_keys(
        dictionary_keys_data, read_lengths, numreads, current_dict.end);

    // Preserve compacted key order for read-id bucket population after MPHF
    // build, while dictionary_keys_data is sorted/deduplicated for MPHF input.
    std::vector<uint64_t> compacted_keys;
    compacted_keys.assign(dictionary_keys_data,
                          dictionary_keys_data + current_dict.dict_numreads);

    current_dict.numkeys = detail::sort_and_deduplicate_keys(
        dictionary_keys_data, current_dict.dict_numreads);

    SPRING_LOG_INFO(std::string("Dictionary ") +
                    std::to_string(dict_index + 1) + " of " +
                    std::to_string(numdict) + ": Building MPHF for " +
                    std::to_string(current_dict.numkeys) + " keys...");
    pthash::build_configuration config;
    // pthash's internal builder is faster here when run serially
    config.num_threads = 1;
    config.minimal = false;
    config.verbose = false;
    current_dict.bphf = std::make_unique<boophf_t>();
    SPRING_LOG_INFO("  Building MPHF...");
    // The pthash external-memory builder is designed for large key sets.
    // For small sets, the internal builder is both faster and more reliable.
    constexpr uint64_t kExternalMphfMinKeys = 1'000'000;
    const bool use_external = use_external_mphf && !mphf_tmp_dir.empty() &&
                              current_dict.numkeys >= kExternalMphfMinKeys;
    const auto mphf_build_start = std::chrono::steady_clock::now();
    if (use_external) {
      config.tmp_dir = mphf_tmp_dir;
      current_dict.bphf->build_in_external_memory(dictionary_keys_data,
                                                  current_dict.numkeys, config);
    } else {
      current_dict.bphf->build_in_internal_memory(dictionary_keys_data,
                                                  current_dict.numkeys, config);
    }
    const auto mphf_build_end = std::chrono::steady_clock::now();
    current_dict.numkeys =
        static_cast<uint32_t>(current_dict.bphf->table_size());
    SPRING_LOG_INFO(std::string("Done. (T=") +
                    std::to_string(current_dict.numkeys) + ")");
    SPRING_LOG_INFO("  MPHF build time: " +
                    format_seconds(mphf_build_end - mphf_build_start) + " s");

    SPRING_LOG_INFO("  Computing hash values in memory...");
    const auto hash_pass_start = std::chrono::steady_clock::now();
    const std::vector<uint64_t> hash_values =
        detail::compute_hash_values(current_dict, compacted_keys.data(),
                                    current_dict.dict_numreads, dict_index);
    const auto hash_pass_end = std::chrono::steady_clock::now();
    SPRING_LOG_INFO("  Hash pass time: " +
                    format_seconds(hash_pass_end - hash_pass_start) + " s");

    current_dict.startpos =
        std::make_unique<uint32_t[]>(current_dict.numkeys + 1);
    std::fill_n(current_dict.startpos.get(), current_dict.numkeys + 1, 0);
    detail::count_bucket_sizes(current_dict, hash_values, dict_index);
    detail::finalize_bucket_offsets(current_dict);
    detail::populate_bucket_read_ids(current_dict, read_lengths, hash_values,
                                     dict_index, numreads);
    detail::restore_bucket_starts(current_dict);
  }

  SPRING_LOG_INFO("  Done finalization.");
  return;
}

template <size_t bitset_size>
void generatemasks(std::bitset<bitset_size> **mask, const int max_readlen,
                   const int bpb) {
  // Zero trailing positions so shifted reads can be compared consistently.
  for (int i = 0; i < max_readlen; i++) {
    for (int j = 0; j < max_readlen; j++) {
      mask[i][j].reset();
      for (int k = bpb * i; k < bpb * max_readlen - bpb * j; k++)
        mask[i][j][k] = 1;
    }
  }
  return;
}

template <size_t bitset_size>
void chartobitset(char *s, const int readlen, std::bitset<bitset_size> &b,
                  std::bitset<bitset_size> **basemask) {
  b.reset();
  for (int i = 0; i < readlen; i++)
    b |= basemask[i][(uint8_t)s[i]];
  return;
}

} // namespace spring

#endif // SPRING_BITSET_DICTIONARY_H_
