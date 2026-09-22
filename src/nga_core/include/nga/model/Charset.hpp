#pragma once

#include "nga/diag/SourceLocation.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/Token.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace nga::model
{

/// A Charset's position within its Module.
struct CharsetIndex
{
  std::uint32_t value = 0;

  friend bool operator==( CharsetIndex, CharsetIndex ) = default;
};

/// One entry of a `.charset`: a run of code points onto consecutive bytes.
///
/// The characters are kept as the literal that was written rather than as a
/// decoded list, because decoding is the resolving Step's business and a Module
/// is assembled before any of that can be answered.
struct CharsetEntry
{
  syntax::Token characters;    ///< the string literal, in the order written
  syntax::ExpressionPtr start; ///< the byte its first character maps to
};

/// What a derived `.charset` derives from.
struct CharsetBase
{
  syntax::Token name;

  /// Null when the derivation named no transform. There is exactly one
  /// transform and it is `^` — see docs/decisions/0013-charset-declaration.md.
  syntax::ExpressionPtr mask;
};

/// A `.charset` as it was written, before any of it is resolved.
///
/// Nothing here is a mapping yet. A base may be declared below its derivation
/// or exported by a Module this thread never sees, and a start byte is an
/// expression, so the table cannot be built until every Module's Symbols are
/// known. Assemble records; Merge resolves.
struct CharsetDeclaration
{
  syntax::Token name;
  std::optional<CharsetBase> base;
  std::vector<CharsetEntry> entries;
  diag::SourceSpan span;
};

} // namespace nga::model
