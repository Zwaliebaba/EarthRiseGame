# ERHeadless — Win32 console host that runs scripted bot clients (no rendering).
add_executable(ERHeadless ERHeadless.cpp)

target_compile_definitions(ERHeadless PRIVATE ERHEADLESS WIN32 _WINDOWS)
target_precompile_headers(ERHeadless PRIVATE pch.h)

# NeuronClient pulls in NeuronCore (and ws2_32 + bcrypt) transitively.
target_link_libraries(ERHeadless PRIVATE NeuronClient)
