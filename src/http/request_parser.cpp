#include "http/request_parser.hpp"

#include <algorithm>

namespace httpd {
namespace {

constexpr std::string_view kCRLF = "\r\n";

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 1*DIGIT without overflow (19 digits always fit in 64 bits).
bool parse_decimal(std::string_view s, std::size_t& out) noexcept {
  if (s.empty() || s.size() > 19) return false;
  std::uint64_t v = 0;
  for (char c : s) {
    if (!is_digit(c)) return false;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  out = static_cast<std::size_t>(v);
  return true;
}

// Content-Length may legally repeat as "5, 5" (RFC 9110 §8.6) - every member
// must be identical, otherwise the framing is ambiguous and we must reject it.
bool parse_content_length(std::string_view value, std::size_t& out) noexcept {
  bool first = true;
  for (;;) {
    std::size_t comma = value.find(',');
    std::size_t n = 0;
    if (!parse_decimal(trim_ows(value.substr(0, comma)), n)) return false;
    if (!first && n != out) return false;
    out = n;
    first = false;
    if (comma == std::string_view::npos) return true;
    value.remove_prefix(comma + 1);
  }
}

bool valid_target(std::string_view t) noexcept {
  if (t.empty()) return false;
  for (char c : t) {
    auto u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7f) return false;  // no whitespace or control bytes
  }
  return true;
}

bool starts_with_icase(std::string_view s, std::string_view prefix) noexcept {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

}  // namespace

bool Request::keep_alive() const {
  if (headers.has_token("Connection", "close")) return false;
  if (version_minor == 0) return headers.has_token("Connection", "keep-alive");
  return true;  // persistent by default in HTTP/1.1
}

int status_for(ParseError e) noexcept {
  switch (e) {
    case ParseError::None: return 200;
    case ParseError::BadRequest: return 400;
    case ParseError::UriTooLong: return 414;
    case ParseError::HeaderFieldsTooLarge: return 431;
    case ParseError::PayloadTooLarge: return 413;
    case ParseError::NotImplemented: return 501;
    case ParseError::VersionNotSupported: return 505;
  }
  return 400;
}

const char* to_string(ParseError e) noexcept {
  switch (e) {
    case ParseError::None: return "none";
    case ParseError::BadRequest: return "bad_request";
    case ParseError::UriTooLong: return "uri_too_long";
    case ParseError::HeaderFieldsTooLarge: return "header_fields_too_large";
    case ParseError::PayloadTooLarge: return "payload_too_large";
    case ParseError::NotImplemented: return "not_implemented";
    case ParseError::VersionNotSupported: return "version_not_supported";
  }
  return "unknown";
}

void RequestParser::reset() {
  state_ = State::RequestLine;
  req_ = Request{};
  error_ = ParseError::None;
  error_detail_.clear();
  header_bytes_ = 0;
  header_count_ = 0;
  remaining_ = 0;
}

Request RequestParser::take() {
  Request r = std::move(req_);
  reset();
  return r;
}

void RequestParser::fail(ParseError e, const char* detail) {
  state_ = State::Error;
  error_ = e;
  error_detail_ = detail;
}

std::size_t RequestParser::feed(std::string_view data) {
  std::size_t consumed = 0;
  for (;;) {
    std::string_view rest = data.substr(consumed);
    switch (state_) {
      case State::RequestLine: {
        std::size_t eol = rest.find(kCRLF);
        if (eol == std::string_view::npos) {
          // The unterminated tail may end in the '\r' of the CRLF, hence +1.
          if (rest.size() > limits_.max_request_line + 1) fail(ParseError::UriTooLong, "request-line too long");
          return consumed;
        }
        if (eol == 0) {  // RFC 9112 §2.2: ignore empty lines before a request
          consumed += kCRLF.size();
          continue;
        }
        if (eol > limits_.max_request_line) {
          fail(ParseError::UriTooLong, "request-line too long");
          return consumed;
        }
        if (!parse_request_line(rest.substr(0, eol))) return consumed;
        consumed += eol + kCRLF.size();
        state_ = State::Headers;
        continue;
      }

      case State::Headers:
      case State::Trailers: {
        std::size_t eol = rest.find(kCRLF);
        if (eol == std::string_view::npos) {
          if (header_bytes_ + rest.size() + 1 > limits_.max_header_bytes) {
            fail(ParseError::HeaderFieldsTooLarge, "header section too large");
          }
          return consumed;
        }
        std::size_t line_len = eol + kCRLF.size();
        if (header_bytes_ + line_len > limits_.max_header_bytes) {
          fail(ParseError::HeaderFieldsTooLarge, "header section too large");
          return consumed;
        }
        header_bytes_ += line_len;
        consumed += line_len;
        if (eol == 0) {  // blank line: end of the header (or trailer) section
          if (state_ == State::Trailers) {
            state_ = State::Done;
          } else if (!finish_headers()) {
            return consumed;
          }
          continue;
        }
        if (++header_count_ > limits_.max_header_count) {
          fail(ParseError::HeaderFieldsTooLarge, "too many header fields");
          return consumed;
        }
        Headers& into = state_ == State::Headers ? req_.headers : req_.trailers;
        if (!parse_field_line(rest.substr(0, eol), into)) return consumed;
        continue;
      }

      case State::Body:
      case State::ChunkData: {
        if (rest.empty()) return consumed;
        std::size_t n = std::min(rest.size(), remaining_);
        req_.body.append(rest.data(), n);
        consumed += n;
        remaining_ -= n;
        if (remaining_ == 0) state_ = state_ == State::Body ? State::Done : State::ChunkDataEnd;
        continue;
      }

      case State::ChunkDataEnd: {
        if (rest.size() < kCRLF.size()) return consumed;
        if (rest.substr(0, kCRLF.size()) != kCRLF) {
          fail(ParseError::BadRequest, "missing CRLF after chunk data");
          return consumed;
        }
        consumed += kCRLF.size();
        state_ = State::ChunkSize;
        continue;
      }

      case State::ChunkSize: {
        std::size_t eol = rest.find(kCRLF);
        if (eol == std::string_view::npos) {
          if (rest.size() > limits_.max_chunk_line + 1) fail(ParseError::BadRequest, "chunk-size line too long");
          return consumed;
        }
        if (eol > limits_.max_chunk_line) {
          fail(ParseError::BadRequest, "chunk-size line too long");
          return consumed;
        }
        if (!parse_chunk_size(rest.substr(0, eol))) return consumed;
        consumed += eol + kCRLF.size();
        continue;
      }

      case State::Done:
      case State::Error:
        return consumed;
    }
  }
}

// request-line = method SP request-target SP HTTP-version
bool RequestParser::parse_request_line(std::string_view line) {
  std::size_t sp1 = line.find(' ');
  std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos || line.find(' ', sp2 + 1) != std::string_view::npos) {
    fail(ParseError::BadRequest, "request-line must have exactly three parts");
    return false;
  }
  std::string_view method = line.substr(0, sp1);
  std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  std::string_view version = line.substr(sp2 + 1);

