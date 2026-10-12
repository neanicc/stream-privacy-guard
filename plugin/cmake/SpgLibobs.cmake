# Finds libobs for the plugin as the imported target OBS::libobs, using OBS's
# own CMake package (libobsConfig.cmake) from inputs pinned in deps/manifest.json:
#   linux-x64    official Ubuntu package, unpacked by scripts/fetch_deps.py
#                (obs-studio-ubuntu), plus SIMDe headers (simde)
#   macos-arm64, built from the OBS sources against obs-deps by
#   windows-x64  build-aux/build_libobs.py into $SPG_CACHE_DIR/deps/libobs/
# Sets SPG_LIBOBS_VERSION, SPG_LIBOBS_PREFIX and SPG_OBS_DEPS_DIR (macOS and
# Windows only: FFmpeg and the other runtime libraries libobs loads).

function(_spg_plugin_input_missing what)
  message(FATAL_ERROR "SPG_BUILD_PLUGIN: ${what} not found for ${SPG_PLATFORM}. "
                      "Run scripts/ci/run.sh setup (it fetches the pinned OBS inputs and builds libobs), "
                      "or configure with -DSPG_BUILD_PLUGIN=OFF.")
endfunction()

string(JSON SPG_LIBOBS_VERSION GET "${_spg_manifest_json}" deps obs-studio version)
set(SPG_OBS_DEPS_DIR "")

if(SPG_PLATFORM STREQUAL "linux-x64")
  spg_dep_locate(obs-studio-ubuntu SPG_LIBOBS_PREFIX _spg_deb_version)
  if(NOT SPG_LIBOBS_PREFIX)
    _spg_plugin_input_missing("obs-studio-ubuntu ${_spg_deb_version}")
  endif()
  spg_dep_locate(simde _spg_simde_dir _spg_simde_version)
  if(NOT _spg_simde_dir)
    _spg_plugin_input_missing("simde ${_spg_simde_version}")
  endif()
  # libobsConfig.cmake requires SIMDe; its finder accepts this cache variable.
  set(SIMDe_INCLUDE_DIR "${_spg_simde_dir}" CACHE PATH "SIMDe headers (deps/manifest.json)" FORCE)
  set(libobs_DIR "${SPG_LIBOBS_PREFIX}/lib/x86_64-linux-gnu/cmake/libobs" CACHE PATH "libobs CMake package (pinned)" FORCE)
elseif(SPG_PLATFORM STREQUAL "macos-arm64" OR SPG_PLATFORM STREQUAL "windows-x64")
  set(SPG_LIBOBS_PREFIX "${SPG_CACHE_DIR}/deps/libobs/${SPG_LIBOBS_VERSION}-${SPG_PLATFORM}")
  set(_spg_libobs_marker "${SPG_LIBOBS_PREFIX}/.spg-build.json")
  # Accept the cached libobs only when it was built from the inputs pinned now:
  # the same archives as deps/manifest.json and the same builder script and OBS
  # overrides (build_libobs.builder_digest()). Anything else needs setup.
  set(_spg_libobs_inputs obs-studio obs-deps)
  if(SPG_PLATFORM STREQUAL "windows-x64")
    list(APPEND _spg_libobs_inputs obs-deps-x86)
  endif()
  set(_spg_libobs_stale "")
  if(EXISTS "${_spg_libobs_marker}")
    file(READ "${_spg_libobs_marker}" _spg_libobs_build)
    foreach(_input IN LISTS _spg_libobs_inputs)
      string(JSON _want GET "${_spg_manifest_json}" deps ${_input} platforms ${SPG_PLATFORM} sha256)
      string(JSON _built ERROR_VARIABLE _err GET "${_spg_libobs_build}" inputs ${_input})
      if(NOT _built STREQUAL _want)
        list(APPEND _spg_libobs_stale ${_input})
      endif()
    endforeach()
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
    set(_spg_builder "${PROJECT_SOURCE_DIR}/build-aux/build_libobs.py")
    # Ask the builder itself so the digest has one definition.
    string(CONCAT _spg_digest_py
      "import importlib.util, sys\n"
      "spec = importlib.util.spec_from_file_location('build_libobs', sys.argv[1])\n"
      "module = importlib.util.module_from_spec(spec)\n"
      "spec.loader.exec_module(module)\n"
      "print(module.builder_digest())\n")
    execute_process(
      COMMAND "${Python3_EXECUTABLE}" -I -c "${_spg_digest_py}" "${_spg_builder}"
      OUTPUT_VARIABLE _want OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _rc)
    string(JSON _built ERROR_VARIABLE _err GET "${_spg_libobs_build}" inputs builder)
    if(NOT _rc EQUAL 0 OR NOT _built STREQUAL _want)
      list(APPEND _spg_libobs_stale builder)
    endif()
    # Reconfigure when the builder, its overrides or the installed libobs change.
    file(GLOB _spg_overrides "${PROJECT_SOURCE_DIR}/build-aux/obs-cmake/*")
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${_spg_builder}" ${_spg_overrides} "${_spg_libobs_marker}")
  endif()
  if(NOT EXISTS "${_spg_libobs_marker}")
    _spg_plugin_input_missing("libobs ${SPG_LIBOBS_VERSION} built from the pinned sources")
  elseif(_spg_libobs_stale)
    string(JOIN ", " _spg_libobs_stale ${_spg_libobs_stale})
    _spg_plugin_input_missing("libobs ${SPG_LIBOBS_VERSION} built from the current inputs (the cached build differs in: ${_spg_libobs_stale})")
  endif()
  spg_dep_locate(obs-deps SPG_OBS_DEPS_DIR _spg_obs_deps_version)
  if(NOT SPG_OBS_DEPS_DIR)
    _spg_plugin_input_missing("obs-deps ${_spg_obs_deps_version}")
  endif()
  # SIMDe (and w32-pthreads on Windows) are found through these prefixes.
  list(APPEND CMAKE_PREFIX_PATH "${SPG_LIBOBS_PREFIX}" "${SPG_OBS_DEPS_DIR}")
  if(APPLE)
    set(libobs_DIR "${SPG_LIBOBS_PREFIX}/Frameworks/libobs.framework/Resources/cmake" CACHE PATH "libobs CMake package (pinned)" FORCE)
  else()
    # OBS installs every package config of its Windows Development component flat in <prefix>/cmake.
    set(libobs_DIR "${SPG_LIBOBS_PREFIX}/cmake" CACHE PATH "libobs CMake package (pinned)" FORCE)
    set(w32-pthreads_DIR "${SPG_LIBOBS_PREFIX}/cmake" CACHE PATH "w32-pthreads CMake package (pinned)" FORCE)
  endif()
else()
  message(FATAL_ERROR "SPG_BUILD_PLUGIN: no libobs for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}; "
                      "supported: linux-x64, macos-arm64, windows-x64.")
endif()

find_package(libobs ${SPG_LIBOBS_VERSION} EXACT REQUIRED CONFIG NO_DEFAULT_PATH)
message(STATUS "libobs ${libobs_VERSION}: ${SPG_LIBOBS_PREFIX}")
