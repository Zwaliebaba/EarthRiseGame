# AddMsTest.cmake — MS Native Unit Test (CppUnitTestFramework) support.
#
# Builds each Testing/* project as a DLL against the VS CppUnitTest framework and
# registers it with CTest via vstest.console.exe. Windows/VS-only by nature; the
# whole thing no-ops cleanly if the framework/runner can't be found (e.g. a build
# outside a VS dev environment), so the rest of the build still works.

set(NEURON_MSTEST_AVAILABLE OFF)
if(DEFINED ENV{VSINSTALLDIR})
  file(TO_CMAKE_PATH "$ENV{VSINSTALLDIR}" _vsroot)
  set(NEURON_CPPUNIT_INC "${_vsroot}/VC/Auxiliary/VS/UnitTest/include")
  # CppUnitTest.h auto-links via #pragma comment(lib, "x64\\...Framework.lib"),
  # so the search path is .../UnitTest/lib (the parent of x64/), not lib/x64.
  set(NEURON_CPPUNIT_LIB "${_vsroot}/VC/Auxiliary/VS/UnitTest/lib")
  find_program(VSTEST_CONSOLE NAMES vstest.console.exe
    HINTS "${_vsroot}/Common7/IDE/Extensions/TestPlatform"
          "${_vsroot}/Common7/IDE/CommonExtensions/Microsoft/TestWindow")
  if(EXISTS "${NEURON_CPPUNIT_INC}/CppUnitTest.h" AND VSTEST_CONSOLE)
    set(NEURON_MSTEST_AVAILABLE ON)
    message(STATUS "EarthRise: MS unit tests enabled (vstest at ${VSTEST_CONSOLE})")
  endif()
endif()
if(NOT NEURON_MSTEST_AVAILABLE)
  message(STATUS "EarthRise: CppUnitTest framework / vstest.console not found — MS unit tests skipped.")
endif()

# neuron_add_mstest(<Name> SOURCES ... [INCLUDES ...] [LIBS ...])
#   Sources/includes are relative to the calling CMakeLists' directory (so the
#   per-test pch.h / CppUnitTest.h includes resolve). Registers a CTest test that
#   runs the DLL under vstest.console.
function(neuron_add_mstest NAME)
  cmake_parse_arguments(A "" "" "SOURCES;INCLUDES;LIBS" ${ARGN})
  add_library(${NAME} SHARED ${A_SOURCES})
  target_include_directories(${NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} "${NEURON_CPPUNIT_INC}" ${A_INCLUDES})
  # The framework lib itself is auto-linked by CppUnitTest.h's #pragma comment(lib);
  # we only need its search directory on the path, plus the per-test LIBS.
  target_link_directories(${NAME} PRIVATE "${NEURON_CPPUNIT_LIB}")
  target_link_libraries(${NAME} PRIVATE ${A_LIBS})
  add_test(NAME ${NAME}
    COMMAND "${VSTEST_CONSOLE}" "$<TARGET_FILE:${NAME}>" /Platform:x64)
endfunction()
