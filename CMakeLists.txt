cmake_minimum_required(VERSION 3.25)

# The pinned ONNX Runtime 1.30.0 macOS build targets macOS 14.
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()

project(StreamPrivacyGuard
  VERSION 0.1.0
  DESCRIPTION "OBS filter that blurs sensitive content before it airs"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SPG_BUILD_TESTS "Build unit and integration tests" ON)
option(SPG_WITH_DEPS "Use the prebuilt ONNX Runtime and OpenCV pinned in deps/manifest.json" ON)
option(SPG_USE_CCACHE "Use ccache (from deps/manifest.json or PATH) when available" ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(SpgManifest)
include(SpgCcache)
include(SpgWarnings)
if(SPG_WITH_DEPS)
  include(SpgDeps)
endif()

add_subdirectory(core)
add_subdirectory(cli)

if(SPG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
