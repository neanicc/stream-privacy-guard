# cmake -DNM=<nm> -DMODULE=<obs-spg module> -P check_exports.cmake
# Fails unless the module exports every entry point libobs resolves when it
# loads a plugin (libobs/obs-module.c).
if(NOT NM OR NOT MODULE)
  message(FATAL_ERROR "usage: cmake -DNM=<nm> -DMODULE=<module> -P check_exports.cmake")
endif()

if(APPLE OR CMAKE_HOST_APPLE)
  set(_args -gU)          # global, defined symbols (Mach-O names carry a leading underscore)
else()
  set(_args -D --defined-only)
endif()
execute_process(COMMAND "${NM}" ${_args} "${MODULE}"
  OUTPUT_VARIABLE _symbols ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${NM} failed on ${MODULE} (${_rc}): ${_err}")
endif()

set(_missing "")
foreach(_name obs_module_load obs_module_unload obs_module_set_pointer obs_module_ver
              obs_module_set_locale obs_module_free_locale obs_module_name obs_module_description)
  if(NOT _symbols MATCHES "[ \t]_?${_name}(\r?\n|$)")
    list(APPEND _missing ${_name})
  endif()
endforeach()
if(_missing)
  message(FATAL_ERROR "${MODULE} does not export: ${_missing}")
endif()
message(STATUS "${MODULE} exports every libobs module entry point")
