#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/ContainerFacts.hpp"
#include "nga/model/MemoryMap.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Symbols.hpp"

#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace nga::model
{

/// One set of the facts of a build, named on `--fact` and separated from the
/// others by what it costs rather than by what it is about.
///
/// The sets a task has not built yet are not declared: a name that answered
/// with nothing would look like a set that is empty, and send a consumer
/// looking for a reason it is not there. See docs/spec/facts.md.
enum class FactSet : std::uint8_t
{
  DIAGNOSTICS, ///< Every finding, whether or not the build survived them.
  PROJECT,     ///< The Project after `include`: Modules, Phases, Target, options.
  MAP,         ///< The MemoryMap whole, which `--map` renders as text.
  SOURCES,     ///< The text of the files that are not on disk, and only those.
  TOKENS,      ///< Every token of every file of the build, classified.
  SYMBOLS,     ///< Every Symbol, where it is defined and where it is reached from.
  BYTES,       ///< The Container read back: what wrote every byte of it.
};

/// The set a `--fact` argument names, or nothing.
std::optional<FactSet> factSetFor( std::string_view name );

/// Every name `--fact` takes, in the order they are written, for a message
/// that has to say what was expected.
std::string factSetNames();

/// Every set there is, which is what `all` asks for.
///
/// Here rather than in the caller, because a list of sets repeated anywhere
/// else is a list that will be short by one the day a set is added -- and
/// `--fact all` answering with less than all of them is the kind of lie
/// nothing else would catch.
std::set<FactSet> everyFactSet();

/// What `--facts` is written from. A pointer is null where the run did not get
/// that far: a Project that failed to load has no Layout, so there is no map,
/// and the document says so by leaving the set out rather than by inventing an
/// empty one.
struct Facts
{
  std::string projectPath;
  Project const* project = nullptr;
  MemoryMap const* map = nullptr;
  std::vector<SymbolFacts> const* symbols = nullptr;
  ContainerFacts const* bytes = nullptr;
};

/// The facts of one build as a single JSON document.
///
/// `diagnostics` is written whenever it is asked for, however the build ended:
/// a finding exists from the first lexical error, where a Section's address
/// exists only if Place succeeded. The exit code says the build failed; this
/// says what the tool learned before it did.
std::string renderFactsJson( diag::SourceManager const& sources,
                             diag::DiagnosticSink const& sink,
                             Facts const& facts,
                             std::set<FactSet> const& wanted );

} // namespace nga::model
