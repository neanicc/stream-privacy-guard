# spg-ocr: text recognition for detected text regions (SPG-07, lane:vision):
# PP-OCR CTC recognizer, crop planning and the per-track cache. Consumers
# include "ocr/<name>.hpp"; ONNX Runtime and OpenCV stay private to the
# libraries below it.
add_library(spg-ocr STATIC
  ctc_decode.cpp
  crop_plan.cpp
  text_recognizer.cpp
  recognition_cache.cpp)
add_library(spg::ocr ALIAS spg-ocr)
target_include_directories(spg-ocr PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/core/src>")
target_link_libraries(spg-ocr PUBLIC spg::core spg::runtime spg::detect)
spg_target_defaults(spg-ocr)
