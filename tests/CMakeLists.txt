include(FetchContent)

# Catch2 is pinned by version and SHA-256 of the release source archive.
FetchContent_Declare(Catch2
  URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
  URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# Unit tests for spg-core -----------------------------------------------------
add_executable(spg-unit-tests
  unit/contracts_test.cpp
  unit/geometry_test.cpp
  unit/json_test.cpp
  unit/policy_pii_test.cpp
  unit/policy_sentinel_test.cpp
  unit/policy_test.cpp
  unit/version_test.cpp)
target_link_libraries(spg-unit-tests PRIVATE spg::core Catch2::Catch2WithMain)
target_compile_definitions(spg-unit-tests PRIVATE
  SPG_EXPECTED_VERSION="${PROJECT_VERSION}"
  SPG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
spg_target_defaults(spg-unit-tests)
# Each unit test takes well under a second; the timeout turns a hang (such as
# a render-facing scheduler call waiting on a stalled stage) into a failure.
catch_discover_tests(spg-unit-tests TEST_PREFIX "unit." DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 300)
# Tracker and temporal hold (SPG-09).
target_sources(spg-unit-tests PRIVATE unit/tracker_test.cpp)
# Scheduler, lookahead and fail-closed release (SPG-10). The mask helper tests
# include core/src/mask/ directly.
target_sources(spg-unit-tests PRIVATE
  unit/lookahead_test.cpp
  unit/lookahead_property_test.cpp
  unit/scheduler_test.cpp)
target_include_directories(spg-unit-tests PRIVATE "${PROJECT_SOURCE_DIR}/core/src")

# Prebuilt dependencies link, load and run ------------------------------------
if(SPG_WITH_DEPS)
  add_executable(spg-deps-tests
    integration/deps_smoke_test.cpp)
  target_link_libraries(spg-deps-tests PRIVATE spg::onnxruntime spg::opencv Catch2::Catch2WithMain)
  target_compile_definitions(spg-deps-tests PRIVATE
    SPG_EXPECTED_ORT_VERSION="${SPG_ORT_VERSION}"
    SPG_EXPECTED_OPENCV_VERSION="${SPG_OPENCV_VERSION}")
  spg_target_defaults(spg-deps-tests)
  spg_copy_runtime_dlls(spg-deps-tests)
  catch_discover_tests(spg-deps-tests TEST_PREFIX "deps." DISCOVERY_MODE PRE_TEST)
endif()

# spg-cli behavior --------------------------------------------------------------
add_test(NAME cli.version COMMAND spg-cli --version)
set_tests_properties(cli.version PROPERTIES
  PASS_REGULAR_EXPRESSION "^spg-cli ${PROJECT_VERSION}\r?\n$")
add_test(NAME cli.help COMMAND spg-cli --help)
set_tests_properties(cli.help PROPERTIES PASS_REGULAR_EXPRESSION "usage: spg-cli")
# Unimplemented and unknown commands must exit non-zero.
foreach(command run bench eval scrub frobnicate)
  add_test(NAME cli.rejects-${command} COMMAND spg-cli ${command})
  set_tests_properties(cli.rejects-${command} PROPERTIES WILL_FAIL TRUE)
endforeach()
add_test(NAME cli.rejects-no-arguments COMMAND spg-cli)
set_tests_properties(cli.rejects-no-arguments PROPERTIES WILL_FAIL TRUE)

# Dependency fetch script --------------------------------------------------------
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
add_test(NAME scripts.fetch_deps
  COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_fetch_deps.py")

# Model fetch script ---------------------------------------------------------------
add_test(NAME scripts.fetch_models
  COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_fetch_models.py")

# Eval scorer and suite generators (lane:eval) -------------------------------------
# The scorer test is standard library only. The generator test fetches the
# Pillow wheel pinned in eval/pydeps.json into the cache on first use.
add_test(NAME scoring.scorer
  COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_scorer.py")
add_test(NAME scoring.generators
  COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_generators.py")
set_tests_properties(scoring.generators PROPERTIES ENVIRONMENT "SPG_CACHE_DIR=${SPG_CACHE_DIR}")

# ONNX Runtime session wrapper ------------------------------------------------------
if(SPG_WITH_DEPS)
  # Tiny ONNX models generated at build time (no weights in Git) plus a listing
  # of the pinned models in $SPG_CACHE_DIR, which the test skips when absent.
  set(_spg_runtime_fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures/runtime")
  set(_spg_runtime_fixture_script "${CMAKE_CURRENT_SOURCE_DIR}/integration/make_runtime_fixtures.py")
  add_custom_command(
    OUTPUT "${_spg_runtime_fixtures}/scale_static.onnx" "${_spg_runtime_fixtures}/scale_dynamic.onnx"
           "${_spg_runtime_fixtures}/not_a_model.onnx" "${_spg_runtime_fixtures}/models.txt"
           "${_spg_runtime_fixtures}/text_wrong_shape.onnx" "${_spg_runtime_fixtures}/text_sqrt.onnx"
           "${_spg_runtime_fixtures}/rec_softmax.onnx" "${_spg_runtime_fixtures}/rec_sqrt.onnx"
           "${_spg_runtime_fixtures}/rec_wrong_classes.onnx" "${_spg_runtime_fixtures}/rec_no_dictionary.onnx"
    COMMAND Python3::Interpreter -I "${_spg_runtime_fixture_script}"
            --out "${_spg_runtime_fixtures}"
            --manifest "${PROJECT_SOURCE_DIR}/models/manifest.json"
            --cache "${SPG_CACHE_DIR}"
    DEPENDS "${_spg_runtime_fixture_script}" "${PROJECT_SOURCE_DIR}/models/manifest.json"
    COMMENT "Generating ONNX Runtime test fixtures"
    VERBATIM)
  add_custom_target(spg-runtime-fixtures
    DEPENDS "${_spg_runtime_fixtures}/scale_static.onnx" "${_spg_runtime_fixtures}/models.txt")

  add_executable(spg-runtime-tests integration/runtime_session_test.cpp)
  add_dependencies(spg-runtime-tests spg-runtime-fixtures)
  target_link_libraries(spg-runtime-tests PRIVATE spg::runtime Catch2::Catch2WithMain)
  target_compile_definitions(spg-runtime-tests PRIVATE SPG_RUNTIME_FIXTURES="${_spg_runtime_fixtures}")
  spg_target_defaults(spg-runtime-tests)
  spg_copy_runtime_dlls(spg-runtime-tests)
  catch_discover_tests(spg-runtime-tests TEST_PREFIX "runtime." DISCOVERY_MODE PRE_TEST)
endif()

# Detectors (lane:vision) ------------------------------------------------------------
if(SPG_WITH_DEPS)
  # The pinned model paths, from the manifest; the model tests skip when a
  # model is not cached. The runtime fixtures supply a model that is neither.
  # Text fixtures are drawn at test time with OpenCV (synthetic_text.hpp).
  set(_spg_models_manifest "${PROJECT_SOURCE_DIR}/models/manifest.json")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_spg_models_manifest}")
  file(READ "${_spg_models_manifest}" _spg_models_json)
  string(JSON _spg_yunet_version GET "${_spg_models_json}" models yunet version)
  string(JSON _spg_yunet_file GET "${_spg_models_json}" models yunet file)
  string(JSON _spg_ppocr_det_version GET "${_spg_models_json}" models ppocr-det-en version)
  string(JSON _spg_ppocr_det_file GET "${_spg_models_json}" models ppocr-det-en file)

  add_executable(spg-detect-tests
    unit/face_detect_test.cpp
    unit/text_detect_test.cpp
    integration/face_detector_test.cpp
    integration/text_detector_test.cpp)
  add_dependencies(spg-detect-tests spg-runtime-fixtures)
  target_link_libraries(spg-detect-tests PRIVATE spg::detect spg::opencv Catch2::Catch2WithMain)
  target_compile_definitions(spg-detect-tests PRIVATE
    SPG_YUNET_MODEL="${SPG_CACHE_DIR}/models/yunet/${_spg_yunet_version}/${_spg_yunet_file}"
    SPG_PPOCR_DET_MODEL="${SPG_CACHE_DIR}/models/ppocr-det-en/${_spg_ppocr_det_version}/${_spg_ppocr_det_file}"
    SPG_RUNTIME_FIXTURES="${_spg_runtime_fixtures}")
  spg_target_defaults(spg-detect-tests)
  spg_copy_runtime_dlls(spg-detect-tests)
  catch_discover_tests(spg-detect-tests TEST_PREFIX "detect." DISCOVERY_MODE PRE_TEST)
endif()

# Text recognizer (SPG-07, lane:vision) ----------------------------------------------
if(SPG_WITH_DEPS)
  string(JSON _spg_ppocr_rec_version GET "${_spg_models_json}" models ppocr-rec-en version)
  string(JSON _spg_ppocr_rec_file GET "${_spg_models_json}" models ppocr-rec-en file)
  # Crops are made-up strings drawn at test time (synthetic_text.hpp).
  add_executable(spg-ocr-tests
    unit/ocr_test.cpp
    integration/text_recognizer_test.cpp)
  add_dependencies(spg-ocr-tests spg-runtime-fixtures)
  target_link_libraries(spg-ocr-tests PRIVATE spg::ocr spg::opencv Catch2::Catch2WithMain)
  target_compile_definitions(spg-ocr-tests PRIVATE
    SPG_PPOCR_REC_MODEL="${SPG_CACHE_DIR}/models/ppocr-rec-en/${_spg_ppocr_rec_version}/${_spg_ppocr_rec_file}"
    SPG_PPOCR_DET_MODEL="${SPG_CACHE_DIR}/models/ppocr-det-en/${_spg_ppocr_det_version}/${_spg_ppocr_det_file}"
    SPG_RUNTIME_FIXTURES="${_spg_runtime_fixtures}")
  spg_target_defaults(spg-ocr-tests)
  spg_copy_runtime_dlls(spg-ocr-tests)
  catch_discover_tests(spg-ocr-tests TEST_PREFIX "ocr." DISCOVERY_MODE PRE_TEST)
endif()
