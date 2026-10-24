// Centralizes compile-time tuning constants and runtime compression parameters.

#ifndef SPRING_PARAMS_H_
#define SPRING_PARAMS_H_

#include <cstdint>
#include <iosfwd>
#include <string>

namespace spring {

constexpr uint32_t ARCHIVE_METADATA_MAGIC = 0x32505343U;
constexpr uint32_t LEGACY_ARCHIVE_FORMAT_VERSION = 0;
// v1: optional trailing fields inferred via peek()!=eof (including digests).
// v2: length-prefixed extension trailer with an explicit digests_present flag.
constexpr uint32_t ARCHIVE_FORMAT_VERSION_V1 = 1;
constexpr uint32_t ARCHIVE_FORMAT_VERSION_V2 = 2;
constexpr uint32_t CURRENT_ARCHIVE_FORMAT_VERSION = ARCHIVE_FORMAT_VERSION_V2;

// Shared bounds and sentinel values used across the compression pipeline.
constexpr uint16_t MAX_READ_LEN = 511;
constexpr uint32_t MAX_READ_LEN_LONG = 4294967290U;
constexpr uint32_t MAX_NUM_READS = 4294967290U;

// Minimum poly-A/T run length (exclusive) required to strip the tail.
// Runs of exactly this length are kept; only strictly longer runs are stripped.
constexpr uint32_t POLY_AT_TAIL_MIN_LEN = 20;

// Minimum terminal overlap against the canonical Tn5/Nextera mosaic-end
// adapter required before the overlap is stripped for ATAC assays.
constexpr uint32_t ATAC_ADAPTER_MIN_MATCH = 12;

// Reordering parameters.
constexpr int NUM_DICT_REORDER = 2;
constexpr int MAX_SEARCH_REORDER = 1000;
constexpr int THRESH_REORDER = 4;
// Keep this a power of two so lock sharding can use fast masking.
constexpr int NUM_LOCKS_REORDER = 0x10000;
constexpr float STOP_CRITERIA_REORDER = 0.5F;

namespace detail {
inline uint32_t lock_shard(const uint64_t item_id) {
  return static_cast<uint32_t>(item_id & (NUM_LOCKS_REORDER - 1));
}
} // namespace detail

// Encoding parameters.
constexpr int NUM_DICT_ENCODER = 2;
constexpr int MAX_SEARCH_ENCODER = 1000;
constexpr int THRESH_ENCODER = 24;

// For small read pools, MPHF build overhead can dominate runtime. Use a
// single dictionary in reorder/encoder to reduce build cost with minimal
// sensitivity loss on tiny inputs.
constexpr uint32_t DICT_SINGLE_STAGE_READ_THRESHOLD = 50000;

// Block sizing parameters for stream chunking and BSC compression.
constexpr int NUM_READS_PER_BLOCK = 256000;
constexpr int NUM_READS_PER_BLOCK_LONG = 10000;
constexpr int BSC_BLOCK_SIZE = 64;

// Default compression level (1-9) used by the CLI. This value is passed
// directly to gzip (1-9) and scaled to Zstd (1-22) where Zstd is used.
static constexpr int DEFAULT_COMPRESSION_LEVEL = 6;

// Maximum allowed growth (in bases) for a single consensus contig before
// forcing a break to prevent memory exhaustion or pathological reordering.
constexpr int64_t MAX_CONTIG_GROWTH = 64 * 1024 * 1024; // 64 MB

struct compression_params {
  struct EncodingConfig {
    bool paired_end = false;
    bool preserve_order = true;
    bool preserve_quality = true;
    bool preserve_id = true;
    bool long_flag = false;
    int num_thr = 1;
    int compression_level = DEFAULT_COMPRESSION_LEVEL;
    int num_reads_per_block = NUM_READS_PER_BLOCK;
    int num_reads_per_block_long = NUM_READS_PER_BLOCK_LONG;
    bool fasta_mode = false;
    bool use_crlf = false;
    bool use_crlf_by_stream[2] = {false, false};
    uint32_t cb_len = 16;      // CB length for extraction/display.
    // Compression-time only (not archived): true when the caller set --cb-len
    // / CompressOptions.cb_len_was_explicit. Auto-detected sc-rna requires
    // this before R1 prefix stripping so a default of 16 cannot treat
    // genomic sequence as barcode after a weak misclassification.
    bool cb_len_was_explicit = false;
    bool barcode_sort = false; // Legacy field; always false in new archives.
    bool bisulfite_ternary = false;
    // Per-stream depleted base for bisulfite ternary reorder modeling.
    // [0]=R1/SE, [1]=R2. Older archives stored only [0]; readers copy it to
    // [1] when the mate field is absent.
    char depleted_base_by_stream[2] = {'N', 'N'};
    bool cb_prefix_source_external =
        false; // Compression-time only: CB comes from I1/external lane.
    bool poly_at_stripped = false; // True when poly-A/T tail stripping was
                                   // applied during RNA-mode compression.
    bool atac_adapter_stripped =
        false; // True when ATAC adapter read-through stripping was applied.
    bool cb_prefix_stripped = false; // True when CB prefix was extracted from
                                     // R1 and stored in cb_prefix.dna.bsc.
    uint32_t cb_prefix_len = 0;      // Number of bases stripped from R1 start.
    bool index_id_suffix_reconstructed =
        false; // True when grouped sc-RNA index IDs omit the trailing I1/I2
               // token and restore it from decoded index reads.
    // disk_path memory reduction: use pthash external-memory builder for MPHF
    // construction.  When true, mphf_tmp_dir must be set.
    bool use_external_mphf = false;
    std::string mphf_tmp_dir;
    // disk_path memory reduction: when non-empty, encoder_main writes all
    // per-thread metadata (position, orientation, noise, read-length, order)
    // directly to this directory instead of accumulating in RAM.  The
    // per-thread files are stream-merged into final files after the OMP loop.
    std::string encoder_metadata_spill_dir;
    // disk_path memory reduction: when non-empty, reorder_main (chunked mode)
    // spills clean_read_streams and per-chunk singleton_read_bytes directly to
    // this directory, avoiding the ~29 GB stream + ~7 GB/chunk singleton RAM.
    // Must point at reorder_artifact_dir; the workflow detects pre-spilled
    // artifacts and skips the duplicate spill_reorder_encoder_artifact call.
    std::string reorder_spill_dir;
  } encoding;

