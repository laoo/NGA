#pragma once

#include "nga/diag/SourceLocation.hpp"
#include "nga/syntax/Statement.hpp"
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

/// A `.charset` as it was written, before any of it is resolved.
///
/// Nothing here is a mapping yet. A base may be declared below its derivation
/// or exported by a Module this thread never sees, and a start byte is an
/// expression, so the table cannot be built until every Module's Symbols are
/// known. Assemble records; Merge resolves.
struct CharsetDeclaration
{
  syntax::Token name;
  std::optional<syntax::CharsetBase> base;
  std::vector<syntax::CharsetEntry> entries;
  diag::SourceSpan span;
};

} // namespace nga::model
