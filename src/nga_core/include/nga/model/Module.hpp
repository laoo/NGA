#pragma once

#include "nga/diag/DiagnosticId.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Charset.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Section.hpp"
#include "nga/model/Symbol.hpp"
#include "nga/syntax/Expression.hpp"

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nga::model
{

/// A condition checked after Place. It is the one expression that cannot be
/// folded early even in principle, which is what makes it the regression test
/// for that discipline.
struct Assertion
{
  syntax::ExpressionPtr condition;
  diag::SourceSpan span;
};

/// `.implements SLOT, SYMBOL` as written: two names, resolved by Merge, since
/// the Slot may be declared in a Module this one never saw.
struct Implementation
{
  std::string_view slot;
  std::string_view symbol;
  diag::SourceSpan slotSpan;
  diag::SourceSpan span;

  /// The Namespace the directive stood in — `one.` — and so where both
  /// names are looked for first; empty at the top level. See 0045.
  std::string_view scope;
};

/// `.proc NAME, as TYPE` as written: the Proc, and the function type whose
/// Temporaries hold its arguments — see docs/decisions/0065-handlers.md.
/// Nothing is resolved here: the type is a Proc of another Module as often as
/// not.
struct Member
{
  SectionIndex section;
  std::string_view type;
  diag::SourceSpan span;

  /// The Namespace the `.proc` stood in, and so where the type is looked for
  /// first; empty at the top level.
  std::string_view scope;
};

/// `.transform FORMAT LABEL` as written: a decoder of the format named, at a
/// Label of this Module. Numbered and dispatched to at the end of Assemble,
/// once every Module's declarations are in hand — see
/// docs/spec/transition.md.
struct TransformDeclaration
{
  std::string_view format;

  /// As written, and once the Module is read to the end, the qualified name
  /// of the Symbol it resolved to in the Namespace it stood in.
  std::string_view label;
  diag::SourceSpan formatSpan;
  diag::SourceSpan labelSpan;
  diag::SourceSpan span;
  std::string_view scope;
};

/// `.driver ROLE LABEL` as written: what the storage driver provides, and
/// where — or, for `window`, the range it occupies, as two expressions the
/// end of Assemble evaluates. See docs/spec/transition.md.
struct DriverRole
{
  std::string_view role;

  /// The Window a `show`, `showAt` or `stream` names; empty for a role of
  /// the stream's own.
  std::string_view window;
  diag::SourceSpan windowSpan;

  /// The macro, as written, and once the Module is read to the end, the
  /// qualified name of the Symbol it resolved to in the Namespace it stood
  /// in; empty for `stream`, which names no macro.
  std::string_view label;
  diag::SourceSpan roleSpan;
  diag::SourceSpan labelSpan;
  diag::SourceSpan span;
  std::string_view scope;
};

/// `.off CODE` as written: the warning it silences on the statement below,
/// and where that statement begins. Applied at the end of a run, once every
/// Step has reported — see docs/decisions/0039-off.md.
struct Suppression
{
  diag::DiagnosticId id;
  std::string code;

  /// The first token of the statement the `.off` stood before; absent when
  /// nothing followed it.
  std::optional<diag::SourceLocation> statement;
  diag::SourceSpan span;
};

/// `.source "PATH", LINE` as written: where in the source the text from this
/// point on came from, until the next one. Carried to the SourceManager once
/// every Module is assembled, and read only when a finding is rendered — see
/// docs/decisions/0063-source-marks.md.
struct SourceMark
{
  std::string path;
  std::uint32_t line = 0;

  /// Where the mark stands; every position from here to the next mark in
  /// the file is covered by it.
  diag::SourceLocation from;
  diag::SourceSpan span;
};

/// `.macro NAME PATTERN` to `.endm`, assembled once into a template: a
/// Section holding the body's Chunks, placed by nothing and instantiated at
/// each use by Expand. A parameter is a name the body uses as it uses a
/// Constant, and every node that names one is recorded here, so that
/// instantiation substitutes the argument's expression for it without
/// looking at text. The last parameter may be a pack, and a `.case` binds
/// names of its own, recorded the same way. See docs/decisions/0043-macros.md
/// and 0050.
struct MacroDefinition
{
  syntax::Token name;
  syntax::Pattern parameters;
  Section body;

  /// Each node of the body that names a parameter, by the parameter's index.
  std::unordered_map<syntax::Expression const*, std::uint32_t> parameterUses;

  /// Each node of the body that names what a `.case` bound.
  std::unordered_map<syntax::Expression const*, CaseBinding> caseUses;
  diag::SourceSpan span;

  /// The Namespace the definition stood in, where a use inside the body
  /// looks for a name first; empty at the top level.
  std::string_view scope;
};

/// One `.asm` source file, assembled.
///
/// Everything here is produced by one thread from one file, with nothing
/// shared: Modules assemble in parallel and determinism is a hard requirement,
/// so there is no global state to reach for and none to add.
///
/// It carries what the Project said about it — its name and its Residency —
/// so that a Step reading either needs the Module and not the Project, and so
/// that a Section inherits its Residency by construction.
class Module
{
public:
  Module( std::string name, diag::FileId file, Residency residency )
      : mName( std::move( name ) ), mFile( file ), mResidency( std::move( residency ) )
  {
  }

  /// Move-only, said out loud rather than left to be derived.
  ///
  /// A Module owns expressions through unique_ptr, so copying one is never
  /// what anybody meant. Leaving it implicit is not the same thing: a
  /// `std::vector` of a move-only type still *declares* a copy constructor, so
  /// a Module would look copy-constructible, and a container whose move is not
  /// noexcept — which is true of MSVC's `unordered_map` — reaches for that copy
  /// and fails to compile deep inside the standard library.
  Module( Module const& ) = delete;
  Module& operator=( Module const& ) = delete;
  Module( Module&& ) = default;
  Module& operator=( Module&& ) = default;
  ~Module() = default;

  [[nodiscard]] std::string_view name() const
  {
    return mName;
  }

  [[nodiscard]] diag::FileId file() const
  {
    return mFile;
  }

  /// The Residency of every Section here. See
  /// docs/decisions/0016-phases-and-residency.md.
  [[nodiscard]] Residency const& residency() const
  {
    return mResidency;
  }

  /// Whether this Module runs while a Bank is switched in — the Target's
  /// Transition routine — so that none of its Sections may live in the Window.
  [[nodiscard]] bool outsideWindow() const
  {
    return mOutsideWindow;
  }

  void keepOutsideWindow()
  {
    mOutsideWindow = true;
  }

  [[nodiscard]] std::vector<Section> const& sections() const
  {
    return mSections;
  }

  [[nodiscard]] Section& sectionAt( SectionIndex index )
  {
    return mSections[index.value];
  }

  [[nodiscard]] Section const& sectionAt( SectionIndex index ) const
  {
    return mSections[index.value];
  }

  [[nodiscard]] SymbolTable const& symbols() const
  {
    return mSymbols;
  }

  [[nodiscard]] SymbolTable& symbols()
  {
    return mSymbols;
  }

  [[nodiscard]] std::vector<Assertion> const& assertions() const
  {
    return mAssertions;
  }

  [[nodiscard]] std::vector<Member> const& members() const
  {
    return mMembers;
  }

  void addMember( Member what )
  {
    mMembers.push_back( what );
  }

  [[nodiscard]] std::vector<Implementation> const& implementations() const
  {
    return mImplementations;
  }

  [[nodiscard]] std::vector<CharsetDeclaration> const& charsets() const
  {
    return mCharsets;
  }

  [[nodiscard]] std::vector<Suppression> const& suppressions() const
  {
    return mSuppressions;
  }

  [[nodiscard]] std::vector<SourceMark> const& sourceMarks() const
  {
    return mSourceMarks;
  }

  [[nodiscard]] std::vector<TransformDeclaration> const& transforms() const
  {
    return mTransforms;
  }

  [[nodiscard]] std::vector<DriverRole> const& driverRoles() const
  {
    return mDriverRoles;
  }

  /// For the end of Assemble alone, which resolves a declaration's name in
  /// the Namespace it stood in and writes the qualified name back.
  [[nodiscard]] std::vector<TransformDeclaration>& transforms()
  {
    return mTransforms;
  }

  [[nodiscard]] std::vector<DriverRole>& driverRoles()
  {
    return mDriverRoles;
  }

  /// Text of this Module's own that no source file holds: a qualified name,
  /// `one.start`, made of a Namespace's path and a name. Kept for as long as
  /// the Module is, so that a Symbol's name may view into it as it views
  /// into a source file.
  std::string_view intern( std::string text );

  /// The Namespace an expression node was written in — `one.` — where a
  /// name in it is looked for first, then in each enclosing Namespace, then
  /// at the top level. Recorded by Assemble for the nodes inside a
  /// `.namespace` block, and absent for every other: the top level is the
  /// whole search. See docs/decisions/0045-namespaces.md.
  [[nodiscard]] std::optional<std::string_view> scopeOf( syntax::Expression const* node ) const;
  void bindScope( syntax::Expression const* node, std::string_view scope );

  [[nodiscard]] CharsetDeclaration const& charsetAt( CharsetIndex index ) const
  {
    return mCharsets[index.value];
  }

  [[nodiscard]] std::vector<MacroDefinition> const& macros() const
  {
    return mMacros;
  }

  [[nodiscard]] MacroDefinition const& macroAt( MacroIndex index ) const
  {
    return mMacros[index.value];
  }

  /// For Assemble alone, which builds the body as it reads it.
  [[nodiscard]] MacroDefinition& macroAt( MacroIndex index )
  {
    return mMacros[index.value];
  }

  MacroIndex addMacro( MacroDefinition macro );

  /// The Module an expression of an expansion reads its names in, where that
  /// is not the Module the expansion stands in: a node instantiated from a
  /// macro's body names what the macro's Module sees, and one instantiated
  /// from an argument names what the use's Module sees. Written by Expand
  /// for the nodes it clones, and asked by every Step that resolves a name;
  /// absent for every node Assemble read from this Module's text.
  [[nodiscard]] std::optional<ModuleIndex> homeOf( syntax::Expression const* node ) const;
  void bindHome( syntax::Expression const* node, ModuleIndex home );

  SectionIndex addSection( Section section );
  void addAssertion( Assertion assertion );
  void addSuppression( Suppression suppression );
  void addSourceMark( SourceMark mark );
  void addTransform( TransformDeclaration declaration );
  void addDriverRole( DriverRole role );
  void addImplementation( Implementation implementation );
  CharsetIndex addCharset( CharsetDeclaration declaration );

  /// Where a local label reference resolved to, or nothing when it did not —
  /// which has already been reported. Local labels are the one thing that
  /// resolves inside Assemble, and this is the whole of what survives it: past
  /// here a local reference names a position exactly as a Label does, and no
  /// later Step learns that directions exist.
  [[nodiscard]] std::optional<LabelPosition> localTarget( syntax::Expression const* reference ) const;
  void bindLocal( syntax::Expression const* reference, LabelPosition target );

  /// What a diagnostic and the memory map call one of this Module's
  /// Sections: its first Label — a Proc's, a Temporary's, the Cell's
  /// `ngaCurrentPhase`, or whichever a `.section` holds first. One with no
  /// Label is named after the Module, because `(anonymous)` on its own
  /// identifies nothing once there are two of them.
  [[nodiscard]] std::string displayNameOf( SectionIndex index, diag::SourceManager const& sources ) const;

private:
  std::string mName;
  diag::FileId mFile;
  Residency mResidency;
  bool mOutsideWindow = false;

  std::vector<Section> mSections;
  SymbolTable mSymbols;
  std::vector<Assertion> mAssertions;
  std::vector<Implementation> mImplementations;
  std::vector<Member> mMembers;
  std::vector<CharsetDeclaration> mCharsets;
  std::vector<Suppression> mSuppressions;
  std::vector<SourceMark> mSourceMarks;
  std::vector<TransformDeclaration> mTransforms;
  std::vector<DriverRole> mDriverRoles;
  std::vector<MacroDefinition> mMacros;

  std::unordered_map<syntax::Expression const*, LabelPosition> mLocalTargets;
  std::unordered_map<syntax::Expression const*, ModuleIndex> mExpressionHomes;
  std::unordered_map<syntax::Expression const*, std::string_view> mExpressionScopes;

  /// A deque, whose elements never move, so that a view into one is good for
  /// the Module's life.
  std::deque<std::string> mInterned;
};

} // namespace nga::model
