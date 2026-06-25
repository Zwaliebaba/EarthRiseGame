neuron_add_mstest(NeuronClientTest
  SOURCES NeuronClientTest.cpp
  INCLUDES ${CMAKE_SOURCE_DIR}/NeuronClient ${CMAKE_SOURCE_DIR}/NeuronCore
  LIBS NeuronCore)
