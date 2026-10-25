#pragma once

#include "nga/diag/SourceManager.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Evaluate.hpp"
#include "nga/model/Merge.hpp"

#include <cstdint>
#include <optional>
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

/// Where in its own file a span stands, which is what a consumer can use: the
/// SourceManager's offsets are positions in a space it lays every file out
/// in. Nothing for a span in no file, which is what a Section the tool built
/// has.
std::optional<SymbolSite> siteOf( diag::SourceManager const& sources, diag::SourceSpan span );

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

  /// Whether another Module reaches it by its name alone, which is what an
  /// editor offers a name from another Module for.
  bool exported = false;
  SymbolSite definition;
  std::vector<SymbolSite> uses;

  /// The Section a Label stands in, named as the memory map names it, so
  /// that a reader can look up its Phases there; empty for anything else.
  std::string section{};

  /// Where a Label stands, which is a linear address when its Section stands
  /// in a unit set. Nothing where the build has no Layout, or where Prune
  /// dropped the Section and nothing gave it one.
  std::optional<std::int64_t> address{};

  /// What a Constant evaluates to, where it evaluates to anything.
  std::optional<std::int64_t> value{};

  /// The Pane a Label's Section is in and the state its Window shows it in,
  /// as the map has them; empty for a Section in `fixed`.
  std::string pane{};
  std::uint32_t paneState = 0;

  /// The unit set a Label's Section stands in, which is what says `address`
  /// is linear; empty for any other — see
  /// docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
  std::string units{};

  /// Which Symbol of the build this is, for placeSymbols; nothing writes it.
  SymbolRef where{};
};

/// Every Symbol the program defines, with every place its name is written,
/// in definition order: Module by Module, and inside a Module the order the
/// names were declared.
///
/// A use is a place the **name is written** and resolves: what a Chunk
/// encodes, a Constant's value, a Section's attributes, the condition of every
/// branch of `.if` and `.match`, a macro's body, an `.assert`, a `.export`
/// list, `.endp then`, `in`, `under` and `as`, `.implements`, `.transform`,
/// `.driver`, a character set's base and a literal's prefix, and in the
/// Project file the Label a Phase starts at, a Label `load` calls, the Label
/// a `transform` names and the Window of a `panes` block — every place a
/// rename has to reach. A local label is not here: it names a position in its
/// own Module and has no Symbol to be a use of.
///
/// Read off a build that is merged and **not yet expanded**: Expand empties
/// the branches a condition excluded, and a name written there is written all
/// the same. What the Symbols stand at and evaluate to is placeSymbols's.
std::vector<SymbolFacts> symbolsOf( Merged const& build );

/// Where each Label stands and what each Constant evaluates to, asked of
/// `known`, which a finished build answers — see SymbolFacts.
void placeSymbols( std::vector<SymbolFacts>& facts, Merged const& build, ValueSource& known );

} // namespace nga::model
