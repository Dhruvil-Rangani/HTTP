# httpd-cpp - Linux only (epoll, sendfile, SO_REUSEPORT, openat2).
#
#   make              release build of httpd, loadgen and the test binary
#   make test         unit + end-to-end tests
#   make asan         tests under AddressSanitizer + UndefinedBehaviorSanitizer
#   make tsan         tests under ThreadSanitizer
#   make fuzz         differential parser fuzzing (built-in mutator, ASan/UBSan)
#   make fuzz-libfuzzer   same target under clang's libFuzzer
#   make bench        start httpd and hammer it with loadgen
#   make run          start httpd on :8080

CXX      ?= g++
OPT      ?= -O2 -g
SAN      ?=
BUILD    ?= build

CXXFLAGS := -std=c++17 $(OPT) $(SAN) -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor \
            -Wcast-align -Wformat=2 -Wimplicit-fallthrough -pthread -Isrc -MMD -MP
LDFLAGS  := $(SAN) -pthread

CORE_SRCS := src/http/headers.cpp src/http/request_parser.cpp src/http/response.cpp \
             src/obs/metrics.cpp src/server/router.cpp src/server/server.cpp \
             src/app/routes.cpp src/app/static_files.cpp src/util/crc32.cpp
TEST_SRCS := tests/test_main.cpp tests/test_request_parser.cpp tests/test_response.cpp tests/test_server.cpp

CORE_OBJS := $(CORE_SRCS:%.cpp=$(BUILD)/%.o)
TEST_OBJS := $(TEST_SRCS:%.cpp=$(BUILD)/%.o)
ALL_OBJS  := $(CORE_OBJS) $(TEST_OBJS) $(BUILD)/src/main.o $(BUILD)/tools/loadgen.o

.PHONY: all test asan tsan fuzz fuzz-libfuzzer bench run clean

all: $(BUILD)/httpd $(BUILD)/loadgen $(BUILD)/httpd_tests

$(BUILD)/libhttpd.a: $(CORE_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/httpd: $(BUILD)/src/main.o $(BUILD)/libhttpd.a
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/httpd_tests: $(TEST_OBJS) $(BUILD)/libhttpd.a
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/loadgen: $(BUILD)/tools/loadgen.o
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(BUILD)/httpd_tests
	./$(BUILD)/httpd_tests

asan:
	$(MAKE) BUILD=build/asan OPT="-O1 -g" SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

# TSan's shadow memory layout breaks with the 32-bit mmap ASLR of recent
# kernels ("unexpected memory mapping"), so run the binary with ASLR disabled.
tsan:
	$(MAKE) BUILD=build/tsan OPT="-O1 -g" SAN="-fsanitize=thread" build/tsan/httpd_tests
	setarch "$$(uname -m)" -R ./build/tsan/httpd_tests

FUZZ_ITERS ?= 300000
$(BUILD)/fuzz/fuzz_standalone: fuzz/fuzz_request_parser.cpp src/http/request_parser.cpp src/http/headers.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O1 -g -Isrc -DHTTPD_FUZZ_STANDALONE -fsanitize=address,undefined \
	    -fno-sanitize-recover=all $^ -o $@

fuzz: $(BUILD)/fuzz/fuzz_standalone
	./$< $(FUZZ_ITERS)

fuzz-libfuzzer:
	@mkdir -p $(BUILD)/fuzz fuzz/corpus
	clang++ -std=c++17 -O1 -g -Isrc -fsanitize=fuzzer,address,undefined fuzz/fuzz_request_parser.cpp \
	    src/http/request_parser.cpp src/http/headers.cpp -o $(BUILD)/fuzz/fuzz_libfuzzer
	./$(BUILD)/fuzz/fuzz_libfuzzer -max_total_time=60 fuzz/corpus

bench: $(BUILD)/httpd $(BUILD)/loadgen
	./scripts/bench.sh

run: $(BUILD)/httpd
	./$(BUILD)/httpd --port 8080 --root public

clean:
	rm -rf build

-include $(ALL_OBJS:.o=.d)
