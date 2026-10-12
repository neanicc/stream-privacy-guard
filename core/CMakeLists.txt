# spg-core: the OBS-free pipeline. Contracts (SPG-02) live in include/spg/ and
# src/contracts/; stages arrive with later tickets in their own src/ folders.
include(FetchContent)

# nlohmann/json 3.12.0 (MIT), pinned by SHA-256 of the release's json.tar.xz.
# Private to spg-core: consumers see only std::string in the JSON API.
set(JSON_BuildTests OFF)
set(JSON_Install OFF)
set(JSON_ImplicitConversions OFF)
set(JSON_GlobalUDLs OFF)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
FetchContent_MakeAvailable(nlohmann_json)

add_library(spg-core
  src/version.cpp
  src/contracts/contracts.cpp
  src/contracts/json.cpp
  src/frame/frame_view.cpp
  src/frame/geometry.cpp)
add_library(spg::core ALIAS spg-core)
target_include_directories(spg-core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(spg-core PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(spg-core PRIVATE SPG_VERSION_STRING="${PROJECT_VERSION}")
spg_target_defaults(spg-core)

# ONNX Runtime session wrapper; needs the prebuilt ONNX Runtime.
if(SPG_WITH_DEPS)
  add_subdirectory(src/runtime)
endif()