  if (!is_token(method)) {
    fail(ParseError::BadRequest, "invalid method");
    return false;
  }
  if (!valid_target(target)) {
    fail(ParseError::BadRequest, "invalid request-target");
    return false;
  }
  // HTTP-version = "HTTP/" DIGIT "." DIGIT (case-sensitive)
  if (version.size() != 8 || version.substr(0, 5) != "HTTP/" || !is_digit(version[5]) ||
      version[6] != '.' || !is_digit(version[7])) {
    fail(ParseError::BadRequest, "malformed HTTP-version");
    return false;
  }
  int major = version[5] - '0';
  int minor = version[7] - '0';
  if (major != 1 || minor > 1) {
    fail(ParseError::VersionNotSupported, "only HTTP/1.0 and HTTP/1.1 are supported");
    return false;
  }

  // origin-form "/path?query", absolute-form "http://host/path", or "*" for OPTIONS.
  std::string_view path_and_query = target;
  if (starts_with_icase(target, "http://") || starts_with_icase(target, "https://")) {
    std::size_t authority = target.find("//") + 2;
    std::size_t slash = target.find('/', authority);
    path_and_query = slash == std::string_view::npos ? std::string_view("/") : target.substr(slash);
  } else if (target.front() != '/' && !(target == "*" && method == "OPTIONS")) {
    fail(ParseError::BadRequest, "unsupported request-target form");
    return false;
  }

