# Replaces OBS's own cmake/<os>/buildspec.cmake while build-aux/build_libobs.py
# configures the OBS sources. OBS's version downloads obs-deps, Qt and CEF at
# configure time; SPG passes the obs-deps archive that deps/manifest.json pins
# (already SHA-256 verified by scripts/fetch_deps.py) through CMAKE_PREFIX_PATH
# instead, and libobs needs neither Qt nor CEF. CMake finds this file first
# because build_libobs.py puts this directory at the front of CMAKE_MODULE_PATH.
include_guard(GLOBAL)
message(STATUS "SPG: OBS dependency download disabled; using CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
