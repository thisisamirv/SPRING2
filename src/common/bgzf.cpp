// Implements background and multi-threaded BGZF (Block Gzip Format) handling
// used to read and process gzipped FASTQ inputs efficiently.

#include "bgzf.h"
#include <algorithm>
#include <libdeflate.h>
#include <stdexcept>

namespace spring {

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
  std::vector<char> compressed_buf(65536 + 1024);

  while (remaining > 0) {
    size_t to_compress = std::min<size_t>(remaining, 65536);
    size_t compressed_size = libdeflate_deflate_compress(
        compressor, ptr, to_compress, compressed_buf.data(),
        compressed_buf.size());

    if (compressed_size == 0) {
      libdeflate_free_compressor(compressor);
      throw std::runtime_error("BGZF block compression failed");
    }

    std::string block;
    block.reserve(10 + 8 + compressed_size + 8);
    // Header
    const unsigned char header[10] = {0x1f, 0x8b, 0x08, 0x04, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0xff};
    block.append(reinterpret_cast<const char *>(header), 10);
    // Extra
    uint16_t xlen = 6;
    uint16_t slen = 2;
    uint16_t bsiz = static_cast<uint16_t>(18 + compressed_size + 8 - 1);
    block.append(reinterpret_cast<const char *>(&xlen), 2);
    block.push_back('B');
    block.push_back('C');
    block.append(reinterpret_cast<const char *>(&slen), 2);
    block.append(reinterpret_cast<const char *>(&bsiz), 2);
    // Data
    block.append(compressed_buf.data(), compressed_size);
    // Trailer
    uint32_t crc = libdeflate_crc32(0, ptr, to_compress);
    uint32_t isize = static_cast<uint32_t>(to_compress);
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
