# Imported targets for the prebuilt libraries pinned in deps/manifest.json:
#   spg::onnxruntime  shared ONNX Runtime (CPU package; CoreML EP on macOS)
#   spg::opencv       static minimal OpenCV (core, imgproc)
# Run scripts/ci/run.sh setup first, or configure with -DSPG_WITH_DEPS=OFF.

function(_spg_require_dep name dir version)
  if(dir STREQUAL "")
    if(SPG_PLATFORM STREQUAL "")
      message(FATAL_ERROR "${name}: no prebuilt package for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}; "
                          "deps/manifest.json covers linux-x64, macos-arm64 and windows-x64. "
                          "Configure with -DSPG_WITH_DEPS=OFF to build without it.")
    endif()
    message(FATAL_ERROR "${name} ${version} for ${SPG_PLATFORM} is not in ${SPG_CACHE_DIR}/deps. "
                        "Run scripts/ci/run.sh setup (or python3 scripts/fetch_deps.py) first.")
  endif()
endfunction()

# ONNX Runtime ---------------------------------------------------------------
spg_dep_locate(onnxruntime SPG_ORT_DIR SPG_ORT_VERSION)
_spg_require_dep(onnxruntime "${SPG_ORT_DIR}" "${SPG_ORT_VERSION}")

add_library(spg::onnxruntime SHARED IMPORTED GLOBAL)
if(WIN32)
  set_target_properties(spg::onnxruntime PROPERTIES
    IMPORTED_LOCATION "${SPG_ORT_DIR}/lib/onnxruntime.dll"
    IMPORTED_IMPLIB "${SPG_ORT_DIR}/lib/onnxruntime.lib")
elseif(APPLE)
  set_target_properties(spg::onnxruntime PROPERTIES
    IMPORTED_LOCATION "${SPG_ORT_DIR}/lib/libonnxruntime.${SPG_ORT_VERSION}.dylib"
    IMPORTED_SONAME "@rpath/libonnxruntime.1.dylib")
else()
  set_target_properties(spg::onnxruntime PROPERTIES
    IMPORTED_LOCATION "${SPG_ORT_DIR}/lib/libonnxruntime.so.${SPG_ORT_VERSION}"
    IMPORTED_SONAME "libonnxruntime.so.1")
endif()
set_target_properties(spg::onnxruntime PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${SPG_ORT_DIR}/include")
message(STATUS "ONNX Runtime ${SPG_ORT_VERSION}: ${SPG_ORT_DIR}")

# OpenCV ---------------------------------------------------------------------
spg_dep_locate(opencv SPG_OPENCV_DIR SPG_OPENCV_VERSION)
_spg_require_dep(opencv "${SPG_OPENCV_DIR}" "${SPG_OPENCV_VERSION}")

if(WIN32)
  # Point straight at the static MSVC package so OpenCV's top-level config does
  # not have to recognize the toolset version.
  set(_spg_opencv_config "${SPG_OPENCV_DIR}/x64/vc17/staticlib")
else()
  set(_spg_opencv_config "${SPG_OPENCV_DIR}/lib/cmake/opencv4")
endif()
set(OpenCV_STATIC ON)
find_package(OpenCV ${SPG_OPENCV_VERSION} EXACT REQUIRED CONFIG
  COMPONENTS core imgproc
  PATHS "${_spg_opencv_config}"
  NO_DEFAULT_PATH)

add_library(spg::opencv INTERFACE IMPORTED GLOBAL)
target_link_libraries(spg::opencv INTERFACE opencv_core opencv_imgproc)
message(STATUS "OpenCV ${OpenCV_VERSION}: ${SPG_OPENCV_DIR}")

# spg_copy_runtime_dlls(<target>)
# Windows has no rpath: put imported DLLs next to the executable. (An older
# onnxruntime.dll ships in System32; the application directory wins.)
function(spg_copy_runtime_dlls target)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "$<TARGET_RUNTIME_DLLS:${target}>" "$<TARGET_FILE_DIR:${target}>"
      COMMAND_EXPAND_LISTS
      VERBATIM)
  endif()
endfunction()
