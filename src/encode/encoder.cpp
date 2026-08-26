// Encodes reordered reads against their consensus/reference representation and
// writes the compressed sequence, position, noise, and unaligned side streams.

#include "encoder.h"
#include "io_utils.h"
#include "progress.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <omp.h>
#include <sstream>
#include <string>
#include <vector>

namespace spring {

std::string thread_file_path(const std::string &base_path, const int thread_id,
                             const char *suffix = "") {
  return base_path + '.' + std::to_string(thread_id) + suffix;
}

template <typename T> void append_binary(std::string &buffer, const T &value) {
  const size_t old_size = buffer.size();
  buffer.resize(old_size + sizeof(T));
  std::memcpy(buffer.data() + old_size, &value, sizeof(T));
}

uint64_t write_packed_sequence(const std::string &sequence_bytes,
                               std::vector<char> &packed_bytes,
                               bool bisulfite_ternary) {
  static const std::array<uint8_t, 128> base_to_int = []() {
    std::array<uint8_t, 128> table{};
    table[static_cast<uint8_t>('A')] = 0;
    table[static_cast<uint8_t>('a')] = 0;
    table[static_cast<uint8_t>('C')] = 2;
    table[static_cast<uint8_t>('c')] = 2;
    table[static_cast<uint8_t>('G')] = 1;
    table[static_cast<uint8_t>('g')] = 1;
    table[static_cast<uint8_t>('T')] = 3;
    table[static_cast<uint8_t>('t')] = 3;
    return table;
  }();

  packed_bytes.clear();
  packed_bytes.reserve(std::max<size_t>(sequence_bytes.size() / 2, 1));
  std::array<char, 5> trailing_bases{};
  uint64_t sequence_length = 0;
  size_t trailing_count = 0;
  const size_t bases_per_byte = bisulfite_ternary ? 5 : 4;

  size_t cursor = 0;
  while (cursor < sequence_bytes.size()) {
    if (cursor + sizeof(uint32_t) > sequence_bytes.size()) {
      throw std::runtime_error(
          "Corrupted sequence buffer: truncated read length header.");
    }
    uint32_t record_len = 0;
    std::memcpy(&record_len, sequence_bytes.data() + cursor, sizeof(uint32_t));
    cursor += sizeof(uint32_t);
    if (cursor + record_len > sequence_bytes.size()) {
      throw std::runtime_error(
          "Corrupted sequence buffer: truncated read payload.");
    }
    const char *dna = sequence_bytes.data() + cursor;
    cursor += record_len;

    sequence_length += record_len;
    for (uint32_t input_index = 0; input_index < record_len; input_index++) {
      trailing_bases[trailing_count++] = dna[input_index];
      if (trailing_count < bases_per_byte) {
        continue;
      }

      if (!bisulfite_ternary) {
        packed_bytes.push_back(
            static_cast<char>(64 * base_to_int[(uint8_t)trailing_bases[3]] +
                              16 * base_to_int[(uint8_t)trailing_bases[2]] +
                              4 * base_to_int[(uint8_t)trailing_bases[1]] +
                              base_to_int[(uint8_t)trailing_bases[0]]));
      } else {
        bool has_c = false;
        for (int k = 0; k < 5; k++) {
          if (trailing_bases[k] == 'C' || trailing_bases[k] == 'c') {
            has_c = true;
            break;
          }
        }
        if (!has_c) {
          uint8_t packed = 0;
          uint8_t p3 = 1;
          for (int k = 0; k < 5; k++) {
            uint8_t val = base_to_int[static_cast<uint8_t>(trailing_bases[k])];
            if (val == 3)
              val = 2; // T is 2 in ternary
            packed += val * p3;
            p3 *= 3;
          }
          packed_bytes.push_back(static_cast<char>(packed));
        } else {
          packed_bytes.push_back(static_cast<char>(243));
          uint16_t escape = 0;
          for (int k = 0; k < 5; k++) {
            uint8_t val = base_to_int[static_cast<uint8_t>(trailing_bases[k])];
            escape |= (val << (2 * k));
          }
          const size_t old_size = packed_bytes.size();
          packed_bytes.resize(old_size + sizeof(uint16_t));
          std::memcpy(packed_bytes.data() + old_size, &escape,
                      sizeof(uint16_t));
        }
      }
      trailing_count = 0;
    }
  }

  if (trailing_count > 0) {
    if (!bisulfite_ternary) {
      uint8_t packed_byte = 0;
      for (size_t i = 0; i < trailing_count; ++i) {
        packed_byte |= (base_to_int[(uint8_t)trailing_bases[i]] << (2 * i));
      }
      packed_bytes.push_back(static_cast<char>(packed_byte));
    } else {
      packed_bytes.push_back(static_cast<char>(243));
      uint16_t escape = 0;
      for (size_t i = 0; i < trailing_count; ++i) {
        uint8_t val = base_to_int[static_cast<uint8_t>(trailing_bases[i])];
        escape |= (val << (2 * i));
      }
      const size_t old_size = packed_bytes.size();
      packed_bytes.resize(old_size + sizeof(uint16_t));
      std::memcpy(packed_bytes.data() + old_size, &escape, sizeof(uint16_t));
    }
  }

  if (cursor != sequence_bytes.size()) {
    throw std::runtime_error(
        "Corrupted sequence buffer: trailing bytes after expected records.");
  }

  return sequence_length;
}

std::string pack_compress_seq(
    const encoder_global &encoder_state,
    const std::vector<encoded_metadata_buffer> &thread_metadata_outputs,
    uint64_t *thread_sequence_lengths) {
  if (encoder_state.num_thr <= 0)
    return {};
  SPRING_LOG_DEBUG("block_id=enc-pack-main, pack_compress_seq start: threads=" +
                   std::to_string(encoder_state.num_thr));
  std::vector<std::vector<char>> packed_chunks(
      static_cast<size_t>(encoder_state.num_thr));
#pragma omp parallel for schedule(static)
  for (int tid = 0; tid < encoder_state.num_thr; tid++) {
    SPRING_LOG_DEBUG(
        "block_id=enc-pack-chunk-" + std::to_string(tid) +
        ", pack_sequence_chunk start: bytes=" +
        std::to_string(thread_metadata_outputs[tid].sequence_bytes.size()));
    const uint64_t sequence_length =
        write_packed_sequence(thread_metadata_outputs[tid].sequence_bytes,
                              packed_chunks[static_cast<size_t>(tid)],
                              encoder_state.bisulfite_ternary);
    thread_sequence_lengths[tid] = sequence_length;
    if (sequence_length != thread_metadata_outputs[tid].sequence_base_count) {
      throw std::runtime_error(
          "Sequence base count mismatch while packing encoder output.");
    }
    SPRING_LOG_DEBUG("block_id=enc-pack-chunk-" + std::to_string(tid) +
                     ", pack_sequence_chunk done: seq_bases=" +
                     std::to_string(sequence_length));
  }
  SPRING_LOG_DEBUG("block_id=enc-pack-main, per-thread packing complete.");

  size_t packed_size = 0;
  for (int tid = 0; tid < encoder_state.num_thr; tid++) {
    const std::vector<char> &chunk = packed_chunks[static_cast<size_t>(tid)];
    packed_size += sizeof(uint64_t) + chunk.size();
  }
  std::vector<char> monolithic_packed_bytes;
  monolithic_packed_bytes.reserve(packed_size);
  for (int tid = 0; tid < encoder_state.num_thr; tid++) {
    const std::vector<char> &chunk = packed_chunks[static_cast<size_t>(tid)];
    const uint64_t chunk_size = static_cast<uint64_t>(chunk.size());
    const size_t old_size = monolithic_packed_bytes.size();
    monolithic_packed_bytes.resize(old_size + sizeof(uint64_t) + chunk.size());
    std::memcpy(monolithic_packed_bytes.data() + old_size, &chunk_size,
                sizeof(uint64_t));
    if (!chunk.empty()) {
      std::memcpy(monolithic_packed_bytes.data() + old_size + sizeof(uint64_t),
                  chunk.data(), chunk.size());
    }
  }
  SPRING_LOG_DEBUG(
      "block_id=enc-pack-main, monolithic packed buffer assembled: "
      "bytes=" +
      std::to_string(monolithic_packed_bytes.size()));

  if (monolithic_packed_bytes.empty()) {
    return {};
  }

  const std::vector<char> compressed_bytes =
      bsc_compress_bytes(monolithic_packed_bytes);
  SPRING_LOG_DEBUG("block_id=enc-pack-main, in-memory BSC_compress complete: "
                   "bytes=" +
                   std::to_string(compressed_bytes.size()));
  return std::string(compressed_bytes.begin(), compressed_bytes.end());
}

std::string buildcontig(std::list<contig_reads> &current_contig,
                        const uint32_t &list_size) {
  static const char base_char_lookup[5] = {'A', 'C', 'G', 'T', 'N'};
  auto base_index = [](const uint8_t base) -> uint8_t {
    switch (base) {
    case 'A':
    case 'a':
      return 0;
    case 'C':
    case 'c':
      return 1;
    case 'G':
    case 'g':
      return 2;
    case 'T':
    case 't':
      return 3;
    default:
      return 4;
    }
  };
  if (list_size == 1)
    return (current_contig.front()).read;
  auto current_contig_it = current_contig.begin();
  int64_t current_position = 0;
  int64_t contig_size = 0;
  int64_t positions_to_append;
  std::vector<std::array<uint16_t, 4>> base_counts;
  for (; current_contig_it != current_contig.end(); ++current_contig_it) {
    if (current_contig_it == current_contig.begin())
      positions_to_append = (*current_contig_it).read_length;
    else {
      current_position = (*current_contig_it).pos;
      if (current_position + (*current_contig_it).read_length > contig_size)
        positions_to_append =
            current_position + (*current_contig_it).read_length - contig_size;
      else
        positions_to_append = 0;
    }
    if (contig_size + positions_to_append > MAX_CONTIG_GROWTH) {
      std::stringstream ss;
      ss << "Excessive contig growth detected during encoding: "
         << (contig_size + positions_to_append)
         << " bases (pos=" << current_position
         << ", len=" << (*current_contig_it).read_length
         << ", size=" << contig_size << ") exceeds limit of "
         << MAX_CONTIG_GROWTH
         << ". This indicates either pathological input data or memory "
            "corruption.";
      throw std::runtime_error(ss.str());
    }
    base_counts.insert(base_counts.end(), (size_t)positions_to_append,
                       {0, 0, 0, 0});
    contig_size = contig_size + positions_to_append;
    for (size_t i = 0;
         i < static_cast<size_t>((*current_contig_it).read_length); ++i) {
      const size_t idx = static_cast<size_t>(current_position) + i;
      const uint8_t base_idx =
          base_index(static_cast<uint8_t>((*current_contig_it).read[i]));
      if (base_idx < 4 && base_counts[idx][base_idx] < 65535) {
        base_counts[idx][base_idx] += 1;
      }
    }
  }
  std::string ref(base_counts.size(), 'A');
  for (size_t i = 0; i < base_counts.size(); i++) {
    uint16_t best_base_count = 0;
    uint32_t best_base_index = 0;
    for (uint32_t candidate_base_index = 0; candidate_base_index < 4;
         candidate_base_index++)
      if (base_counts[i][candidate_base_index] > best_base_count) {
        best_base_count = base_counts[i][candidate_base_index];
        best_base_index = candidate_base_index;
      }
    ref[i] = base_char_lookup[best_base_index];
  }
  return ref;
}

void writecontig(const std::string &ref,
                 std::list<contig_reads> &current_contig,
                 encoded_metadata_buffer &metadata_output,
                 const encoder_global &eg, uint64_t &abs_pos) {
  uint32_t ref_len = static_cast<uint32_t>(ref.size());
  append_binary(metadata_output.sequence_bytes, ref_len);
  metadata_output.sequence_bytes.append(ref);
  metadata_output.sequence_base_count += ref.size();
  uint16_t pos_var;
  size_t previous_noise_offset = 0;
  auto current_contig_it = current_contig.begin();
  int64_t current_position;
  uint64_t absolute_current_position;
  for (; current_contig_it != current_contig.end(); ++current_contig_it) {
    current_position = (*current_contig_it).pos;
    previous_noise_offset = 0;
    for (size_t read_offset = 0;
         read_offset < static_cast<size_t>((*current_contig_it).read_length);
         ++read_offset) {
      const size_t pos = static_cast<size_t>(current_position) + read_offset;
      if ((*current_contig_it).read[read_offset] != ref[pos]) {
        metadata_output.noise_serialized.push_back(eg.enc_noise[(
            uint8_t)ref[pos]][(uint8_t)(*current_contig_it).read[read_offset]]);
        pos_var = static_cast<uint16_t>(read_offset - previous_noise_offset);
        metadata_output.noise_positions.push_back(pos_var);
        previous_noise_offset = read_offset;
      }
    }
    metadata_output.noise_serialized.push_back('\n');
    absolute_current_position = abs_pos + current_position;
    if (static_cast<uint64_t>(current_position) +
            (*current_contig_it).read_length >
        ref.size()) {
      throw std::runtime_error(
          "writecontig: read at pos=" + std::to_string(current_position) +
          " + len=" + std::to_string((*current_contig_it).read_length) +
          " exceeds ref.size()=" + std::to_string(ref.size()) +
          ", abs_pos=" + std::to_string(abs_pos));
    }
    metadata_output.position_entries.push_back(absolute_current_position);
    metadata_output.read_order_entries.push_back((*current_contig_it).order);
    metadata_output.read_length_entries.push_back(
        (*current_contig_it).read_length);
    metadata_output.orientation_entries.push_back((*current_contig_it).RC);
  }
  abs_pos += ref.size();
  return;
}

void getDataParams(encoder_global &eg, const compression_params &cp,
                   const reorder_encoder_artifact &reorder_artifact) {
  uint32_t clean_read_count;
  uint32_t total_read_count;
  clean_read_count =
      cp.read_info.num_reads_clean[0] + cp.read_info.num_reads_clean[1];
  total_read_count = cp.read_info.num_reads;

  eg.numreads_s = reorder_artifact.singleton_count;
  eg.numreads = clean_read_count - eg.numreads_s;
  eg.numreads_N = total_read_count - clean_read_count;
  eg.bisulfite_ternary = cp.encoding.bisulfite_ternary;
}

void correct_order(uint32_t *order_s, const encoder_global &eg,
                   reorder_encoder_artifact &reorder_artifact) {
  uint32_t total_read_count = eg.numreads + eg.numreads_s + eg.numreads_N;
  std::vector<uint8_t> is_n_read(total_read_count, 0);
  for (uint32_t i = 0; i < eg.numreads_N; i++) {
    is_n_read[order_s[eg.numreads_s + i]] = 1;
  }

  std::vector<uint32_t> cumulative_N_reads(eg.numreads + eg.numreads_s, 0);
  uint32_t clean_read_index = 0;
  uint32_t n_reads_seen = 0;
  for (uint32_t i = 0; i < total_read_count; i++) {
    if (is_n_read[i])
      n_reads_seen++;
    else
      cumulative_N_reads[clean_read_index++] = n_reads_seen;
  }

  // Insert the removed N-read slots back into singleton and clean-read
  // orderings.
  for (uint32_t i = 0; i < eg.numreads_s; i++)
    order_s[i] += cumulative_N_reads[order_s[i]];

  for (reorder_encoder_shard &shard : reorder_artifact.aligned_shards) {
    if (!shard.order_bytes.empty()) {
      // In-memory path: adjust order values in-place.
      for (size_t offset = 0; offset < shard.order_bytes.size();
           offset += sizeof(uint32_t)) {
        uint32_t read_position = 0;
        std::memcpy(&read_position, shard.order_bytes.data() + offset,
                    sizeof(uint32_t));
        if (read_position < cumulative_N_reads.size()) {
          read_position += cumulative_N_reads[read_position];
          std::memcpy(shard.order_bytes.data() + offset, &read_position,
                      sizeof(uint32_t));
        }
      }
    } else if (!shard.order_file.empty()) {
      // Disk-path: read all order values from file, adjust, write back.
      // Skip the round-trip if there are no N-reads (all offsets are zero).
      const bool any_offset =
          std::any_of(cumulative_N_reads.begin(), cumulative_N_reads.end(),
                      [](uint32_t v) { return v != 0; });
      if (any_offset) {
        std::vector<uint32_t> order_vals;
        {
          std::ifstream in(shard.order_file, std::ios::binary);
          if (!in)
            throw std::runtime_error(
                "correct_order: cannot open shard order file: " +
                shard.order_file);
          uint32_t val;
          while (in.read(reinterpret_cast<char *>(&val), sizeof(uint32_t)))
            order_vals.push_back(val);
        }
        for (uint32_t &rp : order_vals)
          if (rp < cumulative_N_reads.size())
            rp += cumulative_N_reads[rp];
        {
          std::ofstream out(shard.order_file,
                            std::ios::binary | std::ios::trunc);
          if (!out)
            throw std::runtime_error(
                "correct_order: cannot write shard order file: " +
                shard.order_file);
          out.write(reinterpret_cast<const char *>(order_vals.data()),
                    static_cast<std::streamsize>(order_vals.size() *
                                                 sizeof(uint32_t)));
        }
      }
    }
  }
  return;
}

} // namespace spring
