# NeuronTools — build-time CLIs (not shipped with the game).
#   datacook  — text universe source -> packed binary
#   datacheck — referential-integrity gate for universe source (CI)
# Both share header-only helpers (ToolCommon.h / UniverseSource.h) and use the
# NeuronCore universe codec/validator.
foreach(_tool datacook datacheck)
  add_executable(${_tool} datacook/${_tool}.cpp)
  target_include_directories(${_tool} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/datacook)
  target_link_libraries(${_tool} PRIVATE NeuronCore)
endforeach()

# ── Portable test suite (custom ER_TEST harness) → CTest ─────────────────────
# One console exe over all testrunner/*.cpp. Most tested code is header-only
# (parsers, sim logic, client controllers); only NeuronCore has compiled TUs, so
# that is the sole link dependency — the rest are include paths. Uses the real
# Windows SDK <DirectXMath.h> (the testrunner/dxmath_shim was for the dropped
# Linux path, so it is intentionally NOT on the include path).
if(BUILD_TESTING)
  file(GLOB _testrunner_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/testrunner/*.cpp")
  add_executable(neuron_tests ${_testrunner_srcs})
  target_include_directories(neuron_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/testrunner
    ${CMAKE_SOURCE_DIR}/NeuronClient
    ${CMAKE_SOURCE_DIR}/NeuronRender
    ${CMAKE_SOURCE_DIR}/NeuronAudio
  )
  target_link_libraries(neuron_tests PRIVATE NeuronCore)
  add_test(NAME neuron_tests COMMAND neuron_tests)
endif()
