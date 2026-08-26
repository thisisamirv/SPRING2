#ifndef SPRING_COMMON_LEGACY_ID_CODEC_H_
#define SPRING_COMMON_LEGACY_ID_CODEC_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace spring {

void decompress_legacy_id_block_bytes(std::string_view input_bytes,
                                      std::string *id_array, uint32_t num_ids);

} // namespace spring

#endif // SPRING_COMMON_LEGACY_ID_CODEC_H_