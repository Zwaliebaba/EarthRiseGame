# Linux build for the platform-independent parser tests (masterplan §16.2).
# These exercise the same WAV/DDS/CMO/Font parser logic the Windows MSTest
# projects use, so CI catches parser regressions without a Windows build.
#
#   make        # build + run the tests
#   make build  # build only
#   make clean

CXX ?= g++
# The sim integration tests pull in NeuronCore sim headers, which include
# <DirectXMath.h> for XMFLOAT3; dxmath_shim/ supplies a minimal stand-in so the
# platform-independent catalog/snapshot/ECS logic builds on Linux.
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Wpedantic -O2 -g -I../../NeuronCore -I../../NeuronClient -Idxmath_shim

SRCS := main.cpp WavParseTests.cpp DdsParseTests.cpp FontAtlasTests.cpp CmoParseTests.cpp \
        CmoAnimationTests.cpp ShapeCatalogTests.cpp UiLayoutTests.cpp StringTableTests.cpp \
        UniverseDataTests.cpp NavigationTests.cpp EconomyTests.cpp HarvestTests.cpp \
        FleetTests.cpp ClientFleetTests.cpp DeterminismTests.cpp InterestTests.cpp \
        SnapshotDeltaTests.cpp SnapshotSchedulerTests.cpp SnapshotJobsTests.cpp \
        TimeDilationTests.cpp TelemetryTests.cpp ConnectionTableTests.cpp \
        LoadHarnessTests.cpp CameraTests.cpp OnboardingTests.cpp PickingTests.cpp \
        HudOverlayTests.cpp
BIN := testrunner

.PHONY: all build run clean
all: run

build: $(BIN)

# The parser headers under test live in their owning libraries (NeuronAudio /
# NeuronRender), not in NeuronTools — nothing depends on NeuronTools.
PARSER_HEADERS := ../../NeuronAudio/WavParse.h ../../NeuronRender/DdsParse.h \
                  ../../NeuronRender/CmoParse.h ../../NeuronRender/FontAtlasLayout.h \
                  ../datacook/UniverseSource.h ../../NeuronCore/UniverseData.h

$(BIN): $(SRCS) $(wildcard *.h) $(PARSER_HEADERS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(BIN)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
