#include "qey/crc32.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace qey {
namespace {

constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < table.size(); ++i) {
    uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Crc32(std::string_view data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (char ch : data) {
    crc = kTable[(crc ^ static_cast<uint8_t>(ch)) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace qey
