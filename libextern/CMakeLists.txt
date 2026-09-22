# Third-party code carried in the tree. Each library is one directory holding
# upstream's files unmodified and a CMakeLists.txt of ours; README.md here is
# the inventory.
add_subdirectory( quickjs )

if( NGA_BUILD_TESTS )
  # The 6502 core the suite executes Transitions on. Under the tests' gate
  # because they are its only consumer — nothing the tool links reaches it, and
  # a build without the suite does not carry it; it moves out on the day
  # nga_core runs code at build time. See
  # ../docs/decisions/0021-vendored-third-party-code.md.
  add_subdirectory( vrEmu6502 )
endif( )
