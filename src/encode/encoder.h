// Declares encoder interfaces and shared state used by the overlap-based
// read encoding stage.

#ifndef SPRING_ENCODER_H_
#define SPRING_ENCODER_H_

#include "params.h"
#include "raii.h"
#include "read_reordering.h"
#include "stream_reordering.h"
#include <bitset>
#include <cstdint>
#include <list>
#include <omp.h>
#include <string>

namespace spring {

// Forward declarations for types defined in bitset_dictionary.h or
// encoder_impl.h
class bbhashdict;
struct compression_params;
template <size_t bitset_size> struct encoder_global_b;

struct encoder_global {
  uint32_t numreads = 0;
  uint32_t numreads_s = 0;
  uint32_t numreads_N = 0;
  int numdict_s = NUM_DICT_ENCODER;

  int max_readlen = 0;
  int num_thr = 0;

  char enc_noise[128][128] = {{0}};
  bool bisulfite_ternary = false;
  // When non-empty, the encoder flushes per-thread metadata to this directory
  // instead of accumulating all entries in RAM.  Set from
  // compression_params::encoding::encoder_metadata_spill_dir.
  std::string metadata_spill_dir;
};

struct contig_reads {
  std::string read;
  int64_t pos = 0;
  char RC = 0;
  uint32_t order = 0;
  uint16_t read_length = 0;
};

struct encoded_metadata_buffer {
  std::string sequence_bytes;
  uint64_t sequence_base_count = 0;
  std::vector<uint64_t> position_entries;
  std::string noise_serialized;
  std::vector<uint16_t> noise_positions;
  std::vector<uint32_t> read_order_entries;
  std::string orientation_entries;
  std::vector<uint16_t> read_length_entries;
};

// Non-template helpers (definitions in encoder.cpp)
std::string buildcontig(std::list<contig_reads> &current_contig,
                        const uint32_t &list_size);

void writecontig(const std::string &ref,
                 std::list<contig_reads> &current_contig,
                 encoded_metadata_buffer &metadata_output,
                 const encoder_global &eg, uint64_t &abs_pos);

std::string pack_compress_seq(
    const encoder_global &encoder_state,
    const std::vector<encoded_metadata_buffer> &thread_metadata_outputs,
    uint64_t *thread_sequence_lengths);

void getDataParams(encoder_global &eg, const compression_params &cp,
                   const reorder_encoder_artifact &reorder_artifact);

void correct_order(uint32_t *order_s, const encoder_global &eg,
                   reorder_encoder_artifact &reorder_artifact);

// Template interface (definitions in encoder_impl.h)
template <size_t bitset_size>
std::string bitsettostring(std::bitset<bitset_size> encoded_bases,
                           const uint16_t readlen,
                           const encoder_global_b<bitset_size> &egb);

template <size_t bitset_size>
void encode(std::bitset<bitset_size> *reads, bbhashdict *dictionaries,
            uint32_t *read_orders, uint16_t *read_lengths,
            bool *remaining_reads, OmpLock *read_locks,
            OmpLock *dictionary_locks,
            std::vector<encoded_metadata_buffer> &thread_metadata_outputs,
            const reorder_encoder_artifact &reorder_artifact,
            const encoder_global &eg, const encoder_global_b<bitset_size> &egb);

template <size_t bitset_size>
void setglobalarrays(encoder_global &eg, encoder_global_b<bitset_size> &egb);

template <size_t bitset_size>
void readsingletons(std::bitset<bitset_size> *read, uint32_t *order_s,
                    uint16_t *read_lengths_s,
                    const reorder_encoder_artifact &reorder_artifact,
                    const encoder_global &eg,
                    const encoder_global_b<bitset_size> &egb);

template <size_t bitset_size>
reordered_stream_artifact encoder_main(const reorder_encoder_artifact &artifact,
                                       compression_params &cp);

} // namespace spring

#endif // SPRING_ENCODER_H_
