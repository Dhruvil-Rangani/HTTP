#include "util/crc32.hpp"

#include <array>

namespace httpd {
namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    table[i] = c;
  }
  return table;
}

// Built at compile time: no startup cost, lives in .rodata.
constexpr auto kTable = make_table();

}  // namespace

std::uint32_t crc32(std::string_view data, std::uint32_t crc) noexcept {
  crc = ~crc;
  for (unsigned char byte : data) crc = kTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

}  // namespace httpd
