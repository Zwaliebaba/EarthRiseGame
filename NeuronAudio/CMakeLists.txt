# NeuronAudio — XAudio2 mixer / spatializer (static library).
add_library(NeuronAudio STATIC
  AudioEngine.cpp
  Mixer.cpp
  NeuronAudio.cpp
  Spatializer.cpp
  VoicePool.cpp
)

target_include_directories(NeuronAudio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_precompile_headers(NeuronAudio PRIVATE pch.h)

# Uses NeuronCore headers (math/types); XAudio2 for the engine.
target_link_libraries(NeuronAudio PUBLIC NeuronCore xaudio2)
