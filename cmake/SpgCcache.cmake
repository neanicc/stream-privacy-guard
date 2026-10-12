# Compiler cache. Prefers the pinned ccache from deps/manifest.json, then PATH.
# Ninja and Makefile generators use a compiler launcher; the Visual Studio
# generator uses ccache masquerading as cl.exe, as described on the ccache wiki
# (https://github.com/ccache/ccache/wiki/MS-Visual-Studio).

if(NOT SPG_USE_CCACHE)
  return()
endif()

spg_dep_locate(ccache _spg_ccache_dir _spg_ccache_version)
find_program(SPG_CCACHE_PROGRAM ccache HINTS ${_spg_ccache_dir} DOC "ccache executable")
if(NOT SPG_CCACHE_PROGRAM)
  message(STATUS "ccache: not found (run scripts/ci/run.sh setup); building without it")
  return()
endif()

if(CMAKE_GENERATOR MATCHES "Visual Studio")
  file(COPY_FILE "${SPG_CCACHE_PROGRAM}" "${CMAKE_BINARY_DIR}/ccache-msvc/cl.exe" ONLY_IF_DIFFERENT)
  # /Zi writes a shared PDB, which ccache cannot cache; embed debug info instead.
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
  set(CMAKE_VS_GLOBALS
    "CLToolExe=cl.exe"
    "CLToolPath=${CMAKE_BINARY_DIR}/ccache-msvc"
    "UseMultiToolTask=true")
else()
  set(CMAKE_CXX_COMPILER_LAUNCHER "${SPG_CCACHE_PROGRAM}")
endif()
message(STATUS "ccache: ${SPG_CCACHE_PROGRAM}")
