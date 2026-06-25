# CompileHlsl.cmake — reproduce the old NeuronRender <FxCompile> shader codegen.
#
# Each HLSL is compiled by dxc (Shader Model 6.7, entry "main") into a C header
# that declares a byte array named g_p<Stem>, exactly as the .vcxproj did
# (VariableName=g_p%(Filename), HeaderFileOutput=CompiledShaders\%(Filename).h).
# Sources include them as "CompiledShaders/<Stem>.h"; we emit into a build dir and
# put that dir on the target's include path, so nothing lands in the source tree.

# Locate dxc — needed for SM 6.x (fxc only goes to SM 5.1).
file(GLOB _neuron_sdk_bins "C:/Program Files (x86)/Windows Kits/10/bin/*/x64")
find_program(DXC_EXECUTABLE NAMES dxc HINTS ${_neuron_sdk_bins} ENV PATH)
if(NOT DXC_EXECUTABLE)
  message(FATAL_ERROR "dxc.exe not found (needed to compile HLSL Shader Model 6.7). "
                      "Install the Windows SDK or put dxc on PATH.")
endif()
message(STATUS "EarthRise: using dxc at ${DXC_EXECUTABLE}")

# neuron_compile_shaders(<out_headers_var> <src_dir> <out_dir> <stem:profile> ...)
#   profile is e.g. vs_6_7 / ps_6_7. Returns the generated header paths in <out_headers_var>.
function(neuron_compile_shaders OUT_HEADERS SRC_DIR OUT_DIR)
  set(_headers "")
  foreach(_spec IN LISTS ARGN)
    string(REPLACE ":" ";" _parts "${_spec}")
    list(GET _parts 0 _stem)
    list(GET _parts 1 _profile)
    set(_in  "${SRC_DIR}/${_stem}.hlsl")
    set(_out "${OUT_DIR}/${_stem}.h")
    add_custom_command(
      OUTPUT  "${_out}"
      COMMAND "${DXC_EXECUTABLE}" -nologo -T ${_profile} -E main
              -Fh "${_out}" -Vn g_p${_stem} "${_in}"
      DEPENDS "${_in}"
      COMMENT "dxc ${_stem}.hlsl -> ${_stem}.h (${_profile})"
      VERBATIM)
    list(APPEND _headers "${_out}")
  endforeach()
  set(${OUT_HEADERS} "${_headers}" PARENT_SCOPE)
endfunction()
