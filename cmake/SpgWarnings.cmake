# spg_target_defaults(<target>)
# Warnings as errors for SPG's own targets. Third-party code (Catch2, ONNX
# Runtime, OpenCV) is consumed as SYSTEM and is not held to these flags.
function(spg_target_defaults target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual)
  endif()
endfunction()
