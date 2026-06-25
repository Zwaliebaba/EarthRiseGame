neuron_add_mstest(ERHeadlessTest
  SOURCES ERHeadlessTest.cpp
  INCLUDES ${CMAKE_SOURCE_DIR}/NeuronClient ${CMAKE_SOURCE_DIR}/NeuronCore
  LIBS NeuronCore)
