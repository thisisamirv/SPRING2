// Declares structures and APIs for handling BGZF (Block Gzipped Format) data,
// enabling random access or multi-threaded reading of compressed inputs.

#ifndef SPRING_BGZF_H_
#define SPRING_BGZF_H_

#include <string>
#include <vector>

namespace spring {

std::vector<std::string> bgzf_compress_buffer(const std::string &buffer,
                                              int level = 6);

} // namespace spring

#endif
