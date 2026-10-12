# spg-core: the OBS-free pipeline. SPG-01 provides only the target and its
# version; contracts and stages arrive with SPG-02 onward.
add_library(spg-core
  src/version.cpp)
add_library(spg::core ALIAS spg-core)
target_include_directories(spg-core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_definitions(spg-core PRIVATE SPG_VERSION_STRING="${PROJECT_VERSION}")
spg_target_defaults(spg-core)

# ONNX Runtime session wrapper; needs the prebuilt ONNX Runtime.
if(SPG_WITH_DEPS)
  add_subdirectory(src/runtime)
endif()
