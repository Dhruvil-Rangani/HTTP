#pragma once

// Minimal self-registering test framework: no dependencies, one binary.
//
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); REQUIRE(cond); }
//
// CHECK* record a failure and continue; REQUIRE aborts the current test.

#include <cstdio>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Register {
  Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct Abort {};

inline int& current_failures() {
  static int n = 0;
  return n;
}

inline void fail(const char* file, int line, const std::string& what) {
  ++current_failures();
  std::fprintf(stderr, "    %s:%d: %s\n", file, line, what.c_str());
}

int run_all(int argc, char** argv);

}  // namespace testing

#define TEST(name)                                                   \
  static void name();                                                \
  static const ::testing::Register register_##name(#name, &name);    \
  static void name()

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK(" #cond ")");   \
  } while (0)

#define REQUIRE(cond)                                                        \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ::testing::fail(__FILE__, __LINE__, "REQUIRE(" #cond ")");            \
      throw ::testing::Abort{};                                              \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    const auto check_lhs_ = (a);                                                           \
    const auto check_rhs_ = (b);                                                           \
    if (!(check_lhs_ == check_rhs_)) {                                                     \
      std::ostringstream check_os_;                                                        \
      check_os_ << "CHECK_EQ(" #a ", " #b ")\n      lhs: " << check_lhs_                  \
                << "\n      rhs: " << check_rhs_;                                          \
      ::testing::fail(__FILE__, __LINE__, check_os_.str());                                \
    }                                                                                      \
  } while (0)
