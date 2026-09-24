// Parser tests. Every well-formed message is fed at every possible chunk size,
// from one byte per read up to the whole message at once: TCP may split a
// request at any byte boundary.

#include <algorithm>
#include <string>
#include <vector>

#include "http/request_parser.hpp"
#include "testing.hpp"

using namespace httpd;

namespace {

struct Fed {
  RequestParser parser;
  std::string leftover;  // bytes not consumed when parsing stopped
};

// Emulates the server's input buffer: append each slice, feed the parser the
// whole unconsumed buffer, drop what it consumed.
Fed feed_in_chunks(std::string_view data, std::size_t step, ParserLimits limits = {}) {
  Fed f{RequestParser(limits), {}};
  std::string buf;
  std::size_t pos = 0;
  while (pos < data.size() && !f.parser.done() && !f.parser.failed()) {
    std::size_t n = std::min(step, data.size() - pos);
    buf.append(data.substr(pos, n));
    pos += n;
    buf.erase(0, f.parser.feed(buf));
  }
  f.leftover = buf + std::string(data.substr(pos));
  return f;
}

int error_status(std::string_view data, ParserLimits limits = {}) {
  Fed f = feed_in_chunks(data, data.size(), limits);
  return f.parser.failed() ? status_for(f.parser.error()) : 0;
}

const std::string kCurlGet =
    "GET / HTTP/1.1\r\nHost: localhost:42069\r\nUser-Agent: curl/7.81.0\r\nAccept: */*\r\n\r\n";

}  // namespace

TEST(parses_request_line_at_every_chunk_size) {
  for (std::size_t step = 1; step <= kCurlGet.size(); ++step) {
    Fed f = feed_in_chunks(kCurlGet, step);
    REQUIRE(f.parser.done());
    const Request& r = f.parser.request();
    CHECK_EQ(r.method, "GET");
    CHECK_EQ(r.target, "/");
    CHECK_EQ(r.version_major, 1);
    CHECK_EQ(r.version_minor, 1);
    CHECK(f.leftover.empty());
  }
}

TEST(parses_request_line_with_path) {
  const std::string msg = "GET /coffee HTTP/1.1\r\nHost: localhost:42069\r\n\r\n";
  for (std::size_t step = 1; step <= msg.size(); ++step) {
    Fed f = feed_in_chunks(msg, step);
    REQUIRE(f.parser.done());
    CHECK_EQ(f.parser.request().path, "/coffee");
  }
}

TEST(splits_query_string) {
  Fed f = feed_in_chunks("GET /search?q=epoll&page=2 HTTP/1.1\r\nHost: x\r\n\r\n", 7);
  REQUIRE(f.parser.done());
  CHECK_EQ(f.parser.request().path, "/search");
  CHECK_EQ(f.parser.request().query, "q=epoll&page=2");
}

TEST(accepts_absolute_form_target) {
  Fed f = feed_in_chunks("GET http://example.com/a/b?c HTTP/1.1\r\nHost: example.com\r\n\r\n", 5);
  REQUIRE(f.parser.done());
  CHECK_EQ(f.parser.request().path, "/a/b");
  CHECK_EQ(f.parser.request().query, "c");
}

TEST(parses_headers_case_insensitively) {
  for (std::size_t step = 1; step <= kCurlGet.size(); ++step) {
    Fed f = feed_in_chunks(kCurlGet, step);
    REQUIRE(f.parser.done());
    const Headers& h = f.parser.request().headers;
    CHECK_EQ(h.get("host").value_or(""), "localhost:42069");
    CHECK_EQ(h.get("USER-AGENT").value_or(""), "curl/7.81.0");
    CHECK_EQ(h.get("Accept").value_or(""), "*/*");
    CHECK_EQ(h.size(), 3u);
  }
}

TEST(trims_header_whitespace_and_folds_duplicates) {
  Fed f = feed_in_chunks(
      "GET / HTTP/1.1\r\nHost:   localhost:42069   \r\nX-Person: lane\r\nX-Person: prime\r\n\r\n", 3);
  REQUIRE(f.parser.done());
  CHECK_EQ(f.parser.request().headers.get("Host").value_or(""), "localhost:42069");
  CHECK_EQ(f.parser.request().headers.get("x-person").value_or(""), "lane, prime");
}

