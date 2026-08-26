// Reconstructs Spring archives back into FASTQ/FASTA output by decoding packed
// sequences and replaying aligned, unaligned, quality, and id streams.

#include "archive_record_reconstruction.h"
#include "assay_atac.h"
#include "assay_rna.h"
#include "assay_sc_atac.h"
#include "assay_sc_rna.h"
#include "decompress_archive_io.h"
#include "dna_utils.h"
#include "integrity_utils.h"
#include "io_utils.h"
#include "params.h"
#include "parse_utils.h"
#include "progress.h"
#ifndef _WIN32
#include "raii.h"
#endif
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <utility>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <omp.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace spring {

void write_fastq_block(std::ostream &output_stream, std::string *id_buffer,
                       std::string *read_buffer,
                       const std::string *quality_array,
                       uint32_t output_read_count, int num_thr,
                       bool gzip_output, bool bgzf_output,
                       int compression_level, bool use_crlf, bool fasta_mode,
                       bool quality_header_has_id);

FileDecompressionSink::FileDecompressionSink(const std::string &outfile_1,
                                             const std::string &outfile_2,
                                             const compression_params &cp,
                                             const int (&compression_levels)[2],
                                             const bool (&gzip)[2],
                                             const bool (&bgzf)[2],
                                             const bool (&write_enabled)[2])
    : fasta_mode(cp.encoding.fasta_mode), num_thr(cp.encoding.num_thr),
      paired_end(cp.encoding.paired_end) {
  should_gzip[0] = gzip[0];
  should_gzip[1] = gzip[1];
  should_bgzf[0] = bgzf[0];
  should_bgzf[1] = bgzf[1];
  write_enabled_[0] = write_enabled[0];
  write_enabled_[1] = write_enabled[1];
  compression_level_[0] = compression_levels[0];
  compression_level_[1] = compression_levels[1];
  use_crlf_[0] = cp.encoding.use_crlf_by_stream[0];
  use_crlf_[1] = cp.encoding.use_crlf_by_stream[1];
  quality_header_has_id_[0] = cp.read_info.quality_header_has_id_by_stream[0];
  quality_header_has_id_[1] = cp.read_info.quality_header_has_id_by_stream[1];

  if (write_enabled_[0]) {
    output_streams[0].open(outfile_1, std::ios::binary);
    if (!output_streams[0])
      throw std::runtime_error("Failed to open output file: " + outfile_1);
  }
  if (paired_end && write_enabled_[1]) {
    output_streams[1].open(outfile_2, std::ios::binary);
    if (!output_streams[1])
      throw std::runtime_error("Failed to open output file: " + outfile_2);
  }
}

FileDecompressionSink::~FileDecompressionSink() = default;

void FileDecompressionSink::consume_step(std::string *id_buffer,
                                         std::string *read_buffer,
                                         const std::string *quality_buffer,
                                         uint32_t count, int stream_index) {
  for (uint32_t i = 0; i < count; ++i) {
    update_record_crc(sequence_crc_[stream_index], read_buffer[i]);
    if (quality_buffer) {
      update_record_crc(quality_crc_[stream_index], quality_buffer[i]);
    }
    update_record_crc(id_crc_[stream_index], id_buffer[i]);
  }
  if (!write_enabled_[stream_index]) {
    return;
  }
  write_fastq_block(output_streams[stream_index], id_buffer, read_buffer,
                    quality_buffer, count, num_thr, should_gzip[stream_index],
                    should_bgzf[stream_index], compression_level_[stream_index],
                    use_crlf_[stream_index], fasta_mode,
                    quality_header_has_id_[stream_index]);
}

void write_step_output(std::ofstream &output_stream, std::string *id_buffer,
                       std::string *read_buffer,
                       const std::string *quality_array,
                       const uint32_t output_read_count, const int num_thr,
                       const bool gzip_output, const bool bgzf_output,
                       const int compression_level, const bool use_crlf,
                       const bool fasta_mode,
                       const bool quality_header_has_id) {
  write_fastq_block(output_stream, id_buffer, read_buffer, quality_array,
                    output_read_count, num_thr, gzip_output, bgzf_output,
                    compression_level, use_crlf, fasta_mode,
                    quality_header_has_id);
}

void decompress_short(const decompression_archive_artifact &artifact,
                      DecompressionSink &sink, compression_params &cp) {
  SPRING_LOG_DEBUG(
      "decompress_short start: scratch_dir=" + artifact.scratch_dir +
      ", num_reads=" + std::to_string(cp.read_info.num_reads) +
      ", paired_end=" + std::string(cp.encoding.paired_end ? "true" : "false") +
      ", preserve_order=" +
      std::string(cp.encoding.preserve_order ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false"));

  const std::string file_seq = "read_seq.bin";
  const std::string file_flag = "read_flag.txt";
  const std::string file_pos = "read_pos.bin";
  const std::string file_pos_pair = "read_pos_pair.bin";
  const std::string file_rc = "read_rev.txt";
  const std::string file_rc_pair = "read_rev_pair.txt";
  const std::string file_readlength = "read_lengths.bin";
  const std::string file_unaligned = "read_unaligned.txt";
  const std::string file_noise = "read_noise.txt";
  const std::string file_noisepos = "read_noisepos.bin";
  const std::array<std::string, 2> input_quality_paths = {"quality_1",
                                                          "quality_2"};
  const std::array<std::string, 2> input_id_paths = {"id_1", "id_2"};

  const uint32_t num_reads = cp.read_info.num_reads;
  const uint8_t paired_id_code = cp.read_info.paired_id_code;
  const bool paired_id_match = cp.read_info.paired_id_match;
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  const bool paired_end = cp.encoding.paired_end;
  const bool preserve_id = cp.encoding.preserve_id;
  const bool preserve_quality = cp.encoding.preserve_quality;
  const bool preserve_order = cp.encoding.preserve_order;
  const bool poly_at_stripped = cp.encoding.poly_at_stripped;
  const bool atac_adapter_stripped = cp.encoding.atac_adapter_stripped;
  const bool cb_prefix_stripped = cp.encoding.cb_prefix_stripped;
  const bool index_id_suffix_reconstructed =
      cp.encoding.index_id_suffix_reconstructed;
  const bool sc_atac_assay = cp.read_info.assay == "sc-atac";
  const uint32_t cb_prefix_len = cp.encoding.cb_prefix_len;
  const int archive_encoding_thread_count =
      resolve_archive_encoding_thread_count(cp);

  std::array<std::vector<std::string>, 2> monolithic_id_blocks;
  bool monolithic_id[2] = {false, false};
  if (preserve_id) {
    const uint32_t file_read_count = paired_end ? (num_reads / 2) : num_reads;
    monolithic_id_blocks[0] = slice_monolithic_id_blocks(
        artifact, input_id_paths[0] + ".bsc", cp.read_info.file_len_id_thr,
        file_read_count, num_reads_per_block);
    monolithic_id[0] = !monolithic_id_blocks[0].empty();
    if (paired_end && !paired_id_match) {
      monolithic_id_blocks[1] = slice_monolithic_id_blocks(
          artifact, input_id_paths[1] + ".bsc", cp.read_info.file_len_id_thr,
          file_read_count, num_reads_per_block);
      monolithic_id[1] = !monolithic_id_blocks[1].empty();
    }
  }

  std::vector<char> tail_bytes_1;
  std::vector<char> tail_bytes_2;
  memory_cursor *tail_cursor_1 = nullptr;
  memory_cursor *tail_cursor_2 = nullptr;
  std::optional<memory_cursor> tail_cursor_storage_1;
  std::optional<memory_cursor> tail_cursor_storage_2;
  if (poly_at_stripped) {
    tail_bytes_1 = archive_member_bytes(artifact, "tail_1.bin");
    tail_cursor_storage_1.emplace(tail_bytes_1);
    tail_cursor_1 = &*tail_cursor_storage_1;
    if (paired_end) {
      tail_bytes_2 = archive_member_bytes(artifact, "tail_2.bin");
      tail_cursor_storage_2.emplace(tail_bytes_2);
      tail_cursor_2 = &*tail_cursor_storage_2;
    }
  }

  std::vector<char> adapter_bytes_1;
  std::vector<char> adapter_bytes_2;
  std::optional<memory_cursor> adapter_cursor_1;
  std::optional<memory_cursor> adapter_cursor_2;
  if (atac_adapter_stripped) {
    adapter_bytes_1 =
        decompress_archive_bsc_member(artifact, "atac_adapter_1.bin.bsc");
    adapter_cursor_1.emplace(adapter_bytes_1);
    if (paired_end) {
      adapter_bytes_2 =
          decompress_archive_bsc_member(artifact, "atac_adapter_2.bin.bsc");
      adapter_cursor_2.emplace(adapter_bytes_2);
    }
  }

  std::vector<char> cb_seq_bytes;
  std::vector<char> cb_qual_bytes;
  size_t cb_seq_cursor = 0;
  size_t cb_qual_cursor = 0;
  if (cb_prefix_stripped) {
    cb_seq_bytes = decompress_archive_bsc_member(artifact, "cb_prefix.dna.bsc");
    if (preserve_quality) {
      cb_qual_bytes =
          decompress_archive_bsc_member(artifact, "cb_prefix.qual.bsc");
    }
  }

  const uint64_t num_reads_per_step =
      compute_num_reads_per_step(num_reads, num_reads_per_block,
                                 archive_encoding_thread_count, paired_end);

  std::vector<std::string> read_buffer_1(
      static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> read_buffer_2;
  if (paired_end) {
    read_buffer_2.resize(static_cast<size_t>(num_reads_per_step));
  }
  std::vector<std::string> id_buffer(static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> quality_buffer;
  if (preserve_quality) {
    quality_buffer.resize(static_cast<size_t>(num_reads_per_step));
  }
  std::vector<uint32_t> read_lengths_buffer_1(
      static_cast<size_t>(num_reads_per_step));
  std::vector<uint32_t> read_lengths_buffer_2;
  if (paired_end) {
    read_lengths_buffer_2.resize(static_cast<size_t>(num_reads_per_step));
  }
  std::array<std::array<char, 128>, 128> decoded_noise_table;
  set_dec_noise_array(decoded_noise_table);

  omp_set_num_threads(archive_encoding_thread_count);
  reference_sequence_store seq(artifact, file_seq,
                               archive_encoding_thread_count,
                               archive_encoding_thread_count, cp);

  uint32_t num_blocks_done = 0;
  uint32_t num_reads_done = 0;
  for (;;) {
    const uint32_t num_reads_cur_step = compute_num_reads_cur_step(
        num_reads, num_reads_done, num_reads_per_step, paired_end);
    if (num_reads_cur_step == 0) {
      break;
    }
    SPRING_LOG_DEBUG(make_decompress_step_log_message(
        "decompress_short step: num_reads_done=", num_reads_done,
        num_reads_cur_step, num_blocks_done));

    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (stream_index == 1 && !paired_end) {
        continue;
      }

      std::exception_ptr omp_exception;
      const int num_blocks_in_step =
          static_cast<int>((static_cast<uint64_t>(num_reads_cur_step) +
                            num_reads_per_block - 1) /
                           num_reads_per_block);
#pragma omp parallel for num_threads(archive_encoding_thread_count)            \
    schedule(static, 1)
      for (int thread_id_int = 0; thread_id_int < num_blocks_in_step;
           ++thread_id_int) {
        try {
          const uint64_t thread_id = static_cast<uint64_t>(thread_id_int);
          if (thread_id * num_reads_per_block < num_reads_cur_step) {
            const uint32_t thread_read_count = compute_thread_read_count(
                num_reads_cur_step, num_reads_per_block, thread_id);
            const uint64_t buffer_offset = thread_id * num_reads_per_block;
            const uint32_t block_num =
                num_blocks_done + static_cast<uint32_t>(thread_id);

            if (stream_index == 0) {
              const std::vector<char> flag_bytes =
                  cp.read_info.legacy_spring
                      ? decompress_legacy_archive_bsc_member(
                            artifact,
                            compressed_block_file_path(file_flag, block_num))
                      : decompress_archive_bsc_member(
                            artifact,
                            compressed_block_file_path(file_flag, block_num));
              const std::vector<char> noise_bytes =
                  cp.read_info.legacy_spring
                      ? decompress_legacy_archive_bsc_member(
                            artifact,
                            compressed_block_file_path(file_noise, block_num))
                      : decompress_archive_bsc_member(
                            artifact,
                            compressed_block_file_path(file_noise, block_num));
              const std::vector<char> noisepos_bytes =
                  cp.read_info.legacy_spring
                      ? decompress_legacy_archive_bsc_member(
                            artifact, compressed_block_file_path(file_noisepos,
                                                                 block_num))
                      : decompress_archive_bsc_member(
                            artifact, compressed_block_file_path(file_noisepos,
                                                                 block_num));
              const std::vector<char> pos_bytes =
                  cp.read_info.legacy_spring
                      ? decompress_legacy_archive_bsc_member(
                            artifact,
                            compressed_block_file_path(file_pos, block_num))
                      : decompress_archive_bsc_member(
                            artifact,
                            compressed_block_file_path(file_pos, block_num));
              const std::vector<char> rc_bytes =
                  cp.read_info.legacy_spring
                      ? decompress_legacy_archive_bsc_member(
                            artifact,
                            compressed_block_file_path(file_rc, block_num))
                      : decompress_archive_bsc_member(
                            artifact,
                            compressed_block_file_path(file_rc, block_num));
              const std::vector<char> unaligned_bytes =
                  cp.read_info.legacy_spring
                      ? decompress_legacy_archive_bsc_member(
                            artifact, compressed_block_file_path(file_unaligned,
                                                                 block_num))
                      : decompress_archive_bsc_member(
                            artifact, compressed_block_file_path(file_unaligned,
                                                                 block_num));
              const std::vector<char> readlength_bytes =
                  cp.read_info.legacy_spring
                      ? decompress_legacy_archive_bsc_member(
                            artifact, compressed_block_file_path(
                                          file_readlength, block_num))
                      : decompress_archive_bsc_member(
                            artifact, compressed_block_file_path(
                                          file_readlength, block_num));
              memory_cursor flag_cursor(flag_bytes);
              memory_cursor noise_cursor(noise_bytes);
              memory_cursor noisepos_cursor(noisepos_bytes);
              memory_cursor pos_cursor(pos_bytes);
              memory_cursor rc_cursor(rc_bytes);
              memory_cursor unaligned_cursor(unaligned_bytes);
              memory_cursor readlength_cursor(readlength_bytes);
              std::optional<memory_cursor> pos_pair_cursor;
              std::optional<memory_cursor> rc_pair_cursor;
              std::optional<std::vector<char>> pos_pair_bytes;
              std::optional<std::vector<char>> rc_pair_bytes;
              if (paired_end) {
                if (cp.read_info.legacy_spring) {
                  pos_pair_bytes.emplace(decompress_legacy_archive_bsc_member(
                      artifact,
                      compressed_block_file_path(file_pos_pair, block_num)));
                  rc_pair_bytes.emplace(decompress_legacy_archive_bsc_member(
                      artifact,
                      compressed_block_file_path(file_rc_pair, block_num)));
                } else {
                  pos_pair_bytes.emplace(decompress_archive_bsc_member(
                      artifact,
                      compressed_block_file_path(file_pos_pair, block_num)));
                  rc_pair_bytes.emplace(decompress_archive_bsc_member(
                      artifact,
                      compressed_block_file_path(file_rc_pair, block_num)));
                }
                pos_pair_cursor.emplace(*pos_pair_bytes);
                rc_pair_cursor.emplace(*rc_pair_bytes);
              }

              uint64_t previous_position = 0;
              bool first_read_of_block = true;
              for (uint32_t i = static_cast<uint32_t>(buffer_offset);
                   i < buffer_offset + thread_read_count; ++i) {
                const char read_flag = flag_cursor.read_char("read flag");
                const uint16_t read_length =
                    readlength_cursor.read<uint16_t>("read length");
                read_lengths_buffer_1[i] = read_length;
                const bool read_1_is_singleton =
                    (read_flag == '2') || (read_flag == '4');

                uint64_t read_1_position = 0;
                char read_1_orientation = 'd';
                if (!read_1_is_singleton) {
                  if (preserve_order) {
                    read_1_position =
                        pos_cursor.read<uint64_t>("read position");
                  } else if (first_read_of_block) {
                    first_read_of_block = false;
                    read_1_position =
                        pos_cursor.read<uint64_t>("first block position");
                    previous_position = read_1_position;
                  } else {
                    const uint16_t position_delta_16 =
                        pos_cursor.read<uint16_t>("position delta");
                    if (position_delta_16 == 65535) {
                      read_1_position =
                          pos_cursor.read<uint64_t>("absolute position");
                    } else {
                      read_1_position = previous_position + position_delta_16;
                    }
                    previous_position = read_1_position;
                  }

                  read_1_orientation = rc_cursor.read_char("read orientation");
                  std::string read =
                      seq.read(read_1_position, read_lengths_buffer_1[i]);
                  const std::string noise_codes =
                      noise_cursor.read_line("noise code stream");
                  uint16_t previous_noise_position = 0;
                  for (char noise_code : noise_codes) {
                    uint16_t noise_position_delta =
                        noisepos_cursor.read<uint16_t>("noise position delta");
                    noise_position_delta += previous_noise_position;
                    read[noise_position_delta] =
                        decoded_noise_table[static_cast<uint8_t>(
                            read[noise_position_delta])]
                                           [static_cast<uint8_t>(noise_code)];
                    previous_noise_position = noise_position_delta;
                  }
                  read_buffer_1[i] =
                      (read_1_orientation == 'd')
                          ? read
                          : reverse_complement(read, read_lengths_buffer_1[i]);
                } else {
                  read_buffer_1[i].resize(read_lengths_buffer_1[i]);
                  unaligned_cursor.read_bytes(read_buffer_1[i].data(),
                                              read_lengths_buffer_1[i],
                                              "unaligned read");
                }

                if (paired_end) {
                  const bool read_2_is_singleton =
                      (read_flag == '2') || (read_flag == '3');
                  read_lengths_buffer_2[i] =
                      readlength_cursor.read<uint16_t>("mate read length");
                  if (!read_2_is_singleton) {
                    uint64_t read_2_position = 0;
                    char read_2_orientation = 'd';
                    if (read_flag == '1' || read_flag == '4') {
                      read_2_position =
                          pos_cursor.read<uint64_t>("mate position");
                      read_2_orientation =
                          rc_cursor.read_char("mate orientation");
                    } else {
                      const int16_t mate_position_delta =
                          pos_pair_cursor->read<int16_t>("mate position delta");
                      const int64_t mate_position_signed =
                          static_cast<int64_t>(read_1_position) +
                          static_cast<int64_t>(mate_position_delta);
                      if (mate_position_signed < 0) {
                        throw std::runtime_error(
                            "Corrupt archive: negative mate position");
                      }
                      read_2_position =
                          static_cast<uint64_t>(mate_position_signed);
                      const char relative_orientation_flag =
                          rc_pair_cursor->read_char(
                              "relative mate orientation");
                      read_2_orientation =
                          (relative_orientation_flag == '0')
                              ? ((read_1_orientation == 'd') ? 'r' : 'd')
                              : ((read_1_orientation == 'd') ? 'd' : 'r');
                    }

                    std::string read =
                        seq.read(read_2_position, read_lengths_buffer_2[i]);
                    const std::string noise_codes =
                        noise_cursor.read_line("mate noise code stream");
                    uint16_t previous_noise_position = 0;
                    for (char noise_code : noise_codes) {
                      uint16_t noise_position_delta =
                          noisepos_cursor.read<uint16_t>(
                              "mate noise position delta");
                      noise_position_delta += previous_noise_position;
                      read[noise_position_delta] =
                          decoded_noise_table[static_cast<uint8_t>(
                              read[noise_position_delta])]
                                             [static_cast<uint8_t>(noise_code)];
                      previous_noise_position = noise_position_delta;
                    }
                    read_buffer_2[i] =
                        (read_2_orientation == 'd')
                            ? read
                            : reverse_complement(read,
                                                 read_lengths_buffer_2[i]);
                  } else {
                    read_buffer_2[i].resize(read_lengths_buffer_2[i]);
                    unaligned_cursor.read_bytes(read_buffer_2[i].data(),
                                                read_lengths_buffer_2[i],
                                                "unaligned mate read");
                  }
                }
              }
            }

            if (preserve_quality) {
              const std::string quality_member =
                  input_quality_paths[stream_index] + "." +
                  std::to_string(block_num);
              if (cp.read_info.legacy_spring) {
                if (stream_index == 0) {
                  decompress_legacy_archive_bsc_str_array_member(
                      artifact, quality_member,
                      quality_buffer.data() + buffer_offset, thread_read_count,
                      read_lengths_buffer_1.data() + buffer_offset);
                } else {
                  decompress_legacy_archive_bsc_str_array_member(
                      artifact, quality_member,
                      quality_buffer.data() + buffer_offset, thread_read_count,
                      read_lengths_buffer_2.data() + buffer_offset);
                }
              } else if (stream_index == 0) {
                safe_bsc_str_array_decompress_bytes(
                    artifact.require(quality_member), quality_member,
                    quality_buffer.data() + buffer_offset, thread_read_count,
                    read_lengths_buffer_1.data() + buffer_offset);
              } else {
                safe_bsc_str_array_decompress_bytes(
                    artifact.require(quality_member), quality_member,
                    quality_buffer.data() + buffer_offset, thread_read_count,
                    read_lengths_buffer_2.data() + buffer_offset);
              }
            }

            if (!preserve_id) {
              for (uint32_t i = static_cast<uint32_t>(buffer_offset);
                   i < buffer_offset + thread_read_count; ++i) {
                std::string read_id;
                read_id.reserve(32);
                read_id.push_back('@');
                read_id.append(std::to_string(num_reads_done + i + 1));
                read_id.push_back('/');
                read_id.append(std::to_string(stream_index + 1));
                id_buffer[i] = std::move(read_id);
              }
            } else if (stream_index == 1 && paired_id_match) {
              for (uint32_t i = static_cast<uint32_t>(buffer_offset);
                   i < buffer_offset + thread_read_count; ++i) {
                modify_id(id_buffer[i], paired_id_code);
              }
            } else {
              const std::string id_member = input_id_paths[stream_index] + "." +
                                            std::to_string(block_num);
              if (cp.read_info.legacy_spring) {
                decompress_legacy_archive_id_member(
                    artifact, id_member, id_buffer.data() + buffer_offset,
                    thread_read_count);
              } else {
                std::string_view id_bytes;
                if (monolithic_id[stream_index]) {
                  id_bytes = monolithic_id_blocks[stream_index][block_num];
                } else {
                  id_bytes = artifact.require(id_member);
                }
                decompress_id_block_bytes(id_bytes, id_member,
                                          id_buffer.data() + buffer_offset,
                                          thread_read_count, false);
              }
            }
          }
        } catch (...) {
#pragma omp critical
          {
            if (!omp_exception) {
              omp_exception = std::current_exception();
            }
          }
        }
      }

      if (omp_exception) {
        std::rethrow_exception(omp_exception);
      }

      std::string *read_buffer_ptr =
          (stream_index == 0) ? read_buffer_1.data() : read_buffer_2.data();
      std::string *quality_buffer_ptr =
          preserve_quality ? quality_buffer.data() : nullptr;

      if (index_id_suffix_reconstructed && stream_index == 0) {
        for (uint32_t i = 0; i < num_reads_cur_step; ++i) {
          append_grouped_sc_rna_index_suffix_to_id(
              id_buffer[i], read_buffer_1[i],
              paired_end ? &read_buffer_2[i] : nullptr);
        }
      }

      if (cb_prefix_stripped && stream_index == 0) {
        for (uint32_t i = 0; i < num_reads_cur_step; ++i) {
          if (cb_seq_cursor + cb_prefix_len > cb_seq_bytes.size()) {
            throw std::runtime_error(
                "Corrupt archive: truncated CB prefix sequence stream");
          }
          read_buffer_ptr[i].insert(0, cb_seq_bytes.data() + cb_seq_cursor,
                                    cb_prefix_len);
          cb_seq_cursor += cb_prefix_len;
          read_lengths_buffer_1[i] += cb_prefix_len;

          if (quality_buffer_ptr != nullptr) {
            if (cb_qual_cursor + cb_prefix_len > cb_qual_bytes.size()) {
              throw std::runtime_error(
                  "Corrupt archive: truncated CB prefix quality stream");
            }
            quality_buffer_ptr[i].insert(
                0, cb_qual_bytes.data() + cb_qual_cursor, cb_prefix_len);
            cb_qual_cursor += cb_prefix_len;
          }
        }
      }

      if (poly_at_stripped) {
        memory_cursor &tail_cursor =
            (stream_index == 0) ? *tail_cursor_1 : *tail_cursor_2;
        for (uint32_t i = 0; i < num_reads_cur_step; ++i) {
          const uint16_t tail_info = tail_cursor.read<uint16_t>("tail info");
          std::string tail_qual;
          const uint32_t tail_len = tail_info >> 1;
          if (tail_len > 0) {
            tail_qual.resize(tail_len);
            tail_cursor.read_bytes(tail_qual.data(), tail_len, "tail quality");
          }
          restore_rna_tail(read_buffer_ptr[i],
                           quality_buffer_ptr ? &quality_buffer_ptr[i]
                                              : nullptr,
                           tail_info, tail_len > 0 ? &tail_qual : nullptr);
        }
      }

      if (atac_adapter_stripped) {
        memory_cursor &adapter_cursor =
            (stream_index == 0) ? *adapter_cursor_1 : *adapter_cursor_2;
        for (uint32_t i = 0; i < num_reads_cur_step; ++i) {
          const uint8_t adapter_info =
              adapter_cursor.read<uint8_t>("adapter info");
          std::string adapter_qual;
          const uint32_t strip_len = adapter_info >> 1;
          if (strip_len > 0) {
            adapter_qual.resize(strip_len);
            adapter_cursor.read_bytes(adapter_qual.data(), strip_len,
                                      "adapter quality");
          }
          if (sc_atac_assay) {
            restore_sc_atac_adapter_tail(
                read_buffer_ptr[i],
                quality_buffer_ptr ? &quality_buffer_ptr[i] : nullptr,
                adapter_info, strip_len > 0 ? &adapter_qual : nullptr);
          } else {
            restore_atac_adapter_tail(
                read_buffer_ptr[i],
                quality_buffer_ptr ? &quality_buffer_ptr[i] : nullptr,
                adapter_info, strip_len > 0 ? &adapter_qual : nullptr);
          }
        }
      }

      sink.consume_step(id_buffer.data(), read_buffer_ptr, quality_buffer_ptr,
                        num_reads_cur_step, stream_index);
    }

    num_reads_done += num_reads_cur_step;
    if (auto *progress = ProgressBar::GlobalInstance()) {
      progress->update(static_cast<float>(num_reads_done) / num_reads);
    }
    num_blocks_done += archive_encoding_thread_count;
  }

  if (cb_prefix_stripped) {
    if (cb_seq_cursor != cb_seq_bytes.size()) {
      throw std::runtime_error(
          "Corrupt archive: trailing CB prefix sequence bytes remain");
    }
    if (preserve_quality && cb_qual_cursor != cb_qual_bytes.size()) {
      throw std::runtime_error(
          "Corrupt archive: trailing CB prefix quality bytes remain");
    }
  }
  SPRING_LOG_DEBUG("decompress_short complete: total_reads_done=" +
                   std::to_string(num_reads_done));
}

void decompress_long(const decompression_archive_artifact &artifact,
                     DecompressionSink &sink, compression_params &cp) {
  SPRING_LOG_DEBUG(
      "decompress_long start: scratch_dir=" + artifact.scratch_dir +
      ", num_reads=" + std::to_string(cp.read_info.num_reads) +
      ", paired_end=" + std::string(cp.encoding.paired_end ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false"));

  const std::array<std::string, 2> input_read_paths = {"read_1", "read_2"};
  const std::array<std::string, 2> input_quality_paths = {"quality_1",
                                                          "quality_2"};
  const std::array<std::string, 2> input_id_paths = {"id_1", "id_2"};
  const std::array<std::string, 2> input_read_length_paths = {"readlength_1",
                                                              "readlength_2"};

  const uint32_t num_reads = cp.read_info.num_reads;
  const uint8_t paired_id_code = cp.read_info.paired_id_code;
  const bool paired_id_match = cp.read_info.paired_id_match;
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block_long;
  const bool paired_end = cp.encoding.paired_end;
  const bool preserve_id = cp.encoding.preserve_id;
  const bool preserve_quality = cp.encoding.preserve_quality;
  const int archive_encoding_thread_count =
      resolve_archive_encoding_thread_count(cp);

  const uint64_t num_reads_per_step =
      compute_num_reads_per_step(num_reads, num_reads_per_block,
                                 archive_encoding_thread_count, paired_end);

  std::vector<std::string> read_buffer(static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> id_buffer(static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> quality_buffer;
  if (preserve_quality) {
    quality_buffer.resize(static_cast<size_t>(num_reads_per_step));
  }
  std::vector<uint32_t> read_lengths_buffer(
      static_cast<size_t>(num_reads_per_step));

  omp_set_num_threads(archive_encoding_thread_count);

  uint32_t num_blocks_done = 0;
  uint32_t num_reads_done = 0;
  for (;;) {
    const uint32_t num_reads_cur_step = compute_num_reads_cur_step(
        num_reads, num_reads_done, num_reads_per_step, paired_end);
    if (num_reads_cur_step == 0) {
      break;
    }
    SPRING_LOG_DEBUG(make_decompress_step_log_message(
        "decompress_long step: num_reads_done=", num_reads_done,
        num_reads_cur_step, num_blocks_done));

    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (stream_index == 1 && !paired_end) {
        continue;
      }

      std::exception_ptr omp_exception;
      const int num_blocks_in_step =
          static_cast<int>((static_cast<uint64_t>(num_reads_cur_step) +
                            num_reads_per_block - 1) /
                           num_reads_per_block);
#pragma omp parallel for num_threads(archive_encoding_thread_count)            \
    schedule(static, 1)
      for (int thread_id_int = 0; thread_id_int < num_blocks_in_step;
           ++thread_id_int) {
        try {
          const uint64_t thread_id = static_cast<uint64_t>(thread_id_int);
          if (thread_id * num_reads_per_block < num_reads_cur_step) {
            const uint32_t thread_read_count = compute_thread_read_count(
                num_reads_cur_step, num_reads_per_block, thread_id);
            const uint64_t buffer_offset = thread_id * num_reads_per_block;
            const uint32_t block_num =
                num_blocks_done + static_cast<uint32_t>(thread_id);

            const std::vector<char> read_length_bytes =
                cp.read_info.legacy_spring
                    ? decompress_legacy_archive_bsc_member(
                          artifact,
                          compressed_block_file_path(
                              input_read_length_paths[stream_index], block_num))
                    : decompress_archive_bsc_member(
                          artifact, compressed_block_file_path(
                                        input_read_length_paths[stream_index],
                                        block_num));
            memory_cursor read_length_cursor(read_length_bytes);
            for (uint32_t read_index = 0; read_index < thread_read_count;
                 ++read_index) {
              read_lengths_buffer[buffer_offset + read_index] =
                  read_length_cursor.read<uint32_t>("read length block");
            }

            if (cp.read_info.legacy_spring) {
              const std::string read_member =
                  block_file_path(input_read_paths[stream_index], block_num);
              decompress_legacy_archive_bsc_str_array_member(
                  artifact, read_member, read_buffer.data() + buffer_offset,
                  thread_read_count,
                  read_lengths_buffer.data() + buffer_offset);
            } else {
              const std::vector<char> read_bytes =
                  decompress_archive_bsc_member(
                      artifact, compressed_block_file_path(
                                    input_read_paths[stream_index], block_num));
              memory_cursor read_cursor(read_bytes);
              for (uint32_t read_index = 0; read_index < thread_read_count;
                   ++read_index) {
                const uint32_t absolute_index =
                    static_cast<uint32_t>(buffer_offset) + read_index;
                read_buffer[absolute_index].resize(
                    read_lengths_buffer[absolute_index]);
                read_cursor.read_bytes(read_buffer[absolute_index].data(),
                                       read_lengths_buffer[absolute_index],
                                       "long read block");
              }
            }

            if (preserve_quality) {
              const std::string quality_member =
                  block_file_path(input_quality_paths[stream_index], block_num);
              if (cp.read_info.legacy_spring) {
                decompress_legacy_archive_bsc_str_array_member(
                    artifact, quality_member,
                    quality_buffer.data() + buffer_offset, thread_read_count,
                    read_lengths_buffer.data() + buffer_offset);
              } else {
                const std::vector<char> quality_bytes =
                    decompress_archive_bsc_member(artifact, quality_member);
                memory_cursor quality_cursor(quality_bytes);
                for (uint32_t read_index = 0; read_index < thread_read_count;
                     ++read_index) {
                  const uint32_t absolute_index =
                      static_cast<uint32_t>(buffer_offset) + read_index;
                  quality_buffer[absolute_index].resize(
                      read_lengths_buffer[absolute_index]);
                  quality_cursor.read_bytes(
                      quality_buffer[absolute_index].data(),
                      read_lengths_buffer[absolute_index], "quality block");
                }
              }
            }

            if (!preserve_id) {
              for (uint32_t i = static_cast<uint32_t>(buffer_offset);
                   i < buffer_offset + thread_read_count; ++i) {
                std::string read_id;
                read_id.reserve(32);
                read_id.push_back('@');
                read_id.append(std::to_string(num_reads_done + i + 1));
                read_id.push_back('/');
                read_id.append(std::to_string(stream_index + 1));
                id_buffer[i] = std::move(read_id);
              }
            } else if (stream_index == 1 && paired_id_match) {
              for (uint32_t i = static_cast<uint32_t>(buffer_offset);
                   i < buffer_offset + thread_read_count; ++i) {
                modify_id(id_buffer[i], paired_id_code);
              }
            } else {
              const std::string raw_member =
                  block_file_path(input_id_paths[stream_index], block_num);
              const std::string compressed_member = raw_member + ".bsc";
              if (cp.read_info.legacy_spring) {
                decompress_legacy_archive_id_member(
                    artifact, raw_member, id_buffer.data() + buffer_offset,
                    thread_read_count);
              } else if (artifact.contains(compressed_member)) {
                decompress_id_block_bytes(
                    artifact.require(compressed_member), compressed_member,
                    id_buffer.data() + buffer_offset, thread_read_count, false);
              } else {
                decompress_id_block_bytes(
                    artifact.require(raw_member), raw_member,
                    id_buffer.data() + buffer_offset, thread_read_count, true);
              }
            }
          }
        } catch (...) {
#pragma omp critical
          {
            if (!omp_exception) {
              omp_exception = std::current_exception();
            }
          }
        }
      }

      if (omp_exception) {
        std::rethrow_exception(omp_exception);
      }

      sink.consume_step(id_buffer.data(), read_buffer.data(),
                        preserve_quality ? quality_buffer.data() : nullptr,
                        num_reads_cur_step, stream_index);
    }

    num_reads_done += num_reads_cur_step;
    if (auto *progress = ProgressBar::GlobalInstance()) {
      progress->update(static_cast<float>(num_reads_done) / num_reads);
    }
    num_blocks_done += archive_encoding_thread_count;
  }

  SPRING_LOG_DEBUG("decompress_long complete: total_reads_done=" +
                   std::to_string(num_reads_done));
}

} // namespace spring
