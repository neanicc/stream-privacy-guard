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

# Settings and watch-list storage without OBS (SPG-15), so they are
# unit-tested on every OS, including Linux where plugin.loads does not run.
add_library(obs-spg-settings STATIC
  src/settings/filter_settings.cpp
  src/settings/watchlist_store.cpp)
target_include_directories(obs-spg-settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(obs-spg-settings PUBLIC spg::core)
spg_target_defaults(obs-spg-settings)

# Lookahead ring, readback scheduling, draw plans and scheduler retirement
# without OBS (SPG-14), unit-tested on every OS with a fake GPU.
add_library(obs-spg-pipeline STATIC
  src/pipeline/frame_ring.cpp
  src/pipeline/host.cpp
  src/pipeline/mask_levels.cpp
  src/pipeline/pipeline.cpp
  src/pipeline/readback_pool.cpp
  src/pipeline/reaper.cpp
  src/pipeline/ring_budget.cpp)
target_include_directories(obs-spg-pipeline PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(obs-spg-pipeline PUBLIC spg::core)
spg_target_defaults(obs-spg-pipeline)

add_library(obs-spg MODULE
  src/plugin-main.cpp
  src/plugin-support.cpp
  src/spg-filter.cpp
  src/render/filter_render.cpp
  src/render/obs_gpu.cpp
  data/effects/spg-mask.effect
  data/locale/en-US.ini)
target_link_libraries(obs-spg PRIVATE OBS::libobs obs-spg-settings obs-spg-pipeline)
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
  set_source_files_properties(data/effects/spg-mask.effect PROPERTIES MACOSX_PACKAGE_LOCATION Resources/effects)
  set_target_properties(obs-spg PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${SPG_PLUGIN_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "${SPG_PLUGIN_NAME}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
else()
  set_source_files_properties(data/locale/en-US.ini data/effects/spg-mask.effect PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

if(NOT SPG_BUILD_TESTS)
  return()
endif()

# Settings clamping, watch-list file format, permissions and secrecy.
include(FetchContent)
FetchContent_GetProperties(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
add_executable(obs-spg-settings-tests tests/settings_test.cpp)
target_link_libraries(obs-spg-settings-tests PRIVATE obs-spg-settings Catch2::Catch2WithMain)
spg_target_defaults(obs-spg-settings-tests)
catch_discover_tests(obs-spg-settings-tests TEST_PREFIX "plugin.settings." DISCOVERY_MODE PRE_TEST)

# Render pipeline with a fake GPU and a synchronous scheduler (SPG-14).
add_executable(obs-spg-pipeline-tests tests/pipeline_test.cpp tests/ring_budget_test.cpp tests/alloc_fault.cpp)
target_link_libraries(obs-spg-pipeline-tests PRIVATE obs-spg-pipeline Catch2::Catch2WithMain)
target_include_directories(obs-spg-pipeline-tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
spg_target_defaults(obs-spg-pipeline-tests)
catch_discover_tests(obs-spg-pipeline-tests TEST_PREFIX "plugin.pipeline." DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 300)

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
  target_link_libraries(obs-spg-load-test PRIVATE OBS::libobs obs-spg-settings)
  spg_target_defaults(obs-spg-load-test)
  add_test(NAME plugin.loads
    COMMAND obs-spg-load-test $<TARGET_FILE:obs-spg> "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_CURRENT_BINARY_DIR}/plugin-loads-config")
  if(APPLE)
    set_target_properties(obs-spg-load-test PROPERTIES
      BUILD_RPATH "${SPG_LIBOBS_PREFIX}/Frameworks;${SPG_OBS_DEPS_DIR}/lib")
  elseif(WIN32)
    set_tests_properties(plugin.loads PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>;PATH=path_list_prepend:${SPG_OBS_DEPS_DIR}/bin")
  endif()
endif()
