// Provides the templated read-reordering interface.
// Implementation details are moved to read_reordering_impl.h to improve
// compilation efficiency.

#ifndef SPRING_READ_REORDERING_H_
#define SPRING_READ_REORDERING_H_

#include <array>
#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

namespace spring {

struct reorder_input_artifact {
  std::array<std::string, 2> clean_read_streams;
  std::string n_read_bytes;
  std::string n_read_order_bytes;
};

struct reorder_encoder_shard {
  std::string read_bytes;
  std::string orientation_bytes;
  std::string flag_bytes;
  std::string position_bytes;
  std::string order_bytes;
  std::string read_length_bytes;

  // When non-empty, these paths point to the spilled shard files on disk.
  // The encoder streams directly from these files (disk-path only).
  std::string flag_file;
  std::string read_file;
  std::string orientation_file;
  std::string position_file;
  std::string order_file;
  std::string read_length_file;
};

struct reorder_encoder_artifact {
  std::vector<reorder_encoder_shard> aligned_shards;
  std::string singleton_read_bytes;
  std::string singleton_order_bytes;
  std::string n_read_bytes;
  std::string n_read_order_bytes;
  uint32_t singleton_count = 0;

  // When non-empty, these paths point to the spilled singleton stream files on
  // disk. readsingletons() uses them to stream directly from disk, avoiding
  // loading the full raw bytes into RAM (disk-path only).
  std::string singleton_read_file;
  std::string singleton_order_file;
  std::string n_read_file;
  std::string n_read_order_file;
};

// Forward declarations to keep this header thin.
struct compression_params;
template <size_t bitset_size> struct reorder_global;
class bbhashdict;

template <size_t bitset_size>
void bitsettostring(std::bitset<bitset_size> encoded_bases, char *read_chars,
                    const uint16_t readlen,
                    const reorder_global<bitset_size> &rg);

template <size_t bitset_size>
void setglobalarrays(reorder_global<bitset_size> &rg);

template <size_t bitset_size>
void readDnaFile(std::bitset<bitset_size> *read, uint16_t *read_lengths,
                 const reorder_input_artifact &input_artifact,
                 const reorder_global<bitset_size> &rg);

template <size_t bitset_size>
void reorder(std::bitset<bitset_size> *read, bbhashdict *dict,
             uint16_t *read_lengths, const reorder_global<bitset_size> &rg,
             reorder_encoder_artifact &artifact, const bool deterministic_mode);

template <size_t bitset_size>
void writetofile(std::bitset<bitset_size> *read, uint16_t *read_lengths,
                 reorder_global<bitset_size> &rg,
                 reorder_encoder_artifact &artifact);

template <size_t bitset_size>
reorder_encoder_artifact reorder_main(reorder_input_artifact artifact,
                                      const compression_params &cp);

} // namespace spring

#endif // SPRING_READ_REORDERING_H_
