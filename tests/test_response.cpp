#include <cstring>
#include <string>

#include "app/static_files.hpp"
#include "http/response.hpp"
#include "testing.hpp"
#include "util/byte_buffer.hpp"
#include "util/crc32.hpp"

using namespace httpd;

TEST(serializes_content_length_response) {
  Response r = Response::text(200, "hello");
  std::string out;
  HeadOptions opts;
  opts.date = "Wed, 23 Sep 2026 10:00:00 GMT";
  CHECK(write_head(out, r, opts) == Framing::ContentLength);
  CHECK_EQ(out,
           "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/plain; charset=utf-8\r\n"
           "Server: httpd-cpp\r\n"
           "Date: Wed, 23 Sep 2026 10:00:00 GMT\r\n"
           "Content-Length: 5\r\n"
           "\r\n");
}

TEST(handler_cannot_override_framing_headers) {
  Response r = Response::text(200, "abc");
  r.headers.set("Content-Length", "999");
  r.headers.set("Transfer-Encoding", "chunked");
  std::string out;
  write_head(out, r, HeadOptions{});
  CHECK(out.find("Content-Length: 3\r\n") != std::string::npos);
  CHECK(out.find("999") == std::string::npos);
  CHECK(out.find("chunked") == std::string::npos);
}

TEST(connection_header_reflects_keep_alive) {
  Response r = Response::text(404, "");
  std::string out;
  HeadOptions opts;
  opts.keep_alive = false;
  write_head(out, r, opts);
  CHECK(out.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
  CHECK(out.find("Connection: close\r\n") != std::string::npos);

  out.clear();
  opts.keep_alive = true;
  opts.http10_client = true;
  write_head(out, r, opts);
  CHECK(out.find("Connection: keep-alive\r\n") != std::string::npos);
}

TEST(no_body_framing_for_204) {
  Response r;
  r.status = 204;
  std::string out;
  CHECK(write_head(out, r, HeadOptions{}) == Framing::None);
  CHECK(out.find("Content-Length") == std::string::npos);
}

TEST(streaming_framing_depends_on_length_and_client) {
  Response r;
  r.producer = [](std::string&, Headers&) { return false; };
  std::string out;
  CHECK(write_head(out, r, HeadOptions{}) == Framing::Chunked);
  CHECK(out.find("Transfer-Encoding: chunked\r\n") != std::string::npos);

  out.clear();
  HeadOptions old;
  old.http10_client = true;
  CHECK(write_head(out, r, old) == Framing::CloseDelimited);
  CHECK(out.find("Connection: close\r\n") != std::string::npos);

  out.clear();
  r.stream_length = 42;
  CHECK(write_head(out, r, HeadOptions{}) == Framing::ContentLength);
  CHECK(out.find("Content-Length: 42\r\n") != std::string::npos);
}

TEST(chunk_encoding) {
  std::string out;
  append_chunk(out, "Wiki");
  append_chunk(out, "");  // must not emit a terminating zero-size chunk
  append_chunk(out, std::string(26, 'x'));
  Headers trailers;
  trailers.set("X-Sum", "1");
  append_last_chunk(out, trailers);
  CHECK_EQ(out, "4\r\nWiki\r\n1a\r\n" + std::string(26, 'x') + "\r\n0\r\nX-Sum: 1\r\n\r\n");
}

TEST(crc32_matches_reference_vectors) {
  CHECK_EQ(crc32(""), 0u);
  CHECK_EQ(crc32("123456789"), 0xCBF43926u);  // the standard check value
  CHECK_EQ(crc32("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
  // Streaming in pieces gives the same result as one call.
  CHECK_EQ(crc32("56789", crc32("1234")), 0xCBF43926u);
}

TEST(byte_buffer_compacts_and_grows) {
  ByteBuffer b;
  char* p = b.prepare(4);
  std::memcpy(p, "abcd", 4);
  b.commit(4);
  b.consume(2);
  CHECK_EQ(b.readable(), "cd");
  p = b.prepare(10000);  // forces growth, keeps unread bytes
  CHECK(b.writable() >= 10000);
  CHECK_EQ(b.readable(), "cd");
  std::memcpy(p, "ef", 2);
  b.commit(2);
  CHECK_EQ(b.readable(), "cdef");
  b.consume(4);
  CHECK(b.empty());
}

TEST(static_path_sanitizer) {
  std::string out;
  CHECK(StaticFiles::sanitize("css/site.css", out));
  CHECK_EQ(out, "css/site.css");
  CHECK(StaticFiles::sanitize("./a//b/./c.txt", out));
  CHECK_EQ(out, "a/b/c.txt");
  CHECK(StaticFiles::sanitize("hello%20world.txt", out));
  CHECK_EQ(out, "hello world.txt");
  CHECK(StaticFiles::sanitize("", out));
  CHECK_EQ(out, "");
  CHECK(!StaticFiles::sanitize("../etc/passwd", out));
  CHECK(!StaticFiles::sanitize("a/../../b", out));
  CHECK(!StaticFiles::sanitize("%2e%2e/secret", out));
  CHECK(!StaticFiles::sanitize("a%2f..%2fb/../../x", out));
  CHECK(!StaticFiles::sanitize("bad%zz", out));
  CHECK(!StaticFiles::sanitize("trunc%2", out));
  CHECK(!StaticFiles::sanitize("nul%00byte", out));
  CHECK(!StaticFiles::sanitize("win\\path", out));
}
