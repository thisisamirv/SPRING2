// Implements background and multi-threaded BGZF (Block Gzip Format) handling
// used to read and process gzipped FASTQ inputs efficiently.

#include "bgzf.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <libdeflate.h>
#include <stdexcept>

namespace spring {

namespace {

// BGZF block = 10-byte gzip header + 8-byte extra (XLEN/BC/BSIZE) + deflate
// payload + 8-byte trailer. Total size must be <= 65536 so BSIZE (= size - 1)
// fits in uint16_t.
constexpr size_t kBgzfMaxBlockSize = 65536;
constexpr size_t kBgzfGzipHeaderWithExtra = 18;
constexpr size_t kBgzfGzipTrailer = 8;
constexpr size_t kBgzfMaxCompressedPayload =
    kBgzfMaxBlockSize - kBgzfGzipHeaderWithExtra - kBgzfGzipTrailer; // 65510
// Leave headroom for incompressible expansion (same default as htslib).
constexpr size_t kBgzfMaxUncompressed = 0xff00;

} // namespace

std::vector<std::string> bgzf_compress_buffer(const std::string &buffer,
                                              int level) {
  if (buffer.empty())
    return {};

  libdeflate_compressor *compressor = libdeflate_alloc_compressor(level);
  if (!compressor)
    throw std::runtime_error("Failed to allocate libdeflate compressor");

  std::vector<std::string> blocks;
  const char *ptr = buffer.data();
  size_t remaining = buffer.size();
  std::vector<char> compressed_buf(kBgzfMaxCompressedPayload);

  while (remaining > 0) {
    size_t to_compress = std::min(remaining, kBgzfMaxUncompressed);
    size_t compressed_size = 0;

    // Cap the deflate output to the max payload that still fits in a BGZF
    // block. If the chunk is incompressible, shrink and retry.
    while (true) {
      compressed_size = libdeflate_deflate_compress(
          compressor, ptr, to_compress, compressed_buf.data(),
          compressed_buf.size());
      if (compressed_size != 0) {
        break;
      }
      if (to_compress <= 1) {
        libdeflate_free_compressor(compressor);
        throw std::runtime_error("BGZF block compression failed");
      }
      to_compress /= 2;
    }

    const size_t block_size =
        kBgzfGzipHeaderWithExtra + compressed_size + kBgzfGzipTrailer;
    if (block_size > kBgzfMaxBlockSize) {
      libdeflate_free_compressor(compressor);
      throw std::runtime_error("BGZF block exceeds 64 KiB limit");
    }

    std::string block;
    block.reserve(block_size);
    // Header
    const unsigned char header[10] = {0x1f, 0x8b, 0x08, 0x04, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0xff};
    block.append(reinterpret_cast<const char *>(header), 10);
    // Extra
    const uint16_t xlen = 6;
    const uint16_t slen = 2;
    const uint16_t bsiz = static_cast<uint16_t>(block_size - 1);
    block.append(reinterpret_cast<const char *>(&xlen), 2);
    block.push_back('B');
    block.push_back('C');
    block.append(reinterpret_cast<const char *>(&slen), 2);
    block.append(reinterpret_cast<const char *>(&bsiz), 2);
    // Data
    block.append(compressed_buf.data(), compressed_size);
    // Trailer
    const uint32_t crc = libdeflate_crc32(0, ptr, to_compress);
    const uint32_t isize = static_cast<uint32_t>(to_compress);
    block.append(reinterpret_cast<const char *>(&crc), 4);
    block.append(reinterpret_cast<const char *>(&isize), 4);

    blocks.push_back(std::move(block));
    ptr += to_compress;
    remaining -= to_compress;
  }

  libdeflate_free_compressor(compressor);
  return blocks;
}

} // namespace spring