TEST(rejects_malformed_header_lines) {
  // Whitespace between field name and colon (RFC 9112 §5.1).
  CHECK_EQ(error_status("GET / HTTP/1.1\r\n       Host : localhost:42069       \r\n\r\n"), 400);
  CHECK_EQ(error_status("GET / HTTP/1.1\r\nHost : x\r\n\r\n"), 400);
  // Obsolete line folding.
  CHECK_EQ(error_status("GET / HTTP/1.1\r\nHost: x\r\nX-A: a\r\n  continued\r\n\r\n"), 400);
  // No colon, invalid name characters.
  CHECK_EQ(error_status("GET / HTTP/1.1\r\nHost: x\r\nnocolon\r\n\r\n"), 400);
  CHECK_EQ(error_status("GET / HTTP/1.1\r\nHost: x\r\nH@st: y\r\n\r\n"), 400);
  // Bare LF / NUL smuggled inside a value.
  CHECK_EQ(error_status("GET / HTTP/1.1\r\nHost: x\r\nX-A: a\nb\r\n\r\n"), 400);
  static const char kNul[] = "GET / HTTP/1.1\r\nHost: x\r\nX-A: a\0b\r\n\r\n";
  CHECK_EQ(error_status(std::string(kNul, sizeof kNul - 1)), 400);
}

TEST(waits_for_end_of_headers) {
  Fed f = feed_in_chunks("GET / HTTP/1.1\r\nHost: localhost:42069\r\n", 4);
  CHECK(!f.parser.done());
  CHECK(!f.parser.failed());
  CHECK(f.parser.started());
  CHECK(!f.parser.headers_complete());
}

TEST(rejects_bad_request_lines) {
  CHECK_EQ(error_status("/coffee HTTP/1.1\r\nHost: x\r\n\r\n"), 400);        // missing method
  CHECK_EQ(error_status("GET /coffee\r\nHost: x\r\n\r\n"), 400);             // missing version
  CHECK_EQ(error_status("GET  / HTTP/1.1\r\nHost: x\r\n\r\n"), 400);         // double space
  CHECK_EQ(error_status("GET / HTTP/1.1 extra\r\nHost: x\r\n\r\n"), 400);    // four parts
  CHECK_EQ(error_status("G(T / HTTP/1.1\r\nHost: x\r\n\r\n"), 400);          // method not a token
  CHECK_EQ(error_status("GET coffee HTTP/1.1\r\nHost: x\r\n\r\n"), 400);     // not origin-form
  CHECK_EQ(error_status("GET / HTTX/1.1\r\nHost: x\r\n\r\n"), 400);
  CHECK_EQ(error_status("GET / http/1.1\r\nHost: x\r\n\r\n"), 400);          // version is case-sensitive
  CHECK_EQ(error_status("GET / HTTP/1.10\r\nHost: x\r\n\r\n"), 400);
}

TEST(rejects_unsupported_versions_with_505) {
  CHECK_EQ(error_status("GET / HTTP/2.0\r\nHost: x\r\n\r\n"), 505);
  CHECK_EQ(error_status("GET / HTTP/1.2\r\nHost: x\r\n\r\n"), 505);
}

TEST(requires_exactly_one_host_in_http11) {
  CHECK_EQ(error_status("GET / HTTP/1.1\r\n\r\n"), 400);
  CHECK_EQ(error_status("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"), 400);
  Fed f = feed_in_chunks("GET / HTTP/1.0\r\n\r\n", 2);
  CHECK(f.parser.done());
}

TEST(ignores_leading_empty_lines) {
  Fed f = feed_in_chunks("\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n", 1);
  REQUIRE(f.parser.done());
  CHECK_EQ(f.parser.request().method, "GET");
}

TEST(reads_content_length_body_at_every_chunk_size) {
  const std::string msg =
      "POST /submit HTTP/1.1\r\nHost: localhost:42069\r\nContent-Length: 13\r\n\r\nhello world!\n";
  for (std::size_t step = 1; step <= msg.size(); ++step) {
    Fed f = feed_in_chunks(msg, step);
    REQUIRE(f.parser.done());
    CHECK_EQ(f.parser.request().body, "hello world!\n");
  }
}

TEST(waits_when_body_shorter_than_content_length) {
  Fed f = feed_in_chunks("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 20\r\n\r\npartial content", 4);
  CHECK(!f.parser.done());
  CHECK(!f.parser.failed());
  CHECK(f.parser.headers_complete());
}

TEST(decodes_chunked_body_with_extensions_and_trailers) {
  const std::string msg =
      "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
      "4\r\nWiki\r\n"
      "5;ext=\"1\"\r\npedia\r\n"
      "E\r\n in\r\n\r\nchunks.\r\n"
      "0\r\nX-Checksum: abc\r\n\r\n";
  for (std::size_t step = 1; step <= msg.size(); ++step) {
    Fed f = feed_in_chunks(msg, step);
    REQUIRE(f.parser.done());
    CHECK_EQ(f.parser.request().body, "Wikipedia in\r\n\r\nchunks.");
    CHECK_EQ(f.parser.request().trailers.get("x-checksum").value_or(""), "abc");
    CHECK(f.leftover.empty());
  }
}

