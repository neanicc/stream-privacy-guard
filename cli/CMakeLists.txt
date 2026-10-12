# spg-cli: drives spg-core on PNG sequences. Subcommands land in SPG-11.
add_executable(spg-cli src/main.cpp)
target_link_libraries(spg-cli PRIVATE spg::core)
spg_target_defaults(spg-cli)
