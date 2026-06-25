# NeuronCore — shared engine core (was NeuronCore.vcxitems, now a static library).
add_library(NeuronCore STATIC
  CngCrypto.cpp
  NeuronCore.cpp
  SimComponents.cpp
  WinsockSocket.cpp
)

# Headers live in this directory; consumers include them by bare name.
target_include_directories(NeuronCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_precompile_headers(NeuronCore PRIVATE pch.h)

# Winsock (WinsockSocket) + CNG/BCrypt (CngCrypto, Pbkdf2). PUBLIC so every
# consumer of NeuronCore links them transitively.
target_link_libraries(NeuronCore PUBLIC ws2_32 bcrypt)
