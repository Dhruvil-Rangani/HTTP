#include "http/response.hpp"

#include <charconv>

namespace httpd {
namespace {

void append_number(std::string& out, std::uint64_t v, int base = 10) {
  char buf[24];
  auto res = std::to_chars(buf, buf + sizeof buf, v, base);
  out.append(buf, res.ptr);
}

bool is_framing_header(std::string_view name) noexcept {
  return iequals(name, "Content-Length") || iequals(name, "Transfer-Encoding") ||
         iequals(name, "Connection") || iequals(name, "Date");
}

}  // namespace

std::string_view reason_phrase(int status) noexcept {
  switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
  }
}

Response Response::text(int status, std::string body, std::string_view content_type) {
  Response r;
  r.status = status;
  r.headers.set("Content-Type", content_type);
  r.body = std::move(body);
  return r;
}

Framing write_head(std::string& out, const Response& res, const HeadOptions& opts) {
  Framing framing;
  std::uint64_t length = 0;
  if (res.status / 100 == 1 || res.status == 204 || res.status == 304) {
    framing = Framing::None;
  } else if (res.file.valid()) {
    framing = Framing::ContentLength;
    length = res.file_length;
  } else if (res.producer) {
    if (res.stream_length) {
      framing = Framing::ContentLength;
      length = *res.stream_length;
    } else {
      framing = opts.http10_client ? Framing::CloseDelimited : Framing::Chunked;
    }
  } else {
    framing = Framing::ContentLength;
    length = res.body.size();
  }
  const bool keep_alive = opts.keep_alive && framing != Framing::CloseDelimited;

  // We always speak HTTP/1.1, also to 1.0 clients (RFC 9110 §6.2).
  out.append("HTTP/1.1 ");
  append_number(out, static_cast<std::uint64_t>(res.status));
  out.push_back(' ');
  out.append(reason_phrase(res.status));
  out.append("\r\n");

  for (const auto& [name, value] : res.headers) {
    if (is_framing_header(name)) continue;
    out.append(name).append(": ").append(value).append("\r\n");
  }
  if (!res.headers.has("Server")) out.append("Server: httpd-cpp\r\n");
  if (!opts.date.empty()) out.append("Date: ").append(opts.date).append("\r\n");

  if (framing == Framing::ContentLength) {
    out.append("Content-Length: ");
    append_number(out, length);
    out.append("\r\n");
  } else if (framing == Framing::Chunked) {
    out.append("Transfer-Encoding: chunked\r\n");
  }

  if (!keep_alive) {
    out.append("Connection: close\r\n");
  } else if (opts.http10_client) {
    out.append("Connection: keep-alive\r\n");
  }
  out.append("\r\n");
  return framing;
}

void append_chunk(std::string& out, std::string_view data) {
  if (data.empty()) return;  // a zero-size chunk would terminate the body
  append_number(out, data.size(), 16);
  out.append("\r\n").append(data).append("\r\n");
}

void append_last_chunk(std::string& out, const Headers& trailers) {
  out.append("0\r\n");
  for (const auto& [name, value] : trailers) out.append(name).append(": ").append(value).append("\r\n");
  out.append("\r\n");
}

}  // namespace httpd
