# spg-detect: detectors that turn readback frames into Source-space detections
# (lane:vision). Faces arrive with SPG-05; the text detector (SPG-06) adds its
# own files here. Consumers include "detect/<name>.hpp"; OpenCV and ONNX
# Runtime stay private.
add_library(spg-detect STATIC
  frame_image.cpp
  yunet.cpp
  face_detector.cpp)
add_library(spg::detect ALIAS spg-detect)
target_include_directories(spg-detect PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/core/src>")
target_link_libraries(spg-detect
  PUBLIC spg::core spg::runtime
  PRIVATE spg::opencv)
spg_target_defaults(spg-detect)
