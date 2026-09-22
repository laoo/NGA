#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Module.hpp"
#include "nga/syntax/Builder.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nga::model
{

/// Assemble, as the Builder the `.asm` grammar drives.
///
/// It **records**: Sections, Chunks, Symbols and the facts a later Step will
/// need, and it checks only what one Module can settle alone. Whether a
/// mnemonic exists, whether a name resolves, whether a Section may hold what it
/// was given — none of that is here, because none of it can be answered from
/// one file. Anything not recorded is unrecoverable, since the token stream is
/// not kept.
class ModuleBuilder final : public syntax::Builder
{
public:
  ModuleBuilder( diag::SourceManager const& sources, diag::DiagnosticSink& sink, Module& module );

  void beginSection( syntax::SectionAttributes attributes, diag::SourceSpan span ) override;
  void endSection( diag::SourceSpan span ) override;
  void beginProc( syntax::Token name, syntax::SectionAttributes attributes, diag::SourceSpan span ) override;
  void endProc( std::optional<syntax::Token> then, diag::SourceSpan span ) override;
  void defineLabel( syntax::Token name, diag::SourceSpan span ) override;
  void defineConstant( syntax::Token name, syntax::ExpressionPtr value, diag::SourceSpan span ) override;
  void emitInstruction( syntax::Token mnemonic,
                        syntax::OperandShape shape,
                        syntax::ExpressionPtr operand,
                        diag::SourceSpan span ) override;
  void emitData( syntax::DataWidth width, std::vector<syntax::ExpressionPtr> items, diag::SourceSpan span ) override;
  void reserve( syntax::ExpressionPtr size, diag::SourceSpan span ) override;
  void reserveTemporary( syntax::Token name,
                         syntax::ExpressionPtr size,
                         PlacementClass placement,
                         diag::SourceSpan span ) override;
  void transition( syntax::Token phase, diag::SourceSpan span ) override;
  void dispatch( std::vector<syntax::ExpressionPtr> targets, diag::SourceSpan span ) override;
  void beginMacro( syntax::Token name, syntax::Pattern parameters, diag::SourceSpan span ) override;
  void endMacro( diag::SourceSpan span ) override;
  void useMacro( std::vector<syntax::Token> path,
                 syntax::Token name,
                 std::vector<syntax::ExpressionPtr> arguments,
                 diag::SourceSpan span ) override;
  void beginNamespace( std::vector<syntax::Token> path, diag::SourceSpan span ) override;
  void endNamespace( diag::SourceSpan span ) override;
  void beginConditional( syntax::ExpressionPtr condition, diag::SourceSpan span ) override;
  void nextBranch( syntax::ExpressionPtr condition, diag::SourceSpan span ) override;
  void endConditional( diag::SourceSpan span ) override;
  void beginMatch( syntax::Token subject, diag::SourceSpan span ) override;
  void beginCase( syntax::Pattern pattern, diag::SourceSpan span ) override;
  void endMatch( diag::SourceSpan span ) override;
  void addAssertion( syntax::ExpressionPtr condition, diag::SourceSpan span ) override;
  void exportSymbol( syntax::Token name, diag::SourceSpan span ) override;
  void silence( syntax::Token code, std::optional<diag::SourceLocation> statement, diag::SourceSpan span ) override;
  void markSource( syntax::Token path, syntax::Token line, diag::SourceSpan span ) override;
  void with( model::WithForm form,
             syntax::ExpressionPtr what,
             std::optional<syntax::Token> state,
             diag::SourceSpan span ) override;
  void dropWiths() override;
  void taking( model::Taking kind, std::vector<syntax::Token> followers, diag::SourceSpan span ) override;
  void dropTaking() override;
  void declare( model::Declaring what,
                std::optional<model::DeclaredType> type,
                std::string place,
                diag::SourceSpan span ) override;
  void dropDeclaration() override;
  void declareTransform( syntax::Token format, syntax::Token label, diag::SourceSpan span ) override;
  void declareDriverRole( syntax::Token role, std::vector<syntax::Token> names, diag::SourceSpan span ) override;
  void declareSlot( syntax::Token name, Binding binding, PlacementClass placement, diag::SourceSpan span ) override;
  void implement( syntax::Token slot, syntax::Token symbol, diag::SourceSpan span ) override;
  void declareCharset( syntax::Token name,
                       std::optional<CharsetBase> base,
                       std::vector<CharsetEntry> entries,
                       diag::SourceSpan span ) override;

  /// What a Module can only settle once it has been read to the end: local
  /// label resolution, which needs every definition in a Proc before a
  /// reference into it can choose one, and the exports, which are allowed to
  /// name a Symbol the file defines further down.
  void finish();

private:
  /// A definition, in source order within its Proc.
  struct LocalDefinition
  {
    std::string_view name; ///< empty for the anonymous `@`
    std::uint32_t proc = 0;

    /// Which conditional branch it stands in, or zero for none. A definition
    /// is visible to a reference in the same branch or in one it contains,
    /// and to nothing else: a branch not taken is not there at all.
    std::uint32_t branch = 0;

    /// Written without the `@`, which a macro body allows: such a name is
    /// unique in its body, where an `@` one may repeat and be taken by
    /// direction.
    bool plain = false;
    diag::SourceLocation position;
    LabelPosition target;
  };

  /// A plain name used inside a macro body, which is a label of the body when
  /// the body defines one of that name and an ordinary Symbol otherwise. The
  /// node is settled once the body has been read to its end.
  struct BodyName
  {
    syntax::Expression* node = nullptr;
    std::string_view name;
    std::uint32_t scope = 0;
    std::uint32_t branch = 0;
  };

  /// A use, remembered by the node that carries it. Direction is compared by
  /// position in the source and never by address, which is why the position is
  /// what is kept here.
  struct LocalReference
  {
    syntax::Expression const* node = nullptr;
    std::string_view name;
    std::uint32_t proc = 0;
    std::uint32_t branch = 0;
    diag::SourceLocation position;
    syntax::Direction direction = syntax::Direction::NONE;
  };

  /// The Section a statement belongs to: the open Proc's or the open
  /// `.section`'s. One of them is open, since the grammar refuses a
  /// statement that stands in neither.
  SectionIndex currentSection( diag::SourceSpan span );

  /// Where a statement's Chunk goes: the open macro's body while one is
  /// open, which is a template and no Section of the Module, and the current
  /// Section otherwise.
  Section& currentTarget( diag::SourceSpan span );

  /// Reports and answers true when the open Section is a Temporary, which
  /// holds reservations only.
  bool refusedInTemporary( diag::SourceSpan span );

  void defineSymbol( syntax::Token name, SymbolKind kind, SymbolValue value );
  void defineLocalLabel( syntax::Token name );

  /// A label of the innermost scope that is not a Symbol: `@name` anywhere,
  /// and a plain name in a macro body, where each expansion needs its own and
  /// a name is what a Symbol is identified by. `plain` decides which of the
  /// two spellings this is, and the two may not share a name in one body.
  void defineLocalNamed( syntax::Token name, std::string_view bare, bool plain );

  /// The Namespace statements stand in right now, as a prefix — `one.two.`
  /// — or empty at the top level.
  [[nodiscard]] std::string_view currentScope() const;

  /// A Symbol of this Module by a name written in `scope`: the name under
  /// the scope, then under each enclosing Namespace, then at the top level.
  [[nodiscard]] Symbol const* findScoped( std::string_view scope, std::string_view name ) const;

  /// `dotted` is set on the receiver of a qualified name, whose leftmost name
  /// is read as a Namespace or a Section and never as a label of a body.
  void collectReferences( syntax::Expression* node, bool dotted = false );

  /// Inside a macro body: records every node that names a parameter or what
  /// a `.case` bound, so that instantiation substitutes the argument for it
  /// without reading text. A spread that names no pack is refused here.
  void markParameters( syntax::Expression* node );

  /// What `name` means among the names a body binds right now: a parameter
  /// of the macro, or a name of the open `.case` of an enclosing `.match`,
  /// innermost first.
  struct BoundName
  {
    std::optional<std::uint32_t> parameter;
    std::optional<CaseBinding> caseName;
    bool pack = false;
  };

  [[nodiscard]] std::optional<BoundName> boundName( std::string_view name ) const;

  /// Whether `name` is bound by any `.case` of the open body read so far.
  [[nodiscard]] bool boundByAnyCase( std::string_view name ) const;

  /// A parameter named like a Symbol this Module defines is refused, once
  /// the Module has been read to the end and every Symbol is known.
  void checkParameters();
  void resolveLocalLabels();
  void applyExports();

  /// A decoder's Label is looked up once the Module has been read to the
  /// end, since the declaration may stand above the Proc, and is exported:
  /// the dispatcher the tool builds is another Module.
  void applyTransforms();

  /// A role's macro is looked up once the Module has been read to the end,
  /// as a decoder's Label is; `stream` names a Window and no macro, and
  /// whether the Window exists is asked where the driver is resolved.
  void applyDriverRoles();

  /// A `.with` waiting for its statement: what it named, and where.
  struct PendingWith
  {
    WithForm form = WithForm::NONE;
    syntax::Token directive;
    syntax::ExpressionPtr what;
    std::optional<syntax::Token> state;
    diag::SourceSpan span;
  };

  std::vector<PendingWith> mPendingWiths;

  /// A `.own` or `.root` waiting for the statement below it.
  std::optional<Taking> mPendingTaking;
  std::vector<syntax::Token> mPendingFollowers;

  /// Marks the Chunk just appended under a pending taking, and clears it.
  void applyTaking( Section& holder, ChunkIndex chunk );

  /// The Chunks a `.with` puts around the statement being appended: the
  /// entries before it, in the order written, and the exits after it, in
  /// reverse — see docs/decisions/0055-with.md.
  void enterWiths( diag::SourceSpan span );
  void leaveWiths( diag::SourceSpan span );

  /// `then` names a Proc of this Module, which may be defined below the
  /// `.endp` that names it, so the names are resolved once the Module has
  /// been read to the end — and the rules on the pair are checked there.
  void resolveThens();

  [[nodiscard]] std::string_view textOf( syntax::Token token ) const;
  void report( diag::Diagnostic value ) const;

  diag::SourceManager const* mSources;
  diag::DiagnosticSink* mSink;
  Module* mModule;

  std::optional<SectionIndex> mOpenSection;
  std::optional<SectionIndex> mOpenProcSection;

  /// A `.declare` waiting for the `.ztemp` below it: what it says that
  /// variable is to the open Proc, and the type it wrote, if any.
  struct PendingDeclaration
  {
    Declaring what = Declaring::ARGUMENT;
    std::optional<DeclaredType> type;
    diag::SourceSpan span{};
    std::string place{};
  };

  /// Says the declaration on the open Proc: the reservation below it, where
  /// it has one, and `argued` whether that reservation is an argument
  /// already.
  void applyDeclaration( PendingDeclaration const& pending, std::optional<SectionIndex> temporary, bool& argued );

  /// The `.declare`s waiting for one reservation, in the order they were
  /// written: one, or the two that say a byte is both an argument and the
  /// result — see docs/decisions/0119-one-temporary-carries-two-roles.md.
  std::vector<PendingDeclaration> mPendingDeclarations;

  /// Whether the open `.section` has been given a Label, and where it
  /// opened: a Section is named by its first Label, so one that holds none
  /// is refused when it closes.
  bool mOpenSectionLabelled = false;

  /// Where a Label was defined last, as the position it names: what tells a
  /// `.dispatch` row continuing the one above it from one a Label has cut
  /// off. Absent in a macro body, which holds no `.dispatch`.
  std::optional<std::pair<SectionIndex, ChunkIndex>> mLabelledAt;
  diag::SourceSpan mOpenSectionSpan{};

  /// The scope of local labels: a Proc, or a macro body, which is a scope of
  /// its own closed in both directions. Neither nests, and each is numbered
  /// in the order it opens, which is all a local label needs.
  std::uint32_t mScopeCount = 0;
  std::optional<std::uint32_t> mOpenScope;

  /// The local-label scope of the Proc a Section opened in stands in, while
  /// that Section's own is open.
  std::optional<std::uint32_t> mProcLocalScope;
  std::optional<MacroIndex> mOpenMacro;

  /// One conditional being read: which Section holds it, where in that
  /// Section's list it is, the branch id of the branch open now, and the
  /// branch the whole conditional stands in, which each `.case` opens under.
  struct OpenConditional
  {
    SectionIndex section;
    std::optional<MacroIndex> macro;
    std::size_t index = 0;
    std::uint32_t branch = 0;
    std::uint32_t parent = 0;
  };

  [[nodiscard]] Conditional& openConditionalAt( OpenConditional const& open );

  /// Ends the branch open now at the Chunk that comes next.
  void closeBranch();

  std::vector<OpenConditional> mOpenConditionals;

  /// Per branch id, the branch that contains it; zero is "no branch" and is
  /// its own parent. What the visibility rule for local labels walks.
  std::vector<std::uint32_t> mBranchParents{ 0 };

  [[nodiscard]] std::uint32_t currentBranch() const
  {
    return mOpenConditionals.empty() ? 0U : mOpenConditionals.back().branch;
  }

  /// Whether a definition in `definition` is visible to a reference in
  /// `reference`: the same branch, or one containing it.
  [[nodiscard]] bool branchReaches( std::uint32_t definition, std::uint32_t reference ) const;

  std::vector<LocalDefinition> mLocalDefinitions;
  std::vector<LocalReference> mLocalReferences;
  std::vector<BodyName> mBodyNames;

  /// An export, and the Namespace it stood in, where its name is looked for
  /// first once the Module is read to the end.
  struct Export
  {
    syntax::Token name;
    std::string_view scope;
  };

  std::vector<Export> mExports;
  std::vector<SectionIndex> mThens;

  /// The open Namespaces as prefixes, innermost last, and how many of them
  /// each `.namespace` directive opened, so that its end closes as many.
  std::vector<std::string_view> mScopes;
  std::vector<std::size_t> mScopeDepths;

  /// The scopes a `.proc` opened, as prefixes. A Proc is every procedure and
  /// a `.namespace` block is a deliberate act, so a name inside a Proc may
  /// hide one outside it and a name inside a block may not — the leading dot
  /// is what reaches past either.
  std::unordered_set<std::string_view> mProcScopes;
};

} // namespace nga::model
