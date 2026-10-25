CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -march=native -flto -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -pthread
INCLUDES = -Iinclude

SRCS = src/main.cpp
HEADERS = $(wildcard include/slew/*.hpp include/slew/*/*.hpp)
TARGET = bin/slew

TEST_PARSER = bin/test_parser
TEST_CACHE = bin/test_cache
TEST_RING = bin/test_ring

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(TEST_PARSER): tests/unit/test_parser.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(TEST_CACHE): tests/unit/test_cache.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(TEST_RING): tests/unit/test_ring.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

tests: $(TEST_PARSER) $(TEST_CACHE) $(TEST_RING)

test: tests
	@echo "--- Running test_parser ---"
	@./$(TEST_PARSER)
	@echo "--- Running test_cache ---"
	@./$(TEST_CACHE)
	@echo "--- Running test_ring ---"
	@./$(TEST_RING)

clean:
	rm -rf bin

.PHONY: all tests test clean
