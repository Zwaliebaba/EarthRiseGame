# MS Native Unit Test projects (CppUnitTestFramework) → CTest via vstest.console.
if(NOT BUILD_TESTING OR NOT NEURON_MSTEST_AVAILABLE)
  return()
endif()

add_subdirectory(NeuronCoreTest)
add_subdirectory(NeuronClientTest)
add_subdirectory(NeuronRenderTest)
add_subdirectory(NeuronAudioTest)
add_subdirectory(ERHeadlessTest)
add_subdirectory(ERServerTest)
add_subdirectory(EarthRiseTest)
