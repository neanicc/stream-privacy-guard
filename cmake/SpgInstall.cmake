# Install layout of the OBS plugin (SPG-17), component "obs-spg". The root
# CMakeLists.txt includes this after plugin/, and scripts/package.sh stages it:
#
#   cmake --install <build> --config Release --component obs-spg --prefix <stage>
#
# The layout is what OBS 32 loads from its per-user (or, on Windows,
# per-machine) plugin directory:
#
#   macOS           obs-spg.plugin/Contents/MacOS/obs-spg, Contents/Resources/ = data
#   Windows, Linux  obs-spg/bin/64bit/obs-spg.{dll,so}, obs-spg/data/
#
# Whatever the module build puts into its macOS bundle (locale, effects, and
# Contents/Frameworks/ once the plugin loads ONNX Runtime) is installed with the
# bundle. On Windows and Linux ONNX Runtime goes beside the module when the
# module links it. Third-party notices go into the data directory (licenses/).
# Catch2 and other test-only inputs are never part of this component.

set(_spg_component obs-spg)
get_target_property(_spg_module_name obs-spg OUTPUT_NAME)

if(APPLE)
  install(TARGETS obs-spg LIBRARY DESTINATION . COMPONENT ${_spg_component})
  set(_spg_data_dir "${_spg_module_name}.plugin/Contents/Resources")
  set(_spg_bin_dir "")
else()
  set(_spg_bin_dir "${_spg_module_name}/bin/64bit")
  set(_spg_data_dir "${_spg_module_name}/data")
  install(TARGETS obs-spg
    LIBRARY DESTINATION "${_spg_bin_dir}" COMPONENT ${_spg_component}
    RUNTIME DESTINATION "${_spg_bin_dir}" COMPONENT ${_spg_component})
  install(DIRECTORY "${PROJECT_SOURCE_DIR}/plugin/data/"
    DESTINATION "${_spg_data_dir}" COMPONENT ${_spg_component})
endif()

# _spg_links_target(<target> <wanted> <out-var>)
# TRUE when <target> links <wanted> directly or through static libraries
# (their PRIVATE links are recorded as $<LINK_ONLY:...> in
# INTERFACE_LINK_LIBRARIES).
function(_spg_links_target target wanted out_var)
  set(_seen "")
  set(_queue "${target}")
  set(_found FALSE)
  while(_queue)
    list(POP_FRONT _queue _name)
    if(NOT TARGET "${_name}")
      continue()
    endif()
    get_target_property(_real "${_name}" ALIASED_TARGET)
    if(_real)
      set(_name "${_real}")
    endif()
    if(_name IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen "${_name}")
    if(_name STREQUAL wanted)
      set(_found TRUE)
      break()
    endif()
    foreach(_prop IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_libs "${_name}" ${_prop})
      if(NOT _libs)
        continue()
      endif()
      foreach(_lib IN LISTS _libs)
        string(REGEX REPLACE "^\\$<LINK_ONLY:(.+)>$" "\\1" _lib "${_lib}")
        # Skip other generator expressions and the ::@(dir) scope markers.
        if(_lib MATCHES "^\\$<" OR _lib MATCHES "^::@")
          continue()
        endif()
        string(REGEX REPLACE "::@\\(.*\\)$" "" _lib "${_lib}")
        list(APPEND _queue "${_lib}")
      endforeach()
    endforeach()
  endwhile()
  set(${out_var} ${_found} PARENT_SCOPE)
endfunction()

set(SPG_PACKAGE_SHIPS_ORT FALSE)
if(TARGET spg::onnxruntime)
  _spg_links_target(obs-spg spg::onnxruntime SPG_PACKAGE_SHIPS_ORT)
endif()
message(STATUS "obs-spg package ships ONNX Runtime: ${SPG_PACKAGE_SHIPS_ORT}")

# The name the module asks the loader for (the SONAME on Linux); the macOS
# bundle already holds its copy in Contents/Frameworks.
if(SPG_PACKAGE_SHIPS_ORT AND NOT APPLE)
  if(WIN32)
    set(_spg_ort_name onnxruntime.dll)
  else()
    get_target_property(_spg_ort_name spg::onnxruntime IMPORTED_SONAME)
  endif()
  install(FILES "$<TARGET_FILE:spg::onnxruntime>"
    DESTINATION "${_spg_bin_dir}" RENAME "${_spg_ort_name}" COMPONENT ${_spg_component})
endif()

# Licenses ---------------------------------------------------------------------
set(_spg_licenses "${_spg_data_dir}/licenses")
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
  DESTINATION "${_spg_licenses}" RENAME obs-spg-LICENSE.txt COMPONENT ${_spg_component})
install(FILES "${PROJECT_SOURCE_DIR}/build-aux/package/THIRD-PARTY-NOTICES.txt"
  DESTINATION "${_spg_licenses}" COMPONENT ${_spg_component})

if(SPG_ORT_DIR)
  install(FILES "${SPG_ORT_DIR}/LICENSE"
    DESTINATION "${_spg_licenses}/onnxruntime" RENAME LICENSE.txt COMPONENT ${_spg_component})
  install(FILES "${SPG_ORT_DIR}/ThirdPartyNotices.txt"
    DESTINATION "${_spg_licenses}/onnxruntime" COMPONENT ${_spg_component})
endif()

if(SPG_OPENCV_DIR)
  # The prebuilt package has no top-level LICENSE, so the text of the pinned
  # release is kept in build-aux/package/licenses/. Its bundled third-party
  # licenses (zlib, flatbuffers, ...) are where OpenCV's install puts them.
  install(FILES "${PROJECT_SOURCE_DIR}/build-aux/package/licenses/opencv-${SPG_OPENCV_VERSION}-LICENSE.txt"
    DESTINATION "${_spg_licenses}/opencv" RENAME LICENSE.txt COMPONENT ${_spg_component})
  set(_spg_opencv_3rdparty "")
  foreach(_dir IN ITEMS share/licenses/opencv4 etc/licenses x64/vc17/etc/licenses)
    if(IS_DIRECTORY "${SPG_OPENCV_DIR}/${_dir}")
      set(_spg_opencv_3rdparty "${SPG_OPENCV_DIR}/${_dir}")
      break()
    endif()
  endforeach()
  if(_spg_opencv_3rdparty)
    install(DIRECTORY "${_spg_opencv_3rdparty}/"
      DESTINATION "${_spg_licenses}/opencv/3rdparty" COMPONENT ${_spg_component})
  else()
    # scripts/package.sh fails when this directory is missing.
    message(WARNING "OpenCV third-party licenses not found under ${SPG_OPENCV_DIR}")
  endif()
endif()

include(FetchContent)
FetchContent_GetProperties(nlohmann_json SOURCE_DIR _spg_json_src)
if(_spg_json_src)
  install(FILES "${_spg_json_src}/LICENSE.MIT"
    DESTINATION "${_spg_licenses}/nlohmann-json" RENAME LICENSE.txt COMPONENT ${_spg_component})
endif()
