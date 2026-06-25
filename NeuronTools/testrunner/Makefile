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
        UniverseDataTests.cpp CombatDataTests.cpp CombatTests.cpp FittingTests.cpp \
        ProjectileTests.cpp EwarLogiTests.cpp CombatScenarioTests.cpp BalanceTests.cpp \
        NavigationTests.cpp EconomyTests.cpp HarvestTests.cpp \
        FleetTests.cpp ClientFleetTests.cpp DeterminismTests.cpp InterestTests.cpp \
        SnapshotDeltaTests.cpp SnapshotSchedulerTests.cpp SnapshotJobsTests.cpp \
        TimeDilationTests.cpp TelemetryTests.cpp ConnectionTableTests.cpp \
        LoadHarnessTests.cpp CameraTests.cpp OnboardingTests.cpp PickingTests.cpp \
        HudOverlayTests.cpp ReconnectTests.cpp ReconnectControllerTests.cpp \
        EconomyOutboxTests.cpp OutboxTests.cpp WarmRestartTests.cpp \
        WarmRestartCaptureTests.cpp Pbkdf2Tests.cpp PersistTelemetryTests.cpp \
        JsonParseTests.cpp ClientConfigTests.cpp ServerStatusTests.cpp
BIN := testrunner

.PHONY: all build run clean
all: run

build: $(BIN)

# Header prerequisites: rebuild when ANY header the tests pull in changes. Make can't
# see through #include, and this is a single-shot compile of all TUs (no .o/-MMD dep
# files), so list every directory on the include path plus the owning-library headers
# under test (NeuronAudio / NeuronRender / datacook). Without this, editing e.g.
# NeuronCore/ServerUniverse.h would NOT trigger a rebuild and `make` would silently run
# a stale binary. Wildcards so new headers are picked up automatically.
HEADERS := $(wildcard *.h) $(wildcard dxmath_shim/*.h) \
           $(wildcard ../../NeuronCore/*.h) $(wildcard ../../NeuronClient/*.h) \
           $(wildcard ../../NeuronAudio/*.h) $(wildcard ../../NeuronRender/*.h) \
           $(wildcard ../datacook/*.h)

$(BIN): $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(BIN)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
