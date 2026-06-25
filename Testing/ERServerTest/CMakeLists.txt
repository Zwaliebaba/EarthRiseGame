# ERServerTest compiles the persistence TUs it exercises directly (they live in
# the ERServer exe, not a library), and links NeuronCore for CngCrypto + ODBC.
neuron_add_mstest(ERServerTest
  SOURCES
    ERServerTest.cpp
    ${CMAKE_SOURCE_DIR}/ERServer/persist/OdbcConnection.cpp
    ${CMAKE_SOURCE_DIR}/ERServer/persist/PersistConfig.cpp
  INCLUDES ${CMAKE_SOURCE_DIR}/ERServer ${CMAKE_SOURCE_DIR}/NeuronCore
  LIBS NeuronCore odbc32 odbccp32)
