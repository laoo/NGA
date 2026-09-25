#pragma once

#include "nga/diag/SourceManager.hpp"
#include "nga/model/Build.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nga::model
{

/// Where a name stands in a file, which is what a reader clicks on.
struct SymbolSite
{
  diag::FileId file;
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
};

/// One Symbol of the program: where it was defined and everywhere it is
/// reached from.
///
/// A use is a place the **name** is written, and it is attributed to the
/// Symbol that name resolves to and no further. A Constant `operand = label +
/// 1` used in a Chunk is a use of `operand`, not of `label`: a reader clicking
/// it wants the Constant, which is what they can see.
///
/// That is the one thing here that differs from
/// [walkReferences](References.hpp), which follows a Constant through to the
/// memory it reaches, because Prune and the type check ask what a Chunk
/// encodes rather than what its author wrote.
struct SymbolFacts
{
  std::string name;
  std::string module;
  std::string kind;
  SymbolSite definition;
  std::vector<SymbolSite> uses;
};

/// Every Symbol the program defines, with its uses, in definition order:
/// Module by Module, and inside a Module the order the names were declared.
///
/// Read off a finished build and deciding nothing, as the MemoryMap is. A
/// local label is not here: it names a position in its own Module and has no
/// Symbol to be a use of.
std::vector<SymbolFacts> symbolsOf( Merged const& build );

} // namespace nga::model
