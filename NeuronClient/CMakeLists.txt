# NeuronClient — client session / replication / interpolation (static library).
#
# The old project pulled NeuronCore in via a shared-items <Import>; here it links
# the NeuronCore static library instead (same code, expressed as a dependency).
add_library(NeuronClient STATIC
  NeuronClient.cpp
)

target_include_directories(NeuronClient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_precompile_headers(NeuronClient PRIVATE pch.h)

target_link_libraries(NeuronClient PUBLIC NeuronCore)
