# Injected with CMAKE_PROJECT_INCLUDE when build-aux/build_libobs.py configures
# OBS 32.1 on macOS. libobs-metal is written in Swift, but OBS 32.1 enables the
# Swift language only from a plugin directory, so a configure with
# ENABLE_PLUGINS=OFF fails with "can not determine linker language for target:
# libobs-metal". OBS 32.2 enables Swift in its macOS compiler configuration.
enable_language(Swift)
