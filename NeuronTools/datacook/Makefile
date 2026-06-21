# Build the universe data tooling (masterplan §12.6). Linux-buildable like the
# testrunner; shares NeuronCore's data model/codec/validation and the dxmath
# shim so it compiles without a Windows DirectXMath.
#
#   make            # build datacook + datacheck
#   make check      # validate the authored datasets under Config/universe/
#   make clean

CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Wpedantic -O2 -g \
            -I../../NeuronCore -I../testrunner/dxmath_shim

HDRS := UniverseSource.h ToolCommon.h ../../NeuronCore/UniverseData.h

.PHONY: all check clean
all: datacook datacheck

datacook: datacook.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) datacook.cpp -o datacook

datacheck: datacheck.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) datacheck.cpp -o datacheck

# CI gate: every authored dataset must pass referential integrity.
check: datacheck
	./datacheck ../../Config/universe/*.universe

clean:
	rm -f datacook datacheck
