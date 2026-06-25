# Audio tests cover header-only math/parsers (SpatialMath/WavParse) — no XAudio2.
neuron_add_mstest(NeuronAudioTest
  SOURCES NeuronAudioTest.cpp
  INCLUDES ${CMAKE_SOURCE_DIR}/NeuronAudio ${CMAKE_SOURCE_DIR}/NeuronCore
  LIBS NeuronCore)
