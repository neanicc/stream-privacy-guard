# spg-runtime: ONNX Runtime session wrapper (lane:vision). Detectors link it;
# nothing outside this directory includes ONNX Runtime headers.
add_library(spg-runtime STATIC onnx_session.cpp)
add_library(spg::runtime ALIAS spg-runtime)
# Consumers include "runtime/onnx_session.hpp".
target_include_directories(spg-runtime PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/core/src>")
target_link_libraries(spg-runtime PRIVATE spg::onnxruntime)
spg_target_defaults(spg-runtime)
