#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "http/headers.hpp"
#include "util/unique_fd.hpp"

namespace httpd {

std::string_view reason_phrase(int status) noexcept;

// Pull-based streaming body. The connection calls it whenever the socket has
// drained below its low-water mark, so a slow client naturally throttles the
// producer (backpressure) and memory stays bounded however large the body is.
// Contract: append at least one byte to `out` and return true, or return
// false when the body is finished (optionally appending a final piece and
// setting `trailers`, which are sent only with chunked framing). Runs on the
// event-loop thread, so it must never block.
using BodyProducer = std::function<bool(std::string& out, Headers& trailers)>;

struct Response {
  int status = 200;
  Headers headers;
  std::string body;

  // Zero-copy file body, transmitted with sendfile(2). Wins over `body`.
  UniqueFd file;
  std::uint64_t file_offset = 0;
  std::uint64_t file_length = 0;

  // Streaming body. With `stream_length` it is framed by Content-Length,
  // otherwise with chunked transfer coding (close-delimited for HTTP/1.0).
  BodyProducer producer;
  std::optional<std::uint64_t> stream_length;

  static Response text(int status, std::string body,
                       std::string_view content_type = "text/plain; charset=utf-8");
};

enum class Framing : std::uint8_t {
  None,            // 1xx / 204 / 304: no body by definition
  ContentLength,
  Chunked,
  CloseDelimited,  // body ends when the connection closes (HTTP/1.0 streaming)
};

struct HeadOptions {
  bool keep_alive = true;
  bool http10_client = false;
  std::string_view date;  // preformatted IMF-fixdate; omitted when empty
};

// Appends the status line and header section to `out`. Framing headers
// (Content-Length / Transfer-Encoding / Connection) are always derived here
// from the response itself, so a handler can never produce a message whose
// declared length disagrees with what is sent. Returns the framing to use.
Framing write_head(std::string& out, const Response& res, const HeadOptions& opts);

// Chunked transfer coding (RFC 9112 §7.1).
void append_chunk(std::string& out, std::string_view data);
void append_last_chunk(std::string& out, const Headers& trailers);

}  // namespace httpd
