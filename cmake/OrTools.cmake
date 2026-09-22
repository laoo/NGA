# OR-Tools arrives as a prebuilt static archive, not as sources. Why it is built
# elsewhere, why statically, and what that costs is in
# docs/decisions/0029-cp-sat-and-or-tools.md; this file is the entire consumer
# side of that decision.
#
# Bumping the version means editing the version and the three hashes below, and
# nothing else. The archives are built by https://github.com/laoo/nga-deps from
# a tag, one Release per OR-Tools version.

set( NGA_ORTOOLS_VERSION "9.15" )

set( NGA_ORTOOLS_PREFIX "" CACHE PATH
  "An already unpacked nga-deps archive to build against instead of downloading one" )

# The asset carries the compiler only on Linux, the one platform where the C++
# runtime is not the platform's own.
if( CMAKE_SYSTEM_NAME STREQUAL "Linux" )
  set( ngaOrToolsAsset "or-tools-${NGA_ORTOOLS_VERSION}-linux-x64-gcc14.tar.gz" )
  set( ngaOrToolsHash  "SHA256=99d7141385f697fbc1c3b3f2f0b310f83948bc4db70305820f0ea8b2f423ad02" )
elseif( CMAKE_SYSTEM_NAME STREQUAL "Darwin" )
  set( ngaOrToolsAsset "or-tools-${NGA_ORTOOLS_VERSION}-macos-arm64.tar.gz" )
  set( ngaOrToolsHash  "SHA256=8b9d140d37bd9d532080a04a6aba134a26d01048ffd2403f0b10107aaf42fc6b" )
elseif( CMAKE_SYSTEM_NAME STREQUAL "Windows" )
  set( ngaOrToolsAsset "or-tools-${NGA_ORTOOLS_VERSION}-windows-x64-msvc.zip" )
  set( ngaOrToolsHash  "SHA256=3780e73da9a251750e01bf09c045b1303a8078c5d9c6213afd24e480878cb29b" )
else( )
  message( FATAL_ERROR
    "No OR-Tools archive is built for ${CMAKE_SYSTEM_NAME}. Build one in "
    "https://github.com/laoo/nga-deps and add it here, or point "
    "NGA_ORTOOLS_PREFIX at an unpacked prefix you built yourself." )
endif( )

# These are checked here, with a sentence each, because every one of them
# otherwise surfaces as a link error minutes later that names nothing useful.

if( CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$" )
  message( FATAL_ERROR
    "The Linux OR-Tools archive is x86_64 and this is ${CMAKE_SYSTEM_PROCESSOR}." )
endif( )

if( CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_FLAGS MATCHES "-stdlib=libc\\+\\+" )
  message( FATAL_ERROR
    "The Linux OR-Tools archive is built against libstdc++, and this build asks for libc++. "
    "The two disagree about the layout of every standard type that crosses the boundary." )
endif( )

if( APPLE AND NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" )
  message( FATAL_ERROR
    "The macOS OR-Tools archive is arm64 only and this is ${CMAKE_SYSTEM_PROCESSOR}." )
endif( )

if( WIN32 AND NOT MSVC )
  message( FATAL_ERROR
    "The Windows OR-Tools archive is built with MSVC and cannot be linked by another toolchain." )
endif( )

if( MSVC )
  # The archive is /MD in every configuration, so NGA has to be as well --
  # otherwise a Debug build pulls in /MDd and disagrees with it about
  # _ITERATOR_DEBUG_LEVEL, which is a link error at best. Setting it here rather
  # than picking a configuration means a Debug build in an IDE also links.
  set( CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" )
endif( )

if( NGA_ORTOOLS_PREFIX )
  set( ngaOrToolsPrefix "${NGA_ORTOOLS_PREFIX}" )
else( )
  # FetchContent, and not a step in ci.yml, so that CI and `cmake --preset debug`
  # on a desk do the same thing and a failure reproduces locally. Nothing is
  # built here: the archive is downloaded and unpacked into build/<preset>/_deps.
  include( FetchContent )
  FetchContent_Declare( ortools_archive
    URL      "https://github.com/laoo/nga-deps/releases/download/or-tools-${NGA_ORTOOLS_VERSION}/${ngaOrToolsAsset}"
    URL_HASH "${ngaOrToolsHash}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE )
  FetchContent_MakeAvailable( ortools_archive )
  set( ngaOrToolsPrefix "${ortools_archive_SOURCE_DIR}" )
endif( )

list( PREPEND CMAKE_PREFIX_PATH "${ngaOrToolsPrefix}" )
find_package( ortools CONFIG REQUIRED )
