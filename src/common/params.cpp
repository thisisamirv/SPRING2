// Implements compression-parameter serialization and archive metadata
// helpers used to persist runtime settings.

#include "params.h"
#include "core_utils.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace spring {

namespace {

// Metadata strings (paths, notes, assay labels, versions) should be small.
// Cap before allocating so a corrupt length cannot OOM the process.
constexpr uint32_t kMaxMetadataStringBytes = 16u * 1024u * 1024u;

std::optional<uint64_t> remaining_stream_bytes(std::istream &in) {
  const std::istream::pos_type current = in.tellg();
  if (current == std::istream::pos_type(-1)) {
    return std::nullopt;
  }

  in.seekg(0, std::ios::end);
  const std::istream::pos_type end = in.tellg();
  in.seekg(current);
  if (!in || end == std::istream::pos_type(-1) || end < current) {
    in.clear();
    in.seekg(current);
    return std::nullopt;
  }
  return static_cast<uint64_t>(end - current);
}

} // namespace

void write_bool(std::ostream &out, bool value) {
  uint8_t byte = value ? 1 : 0;
  out.write(byte_ptr(&byte), sizeof(uint8_t));
}

bool read_bool(std::istream &in) {
  uint8_t byte = 0;
  in.read(byte_ptr(&byte), sizeof(uint8_t));
  return byte != 0;
}

void write_string(std::ostream &out, const std::string &s) {
  uint32_t len = static_cast<uint32_t>(s.length());
  out.write(byte_ptr(&len), sizeof(uint32_t));
  if (len > 0) {
    out.write(s.data(), len);
  }
}

std::string read_string(std::istream &in) {
  uint32_t len = 0;
  in.read(byte_ptr(&len), sizeof(uint32_t));
  if (!in) {
    throw std::runtime_error(
        "Corrupt archive: truncated string length in metadata.");
  }
  if (len == 0) {
    return std::string();
  }
  if (len > kMaxMetadataStringBytes) {
    throw std::runtime_error(
        "Corrupt archive: metadata string length is too large (" +
        std::to_string(len) + " bytes).");
  }

  if (const std::optional<uint64_t> remaining = remaining_stream_bytes(in)) {
    if (static_cast<uint64_t>(len) > *remaining) {
      throw std::runtime_error(
          "Corrupt archive: string length exceeds remaining metadata (" +
          std::to_string(len) + " > " + std::to_string(*remaining) + ").");
    }
  }

  std::string s(len, '\0');
  in.read(s.data(), static_cast<std::streamsize>(len));
  if (!in || in.gcount() != static_cast<std::streamsize>(len)) {
    throw std::runtime_error(
        "Corrupt archive: truncated string payload in metadata.");
  }
  return s;
}

