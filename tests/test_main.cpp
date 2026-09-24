#include <chrono>
#include <cstring>

#include "testing.hpp"

int testing::run_all(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int passed = 0;
  int failed = 0;
  for (const Case& c : registry()) {
    if (filter && std::strstr(c.name, filter) == nullptr) continue;
    current_failures() = 0;
    auto start = std::chrono::steady_clock::now();
    try {
      c.fn();
    } catch (const Abort&) {
    } catch (const std::exception& e) {
      fail(__FILE__, __LINE__, std::string("uncaught exception: ") + e.what());
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (current_failures() == 0) {
      ++passed;
      std::printf("[ PASS ] %s (%lld ms)\n", c.name, static_cast<long long>(ms));
    } else {
      ++failed;
      std::printf("[ FAIL ] %s\n", c.name);
    }
    std::fflush(stdout);
  }
  std::printf("\n%d passed, %d failed\n", passed, failed);
  return failed == 0 ? 0 : 1;
}

int main(int argc, char** argv) { return testing::run_all(argc, argv); }
