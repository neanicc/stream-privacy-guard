# spg-cli: drives spg-core on PNG sequences: run, eval and bench (SPG-11) and
# scrub (SPG-18). The commands live in spg-cli-lib so the tests can call them
# in process.

# stb_image (PNG decoding) and stb_image_write v2.30 / v1.16, MIT or public
# domain, pinned by commit and the SHA-256 of each header. Downloaded once
# into the build tree; a hash mismatch fails the configure.
set(_spg_stb_commit 31c1ad37456438565541f4919958214b6e762fb4)
set(_spg_stb_dir "${CMAKE_BINARY_DIR}/_deps/stb-${_spg_stb_commit}")
set(_spg_stb_files
  "stb_image.h=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3"
  "stb_image_write.h=cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05")
foreach(_spg_entry IN LISTS _spg_stb_files)
  string(REPLACE "=" ";" _spg_pair "${_spg_entry}")
  list(GET _spg_pair 0 _spg_name)
  list(GET _spg_pair 1 _spg_sha)
  set(_spg_path "${_spg_stb_dir}/${_spg_name}")
  set(_spg_have "")
  if(EXISTS "${_spg_path}")
    file(SHA256 "${_spg_path}" _spg_have)
  endif()
  if(NOT _spg_have STREQUAL _spg_sha)
    file(DOWNLOAD "https://raw.githubusercontent.com/nothings/stb/${_spg_stb_commit}/${_spg_name}" "${_spg_path}"
      EXPECTED_HASH SHA256=${_spg_sha}
      TLS_VERIFY ON
      STATUS _spg_status)
    list(GET _spg_status 0 _spg_code)
    if(NOT _spg_code EQUAL 0)
      file(REMOVE "${_spg_path}")
      message(FATAL_ERROR "stb: downloading ${_spg_name} failed: ${_spg_status}")
    endif()
  endif()
endforeach()

# The stb implementations, without SPG's warnings-as-errors flags.
add_library(spg-stb STATIC src/stb_impl.cpp)
target_include_directories(spg-stb SYSTEM PUBLIC "${_spg_stb_dir}")
target_compile_features(spg-stb PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(spg-stb PRIVATE /w)
else()
  target_compile_options(spg-stb PRIVATE -w)
endif()

add_library(spg-cli-lib STATIC
  src/commands.cpp
  src/driver.cpp
  src/image_io.cpp
  src/inputs.cpp
  src/scrub.cpp
  src/stages.cpp
  src/timing.cpp)
target_include_directories(spg-cli-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(spg-cli-lib
  PUBLIC spg::core
  PRIVATE spg-stb nlohmann_json::nlohmann_json)
if(MSVC)
  # std::getenv for $SPG_CACHE_DIR and $HOME.
  target_compile_definitions(spg-cli-lib PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The pinned model files, relative to $SPG_CACHE_DIR/models, from the manifest.
set(_spg_models_manifest "${PROJECT_SOURCE_DIR}/models/manifest.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_spg_models_manifest}")
file(READ "${_spg_models_manifest}" _spg_models_json)
foreach(_spg_model IN ITEMS yunet ppocr-det-en ppocr-rec-en)
  string(JSON _spg_version GET "${_spg_models_json}" models ${_spg_model} version)
  string(JSON _spg_file GET "${_spg_models_json}" models ${_spg_model} file)
  set(_spg_rel_${_spg_model} "${_spg_model}/${_spg_version}/${_spg_file}")
endforeach()
target_compile_definitions(spg-cli-lib PRIVATE
  SPG_MODEL_YUNET="${_spg_rel_yunet}"
  SPG_MODEL_PPOCR_DET="${_spg_rel_ppocr-det-en}"
  SPG_MODEL_PPOCR_REC="${_spg_rel_ppocr-rec-en}")

if(SPG_WITH_DEPS)
  target_sources(spg-cli-lib PRIVATE src/stages_models.cpp)
  target_link_libraries(spg-cli-lib PRIVATE spg::detect spg::ocr)
else()
  target_sources(spg-cli-lib PRIVATE src/stages_none.cpp)
endif()
spg_target_defaults(spg-cli-lib)

add_executable(spg-cli src/main.cpp)
target_link_libraries(spg-cli PRIVATE spg-cli-lib)
spg_target_defaults(spg-cli)
if(SPG_WITH_DEPS)
  spg_copy_runtime_dlls(spg-cli)
endif()
