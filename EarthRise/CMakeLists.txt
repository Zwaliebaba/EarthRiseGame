# EarthRise — the game client (Win32 desktop, D3D12). Was a UWP/AppContainer
# C++/WinRT app; now a plain Win32 GUI executable.
add_executable(EarthRise WIN32
  App.cpp
)

target_include_directories(EarthRise PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_precompile_headers(EarthRise PRIVATE pch.h)

# NeuronRender (D3D12 + dxgi) and NeuronAudio (XAudio2) are linked; NeuronClient is
# header-only here but linked for its include path + NeuronCore (ws2_32/bcrypt).
target_link_libraries(EarthRise PRIVATE NeuronClient NeuronRender NeuronAudio)

# Stage GameData/ next to the executable so the client resolves assets at runtime
# (App.cpp AssetRoot() = <exe dir>/GameData). copy_directory_if_different only
# touches changed files, so incremental builds stay fast.
add_custom_command(TARGET EarthRise POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
          "${CMAKE_SOURCE_DIR}/GameData" "$<TARGET_FILE_DIR:EarthRise>/GameData"
  COMMENT "Staging GameData/ next to EarthRise.exe"
  VERBATIM)
