# ERServer — Win32 console game server (IOCP UDP + ODBC persistence).
#
# The old project pulled NeuronCore in via shared-items and compiled the platform
# TUs directly (because the lib was a Windows-Store target). NeuronCore is now a
# plain static library, so ERServer simply links it. ERSERVER/ERHEADLESS are not
# referenced inside NeuronCore headers, so this is behavior-identical.
add_executable(ERServer
  ERServer.cpp
  IocpUdpListener.cpp
  persist/PersistConfig.cpp
  persist/OdbcConnection.cpp
  persist/OdbcConnectionPool.cpp
  persist/AccountStore.cpp
  persist/EconomyStore.cpp
  persist/WriteBehindStore.cpp
  persist/SimSnapshotStore.cpp
  persist/PersistenceThread.cpp
)

# Headers are included as "persist/X.h" relative to the project root.
target_include_directories(ERServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ERServer PRIVATE ERSERVER WIN32 _WINDOWS)
target_precompile_headers(ERServer PRIVATE pch.h)

# NeuronCore brings ws2_32 + bcrypt transitively; ERServer adds the ODBC libs.
target_link_libraries(ERServer PRIVATE NeuronCore odbc32 odbccp32)
