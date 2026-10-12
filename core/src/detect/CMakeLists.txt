# spg-detect: detectors that turn readback frames into Source-space detections
# (lane:vision): faces (SPG-05) and text regions (SPG-06). Consumers include
# "detect/<name>.hpp"; OpenCV and ONNX Runtime stay private.
add_library(spg-detect STATIC
  frame_image.cpp
  source_clip.cpp
  yunet.cpp
  face_detector.cpp
  db_postprocess.cpp
  text_detector.cpp)
add_library(spg::detect ALIAS spg-detect)
target_include_directories(spg-detect PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/core/src>")
target_link_libraries(spg-detect
  PUBLIC spg::core spg::runtime
  PRIVATE spg::opencv)
spg_target_defaults(spg-detect)
