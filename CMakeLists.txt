cmake_minimum_required(VERSION 3.16)
project(httpd_cpp LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "httpd-cpp targets Linux (epoll, sendfile, SO_REUSEPORT). On Windows use WSL.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(HTTPD_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")
if(HTTPD_SANITIZE)
  add_compile_options(-fsanitize=${HTTPD_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${HTTPD_SANITIZE})
endif()

find_package(Threads REQUIRED)

add_library(httpd_core STATIC
  src/http/headers.cpp
  src/http/request_parser.cpp
  src/http/response.cpp
  src/obs/metrics.cpp
  src/server/router.cpp
  src/server/server.cpp
  src/app/routes.cpp
  src/app/static_files.cpp
  src/util/crc32.cpp)
target_include_directories(httpd_core PUBLIC src)
target_compile_options(httpd_core PUBLIC -Wall -Wextra -Wpedantic -Wshadow)
target_link_libraries(httpd_core PUBLIC Threads::Threads)

add_executable(httpd src/main.cpp)
target_link_libraries(httpd PRIVATE httpd_core)

add_executable(loadgen tools/loadgen.cpp)
target_link_libraries(loadgen PRIVATE Threads::Threads)

add_executable(httpd_tests
  tests/test_main.cpp
  tests/test_request_parser.cpp
  tests/test_response.cpp
  tests/test_server.cpp)
target_link_libraries(httpd_tests PRIVATE httpd_core)

add_executable(fuzz_parser fuzz/fuzz_request_parser.cpp)
target_compile_definitions(fuzz_parser PRIVATE HTTPD_FUZZ_STANDALONE)
target_link_libraries(fuzz_parser PRIVATE httpd_core)

enable_testing()
add_test(NAME httpd_tests COMMAND httpd_tests)
add_test(NAME fuzz_smoke COMMAND fuzz_parser 20000)
