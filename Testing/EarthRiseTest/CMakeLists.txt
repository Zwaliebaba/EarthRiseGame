# EarthRiseTest covers device-free client logic (StringTable) from the EarthRise app.
neuron_add_mstest(EarthRiseTest
  SOURCES EarthRiseTest.cpp
  INCLUDES ${CMAKE_SOURCE_DIR}/EarthRise ${CMAKE_SOURCE_DIR}/NeuronCore
  LIBS NeuronCore)
