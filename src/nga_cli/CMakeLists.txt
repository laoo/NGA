add_executable( nga_cli src/Main.cpp )

set_target_properties( nga_cli PROPERTIES OUTPUT_NAME nga )

target_compile_definitions( nga_cli PRIVATE NGA_LIB_DIR="${CMAKE_SOURCE_DIR}/lib" )

target_link_libraries( nga_cli
  PRIVATE nga::core CLI11::CLI11 nga::warnings )
