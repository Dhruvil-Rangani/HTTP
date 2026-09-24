// Fuzz target for the incremental request parser.
//
// Besides "no crash / no sanitizer report", it checks a differential property:
// the sequence of parsed requests (and the final error, if any) must be
// identical whether the input arrives in one piece, one byte at a time, or in
// fuzzer-chosen chunk sizes. A parser that only works for "nice" TCP segment
// boundaries is exactly the kind of bug that hides until production.
//
// Builds two ways:
//   clang++ -fsanitize=fuzzer,address ...        -> libFuzzer (make fuzz-libfuzzer)
//   g++ -DHTTPD_FUZZ_STANDALONE -fsanitize=...   -> built-in random mutator (make fuzz)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "http/request_parser.hpp"

using namespace httpd;

namespace {

struct Outcome {
  std::vector<std::string> requests;  // a digest of each parsed request
  int final_status = 0;               // 0 = no error, else the HTTP status
  bool operator==(const Outcome& o) const { return requests == o.requests && final_status == o.final_status; }
};

std::string digest(const Request& r) {
  std::string d = r.method + ' ' + r.target + ' ' + std::to_string(r.version_minor) + '|';
  for (const auto& [k, v] : r.headers) d += k + ':' + v + '|';
  d += std::to_string(r.body.size()) + ':' + r.body + '|';
  for (const auto& [k, v] : r.trailers) d += k + ':' + v + '|';
  return d;
}

Outcome run(std::string_view input, std::size_t step) {
  ParserLimits lim;
  lim.max_request_line = 512;
  lim.max_header_bytes = 2048;
  lim.max_header_count = 32;
  lim.max_body_bytes = 4096;
  lim.max_chunk_line = 64;

  RequestParser p(lim);
  Outcome out;
  std::string buf;
  std::size_t pos = 0;
  while (pos < input.size()) {
    std::size_t n = std::min(step, input.size() - pos);
    buf.append(input.substr(pos, n));
    pos += n;
    for (;;) {
      std::size_t used = p.feed(buf);
      if (used > buf.size()) __builtin_trap();
      buf.erase(0, used);
      if (p.failed()) {
        out.final_status = status_for(p.error());
        return out;
      }
      if (!p.done()) break;
      Request r = p.take();
      if (r.body.size() > lim.max_body_bytes) __builtin_trap();
      out.requests.push_back(digest(r));
    }
  }
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 1) return 0;
  const std::size_t step = data[0] % 17 + 1;
  std::string_view input(reinterpret_cast<const char*>(data + 1), size - 1);

  Outcome whole = run(input, input.size() ? input.size() : 1);
  Outcome bytewise = run(input, 1);
  Outcome chunked = run(input, step);
  if (!(whole == bytewise) || !(whole == chunked)) {
    std::fprintf(stderr, "parser result depends on input segmentation (step=%zu)\n", step);
    __builtin_trap();
  }
  return 0;
}

#ifdef HTTPD_FUZZ_STANDALONE
// A dependency-free mutational fuzzer for toolchains without libFuzzer.
namespace {

std::uint64_t rng_state = 0x9E3779B97F4A7C15ull;
std::uint64_t next_random() {  // xorshift64*
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return rng_state * 0x2545F4914F6CDD1Dull;
}
std::size_t pick(std::size_t n) { return n ? static_cast<std::size_t>(next_random() % n) : 0; }

const char* const kSeeds[] = {
    "GET / HTTP/1.1\r\nHost: localhost:42069\r\nUser-Agent: curl/7.81.0\r\nAccept: */*\r\n\r\n",
    "POST /submit HTTP/1.1\r\nHost: x\r\nContent-Length: 13\r\n\r\nhello world!\n",
    "POST /u HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5;a=b\r\npedia\r\n0\r\nT: 1\r\n\r\n",
    "GET /a?b=c HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /b HTTP/1.1\r\nHost: y\r\n\r\n",
    "\r\nOPTIONS * HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 2, 2\r\n\r\nok",
};
const char* const kTokens[] = {"\r\n", "\r\n\r\n", ":", " ", "0\r\n\r\n", "Content-Length: ", "Transfer-Encoding: chunked",
                               "HTTP/1.1", "ffffffff", "\n", "\r", "\t", "%", ";", ","};

std::string mutate(std::string s) {
  const int rounds = 1 + static_cast<int>(pick(8));
  for (int i = 0; i < rounds; ++i) {
    switch (pick(6)) {
      case 0:  // flip a byte
        if (!s.empty()) s[pick(s.size())] = static_cast<char>(next_random());
        break;
      case 1:  // delete a range
        if (!s.empty()) {
          std::size_t at = pick(s.size());
          s.erase(at, pick(16) + 1);
        }
        break;
      case 2:  // insert a protocol token
        s.insert(pick(s.size() + 1), kTokens[pick(sizeof kTokens / sizeof *kTokens)]);
        break;
      case 3:  // duplicate a range
        if (!s.empty()) {
          std::size_t at = pick(s.size());
          s.insert(pick(s.size() + 1), s.substr(at, pick(32) + 1));
        }
        break;
      case 4:  // splice with another seed
        s += kSeeds[pick(sizeof kSeeds / sizeof *kSeeds)];
        break;
      default:  // truncate
        s.resize(pick(s.size() + 1));
        break;
    }
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  const long iterations = argc > 1 ? std::atol(argv[1]) : 200000;
  if (argc > 2) rng_state = std::strtoull(argv[2], nullptr, 10) | 1;
  for (long i = 0; i < iterations; ++i) {
    std::string input = mutate(kSeeds[pick(sizeof kSeeds / sizeof *kSeeds)]);
    input.insert(input.begin(), static_cast<char>(next_random()));  // byte 0 selects the chunk size
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
  }
  std::printf("fuzz: %ld iterations, no crashes, parser is segmentation-independent\n", iterations);
  return 0;
}
#endif
