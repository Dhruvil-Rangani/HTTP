#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "http/headers.hpp"

namespace httpd {

struct Request {
  std::string method;
  std::string target;  // request-target exactly as received
  std::string path;    // target without the query string
  std::string query;   // text after '?', without the '?'
  int version_major = 1;
  int version_minor = 1;
  Headers headers;
  Headers trailers;  // only populated for chunked bodies
  std::string body;
  bool expect_continue = false;  // client sent "Expect: 100-continue" and a body follows

  bool keep_alive() const;
  bool is_http10() const noexcept { return version_major == 1 && version_minor == 0; }
};

enum class ParseError : std::uint8_t {
  None,
  BadRequest,           // 400
  UriTooLong,           // 414
  HeaderFieldsTooLarge, // 431
  PayloadTooLarge,      // 413
  NotImplemented,       // 501 (unsupported transfer-coding)
  VersionNotSupported,  // 505
};

int status_for(ParseError e) noexcept;
const char* to_string(ParseError e) noexcept;

// Hard limits keep per-connection memory bounded no matter what a peer sends.
struct ParserLimits {
  std::size_t max_request_line = 8 * 1024;
  std::size_t max_header_bytes = 16 * 1024;  // whole header section, trailers included
  std::size_t max_header_count = 100;
  std::size_t max_body_bytes = 8 * 1024 * 1024;
  std::size_t max_chunk_line = 1024;
};

// Incremental (push) HTTP/1.x request parser - RFC 9112.
//
// TCP delivers a byte stream, not messages: a request can arrive one byte at
// a time or several pipelined requests in one read. feed() consumes every
// complete syntactic unit (line, body bytes) it can and returns how many bytes
// it used; the caller keeps the unconsumed tail and presents it again with the
// next read. The parser stops at the end of each request so pipelined
// requests stay in the caller's buffer. The result never depends on how the
// input was split - the fuzz target checks exactly that.
class RequestParser {
 public:
  enum class State : std::uint8_t {
    RequestLine,
    Headers,
    Body,
    ChunkSize,
    ChunkData,
    ChunkDataEnd,
    Trailers,
    Done,
    Error,
  };

  explicit RequestParser(ParserLimits limits = {}) : limits_(limits) {}

  std::size_t feed(std::string_view data);

  State state() const noexcept { return state_; }
  bool done() const noexcept { return state_ == State::Done; }
  bool failed() const noexcept { return state_ == State::Error; }
  // True once the request line has been parsed.
  bool started() const noexcept { return state_ != State::RequestLine; }
  // True once the blank line ending the header section has been seen.
  bool headers_complete() const noexcept {
    return state_ != State::RequestLine && state_ != State::Headers && state_ != State::Error;
  }

  ParseError error() const noexcept { return error_; }
  const std::string& error_detail() const noexcept { return error_detail_; }

  const Request& request() const noexcept { return req_; }
  // Moves the finished request out and resets the parser for the next one.
  Request take();
  void reset();

 private:
  void fail(ParseError e, const char* detail);
  bool parse_request_line(std::string_view line);
  bool parse_field_line(std::string_view line, Headers& into);
  bool finish_headers();
  bool parse_chunk_size(std::string_view line);

  ParserLimits limits_;
  State state_ = State::RequestLine;
  Request req_;
  ParseError error_ = ParseError::None;
  std::string error_detail_;
  std::size_t header_bytes_ = 0;
  std::size_t header_count_ = 0;
  std::size_t remaining_ = 0;  // bytes left in the current body or chunk
};

}  // namespace httpd