namespace {

constexpr char kRc1ArchiveVersion[] = "1.0.0-rc.1";

void apply_legacy_stream_metadata(compression_params &cp) {
  cp.encoding.use_crlf_by_stream[0] = cp.encoding.use_crlf;
  cp.encoding.use_crlf_by_stream[1] =
      cp.encoding.paired_end ? cp.encoding.use_crlf : false;
  cp.read_info.quality_header_has_id_by_stream[0] =
      cp.read_info.quality_header_has_id;
  cp.read_info.quality_header_has_id_by_stream[1] =
      cp.encoding.paired_end ? cp.read_info.quality_header_has_id : false;
}

void initialize_optional_archive_metadata_defaults(compression_params &cp) {
  cp.read_info.assay = "auto";
  cp.read_info.assay_confidence = "N/A";
  cp.read_info.compressor_version = kRc1ArchiveVersion;
  cp.encoding.barcode_sort = false;
  cp.encoding.cb_len = 16;
  cp.encoding.bisulfite_ternary = false;
  cp.encoding.depleted_base_by_stream[0] = 'N';
  cp.encoding.depleted_base_by_stream[1] = 'N';
  cp.encoding.poly_at_stripped = false;
  cp.encoding.cb_prefix_stripped = false;
  cp.encoding.cb_prefix_len = 0;
  cp.read_info.quality_header_has_id = false;
  cp.encoding.atac_adapter_stripped = false;
  cp.encoding.index_id_suffix_reconstructed = false;
  // Absent from older archives: they used unframed raw concatenation.
  cp.read_info.digest_length_framed = false;
}

void write_archive_metadata_header(std::ostream &out, uint32_t version) {
  out.write(byte_ptr(&ARCHIVE_METADATA_MAGIC), sizeof(uint32_t));
  out.write(byte_ptr(&version), sizeof(uint32_t));
}

uint32_t read_archive_metadata_header(std::istream &in) {
  const std::istream::pos_type start_pos = in.tellg();
  uint32_t magic = 0;
  in.read(byte_ptr(&magic), sizeof(uint32_t));
  if (!in.good()) {
    in.clear();
    if (start_pos != std::istream::pos_type(-1)) {
      in.seekg(start_pos);
    }
    return LEGACY_ARCHIVE_FORMAT_VERSION;
  }

  if (magic != ARCHIVE_METADATA_MAGIC) {
    in.clear();
    if (start_pos != std::istream::pos_type(-1)) {
      in.seekg(start_pos);
    }
    return LEGACY_ARCHIVE_FORMAT_VERSION;
  }

  uint32_t version = LEGACY_ARCHIVE_FORMAT_VERSION;
  in.read(byte_ptr(&version), sizeof(uint32_t));
  if (!in.good()) {
    throw std::runtime_error("Archive metadata header is truncated.");
  }

  return version;
}

constexpr size_t kDigestBlockBytes = 6 * sizeof(uint32_t);
constexpr uint32_t kMaxMetadataExtensionBytes = 16u * 1024u * 1024u;

void write_compression_params_core(std::ostream &out,
                                   const compression_params &cp) {
  write_bool(out, cp.encoding.paired_end);
  write_bool(out, cp.encoding.preserve_order);
  write_bool(out, cp.encoding.preserve_quality);
  write_bool(out, cp.encoding.preserve_id);
  write_bool(out, cp.encoding.long_flag);
  // Reserved (was qvz_flag): always written as false to maintain binary format
  // stability. Archives created before QVZ removal stored qvz_flag here.
  write_bool(out, false);
  write_bool(out, cp.quality.ill_bin_flag);
  write_bool(out, cp.quality.bin_thr_flag);
  // Reserved (was qvz_ratio): always written as 0.0 to maintain binary format
  // stability. Archives created before QVZ removal stored qvz_ratio here.
  const double reserved_qvz_ratio = 0.0;
  out.write(byte_ptr(&reserved_qvz_ratio), sizeof(double));
  out.write(byte_ptr(&cp.quality.bin_thr_thr), sizeof(unsigned int));
  out.write(byte_ptr(&cp.quality.bin_thr_high), sizeof(unsigned int));
  out.write(byte_ptr(&cp.quality.bin_thr_low), sizeof(unsigned int));
  out.write(byte_ptr(&cp.read_info.num_reads), sizeof(uint32_t));
  out.write(byte_ptr(&cp.read_info.num_reads_clean[0]), sizeof(uint32_t));
  out.write(byte_ptr(&cp.read_info.num_reads_clean[1]), sizeof(uint32_t));
  out.write(byte_ptr(&cp.read_info.max_readlen), sizeof(uint32_t));
  out.write(byte_ptr(&cp.read_info.paired_id_code), sizeof(uint8_t));
  write_bool(out, cp.read_info.paired_id_match);
  out.write(byte_ptr(&cp.encoding.num_reads_per_block), sizeof(int));
  out.write(byte_ptr(&cp.encoding.num_reads_per_block_long), sizeof(int));
  out.write(byte_ptr(&cp.encoding.num_thr), sizeof(int));
  out.write(byte_ptr(&cp.encoding.compression_level), sizeof(int));
  out.write(reinterpret_cast<const char *>(cp.read_info.file_len_seq_thr),
            sizeof(uint64_t) *
                compression_params::ReadMetadata::kFileLenThrSize);
  out.write(reinterpret_cast<const char *>(cp.read_info.file_len_id_thr),
            sizeof(uint64_t) *
                compression_params::ReadMetadata::kFileLenThrSize);
  write_bool(out, cp.encoding.use_crlf);
  write_string(out, cp.read_info.input_filename_1);
  write_string(out, cp.read_info.input_filename_2);
  write_string(out, cp.read_info.note);
  write_bool(out, cp.encoding.fasta_mode);

  for (int i = 0; i < 2; ++i)
    write_bool(out, cp.gzip.streams[i].was_gzipped);
  for (int i = 0; i < 2; ++i)
    out.write(byte_ptr(&cp.gzip.streams[i].flg), sizeof(uint8_t));
  for (int i = 0; i < 2; ++i)
    out.write(byte_ptr(&cp.gzip.streams[i].mtime), sizeof(uint32_t));
  for (int i = 0; i < 2; ++i)
    out.write(byte_ptr(&cp.gzip.streams[i].xfl), sizeof(uint8_t));
  for (int i = 0; i < 2; ++i)
    out.write(byte_ptr(&cp.gzip.streams[i].os), sizeof(uint8_t));
  for (int i = 0; i < 2; ++i)
    write_string(out, cp.gzip.streams[i].name);
  for (int i = 0; i < 2; ++i)
    write_bool(out, cp.gzip.streams[i].is_bgzf);
  for (int i = 0; i < 2; ++i)
    out.write(byte_ptr(&cp.gzip.streams[i].bgzf_block_size), sizeof(uint16_t));
  for (int i = 0; i < 2; ++i)
    out.write(byte_ptr(&cp.gzip.streams[i].uncompressed_size),
              sizeof(uint64_t));
  for (int i = 0; i < 2; ++i)
    out.write(byte_ptr(&cp.gzip.streams[i].compressed_size), sizeof(uint64_t));
  for (int i = 0; i < 2; ++i)
    out.write(byte_ptr(&cp.gzip.streams[i].member_count), sizeof(uint32_t));
}

void write_digest_block(std::ostream &out, const compression_params &cp) {
  for (int i = 0; i < 2; ++i) {
    out.write(byte_ptr(&cp.read_info.sequence_crc[i]), sizeof(uint32_t));
    out.write(byte_ptr(&cp.read_info.quality_crc[i]), sizeof(uint32_t));
    out.write(byte_ptr(&cp.read_info.id_crc[i]), sizeof(uint32_t));
  }
}

void write_extension_fields_after_digests(std::ostream &out,
                                          const compression_params &cp) {
  write_string(out, cp.read_info.assay);
  write_string(out, cp.read_info.assay_confidence);
  write_string(out, cp.read_info.compressor_version);
  write_bool(out, cp.encoding.barcode_sort);
  out.write(byte_ptr(&cp.encoding.cb_len), sizeof(uint32_t));
  write_bool(out, cp.encoding.bisulfite_ternary);
  out.write(byte_ptr(&cp.encoding.depleted_base_by_stream[0]), sizeof(char));
  write_bool(out, cp.encoding.poly_at_stripped);
  write_bool(out, cp.encoding.cb_prefix_stripped);
  out.write(byte_ptr(&cp.encoding.cb_prefix_len), sizeof(uint32_t));
  write_bool(out, cp.read_info.quality_header_has_id);
  write_bool(out, cp.encoding.atac_adapter_stripped);
  write_bool(out, cp.encoding.index_id_suffix_reconstructed);
  for (int i = 0; i < 2; ++i)
    write_bool(out, cp.encoding.use_crlf_by_stream[i]);
  for (int i = 0; i < 2; ++i)
    write_bool(out, cp.read_info.quality_header_has_id_by_stream[i]);
  write_bool(out, cp.read_info.digest_length_framed);
  // Appended after the v2 fixed fields so older readers that stop at
  // digest_length_framed still parse; mate defaults to stream 0 when absent.
  out.write(byte_ptr(&cp.encoding.depleted_base_by_stream[1]), sizeof(char));
}

// Format v2+ trailer: explicit digests_present, then optional digests, then the
// remaining metadata fields as a fixed layout.
void write_extension_trailer_v2(std::ostream &out,
                                const compression_params &cp) {
  // Current writers always store the digest block (channel zeros are valid).
  write_bool(out, true);
  write_digest_block(out, cp);
  write_extension_fields_after_digests(out, cp);
}

void write_compression_params_body(std::ostream &out,
                                   const compression_params &cp) {
  write_compression_params_core(out, cp);

  std::ostringstream trailer(std::ios::binary);
  write_extension_trailer_v2(trailer, cp);
  const std::string trailer_bytes = trailer.str();
  if (trailer_bytes.size() > kMaxMetadataExtensionBytes) {
    throw std::runtime_error(
        "Archive metadata extension trailer exceeds maximum size.");
  }
  const uint32_t trailer_len = static_cast<uint32_t>(trailer_bytes.size());
  out.write(byte_ptr(&trailer_len), sizeof(uint32_t));
  if (trailer_len > 0) {
    out.write(trailer_bytes.data(),
              static_cast<std::streamsize>(trailer_len));
  }
}

void read_digest_block(std::istream &in, compression_params &cp) {
  char block[kDigestBlockBytes];
  in.read(block, static_cast<std::streamsize>(kDigestBlockBytes));
  if (!in ||
      in.gcount() != static_cast<std::streamsize>(kDigestBlockBytes)) {
    throw std::runtime_error(
        "Corrupt archive: truncated digest block in metadata.");
  }
  size_t offset = 0;
  auto take_u32 = [&](uint32_t &out_value) {
    std::memcpy(&out_value, block + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
  };
  for (int i = 0; i < 2; ++i) {
    take_u32(cp.read_info.sequence_crc[i]);
    take_u32(cp.read_info.quality_crc[i]);
    take_u32(cp.read_info.id_crc[i]);
  }
}

void read_extension_fields_after_digests(
    std::istream &in, compression_params &cp,
    bool &has_stream_specific_fastq_metadata) {
  cp.read_info.assay = read_string(in);
  cp.read_info.assay_confidence = read_string(in);
  cp.read_info.compressor_version = read_string(in);
  cp.encoding.barcode_sort = read_bool(in);
  in.read(byte_ptr(&cp.encoding.cb_len), sizeof(uint32_t));
  cp.encoding.bisulfite_ternary = read_bool(in);
  in.read(byte_ptr(&cp.encoding.depleted_base_by_stream[0]), sizeof(char));
  cp.encoding.poly_at_stripped = read_bool(in);
  cp.encoding.cb_prefix_stripped = read_bool(in);
  in.read(byte_ptr(&cp.encoding.cb_prefix_len), sizeof(uint32_t));
  cp.read_info.quality_header_has_id = read_bool(in);
  cp.encoding.atac_adapter_stripped = read_bool(in);
  cp.encoding.index_id_suffix_reconstructed = read_bool(in);
  for (int i = 0; i < 2; ++i)
    cp.encoding.use_crlf_by_stream[i] = read_bool(in);
  for (int i = 0; i < 2; ++i)
    cp.read_info.quality_header_has_id_by_stream[i] = read_bool(in);
  cp.read_info.digest_length_framed = read_bool(in);
  has_stream_specific_fastq_metadata = true;
  if (!in) {
    throw std::runtime_error(
        "Corrupt archive: truncated metadata extension fields.");
  }
  // Optional mate depleted base (absent in archives written before per-stream
  // bisulfite support).
  if (in.peek() != std::char_traits<char>::eof()) {
    in.read(byte_ptr(&cp.encoding.depleted_base_by_stream[1]), sizeof(char));
    if (!in) {
      throw std::runtime_error(
          "Corrupt archive: truncated mate depleted_base in metadata.");
    }
  } else {
    cp.encoding.depleted_base_by_stream[1] =
        cp.encoding.depleted_base_by_stream[0];
  }
}

void read_extension_trailer_v2(std::istream &in, compression_params &cp,
                               bool &has_stream_specific_fastq_metadata) {
  cp.read_info.digests_present = read_bool(in);
  if (!in) {
    throw std::runtime_error(
        "Corrupt archive: truncated digests_present flag in metadata.");
  }
  if (cp.read_info.digests_present) {
    read_digest_block(in, cp);
  }
  read_extension_fields_after_digests(in, cp,
                                      has_stream_specific_fastq_metadata);
}

// Pre-v2 archives appended optional fields and used peek()!=eof to decide
// whether digests (and later extensions) were present.
void read_extension_trailer_legacy_peek(
    std::istream &in, compression_params &cp,
    bool &has_stream_specific_fastq_metadata) {
  if (in.peek() == std::char_traits<char>::eof()) {
    return;
  }

  if (const std::optional<uint64_t> remaining = remaining_stream_bytes(in)) {
    if (*remaining > 0 && *remaining < kDigestBlockBytes) {
      throw std::runtime_error(
          "Corrupt archive: truncated digest block in metadata.");
    }
  }

  read_digest_block(in, cp);
  if (!in) {
    throw std::runtime_error(
        "Corrupt archive: truncated digest block in metadata.");
  }
  cp.read_info.digests_present = true;

  if (in.peek() == std::char_traits<char>::eof()) {
    return;
  }

  cp.read_info.assay = read_string(in);
  if (in.peek() != std::char_traits<char>::eof()) {
    cp.read_info.assay_confidence = read_string(in);
  }
  if (in.peek() != std::char_traits<char>::eof()) {
    cp.read_info.compressor_version = read_string(in);
  }
  if (in.peek() != std::char_traits<char>::eof()) {
    cp.encoding.barcode_sort = read_bool(in);
    in.read(byte_ptr(&cp.encoding.cb_len), sizeof(uint32_t));
    if (in.peek() != std::char_traits<char>::eof()) {
      cp.encoding.bisulfite_ternary = read_bool(in);
      if (in.peek() != std::char_traits<char>::eof()) {
        in.read(byte_ptr(&cp.encoding.depleted_base_by_stream[0]),
                sizeof(char));
        cp.encoding.depleted_base_by_stream[1] =
            cp.encoding.depleted_base_by_stream[0];
        if (in.peek() != std::char_traits<char>::eof()) {
          cp.encoding.poly_at_stripped = read_bool(in);
          if (in.peek() != std::char_traits<char>::eof()) {
            cp.encoding.cb_prefix_stripped = read_bool(in);
            in.read(byte_ptr(&cp.encoding.cb_prefix_len), sizeof(uint32_t));
            if (in.peek() != std::char_traits<char>::eof()) {
              cp.read_info.quality_header_has_id = read_bool(in);
              if (in.peek() != std::char_traits<char>::eof()) {
                cp.encoding.atac_adapter_stripped = read_bool(in);
                if (in.peek() != std::char_traits<char>::eof()) {
                  cp.encoding.index_id_suffix_reconstructed = read_bool(in);
                  if (in.peek() != std::char_traits<char>::eof()) {
                    for (int i = 0; i < 2; ++i)
                      cp.encoding.use_crlf_by_stream[i] = read_bool(in);
                    for (int i = 0; i < 2; ++i) {
                      cp.read_info.quality_header_has_id_by_stream[i] =
                          read_bool(in);
                    }
                    has_stream_specific_fastq_metadata = true;
                    if (in.peek() != std::char_traits<char>::eof()) {
                      cp.read_info.digest_length_framed = read_bool(in);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

void read_compression_params_core(std::istream &in, compression_params &cp) {
  cp.encoding.paired_end = read_bool(in);
  cp.encoding.preserve_order = read_bool(in);
  cp.encoding.preserve_quality = read_bool(in);
  cp.encoding.preserve_id = read_bool(in);
  cp.encoding.long_flag = read_bool(in);
  // Reserved (was qvz_flag): read and discard to maintain binary format
  // alignment.
  static_cast<void>(read_bool(in));
  cp.quality.ill_bin_flag = read_bool(in);
  cp.quality.bin_thr_flag = read_bool(in);
  // Reserved (was qvz_ratio): read and discard to maintain binary format
  // alignment.
  double discarded_qvz_ratio = 0.0;
  in.read(byte_ptr(&discarded_qvz_ratio), sizeof(double));
  in.read(byte_ptr(&cp.quality.bin_thr_thr), sizeof(unsigned int));
  in.read(byte_ptr(&cp.quality.bin_thr_high), sizeof(unsigned int));
  in.read(byte_ptr(&cp.quality.bin_thr_low), sizeof(unsigned int));
  in.read(byte_ptr(&cp.read_info.num_reads), sizeof(uint32_t));
  in.read(byte_ptr(&cp.read_info.num_reads_clean[0]), sizeof(uint32_t));
  in.read(byte_ptr(&cp.read_info.num_reads_clean[1]), sizeof(uint32_t));
  in.read(byte_ptr(&cp.read_info.max_readlen), sizeof(uint32_t));
  in.read(byte_ptr(&cp.read_info.paired_id_code), sizeof(uint8_t));
  cp.read_info.paired_id_match = read_bool(in);
  in.read(byte_ptr(&cp.encoding.num_reads_per_block), sizeof(int));
  in.read(byte_ptr(&cp.encoding.num_reads_per_block_long), sizeof(int));
  in.read(byte_ptr(&cp.encoding.num_thr), sizeof(int));
  in.read(byte_ptr(&cp.encoding.compression_level), sizeof(int));
  in.read(reinterpret_cast<char *>(cp.read_info.file_len_seq_thr),
          sizeof(uint64_t) * compression_params::ReadMetadata::kFileLenThrSize);
  in.read(reinterpret_cast<char *>(cp.read_info.file_len_id_thr),
          sizeof(uint64_t) * compression_params::ReadMetadata::kFileLenThrSize);
  cp.encoding.use_crlf = read_bool(in);
  cp.read_info.input_filename_1 = read_string(in);
  cp.read_info.input_filename_2 = read_string(in);
  cp.read_info.note = read_string(in);
  cp.encoding.fasta_mode = read_bool(in);

  for (int i = 0; i < 2; ++i)
    cp.gzip.streams[i].was_gzipped = read_bool(in);
  for (int i = 0; i < 2; ++i)
    in.read(byte_ptr(&cp.gzip.streams[i].flg), sizeof(uint8_t));
  for (int i = 0; i < 2; ++i)
    in.read(byte_ptr(&cp.gzip.streams[i].mtime), sizeof(uint32_t));
  for (int i = 0; i < 2; ++i)
    in.read(byte_ptr(&cp.gzip.streams[i].xfl), sizeof(uint8_t));
  for (int i = 0; i < 2; ++i)
    in.read(byte_ptr(&cp.gzip.streams[i].os), sizeof(uint8_t));
  for (int i = 0; i < 2; ++i)
    cp.gzip.streams[i].name = read_string(in);
  for (int i = 0; i < 2; ++i)
    cp.gzip.streams[i].is_bgzf = read_bool(in);
  for (int i = 0; i < 2; ++i)
    in.read(byte_ptr(&cp.gzip.streams[i].bgzf_block_size), sizeof(uint16_t));
  for (int i = 0; i < 2; ++i)
    in.read(byte_ptr(&cp.gzip.streams[i].uncompressed_size), sizeof(uint64_t));
  for (int i = 0; i < 2; ++i)
    in.read(byte_ptr(&cp.gzip.streams[i].compressed_size), sizeof(uint64_t));
  for (int i = 0; i < 2; ++i)
    in.read(byte_ptr(&cp.gzip.streams[i].member_count), sizeof(uint32_t));
}

void read_compression_params_body(std::istream &in, compression_params &cp,
                                  uint32_t archive_format_version) {
  bool has_stream_specific_fastq_metadata = false;
  cp.read_info.archive_format_version = archive_format_version;
  read_compression_params_core(in, cp);

  for (int i = 0; i < 2; ++i) {
    cp.read_info.sequence_crc[i] = 0;
    cp.read_info.quality_crc[i] = 0;
    cp.read_info.id_crc[i] = 0;
  }
  cp.read_info.digests_present = false;
  initialize_optional_archive_metadata_defaults(cp);

  if (archive_format_version >= ARCHIVE_FORMAT_VERSION_V2) {
    uint32_t trailer_len = 0;
    in.read(byte_ptr(&trailer_len), sizeof(uint32_t));
    if (!in) {
      throw std::runtime_error(
          "Corrupt archive: truncated metadata extension length.");
    }
    if (trailer_len > kMaxMetadataExtensionBytes) {
      throw std::runtime_error(
          "Corrupt archive: metadata extension length is too large (" +
          std::to_string(trailer_len) + " bytes).");
    }
    if (const std::optional<uint64_t> remaining = remaining_stream_bytes(in)) {
      if (static_cast<uint64_t>(trailer_len) > *remaining) {
        throw std::runtime_error(
            "Corrupt archive: metadata extension length exceeds remaining "
            "bytes.");
      }
    }
    std::string trailer(trailer_len, '\0');
    if (trailer_len > 0) {
      in.read(trailer.data(), static_cast<std::streamsize>(trailer_len));
      if (!in ||
          in.gcount() != static_cast<std::streamsize>(trailer_len)) {
        throw std::runtime_error(
            "Corrupt archive: truncated metadata extension trailer.");
      }
    }
    std::istringstream trailer_stream(trailer, std::ios::binary);
    read_extension_trailer_v2(trailer_stream, cp,
                              has_stream_specific_fastq_metadata);
    if (trailer_stream.peek() != std::char_traits<char>::eof()) {
      throw std::runtime_error(
          "Corrupt archive: metadata extension trailer has trailing junk.");
    }
  } else {
    read_extension_trailer_legacy_peek(in, cp,
                                       has_stream_specific_fastq_metadata);
  }

  if (!has_stream_specific_fastq_metadata) {
    apply_legacy_stream_metadata(cp);
  }

  cp.encoding.use_crlf =
      cp.encoding.use_crlf_by_stream[0] ||
      (cp.encoding.paired_end && cp.encoding.use_crlf_by_stream[1]);
  cp.read_info.quality_header_has_id =
      cp.read_info.quality_header_has_id_by_stream[0];

  if (cp.read_info.archive_format_version == LEGACY_ARCHIVE_FORMAT_VERSION &&
      cp.read_info.compressor_version.empty()) {
    cp.read_info.compressor_version = kRc1ArchiveVersion;
  }
}

} // namespace

void write_compression_params(std::ostream &out, const compression_params &cp) {
  write_archive_metadata_header(out, CURRENT_ARCHIVE_FORMAT_VERSION);
  write_compression_params_body(out, cp);
}

void read_compression_params(std::istream &in, compression_params &cp) {
  const uint32_t archive_format_version = read_archive_metadata_header(in);
  if (archive_format_version > CURRENT_ARCHIVE_FORMAT_VERSION) {
    throw std::runtime_error("Unsupported archive metadata format version: " +
                             std::to_string(archive_format_version));
  }

  read_compression_params_body(in, cp, archive_format_version);
}

} // namespace spring
