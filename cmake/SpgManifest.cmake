# Reads deps/manifest.json and locates what scripts/fetch_deps.py unpacked into
# $SPG_CACHE_DIR/deps/<name>/<version>-<platform>/. The manifest is the single
# source of truth for versions and hashes; CMake never downloads these itself.

if(DEFINED ENV{SPG_CACHE_DIR} AND NOT "$ENV{SPG_CACHE_DIR}" STREQUAL "")
  set(_spg_cache_default "$ENV{SPG_CACHE_DIR}")
elseif(DEFINED ENV{HOME})
  set(_spg_cache_default "$ENV{HOME}/.cache/spg")
else()
  set(_spg_cache_default "$ENV{USERPROFILE}/.cache/spg")
endif()
set(SPG_CACHE_DIR "${_spg_cache_default}" CACHE PATH "Cache for pinned dependencies and models")
file(TO_CMAKE_PATH "${SPG_CACHE_DIR}" SPG_CACHE_DIR)

set(SPG_DEPS_MANIFEST "${PROJECT_SOURCE_DIR}/deps/manifest.json")
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPG_DEPS_MANIFEST}")
file(READ "${SPG_DEPS_MANIFEST}" _spg_manifest_json)

# Platform keys match scripts/fetch_deps.py host_platform().
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(SPG_PLATFORM "windows-x64")
elseif(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"
                  OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")))
  set(SPG_PLATFORM "macos-arm64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(SPG_PLATFORM "linux-x64")
else()
  set(SPG_PLATFORM "")
endif()

# spg_dep_locate(<name> <dir-var> <version-var>)
# Sets <dir-var> to the unpacked directory, or to "" when it is missing or was
# unpacked from an archive whose hash differs from the manifest.
function(spg_dep_locate name out_dir out_version)
  string(JSON _version GET "${_spg_manifest_json}" deps ${name} version)
  set(${out_version} "${_version}" PARENT_SCOPE)
  set(${out_dir} "" PARENT_SCOPE)
  if(SPG_PLATFORM STREQUAL "")
    return()
  endif()
  string(JSON _sha ERROR_VARIABLE _err GET "${_spg_manifest_json}" deps ${name} platforms ${SPG_PLATFORM} sha256)
  if(_err)
    return()
  endif()
  set(_dir "${SPG_CACHE_DIR}/deps/${name}/${_version}-${SPG_PLATFORM}")
  set(_marker "${_dir}/.spg-fetch.json")
  if(NOT EXISTS "${_marker}")
    return()
  endif()
  file(READ "${_marker}" _marker_json)
  string(JSON _marker_sha ERROR_VARIABLE _err GET "${_marker_json}" sha256)
  if(_err OR NOT _marker_sha STREQUAL _sha)
    message(WARNING "${name}: ${_dir} was unpacked from a different archive than "
                    "deps/manifest.json pins; rerun scripts/ci/run.sh setup.")
    return()
  endif()
  set(${out_dir} "${_dir}" PARENT_SCOPE)
endfunction()
