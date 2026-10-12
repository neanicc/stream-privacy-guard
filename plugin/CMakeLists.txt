# obs-spg: the OBS Studio plugin, a thin adapter over spg-core. Built only with
# -DSPG_BUILD_PLUGIN=ON (scripts/ci/run.sh does); layout follows
# obs-plugintemplate (src/, data/locale/, buildspec.json at the root), with its
# helper scripts in build-aux/.

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SpgLibobs)

if(NOT SPG_WITH_DEPS)
  message(FATAL_ERROR "SPG_BUILD_PLUGIN needs SPG_WITH_DEPS=ON: obs-spg links ONNX Runtime and OpenCV "
                      "for its detection models (SPG-14b).")
endif()

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

# Detection models (SPG-14b) without OBS: the pinned files, checked by size
# and SHA-256, opened through the spg-core runtime wrapper on a loader thread,
# and the run_scan stages built from them. The pins come from
# models/manifest.json at configure time.
set(_spg_models_manifest "${PROJECT_SOURCE_DIR}/models/manifest.json")
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_spg_models_manifest}")
file(READ "${_spg_models_manifest}" _spg_models_json)
set(_spg_pinned_models "// Generated from models/manifest.json by plugin/CMakeLists.txt.\n")
foreach(_spg_model IN ITEMS yunet ppocr-det-en ppocr-rec-en)
  foreach(_spg_field IN ITEMS version file size sha256)
    string(JSON _spg_${_spg_field} GET "${_spg_models_json}" models ${_spg_model} ${_spg_field})
  endforeach()
  string(APPEND _spg_pinned_models
    "{\"${_spg_model}\", \"${_spg_model}/${_spg_version}/${_spg_file}\", ${_spg_size}ULL, \"${_spg_sha256}\"},\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/models/pinned_models.inc"
  CONTENT "${_spg_pinned_models}" @ONLY)

add_library(obs-spg-models STATIC
  src/models/model_hub.cpp
  src/models/model_set.cpp
  src/models/sha256.cpp)
target_include_directories(obs-spg-models
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(obs-spg-models PUBLIC spg::core PRIVATE spg::detect spg::ocr)
spg_target_defaults(obs-spg-models)

add_library(obs-spg MODULE
  src/model_loading.cpp
  src/plugin-main.cpp
  src/plugin-support.cpp
  src/spg-filter.cpp
  src/render/filter_render.cpp
  src/render/obs_gpu.cpp
  data/effects/spg-mask.effect
  data/locale/en-US.ini)
target_link_libraries(obs-spg PRIVATE OBS::libobs obs-spg-settings obs-spg-pipeline obs-spg-models)
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

# ONNX Runtime ships next to the module, the way obs-backgroundremoval and
# obs-detect bundle it, so the plugin never picks up another copy (Windows
# has an older onnxruntime.dll in System32). OpenCV is linked statically.
if(WIN32)
  # libobs loads a module with its own directory added to the DLL search path.
  # Only ONNX Runtime: libobs's own DLLs belong to OBS, never to the plugin.
  add_custom_command(TARGET obs-spg POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:spg::onnxruntime>" "$<TARGET_FILE_DIR:obs-spg>"
    VERBATIM)
elseif(APPLE)
  # Inside the bundle, found through the module's only rpath, so the build
  # tree loads the copy a package would ship (plugin.loads checks it). libobs
  # resolves through the host's rpath (OBS.app, or the load test).
  add_custom_command(TARGET obs-spg POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:obs-spg>/Frameworks"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:spg::onnxruntime>"
            "$<TARGET_BUNDLE_CONTENT_DIR:obs-spg>/Frameworks/libonnxruntime.1.dylib"
    VERBATIM)
  set_target_properties(obs-spg PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@loader_path/../Frameworks")
else()
  add_custom_command(TARGET obs-spg POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:spg::onnxruntime>"
            "$<TARGET_FILE_DIR:obs-spg>/libonnxruntime.so.1"
    VERBATIM)
  set_target_properties(obs-spg PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")
  # Keep the statically linked OpenCV and spg-core symbols out of the dynamic
  # symbol table, so they never interpose another plugin's OpenCV.
  target_link_options(obs-spg PRIVATE "LINKER:--exclude-libs,ALL")
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

# Model files, loader thread and the stages with the pinned models (SPG-14b).
# The model cases skip when the models are not in $SPG_CACHE_DIR.
add_executable(obs-spg-models-tests tests/models_test.cpp)
target_link_libraries(obs-spg-models-tests PRIVATE obs-spg-models spg::opencv Catch2::Catch2WithMain)
target_include_directories(obs-spg-models-tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/integration")
target_compile_definitions(obs-spg-models-tests PRIVATE SPG_TEST_MODELS_DIR="${SPG_CACHE_DIR}/models")
spg_target_defaults(obs-spg-models-tests)
spg_copy_runtime_dlls(obs-spg-models-tests)
catch_discover_tests(obs-spg-models-tests TEST_PREFIX "plugin.models." DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 600)

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
  # Once with the pinned models in $SPG_CACHE_DIR (scripts/ci/run.sh setup
  # fetches them) and once with an empty cache: the module must load them, or
  # report them missing, and fail closed.
  add_test(NAME plugin.loads
    COMMAND obs-spg-load-test $<TARGET_FILE:obs-spg> "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_CURRENT_BINARY_DIR}/plugin-loads-config" models-present)
  add_test(NAME plugin.loads.no_models
    COMMAND obs-spg-load-test $<TARGET_FILE:obs-spg> "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_CURRENT_BINARY_DIR}/plugin-loads-config-no-models" models-absent)
  set_tests_properties(plugin.loads PROPERTIES ENVIRONMENT "SPG_CACHE_DIR=${SPG_CACHE_DIR}" TIMEOUT 600)
  set_tests_properties(plugin.loads.no_models PROPERTIES
    ENVIRONMENT "SPG_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/plugin-loads-empty-cache" TIMEOUT 600)
  if(APPLE)
    set_target_properties(obs-spg-load-test PROPERTIES
      BUILD_RPATH "${SPG_LIBOBS_PREFIX}/Frameworks;${SPG_OBS_DEPS_DIR}/lib")
  elseif(WIN32)
    set_tests_properties(plugin.loads plugin.loads.no_models PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>;PATH=path_list_prepend:${SPG_OBS_DEPS_DIR}/bin")
  endif()
endif()
