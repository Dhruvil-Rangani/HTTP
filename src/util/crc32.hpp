#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace httpd {

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) - the checksum used by
// Ethernet frames, gzip and PNG. Streaming: feed successive pieces by passing
// the previous result back in as `crc`.
std::uint32_t crc32(std::string_view data, std::uint32_t crc = 0) noexcept;

}  // namespace httpd
