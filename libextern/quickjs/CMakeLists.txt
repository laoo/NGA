# QuickJS (the quickjs-ng fork), vendored — see ../README.md for provenance and
# the update rule.
#
# Four translation units and the headers they include; upstream's `qjs` command
# line, its `quickjs-libc.c` and its test programs are not carried, because a
# generator's script reaches the host only through what nga_core hands it — see
# docs/spec/generators.md.
#
# Built with the project's C compiler and none of the project's warning flags:
# upstream's warning set is its own business, and NGA_WERROR must not fail a
# build over a file this repository does not edit. The include directory is
# SYSTEM for the same reason, on the consumer's side.
add_library( quickjs STATIC
  dtoa.c
  libregexp.c
  libunicode.c
  quickjs.c )

set_target_properties( quickjs PROPERTIES
  C_STANDARD          11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS        ON )

# What upstream's own build defines. `_GNU_SOURCE` is unconditional there, and
# on Windows the narrow headers and a Win7 floor are what it asks for.
target_compile_definitions( quickjs PRIVATE _GNU_SOURCE )
if( WIN32 )
  target_compile_definitions( quickjs PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601 )
endif( )

# `-funsigned-char` is upstream's, and it is not decoration: the code indexes
# tables with `char` values.
if( NOT MSVC )
  target_compile_options( quickjs PRIVATE -funsigned-char -Wno-implicit-fallthrough )
else( )
  # The engine's atomics shim falls back to `<stdatomic.h>`, which MSVC keeps
  # behind a flag in C mode; `/utf-8` is for upstream's non-ASCII comments,
  # which MSVC would otherwise read in the system codepage.
  target_compile_options( quickjs PRIVATE /experimental:c11atomics /utf-8 )
endif( )

find_package( Threads REQUIRED )
target_link_libraries( quickjs PUBLIC Threads::Threads ${CMAKE_DL_LIBS} )

target_include_directories( quickjs SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}" )
