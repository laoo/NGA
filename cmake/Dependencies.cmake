include( FetchContent )

# Dependencies are pinned to exact tags: reproducible builds matter more than
# picking up upstream fixes silently.

FetchContent_Declare( CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG        v2.7.2
  GIT_SHALLOW    TRUE
  SYSTEM )

set( SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE )
FetchContent_Declare( spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.17.0
  GIT_SHALLOW    TRUE
  SYSTEM )

FetchContent_MakeAvailable( CLI11 spdlog )

if( NGA_BUILD_TESTS )
  FetchContent_Declare( Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.15.3
    GIT_SHALLOW    TRUE
    SYSTEM )
  FetchContent_MakeAvailable( Catch2 )
  list( APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras" )
endif( )
