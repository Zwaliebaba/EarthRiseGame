cmake_minimum_required(VERSION 3.21)

# vcpkg manifest features are honoured via the toolchain (see CMakePresets.json).
project(EarthRise LANGUAGES CXX)

# ── Build-wide options ───────────────────────────────────────────────────────
option(EARTHRISE_USE_PIX "Enable WinPixEventRuntime GPU markers (requires the vcpkg 'pix' feature)" OFF)

if(NOT MSVC)
  message(FATAL_ERROR "EarthRise currently builds with MSVC only (Windows).")
endif()

# Multi-config generators (Ninja Multi-Config / VS) drive Debug/Release; nothing to set here.

# CTest support (defines BUILD_TESTING, default ON, and calls enable_testing()).
include(CTest)

# ── Global compiler settings (mirrors the former .vcxproj ItemDefinitionGroups) ──
add_compile_options(
  /W4          # WarningLevel Level4
  /bigobj      # AdditionalOptions /bigobj
  /permissive- # standards conformance
  /Zc:__cplusplus
  /std:c++latest   # LanguageStandard stdcpplatest
  # NB: no /MP — Ninja parallelizes per-file already, and /MP races on the
  # shared PCH under Ninja (intermittent C1085 "cannot write precompiled header").
)
add_compile_definitions(
  UNICODE _UNICODE          # CharacterSet Unicode
  WIN32_LEAN_AND_MEAN
)

# Default runtime library = /MDd (Debug) and /MD (Release) — matches the old projects.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Put all binaries in a single per-config dir so the client can find GameData/ later.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# ── HLSL → header shader codegen (dxc) ───────────────────────────────────────
include(${CMAKE_SOURCE_DIR}/cmake/CompileHlsl.cmake)

# ── MS Native Unit Test (CppUnitTestFramework) support ───────────────────────
include(${CMAKE_SOURCE_DIR}/cmake/AddMsTest.cmake)

# ── Libraries (Phase 1) ──────────────────────────────────────────────────────
add_subdirectory(NeuronCore)
add_subdirectory(NeuronAudio)
add_subdirectory(NeuronClient)
add_subdirectory(NeuronRender)

# ── Executables: server, headless host, tools (Phase 2) ──────────────────────
add_subdirectory(ERServer)
add_subdirectory(ERHeadless)
add_subdirectory(NeuronTools)
add_subdirectory(EarthRise)

# ── MS Native Unit Test projects (Phase 3) ───────────────────────────────────
add_subdirectory(Testing)
