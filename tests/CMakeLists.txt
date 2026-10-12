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
  unit/version_test.cpp)
target_link_libraries(spg-unit-tests PRIVATE spg::core Catch2::Catch2WithMain)
target_compile_definitions(spg-unit-tests PRIVATE
  SPG_EXPECTED_VERSION="${PROJECT_VERSION}"
  SPG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
spg_target_defaults(spg-unit-tests)
catch_discover_tests(spg-unit-tests TEST_PREFIX "unit." DISCOVERY_MODE PRE_TEST)

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
