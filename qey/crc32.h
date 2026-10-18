#ifndef QEY_CRC32_H_
#define QEY_CRC32_H_

#include <cstdint>
#include <string_view>

namespace qey {

// Returns the CRC-32 (IEEE 802.3, as used by zlib and PNG) of `data`.
uint32_t Crc32(std::string_view data);

}  // namespace qey

#endif  // QEY_CRC32_H_
