# Render tests cover the header-only parsers (CmoParse/DdsParse/FontAtlasLayout/
# UiLayout) — no D3D12, so this does not link NeuronRender.
neuron_add_mstest(NeuronRenderTest
  SOURCES NeuronRenderTest.cpp
  INCLUDES ${CMAKE_SOURCE_DIR}/NeuronRender ${CMAKE_SOURCE_DIR}/NeuronCore
  LIBS NeuronCore)
