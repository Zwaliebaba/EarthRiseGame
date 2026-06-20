# Linux build for the platform-independent parser tests (masterplan §16.2).
# These exercise the same WAV/DDS/CMO/Font parser logic the Windows MSTest
# projects use, so CI catches parser regressions without a Windows build.
#
#   make        # build + run the tests
#   make build  # build only
#   make clean

CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Wpedantic -O2 -g

SRCS := main.cpp WavParseTests.cpp DdsParseTests.cpp FontAtlasTests.cpp CmoParseTests.cpp
BIN := testrunner

.PHONY: all build run clean
all: run

build: $(BIN)

# The parser headers under test live in their owning libraries (NeuronAudio /
# NeuronRender), not in NeuronTools — nothing depends on NeuronTools.
PARSER_HEADERS := ../../NeuronAudio/WavParse.h ../../NeuronRender/DdsParse.h \
                  ../../NeuronRender/CmoParse.h ../../NeuronRender/FontAtlasLayout.h

$(BIN): $(SRCS) $(wildcard *.h) $(PARSER_HEADERS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(BIN)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