TEST(rejects_bad_chunk_framing) {
  const std::string head = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
  CHECK_EQ(error_status(head + "zz\r\nabc\r\n0\r\n\r\n"), 400);
  CHECK_EQ(error_status(head + "3\r\nabcX\r\n0\r\n\r\n"), 400);  // no CRLF after data
  CHECK_EQ(error_status(head + "\r\n"), 400);                    // empty size
  CHECK_EQ(error_status(head + "11111111111111111\r\n"), 400);   // > 64 bits
}

TEST(guards_against_request_smuggling) {
  // CL + TE together: two servers could disagree on where the message ends.
  CHECK_EQ(error_status("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
           400);
  CHECK_EQ(error_status("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab"), 400);
  CHECK_EQ(error_status("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n"), 400);
  CHECK_EQ(error_status("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0x10\r\n\r\n"), 400);
  CHECK_EQ(error_status("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"), 400);
  CHECK_EQ(error_status("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n"), 501);
  // Identical repeated Content-Length values are allowed.
  Fed f = feed_in_chunks("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2, 2\r\n\r\nok", 1);
  REQUIRE(f.parser.done());
  CHECK_EQ(f.parser.request().body, "ok");
}

TEST(enforces_size_limits) {
  ParserLimits lim;
  lim.max_request_line = 64;
  lim.max_header_bytes = 128;
  lim.max_header_count = 3;
  lim.max_body_bytes = 10;
  CHECK_EQ(error_status("GET /" + std::string(100, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n", lim), 414);
  CHECK_EQ(error_status("GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + std::string(200, 'b') + "\r\n\r\n", lim), 431);
  CHECK_EQ(error_status("GET / HTTP/1.1\r\nHost: x\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", lim), 431);
  CHECK_EQ(error_status("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\n", lim), 413);
  CHECK_EQ(error_status("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                        "8\r\n12345678\r\n8\r\n12345678\r\n0\r\n\r\n",
                        lim),
           413);
  // The limit trips on a partial line too: a peer cannot make us buffer forever.
  Fed f = feed_in_chunks("GET /" + std::string(1000, 'a'), 1, lim);
  CHECK(f.parser.failed());
  CHECK_EQ(status_for(f.parser.error()), 414);
}

TEST(stops_at_request_boundary_for_pipelining) {
  const std::string a = "GET /first HTTP/1.1\r\nHost: x\r\n\r\n";
  const std::string b = "POST /second HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc";
  const std::string c = "GET /third HTTP/1.1\r\nHost: x\r\n\r\n";
  std::string buf = a + b + c;
  RequestParser p;
  std::vector<Request> got;
  while (!buf.empty()) {
    std::size_t used = p.feed(buf);
    buf.erase(0, used);
    REQUIRE(!p.failed());
    REQUIRE(p.done());
    got.push_back(p.take());
  }
  REQUIRE(got.size() == 3);
  CHECK_EQ(got[0].path, "/first");
  CHECK_EQ(got[1].path, "/second");
  CHECK_EQ(got[1].body, "abc");
  CHECK_EQ(got[2].path, "/third");
}

TEST(keep_alive_semantics) {
  auto parse = [](const std::string& msg) {
    RequestParser p;
    p.feed(msg);
    return p.take();
  };
  CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n").keep_alive());
  CHECK(!parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n").keep_alive());
  CHECK(!parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, Close\r\n\r\n").keep_alive());
  CHECK(!parse("GET / HTTP/1.0\r\n\r\n").keep_alive());
  CHECK(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").keep_alive());
}

TEST(flags_expect_continue_only_when_body_follows) {
  RequestParser p;
  p.feed("POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n");
  CHECK(p.headers_complete());
  CHECK(!p.done());
  CHECK(p.request().expect_continue);

  RequestParser q;
  q.feed("GET / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n");
  CHECK(q.done());
  CHECK(!q.request().expect_continue);
}

TEST(parser_is_reusable_after_take) {
  RequestParser p;
  p.feed("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nX-A: 1\r\n\r\nz");
  REQUIRE(p.done());
  Request first = p.take();
  CHECK_EQ(first.body, "z");
  CHECK(!p.started());
  p.feed("GET /again HTTP/1.1\r\nHost: y\r\n\r\n");
  REQUIRE(p.done());
  CHECK_EQ(p.request().path, "/again");
  CHECK(p.request().body.empty());
  CHECK(!p.request().headers.has("X-A"));
}

TEST(header_list_tokens) {
  Headers h;
  h.add("Connection", "keep-alive");
  h.add("connection", " Upgrade ");
  CHECK_EQ(h.get("CONNECTION").value_or(""), "keep-alive,  Upgrade ");
  CHECK(h.has_token("Connection", "upgrade"));
  CHECK(h.has_token("Connection", "KEEP-ALIVE"));
  CHECK(!h.has_token("Connection", "close"));
  h.set("Connection", "close");
  CHECK_EQ(h.get("connection").value_or(""), "close");
  CHECK(h.remove("CONNECTION"));
  CHECK(h.empty());
}
