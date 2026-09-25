# One place defining the warning set, applied through an interface target.
add_library( nga_warnings INTERFACE )
add_library( nga::warnings ALIAS nga_warnings )

if( MSVC )
  target_compile_options( nga_warnings INTERFACE /W4 /permissive- /utf-8 )

  # MSVC's CRT deprecates the standard functions that hand back or fill a
  # buffer — `std::getenv` among them — in favour of `_s` variants the other
  # two toolchains do not have. The code stays standard and the opinion is
  # turned off here.
  #
  # This macro and not `/wd4996`: C4996 is also how MSVC reports
  # `[[deprecated]]`, so disabling the number would cost a real warning to be
  # rid of a house style.
  target_compile_definitions( nga_warnings INTERFACE _CRT_SECURE_NO_WARNINGS )

  if( NGA_WERROR )
    target_compile_options( nga_warnings INTERFACE /WX )
  endif( )
else( )
  target_compile_options( nga_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Wunused -Woverloaded-virtual -Wdouble-promotion )

  # A designated initialiser out of declaration order is ill-formed, and GCC and
  # MSVC both reject it outright. clang accepts it as an extension and says so in
  # a warning, which is one warning among many on a developer's machine and no
  # warning at all in a build that is not `-Werror` — so the first machine to
  # refuse the code was a release runner, after the tag had gone out. It is an
  # error here too, for whichever of the three is compiling.
  if( CMAKE_CXX_COMPILER_ID MATCHES "Clang" )
    target_compile_options( nga_warnings INTERFACE -Werror=reorder-init-list )
  endif( )

  if( NGA_WERROR )
    target_compile_options( nga_warnings INTERFACE -Werror )
  endif( )
endif( )
