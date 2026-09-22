#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Project.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nga::model
{

/// Where a Symbol is defined, across the whole program.
struct SymbolRef
{
  ModuleIndex module;

  /// Position in that Module's symbol table, which is definition order.
  std::uint32_t index = 0;

  friend bool operator==( SymbolRef, SymbolRef ) = default;
};

/// The result of Merge: every exported Symbol of the program, by name.
///
/// It holds positions rather than copies, because a Symbol's value is an
/// expression owned by its Module and duplicating that would mean two answers
/// to one question.
/// `.implements` resolved: the Label or Section that fills a Slot, and where
/// it was said, for the rules the type check applies to the set.
struct ResolvedImplementation
{
  SymbolRef target;
  diag::SourceSpan span;
};

class GlobalSymbols
{
public:
  explicit GlobalSymbols( std::span<Module const> modules ) : mModules( modules ) {}

  [[nodiscard]] std::span<Module const> modules() const
  {
    return mModules;
  }

  [[nodiscard]] Module const& moduleAt( ModuleIndex index ) const
  {
    return mModules[index.value];
  }

  [[nodiscard]] std::optional<SymbolRef> find( std::string_view name ) const;

  /// A name as seen from one Module: its own Symbols first, then the exported
  /// ones. A private Symbol of another Module is simply not here, which is what
  /// privacy by default means in practice.
  [[nodiscard]] std::optional<SymbolRef> lookup( ModuleIndex home, std::string_view name ) const;

  /// The Module a node of `from` reads its names in: `from`, unless the node
  /// was instantiated from a macro's body, which names what the macro's
  /// Module sees. Every Step that resolves a name asks this first, so that
  /// none of them can disagree about it — see Module::homeOf.
  [[nodiscard]] ModuleIndex homeOf( ModuleIndex from, syntax::Expression const& node ) const
  {
    return moduleAt( from ).homeOf( &node ).value_or( from );
  }

  /// A name as seen from one Module and one Namespace: under the scope, then
  /// under each enclosing Namespace, then at the top level — own Symbols
  /// first at each step, then exported ones. See
  /// docs/decisions/0045-namespaces.md.
  [[nodiscard]] std::optional<SymbolRef>
  lookupScoped( ModuleIndex home, std::string_view scope, std::string_view name ) const;

  /// `text` read as the node of `from` would read it: in the node's home
  /// Module and in the Namespace it was written in. What every Step that
  /// resolves a name, a dotted name or a literal's prefix asks.
  [[nodiscard]] std::optional<SymbolRef>
  resolveText( ModuleIndex from, syntax::Expression const& node, std::string_view text ) const;

  /// Whether some Symbol of the program lies under this name as a
  /// Namespace, so that a finding can say so instead of "unknown".
  [[nodiscard]] bool isNamespace( std::string_view name ) const
  {
    return mNamespaces.contains( std::string{ name } );
  }

  void noteNamespace( std::string name )
  {
    mNamespaces.insert( std::move( name ) );
  }

  [[nodiscard]] Symbol const& at( SymbolRef where ) const
  {
    return moduleAt( where.module ).symbols().symbols()[where.index];
  }

  /// Null when it was added; where the name already came from otherwise.
  std::optional<SymbolRef> add( std::string_view name, SymbolRef where );

  /// Records that `target` — a Label or Section of its own Module — fills
  /// `slot` wherever that Module is present.
  void addImplementation( SymbolRef slot, ResolvedImplementation implementation );

  /// Every Implementation of a Slot, in Project order.
  [[nodiscard]] std::span<ResolvedImplementation const> implementationsOf( SymbolRef slot ) const;

  [[nodiscard]] std::size_t size() const
  {
    return mByName.size();
  }

private:
  std::span<Module const> mModules;
  std::unordered_map<std::string_view, SymbolRef> mByName;
  std::unordered_set<std::string> mNamespaces;

  /// Per Module, per Symbol: the Implementations of that Symbol when it is a
  /// Slot. Filled lazily, since most Symbols are not.
  std::vector<std::vector<std::vector<ResolvedImplementation>>> mImplementations;
};

/// Which Charset, across the whole program.
struct CharsetRef
{
  ModuleIndex module;
  CharsetIndex charset;

  friend bool operator==( CharsetRef, CharsetRef ) = default;
};

/// One resolved character set: which byte each code point it maps produces.
///
/// A lookup, and not a function: the mapping is arbitrary by design — screen
/// codes are a permutation and a game's font is whatever the artist drew — so
/// there is nothing here to compute. Hash lookup is safe because nothing walks
/// this to produce output; a diagnostic about a character names the character,
/// which the literal supplies in source order.
class Charset
{
public:
  [[nodiscard]] std::optional<std::uint8_t> byteFor( char32_t codePoint ) const;

  /// True when this code point was not mapped before.
  bool map( char32_t codePoint, std::uint8_t byte );

  /// Overwrites, which is what an entry in a derived block does to its base.
  void remap( char32_t codePoint, std::uint8_t byte );

  [[nodiscard]] std::unordered_map<char32_t, std::uint8_t> const& mappings() const
  {
    return mBytes;
  }

private:
  std::unordered_map<char32_t, std::uint8_t> mBytes;
};

/// The second result of Merge: every Charset of the program, resolved.
///
/// It is Merge's rather than a Step of its own because a table is needed
/// before Size runs — `lda screen'A'` is an Integer operand whose value decides
/// the instruction's width — and cannot be built while a Module is assembled,
/// since a base may be exported by a Module this thread never sees. See
/// docs/decisions/0013-charset-declaration.md.
class Charsets
{
public:
  explicit Charsets( std::size_t moduleCount ) : mByModule( moduleCount ) {}

  [[nodiscard]] Charset const& at( CharsetRef where ) const
  {
    return mByModule[where.module.value][where.charset.value];
  }

  [[nodiscard]] std::vector<Charset>& forModule( ModuleIndex module )
  {
    return mByModule[module.value];
  }

private:
  /// Dense, and indexed exactly as the declarations are, so nothing here
  /// depends on the order the work happened to run in.
  std::vector<std::vector<Charset>> mByModule;
};

/// Which Charset a Symbol names, when it names one at all.
std::optional<CharsetRef> charsetOf( GlobalSymbols const& symbols, SymbolRef where );

/// The Section a Symbol stands for on the left of a dot: the one a Label
/// stands in, wherever in it the Label stands, so that `ptr.runtimeSectionSize`
/// is the size of the Section holding `ptr`. Any other kind stands for
/// nothing.
std::optional<SectionRef> sectionNamedBy( GlobalSymbols const& symbols, SymbolRef where );

/// Resolves every declaration into a table, applying derivations. Reports what
/// only a resolved table can settle: a base that is not a character set, a
/// derivation cycle, a code point mapped twice, and a run that leaves the byte.
Charsets
resolveCharsets( diag::SourceManager const& sources, GlobalSymbols const& symbols, diag::DiagnosticSink& sink );

/// The Merge Step. It builds the global symbol table and reports names exported
/// by more than one Module.
///
/// It never concatenates Sections: a Section comes from one Module, so Chunk
/// order is declaration order and determinism costs nothing. Modules are walked
/// in Project order and Symbols in definition order, which is the whole of what
/// makes the result independent of how the work was scheduled.
GlobalSymbols merge( std::span<Module const> modules, diag::DiagnosticSink& sink );

} // namespace nga::model
