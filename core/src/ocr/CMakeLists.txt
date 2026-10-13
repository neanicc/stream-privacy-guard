# spg-ocr: text recognition for detected text regions (SPG-07, lane:vision):
# PP-OCR CTC recognizer, crop planning, the per-track cache and the
# content-keyed cache in front of the scan stage (SPG-16e). Consumers
# include "ocr/<name>.hpp"; ONNX Runtime and OpenCV stay private to the
# libraries below it.
add_library(spg-ocr STATIC
  ctc_decode.cpp
  crop_plan.cpp
  text_recognizer.cpp
  recognition_cache.cpp
  cached_recognizer.cpp)
add_library(spg::ocr ALIAS spg-ocr)
target_include_directories(spg-ocr PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/core/src>")
target_link_libraries(spg-ocr PUBLIC spg::core spg::runtime spg::detect)
spg_target_defaults(spg-ocr)
