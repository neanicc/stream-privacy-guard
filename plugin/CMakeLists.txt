# obs-spg: the OBS Studio plugin, a thin adapter over spg-core. Built only with
# -DSPG_BUILD_PLUGIN=ON (scripts/ci/run.sh does); layout follows
# obs-plugintemplate (src/, data/locale/, buildspec.json at the root), with its
# helper scripts in build-aux/.

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SpgLibobs)

file(READ "${PROJECT_SOURCE_DIR}/buildspec.json" _spg_buildspec)
string(JSON SPG_PLUGIN_NAME GET "${_spg_buildspec}" name)
string(JSON SPG_PLUGIN_BUNDLE_ID GET "${_spg_buildspec}" platformConfig macos bundleId)
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/buildspec.json")

add_library(obs-spg MODULE
  src/plugin-main.cpp
  src/plugin-support.cpp
  src/spg-filter.cpp
  data/locale/en-US.ini)
target_link_libraries(obs-spg PRIVATE OBS::libobs spg::core)
target_compile_definitions(obs-spg PRIVATE SPG_PLUGIN_VERSION="${PROJECT_VERSION}")
spg_target_defaults(obs-spg)
# OBS finds modules by file name: obs-spg.so / obs-spg.dll / obs-spg.plugin.
# Only the obs_module_* entry points (marked visible by libobs) are exported.
set_target_properties(obs-spg PROPERTIES
  OUTPUT_NAME "${SPG_PLUGIN_NAME}"
  PREFIX ""
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
if(APPLE)
  set_source_files_properties(data/locale/en-US.ini PROPERTIES MACOSX_PACKAGE_LOCATION Resources/locale)
  set_target_properties(obs-spg PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${SPG_PLUGIN_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "${SPG_PLUGIN_NAME}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
else()
  set_source_files_properties(data/locale/en-US.ini PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

if(NOT SPG_BUILD_TESTS)
  return()
endif()

# The module exports the entry points libobs looks up with dlsym/GetProcAddress.
if(CMAKE_NM)
  add_test(NAME plugin.exports
    COMMAND "${CMAKE_COMMAND}" -DNM=${CMAKE_NM} -DMODULE=$<TARGET_FILE:obs-spg>
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exports.cmake")
endif()

# Load the module into a real, headless libobs and check what it registers.
# Needs libobs's runtime libraries: obs-deps provides them on macOS and Windows.
# The Ubuntu package's libobs.so needs system FFmpeg 6, PulseAudio, Wayland and
# others that the pinned inputs do not include, so Linux runs plugin.exports only.
if(SPG_OBS_DEPS_DIR)
  add_executable(obs-spg-load-test tests/plugin_load_test.cpp)
  target_include_directories(obs-spg-load-test PRIVATE src)
  target_link_libraries(obs-spg-load-test PRIVATE OBS::libobs)
  spg_target_defaults(obs-spg-load-test)
  add_test(NAME plugin.loads
    COMMAND obs-spg-load-test $<TARGET_FILE:obs-spg> "${CMAKE_CURRENT_SOURCE_DIR}/data")
  if(APPLE)
    set_target_properties(obs-spg-load-test PROPERTIES
      BUILD_RPATH "${SPG_LIBOBS_PREFIX}/Frameworks;${SPG_OBS_DEPS_DIR}/lib")
  elseif(WIN32)
    set_tests_properties(plugin.loads PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>;PATH=path_list_prepend:${SPG_OBS_DEPS_DIR}/bin")
  endif()
endif()
