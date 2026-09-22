cmake_minimum_required( VERSION 3.25 )

project( NGA
  VERSION     0.1.0
  DESCRIPTION "Resource-aware 6502 assembler with static memory-layout verification"
  LANGUAGES   C CXX )

list( APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" )

set( CMAKE_CXX_STANDARD          23 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS        OFF )
set( CMAKE_EXPORT_COMPILE_COMMANDS ON )

# On where there is a suite to build. The public tree carries none -- see
# scripts/export-public.py -- and a configure there must not fail over a
# directory it was never given.
if( EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt" )
  set( ngaTestsDefault ON )
else( )
  set( ngaTestsDefault OFF )
endif( )

option( NGA_BUILD_TESTS "Build the test suite"           ${ngaTestsDefault} )
option( NGA_WERROR      "Treat compiler warnings as errors" OFF )

include( Warnings )
# Before Dependencies: OrTools pins the MSVC runtime library, and a dependency
# whose targets already exist keeps whatever it was configured with.
include( OrTools )
include( Dependencies )

# Before the consumers: nga_core links the JavaScript engine a generator's
# script runs in, and what the suite executes Transitions on is gated inside.
add_subdirectory( libextern )

add_subdirectory( src/nga_core )
add_subdirectory( src/nga_cli )

if( NGA_BUILD_TESTS )
  enable_testing( )
  add_subdirectory( tests )
endif( )
