// NGA targets C++23 across Apple clang, GCC and MSVC, but only the subset all
// three implement. These assertions fail the build the moment someone reaches
// for a feature that is missing on one of the three platforms, instead of
// letting CI discover it later.

#include <array>
#include <bit>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include <version>

static_assert( __cpp_lib_expected >= 202202L, "std::expected is required" );
static_assert( __cpp_lib_span >= 202002L, "std::span is required" );
static_assert( __cpp_lib_filesystem >= 201703L, "std::filesystem is required" );
static_assert( __cpp_lib_bit_cast >= 201806L, "std::bit_cast is required" );
static_assert( __cpp_lib_to_underlying >= 202102L, "std::to_underlying is required" );
static_assert( __cpp_lib_string_contains >= 202011L, "std::string::contains is required" );
static_assert( __cpp_explicit_this_parameter >= 202110L, "deducing this is required" );

// Two differences between the three standard libraries that no feature macro
// reports, both found by MSVC after the other two had accepted the code.

// A container of a move-only type still *declares* a copy constructor, which
// only fails when it is instantiated. A type holding one therefore looks
// copy-constructible, and a container whose move is not noexcept — true of
// MSVC's unordered_map — reaches for that copy and fails deep inside the
// standard library. Owning types say `= delete` rather than leaving it to be
// derived; see nga::model::Module.
static_assert( std::is_copy_constructible_v<std::vector<std::unique_ptr<int>>>,
               "a container of a move-only type still declares a copy constructor" );

// A standard container's iterator is a pointer on libc++ and a class type on
// MSVC, so `auto*` deduces from one and not from the other. Nothing can be
// asserted either way, since both are conforming — which is exactly why it is
// written down: never deduce a raw pointer from an iterator, whatever a linter
// suggests it should be spelled.

// Character sets map Unicode code points, so `char32_t` and `U'x'` are now
// load-bearing. Both are portable; what is not is the encoding a compiler
// assumes for the source file holding a literal like `U'Ż'`. MSVC reads the
// system codepage unless told otherwise, so cmake/Warnings.cmake passes
// `/utf-8` — and this assertion is what fails if that flag is ever dropped,
// rather than a character set quietly mapping the wrong code point.
static_assert( U'Ż' == 0x017B, "source files are UTF-8 on every compiler" );
static_assert( sizeof( char32_t ) == 4, "a code point is held whole" );