  std::size_t qmark = path_and_query.find('?');
  req_.method.assign(method);
  req_.target.assign(target);
  req_.path.assign(path_and_query.substr(0, qmark));
  req_.query.assign(qmark == std::string_view::npos ? std::string_view() : path_and_query.substr(qmark + 1));
  req_.version_major = major;
  req_.version_minor = minor;
  return true;
}

// field-line = field-name ":" OWS field-value OWS
bool RequestParser::parse_field_line(std::string_view line, Headers& into) {
  std::size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    fail(ParseError::BadRequest, "header line without ':'");
    return false;
  }
  std::string_view name = line.substr(0, colon);
  // Rejects "Host : x" (whitespace before the colon, RFC 9112 §5.1) and
  // obsolete line folding (continuation lines start with whitespace).
  if (!is_token(name)) {
    fail(ParseError::BadRequest, "invalid header field name");
    return false;
  }
  std::string_view value = trim_ows(line.substr(colon + 1));
  for (char c : value) {
    auto u = static_cast<unsigned char>(c);
    // Bare CR/LF/NUL inside a value are a classic request-smuggling vector.
    if ((u < 0x20 && c != '\t') || u == 0x7f) {
      fail(ParseError::BadRequest, "control character in header value");
      return false;
    }
  }
  if (&into == &req_.headers && iequals(name, "Host") && into.has("Host")) {
    fail(ParseError::BadRequest, "duplicate Host header");
    return false;
  }
  into.add(name, value);
  return true;
}

// Decides how the body is framed (RFC 9112 §6.3).
bool RequestParser::finish_headers() {
  const Headers& h = req_.headers;
  if (req_.version_minor == 1 && !h.has("Host")) {
    fail(ParseError::BadRequest, "HTTP/1.1 request without Host");
    return false;
  }

  auto te = h.get("Transfer-Encoding");
  auto cl = h.get("Content-Length");
  if (te && cl) {
    // Front-end and back-end could disagree on where this message ends.
    fail(ParseError::BadRequest, "both Transfer-Encoding and Content-Length");
    return false;
  }

  if (te) {
    if (req_.is_http10()) {
      fail(ParseError::BadRequest, "Transfer-Encoding in an HTTP/1.0 request");
      return false;
    }
    if (!iequals(trim_ows(*te), "chunked")) {
      fail(ParseError::NotImplemented, "unsupported transfer-coding");
      return false;
    }
    state_ = State::ChunkSize;
  } else if (cl) {
    std::size_t length = 0;
    if (!parse_content_length(*cl, length)) {
      fail(ParseError::BadRequest, "invalid Content-Length");
      return false;
    }
    if (length > limits_.max_body_bytes) {
      fail(ParseError::PayloadTooLarge, "body exceeds limit");
      return false;
    }
    remaining_ = length;
    req_.body.reserve(length);
    state_ = length ? State::Body : State::Done;
  } else {
    state_ = State::Done;
  }

  auto expect = h.get("Expect");
  req_.expect_continue = expect && iequals(trim_ows(*expect), "100-continue") && state_ != State::Done;
  return true;
}

// chunk-size [ chunk-ext ] CRLF, chunk-size = 1*HEXDIG
bool RequestParser::parse_chunk_size(std::string_view line) {
  std::string_view digits = trim_ows(line.substr(0, line.find(';')));  // extensions are ignored
  if (digits.empty() || digits.size() > 16) {
    fail(ParseError::BadRequest, "invalid chunk size");
    return false;
  }
  std::uint64_t size = 0;
  for (char c : digits) {
    int v = hex_value(c);
    if (v < 0) {
      fail(ParseError::BadRequest, "invalid chunk size");
      return false;
    }
    size = (size << 4) | static_cast<std::uint64_t>(v);
  }
  if (size == 0) {
    state_ = State::Trailers;
    return true;
  }
  if (size > limits_.max_body_bytes || req_.body.size() + size > limits_.max_body_bytes) {
    fail(ParseError::PayloadTooLarge, "body exceeds limit");
    return false;
  }
  remaining_ = static_cast<std::size_t>(size);
  state_ = State::ChunkData;
  return true;
}

}  // namespace httpd
