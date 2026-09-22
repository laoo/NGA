#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nga::syntax
{

/// The value of a numeric literal.
///
/// Nothing only when the text is not a literal this lexer would accept — a
/// malformed one is rejected where it is read, and so is one too large to be
/// represented, so every NUMBER token has a value and converting one cannot
/// fail. That is the whole reason magnitude is the lexer's business: it makes
/// this total everywhere else.
std::optional<std::int64_t> numericValueOf( std::string_view text );

/// A quoted literal split into what names its character set and what it holds.
struct Quoted
{
  /// Empty when the literal is untranslated.
  std::string_view charset;

  /// Between the quotes, with escapes unresolved.
  std::string_view body;
};

Quoted quotedOf( std::string_view text );

/// How many characters a quoted body holds.
///
/// An escape is one character and so is a multi-byte code point, so
/// `"a\nb"` holds three and `"ŻÓŁW"` holds four rather than the eight bytes
/// its UTF-8 spelling takes. This is what lets a Chunk's size be known before
/// any character set is: exactly one byte per character.
std::uint32_t characterCountOf( std::string_view body );

/// The code points a quoted body holds, escapes resolved.
///
/// Exactly as many as `characterCountOf` counts, which is what the one-byte
/// rule is measured against. Total, and deliberately so: invalid UTF-8 and an
/// escape this language does not have are both lexical errors, and an error
/// stops the pipeline at the end of its Step, so nothing that reaches here has
/// anything left to report.
std::vector<char32_t> codePointsOf( std::string_view body );

/// One code point as the UTF-8 a diagnostic can print.
std::string utf8Of( char32_t codePoint );

/// Length of the UTF-8 sequence at `pos`, or zero if the bytes there are not
/// well-formed. Overlong encodings, surrogates and anything past U+10FFFF are
/// rejected: they are different spellings of the same character, and a source
/// file has room for exactly one. Read by both lexers — the assembler's and
/// the C subset's — since a source file is UTF-8 whichever language it holds.
std::uint32_t utf8LengthAt( std::string_view text, std::uint32_t pos );

/// The value of an untranslated character literal. Nothing when the literal
/// names a character set, which resolves after Merge and not here.
std::optional<std::int64_t> plainCharacterValueOf( std::string_view text );

/// The bytes of an untranslated string literal, escapes resolved. Nothing when
/// the literal names a character set: those bytes come from the table.
std::optional<std::vector<std::uint8_t>> plainBytesOf( std::string_view text );

} // namespace nga::syntax
