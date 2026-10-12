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
  src/frame/geometry.cpp
  src/policy/engine.cpp
  src/policy/normalize.cpp
  src/policy/pii.cpp
  src/policy/watchlist.cpp)
add_library(spg::core ALIAS spg-core)
target_include_directories(spg-core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(spg-core PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(spg-core PRIVATE SPG_VERSION_STRING="${PROJECT_VERSION}")
spg_target_defaults(spg-core)

# Protected memory for watch-list text (SPG-15b): Secret and SecureHeap.
# Windows Error Reporting's dump exclusion is looked up in kernel32 at run
# time (Windows 10 1703 and later), so nothing extra is linked.
target_sources(spg-core PRIVATE src/secure/secret.cpp src/secure/secure_memory.cpp)

# Tracker and temporal hold (SPG-09).
target_sources(spg-core PRIVATE src/track/tracker.cpp)

# ONNX Runtime session wrapper; needs the prebuilt ONNX Runtime.
if(SPG_WITH_DEPS)
  add_subdirectory(src/runtime)
  # Detectors on top of the runtime and OpenCV.
  add_subdirectory(src/detect)
  # Text recognition on the detector's regions (SPG-07).
  add_subdirectory(src/ocr)
endif()