  struct QualityConfig {
    bool ill_bin_flag = false;
    bool bin_thr_flag = false;
    unsigned int bin_thr_thr = 0;
    unsigned int bin_thr_high = 0;
    unsigned int bin_thr_low = 0;
  } quality;

  struct GzipMetadata {
    struct Stream {
      bool was_gzipped = false;
      uint8_t flg = 0;
      uint32_t mtime = 0;
      uint8_t xfl = 0;
      uint8_t os = 0;
      std::string name;
      bool is_bgzf = false;
      uint16_t bgzf_block_size = 0;
      uint64_t uncompressed_size = 0;
      uint64_t compressed_size = 0;
      uint32_t member_count = 0;
    } streams[2];
  } gzip;

  struct ReadMetadata {
    uint32_t num_reads = 0;
    uint32_t num_reads_clean[2] = {0, 0};
    uint32_t max_readlen = 0;
    uint8_t paired_id_code = 0;
    bool paired_id_match = false;
    bool quality_header_has_id =
        false; // True if FASTQ uses "+ID" format instead of "+"
    bool quality_header_has_id_by_stream[2] = {false, false};
    static constexpr size_t kFileLenThrSize = 1024;
    uint64_t file_len_seq_thr[kFileLenThrSize] = {0};
    uint64_t file_len_id_thr[kFileLenThrSize] = {0};
    std::string input_filename_1;
    std::string input_filename_2;
    std::string note;
    std::string assay;
    std::string assay_confidence;
    std::string compressor_version; // SPRING2 version that created this archive
    uint32_t archive_format_version = CURRENT_ARCHIVE_FORMAT_VERSION;
    bool legacy_spring = false; // Runtime-only flag for original SPRING
                                // archives.
    // True when the CRC digest block is present in archive metadata. For
    // format v2+ this is an explicit on-disk flag; older formats infer it from
    // a complete trailing digest block. Zero is a valid CRC32, so presence must
    // not be inferred from the digest value itself.
    bool digests_present = false;
    // When true, record CRCs mix in a little-endian length before each payload
    // so boundary shifts between reads cannot collide. False for older archives
    // that hashed raw concatenation only.
    bool digest_length_framed = true;
    uint32_t sequence_crc[2] = {0, 0};
    uint32_t quality_crc[2] = {0, 0};
    uint32_t id_crc[2] = {0, 0};
  } read_info;
};

// Metadata serialization helpers.
void write_bool(std::ostream &out, bool value);
bool read_bool(std::istream &in);
void write_string(std::ostream &out, const std::string &s);
std::string read_string(std::istream &in);
void write_compression_params(std::ostream &out, const compression_params &cp);
void read_compression_params(std::istream &in, compression_params &cp);

} // namespace spring

#endif // SPRING_PARAMS_H_
