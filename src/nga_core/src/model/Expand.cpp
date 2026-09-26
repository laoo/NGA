#include "nga/model/Expand.hpp"

#include "nga/model/Evaluate.hpp"
#include "nga/model/Isa.hpp"
#include "nga/model/TypeCheck.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model
{

namespace
{

struct MacroRef
{
  ModuleIndex module;
  MacroIndex macro;
};

/// How many uses deep an expansion may go before it is taken to never end.
/// A recursion's branch dies when its argument says so, and one whose
/// condition never turns false is what this catches — clang's limit for
/// templates, and room for a loop unrolled a thousand times. Depth and not
/// repetition, so a cycle through several macros is caught as a macro using
/// itself is, and by the same count.
constexpr std::uint32_t MAX_EXPANSION_DEPTH = 1024;

/// How many statements one use may expand to. A recursion that uses itself
/// twice per level terminates and is still enormous — 2^30 statements at a
/// depth of thirty, which the depth limit has no reason to stop — so what
/// bounds the memory is a count of what was produced. The number is the
/// target's address space in bytes: an expansion with more statements than
/// that could not be placed if every one of them emitted a single byte.
constexpr std::size_t MAX_EXPANSION_CHUNKS = 65536;

/// A half-open range of Chunk indices a decided conditional gave up on.
using DeadRange = std::pair<std::uint32_t, std::uint32_t>;

bool insideDead( std::span<DeadRange const> dead, std::uint32_t index )
{
  return std::ranges::any_of(
      dead, [index]( DeadRange const& range ) { return index >= range.first && index < range.second; } );
}

/// Whether an instruction's shape is one a macro use may have: an operand or
/// none, since an argument is an expression and `#`, an index register and an
/// indirection are the instruction's business.
bool isArgumentShape( syntax::OperandShape shape )
{
  return shape == syntax::OperandShape::NONE || shape == syntax::OperandShape::DIRECT;
}

class Expander
{
public:
  Expander( diag::SourceManager const& sources,
            GlobalSymbols const& symbols,
            Charsets const& charsets,
            Project const& project,
            std::span<Module> modules,
            diag::DiagnosticSink& sink )
      : mSources( &sources ), mSymbols( &symbols ), mCharsets( &charsets ), mProject( &project ), mModules( modules ),
        mSink( &sink )
  {
  }

  void run();

private:
  struct Frame;

  /// What a parameter stands for: an expression, the frame whose template it
  /// was written in — null for an argument written at a top-level use, which
  /// is the use Module's own text — and its declared value when it has one,
  /// which is what the parameter is then replaced by.
  ///
  /// An argument written `U + c`, `c + U` or `U - c`, with `c` declared and
  /// the whole not, is carried as what U stands for and the sum `offset`
  /// beside it, so that a recursion handing on `at + 1` adds one node however
  /// deep it goes; `written` is the argument as written, which the sum
  /// underlines. See docs/decisions/0070-expression-depth.md.
  struct Argument
  {
    syntax::Expression const* tree = nullptr;
    Frame* frame = nullptr;
    std::optional<std::int64_t> value;
    std::optional<std::int64_t> offset;
    syntax::Expression const* written = nullptr;
  };

  /// What a decided `.match` of the body settled: which `.case` is kept,
  /// and the elements of the pack it matched, which the case's names stand
  /// for — the fixed names one each, the case's own pack the rest.
  struct Matched
  {
    std::size_t branch = 0;
    std::vector<Argument> elements;
  };

  /// One instantiation under way: which macro, what its parameters stand for,
  /// where each of its body's Chunks began among the flat inner Chunks, and
  /// the clones of its body's local label references, bound once the body's
  /// end is known.
  struct Frame
  {
    MacroRef macro;
    MacroDefinition const* definition = nullptr;
    std::vector<Argument> arguments;
    std::vector<std::uint32_t> starts;
    std::vector<std::pair<syntax::Expression const*, ChunkIndex>> pendingLocals;

    /// Per conditional of the body, in the body's order: decided, when it
    /// is a `.match` that was.
    std::vector<std::optional<Matched>> matched;

    /// The body's Chunks its conditionals gave up on, and the next Chunk
    /// to instantiate.
    std::vector<DeadRange> dead;
    std::uint32_t next = 0;
  };

  /// The use being expanded: where its Chunk stands.
  struct Use
  {
    ModuleIndex module;
    SectionIndex section;
    ChunkIndex chunk;
  };

  void expandSection( ModuleIndex module, SectionIndex index );

  /// What one side of a `.with` expands to: the driver's `show` or `showAt`
  /// for the Window, and what it is handed.
  struct WithExpansion
  {
    MacroRef macro;
    std::vector<Argument> arguments;
  };

  /// One side of a `.with`: resolves what it names to a Window and what of
  /// it is shown, chooses the driver's `show` or `showAt` for that Window,
  /// and answers with the macro and its argument — the state to show on
  /// the way in and, on the way out, what was shown where the statement
  /// stands: the Section's own Pane, or the Window's base — or with nothing
  /// where the side was refused, which it reported, or has no driver to
  /// expand. `where` is the statement's own Chunk, or the use a body's
  /// `.with` is instantiated at, whose Section is what the exit asks;
  /// `what` the entry's item, cloned where the `.with` stands in a body;
  /// `from` and `scope` where the name is looked up. The Windows shown so
  /// far on the statement are `mOpenWiths`, through bodies as over one
  /// line. See docs/decisions/0055-with.md,
  /// 0068-with-shows-again-what-was-shown.md and 0096-panes-in-c.md.
  std::optional<WithExpansion> resolveWith( Use const& where,
                                            MacroUseContent& use,
                                            syntax::Expression const* what,
                                            diag::SourceSpan span,
                                            ModuleIndex from,
                                            std::string_view scope );

  /// A Chunk a `.with` put around a statement of the Section's own text:
  /// resolved, and expanded in place.
  void expandWith( ModuleIndex module, SectionIndex index, std::uint32_t chunkIndex );

  /// A finding about a `.with`: at the `.with`, and for one in a macro body
  /// once per use, with the use noted, since what the exit shows again and
  /// what the entry changes are the use's Section's.
  void reportWith( diag::Diagnostic finding ) const;

  /// A macro under a `.with` may not return: it would leave the Window as
  /// shown. Asked of every wrapped use once the Section is expanded, and of
  /// every statement under a `.with` of a body inside one.
  void checkWithReturns( ModuleIndex module, SectionIndex index );

  /// The base a Window has across every Phase the Module is present in —
  /// one, or nothing, which is reported at `at`: code present in Phases
  /// that give the Window different bases cannot show one of them again.
  /// The variant's base for a Module in no Phase. See
  /// docs/decisions/0056-a-phase-chooses-a-base.md.
  std::optional<std::uint32_t> baseAcrossResidency( ModuleIndex module, WindowIndex window, diag::SourceSpan at );

  /// The same, in silence, or nothing where the Phases differ.
  [[nodiscard]] std::optional<std::uint32_t> baseIfOneAcrossResidency( ModuleIndex module, WindowIndex window ) const;
  void expandUse( Use const& use, MacroRef macro, std::vector<Argument> arguments );

  /// Appends the body of `macro`, instantiated for `arguments`, to `flat`,
  /// and the bodies of every use it holds in their places. `at` is the use
  /// the whole expansion begins at.
  ///
  /// The instantiations under way are a stack of Frames held here and not
  /// the call stack: how deep an expansion may go is MAX_EXPANSION_DEPTH, a
  /// rule of the language, and must not be whatever a thread's stack holds
  /// under one compiler — a frame is kilobytes in a debug build, and Windows
  /// gives the main thread one megabyte.
  void instantiate(
      Use const& use, MacroRef macro, std::vector<Argument> arguments, syntax::Token at, std::vector<Chunk>& flat );

  /// Pushes an instantiation of `macro` for `arguments` onto `stack`, its
  /// body's conditionals decided — or pushes nothing, for a use refused.
  /// `at` is the name that asked, where arity and depth are reported;
  /// `outer` the use the whole expansion began at. The depth is the height
  /// of the stack.
  void enter( Use const& use,
              MacroRef macro,
              std::vector<Argument> arguments,
              syntax::Token at,
              syntax::Token outer,
              std::deque<Frame>& stack,
              std::vector<Chunk> const& flat );

  /// Answers an `.if`'s branches in order and says which is kept. A
  /// condition without a declared value is reported and its branch is not
  /// taken, so a conditional nobody could decide keeps nothing rather than
  /// guessing. `read` says what a condition is evaluated as: the Section's
  /// own node, or the body's cloned with a frame's arguments in it.
  template <typename Read>
  std::optional<std::size_t> decideIf( ModuleIndex home, Conditional const& conditional, Read const& read );

  /// Answers a `.match` of the body: the first `.case` whose pattern fits
  /// the elements of the pack the subject names, recorded in the frame for
  /// the names the case bound. None fitting is reported at the use.
  std::optional<std::size_t> decideMatch( Frame& frame,
                                          MacroDefinition const& definition,
                                          std::size_t index,
                                          Conditional const& conditional,
                                          syntax::Token at );

  /// Decides every conditional, outside in — a conditional inside a branch
  /// already given up on is never asked — and returns the ranges given up
  /// on. `decide` answers one, given its index among the conditionals.
  template <typename Decide>
  std::vector<DeadRange> decideAll( std::span<Conditional const> conditionals, Decide const& decide );

  /// The elements a pack stands for in `frame`: the macro's own, which are
  /// the arguments its fixed parameters did not take, or a case's, which are
  /// what its fixed names did not take of what it matched. Empty for a node
  /// that names no pack.
  static std::span<Argument const>
  elementsOf( Frame const& frame, MacroDefinition const& definition, syntax::Expression const& node );

  /// The argument as the body reads it: its value where it has one, and a
  /// clone of its expression otherwise.
  syntax::ExpressionPtr substitute( Use const& use, Argument const& argument, syntax::Expression const& at );

  /// The items of a body Chunk as this expansion reads them: each cloned, a
  /// spread replaced by the elements it names.
  std::vector<syntax::ExpressionPtr>
  itemsOf( Use const& use, Frame& frame, MacroDefinition const& definition, Chunk const& templated );

  /// The arguments a nested use hands on: each item an argument of its own,
  /// a spread the elements it names, which are arguments already.
  std::vector<Argument> argumentsOf(
      Use const& use, Frame& frame, MacroDefinition const& definition, Chunk const& templated, std::size_t skip );

  /// What a use names: a macro, or nothing, which has been reported — or
  /// nothing in silence, for a role of a driver the program does not have,
  /// which one finding for the run has said. A role that names a Window
  /// reads it from the use's first item, `first`, and says so in `skip`,
  /// since the Window is not an argument the macro sees.
  std::optional<MacroRef> resolve( ModuleIndex from,
                                   std::string_view scope,
                                   std::span<syntax::Token const> path,
                                   syntax::Token name,
                                   syntax::Expression const* first,
                                   std::size_t& skip );

  /// The macro a plain name reaches, in silence, for a finding that has to
  /// know whether there is one before it says which mistake was made.
  [[nodiscard]] bool namesMacro( ModuleIndex from, std::string_view scope, std::string_view name ) const;

  /// An argument as the body will read it, with its declared value when it
  /// has one. One written in a body is read through the enclosing frame,
  /// which is a clone; the clone is kept in the arena, since every binding
  /// it made is keyed by its nodes' addresses.
  Argument makeArgument( Use const& use, syntax::Expression const& tree, Frame* frame );

  /// The declared value of `node` as the use reads it, if it has one: read
  /// through a clone when it was written in a body, and the clone measured
  /// against the depth limit before anything else reads it.
  std::optional<std::int64_t> valueOf( Use const& use, syntax::Expression const& node, Frame* frame );

  /// What `node`, written in `frame`, stands for: the argument a parameter's
  /// or a `.case`'s name was given, or the node itself.
  Argument carriedBy( Frame* frame, syntax::Expression const& node );

  /// Whether a tree Expand is about to hand on nests deeper than an
  /// expression may, which gives up on the use as a runaway does: once,
  /// underlining `at` in the body, with a note at the use.
  bool refuseTooDeep( Use const& use, syntax::Expression const& tree, diag::SourceSpan at );

  /// A deep copy of `node` as the use's Module will read it: a parameter
  /// replaced by its argument, every name recorded as belonging to the Module
  /// whose text it came from, and a local label of a body left for its frame
  /// to bind. `frame` is the instantiation the node's text belongs to, null
  /// for the use Module's own text; `source` is that text's Module.
  syntax::ExpressionPtr clone( Use const& use, syntax::Expression const& node, Frame* frame, ModuleIndex source );

  [[nodiscard]] Module& moduleAt( ModuleIndex index )
  {
    return mModules[index.value];
  }

  [[nodiscard]] std::string_view textOf( syntax::Token token ) const
  {
    return mSources->textOf( token.span() );
  }

  [[nodiscard]] std::optional<std::int64_t> declaredValue( ModuleIndex home, syntax::Expression const& node ) const
  {
    return declaredValueOf( *mSources, *mSymbols, mCharsets, home, node );
  }

  void report( diag::Diagnostic value ) const
  {
    mSink->add( std::move( value ) );
  }

  diag::SourceManager const* mSources;
  GlobalSymbols const* mSymbols;
  Charsets const* mCharsets;
  Project const* mProject;
  std::span<Module> mModules;
  diag::DiagnosticSink* mSink;

  /// Whether a role was used with no driver to bind it: said once per run.
  bool mRoleWithoutDriver = false;

  /// The Windows shown so far on the statement being expanded, in order:
  /// pushed by each entry, popped by its exit, and nothing for an entry
  /// that was refused. Through a body's `.with` as through the Section's.
  std::vector<std::optional<WindowIndex>> mOpenWiths;

  /// While a body's `.with` is resolved: the use the expansion began at,
  /// which a finding about the `.with` notes.
  std::optional<syntax::Token> mWithOuter;

  /// Whether the expansion under way has been given up on. A use that ran
  /// away is reported once and then abandoned whole: reporting a branch and
  /// carrying on with its siblings is exponential in the depth, which is the
  /// shape of the very expansion being refused.
  bool mAbandoned = false;
};

template <typename Read>
std::optional<std::size_t> Expander::decideIf( ModuleIndex home, Conditional const& conditional, Read const& read )
{
  for ( std::size_t index = 0; index < conditional.branches.size(); ++index )
  {
    Branch const& branch = conditional.branches[index];
    if ( branch.condition == nullptr )
    {
      // `.else`: reached only because no branch before it was taken.
      return index;
    }

    syntax::Expression const& condition = *read( *branch.condition );
    std::optional<std::int64_t> const value = declaredValue( home, condition );
    if ( !value.has_value() )
    {
      report( diag::diagnostic( diag::DiagnosticId::CONDITION_NOT_DECLARED )
                  .at( condition.span.begin, condition.span.length ) );
      continue;
    }
    if ( *value != 0 )
    {
      return index;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> Expander::decideMatch( Frame& frame,
                                                  MacroDefinition const& definition,
                                                  std::size_t index,
                                                  Conditional const& conditional,
                                                  syntax::Token at )
{
  // The subject's elements: the macro's own pack, or the pack of an
  // enclosing `.case`, which an earlier conditional of the body decided —
  // outside in is what guarantees it was. A subject that names no pack was
  // refused where it was written, and keeps nothing here.
  std::span<Argument const> elements;
  if ( conditional.subjectCase.has_value() )
  {
    CaseBinding const& binding = *conditional.subjectCase;
    std::optional<Matched> const& outer = frame.matched[binding.conditional];
    if ( !outer.has_value() || outer->branch != binding.branch )
    {
      return std::nullopt;
    }
    std::span<Argument const> const all = outer->elements;
    elements =
        all.subspan( definition.body.conditionals()[binding.conditional].branches[binding.branch].pattern.fixed() );
  }
  else if ( definition.parameters.pack )
  {
    elements = std::span<Argument const>{ frame.arguments }.subspan( definition.parameters.fixed() );
  }
  else
  {
    return std::nullopt;
  }

  for ( std::size_t branch = 0; branch < conditional.branches.size(); ++branch )
  {
    if ( conditional.branches[branch].pattern.fits( elements.size() ) )
    {
      frame.matched[index] =
          Matched{ .branch = branch, .elements = std::vector<Argument>{ elements.begin(), elements.end() } };
      return branch;
    }
  }

  syntax::Token const subject = conditional.subject.value_or( at );
  report( diag::diagnostic( diag::DiagnosticId::NO_CASE_FITS )
              .at( at.location, at.length )
              .arg( "name", std::string{ textOf( subject ) } )
              .arg( "count", static_cast<std::int64_t>( elements.size() ) )
              .note( diag::diagnostic( diag::DiagnosticId::MATCH_HERE ).at( subject.location, subject.length ) ) );
  return std::nullopt;
}

template <typename Decide>
std::vector<DeadRange> Expander::decideAll( std::span<Conditional const> conditionals, Decide const& decide )
{
  std::vector<DeadRange> dead;
  for ( std::size_t index = 0; index < conditionals.size(); ++index )
  {
    Conditional const& conditional = conditionals[index];
    if ( conditional.branches.empty() || insideDead( dead, conditional.branches.front().first.value ) )
    {
      continue;
    }
    std::optional<std::size_t> const chosen = decide( index, conditional );
    for ( std::size_t branch = 0; branch < conditional.branches.size(); ++branch )
    {
      if ( chosen.has_value() && *chosen == branch )
      {
        continue;
      }
      Branch const& gone = conditional.branches[branch];
      dead.emplace_back( gone.first.value, gone.last.value );
    }
  }
  return dead;
}

bool Expander::namesMacro( ModuleIndex from, std::string_view scope, std::string_view name ) const
{
  std::optional<SymbolRef> const found = mSymbols->lookupScoped( from, scope, name );
  return found.has_value() && mSymbols->at( *found ).kind == SymbolKind::MACRO;
}

std::optional<MacroRef> Expander::resolve( ModuleIndex from,
                                           std::string_view scope,
                                           std::span<syntax::Token const> path,
                                           syntax::Token name,
                                           syntax::Expression const* first,
                                           std::size_t& skip )
{
  std::string_view const text = textOf( name );
  skip = 0;

  // The name as Merge reads any name: from the scope outwards, this Module's
  // own Symbol before an exported one at each step. A Symbol of another
  // kind under the name is what the use reaches, and is refused as such
  // rather than skipped for a macro further out: `lda fill` and `fill 1, 2`
  // in one Module must not read two different `fill`s.
  enum class Found : std::uint8_t
  {
    MACRO,
    OTHER,
    NONE,
  };
  auto const asMacro = [this, from, scope, name]( std::string_view full, std::optional<MacroRef>& macro )
  {
    std::optional<SymbolRef> const found = mSymbols->lookupScoped( from, scope, full );
    if ( !found.has_value() )
    {
      return Found::NONE;
    }
    Symbol const& symbol = mSymbols->at( *found );
    if ( symbol.kind != SymbolKind::MACRO )
    {
      report( diag::diagnostic( diag::DiagnosticId::NOT_A_MACRO )
                  .at( name.location, name.length )
                  .arg( "name", std::string{ full } )
                  .arg( "kind", std::string{ nameOf( symbol.kind ) } ) );
      return Found::OTHER;
    }
    macro = MacroRef{ .module = found->module, .macro = std::get<MacroIndex>( symbol.value ) };
    return Found::MACRO;
  };

  std::optional<MacroRef> macro;
  if ( path.empty() )
  {
    if ( asMacro( text, macro ) != Found::NONE )
    {
      return macro;
    }
    report( diag::diagnostic( diag::DiagnosticId::NOT_INSTRUCTION_NOR_MACRO )
                .at( name.location, name.length )
                .arg( "name", std::string{ text } ) );
    return std::nullopt;
  }

  if ( path.size() != 1 || textOf( path.front() ) != "nga" )
  {
    // A Namespace of the program's own: the qualified name, looked for from
    // the use's scope outwards like any name.
    std::string space;
    for ( syntax::Token const& segment : path )
    {
      if ( !space.empty() )
      {
        space += '.';
      }
      space += textOf( segment );
    }
    std::string const dotted = space + "." + std::string{ text };
    if ( asMacro( dotted, macro ) != Found::NONE )
    {
      return macro;
    }
    if ( !mSymbols->isNamespace( space ) && !mSymbols->isNamespace( std::string{ scope } + space ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::NO_SUCH_NAMESPACE )
                  .at( path.front().location, path.front().length )
                  .arg( "space", space ) );
      return std::nullopt;
    }
    report( diag::diagnostic( diag::DiagnosticId::NOT_INSTRUCTION_NOR_MACRO )
                .at( name.location, name.length )
                .arg( "name", dotted ) );
    return std::nullopt;
  }

  // The tool's namespace: the driver's roles, bound from its declaration.
  std::optional<Driver> const& driver = mProject->driver;
  std::optional<MacroIndex> role;
  if ( text == "open" )
  {
    role = driver.has_value() ? std::optional{ driver->open } : std::nullopt;
  }
  else if ( text == "read" )
  {
    role = driver.has_value() ? std::optional{ driver->read } : std::nullopt;
  }
  else if ( text == "show" || text == "showAt" )
  {
    // The Window first, a Symbol of the Target's in scope everywhere, which
    // chooses the driver's macro for it; what follows is the macro's.
    if ( driver.has_value() )
    {
      std::optional<WindowIndex> window;
      if ( first != nullptr && first->kind == syntax::ExpressionKind::NAME )
      {
        std::optional<SymbolRef> const found = mSymbols->lookupScoped( from, scope, textOf( first->token ) );
        if ( found.has_value() && mSymbols->at( *found ).kind == SymbolKind::WINDOW )
        {
          window = std::get<WindowIndex>( mSymbols->at( *found ).value );
        }
      }
      if ( !window.has_value() )
      {
        diag::SourceSpan const at = first != nullptr ? first->span : name.span();
        report( diag::diagnostic( diag::DiagnosticId::SHOW_NEEDS_WINDOW )
                    .at( at.begin, at.length )
                    .arg( "role", std::string{ text } ) );
        return std::nullopt;
      }
      skip = 1;
      WindowRoles const& roles = driver->windows[window->value];
      role = text == "show" ? roles.show : roles.showAt;
    }
  }
  else
  {
    report( diag::diagnostic( diag::DiagnosticId::NO_SUCH_TOOL_NAME )
                .at( name.location, name.length )
                .arg( "name", std::string{ text } ) );
    return std::nullopt;
  }

  if ( !role.has_value() || !driver.has_value() )
  {
    // The routine and the glue use every role, and a program with an edge
    // and no driver has been told so where the driver is resolved; a
    // decoder or the program's own code using one is told once here.
    if ( !mModules[from.value].name().starts_with( "nga." ) && !mRoleWithoutDriver )
    {
      mRoleWithoutDriver = true;
      report( diag::diagnostic( diag::DiagnosticId::ROLE_WITHOUT_DRIVER )
                  .at( name.location, name.length )
                  .arg( "name", std::string{ text } ) );
    }
    return std::nullopt;
  }
  return MacroRef{ .module = driver->module, .macro = *role };
}

Expander::Argument Expander::makeArgument( Use const& use, syntax::Expression const& tree, Frame* frame )
{
  Argument argument{
    .tree = &tree, .frame = frame, .value = valueOf( use, tree, frame ), .offset = std::nullopt, .written = &tree
  };
  bool const additive =
      tree.kind == syntax::ExpressionKind::BINARY &&
      ( tree.binaryOperator == syntax::BinaryOperator::ADD || tree.binaryOperator == syntax::BinaryOperator::SUBTRACT );
  if ( argument.value.has_value() || !additive || mAbandoned )
  {
    return argument;
  }

  // Decided by the shape and by which side is declared, as folding is. `c - U`
  // is not taken: for an Address it is an error, and for an Integer it turns
  // the sign of what is carried.
  syntax::Expression const* anchor = nullptr;
  std::optional<std::int64_t> sum = valueOf( use, *tree.right, frame );
  if ( sum.has_value() )
  {
    anchor = tree.left.get();
    sum = tree.binaryOperator == syntax::BinaryOperator::SUBTRACT ? -*sum : *sum;
  }
  else if ( tree.binaryOperator == syntax::BinaryOperator::ADD )
  {
    sum = valueOf( use, *tree.left, frame );
    anchor = sum.has_value() ? tree.right.get() : nullptr;
  }
  if ( anchor == nullptr || !sum.has_value() )
  {
    return argument;
  }

  // What U stands for and not U itself: U naming a parameter carried as an
  // offset already would otherwise put this sum on top of that one, and the
  // tree would grow by a level at every use after all.
  Argument const carried = carriedBy( frame, *anchor );
  argument.tree = carried.tree;
  argument.frame = carried.frame;
  argument.offset = carried.offset.value_or( 0 ) + *sum;
  return argument;
}

std::optional<std::int64_t> Expander::valueOf( Use const& use, syntax::Expression const& node, Frame* frame )
{
  if ( frame == nullptr )
  {
    return declaredValue( use.module, node );
  }

  // Written in a body, so the enclosing frame's arguments stand in it: what
  // it evaluates to is only known of the clone. Kept, and not only for the
  // bindings: `n - 1` over a VALUE is three nodes whatever the depth, and
  // that is the whole point of folding.
  syntax::ExpressionPtr probe = clone( use, node, frame, frame->macro.module );
  std::optional<std::int64_t> const value =
      refuseTooDeep( use, *probe, node.span ) ? std::nullopt : declaredValue( use.module, *probe );
  std::vector<syntax::ExpressionPtr> kept;
  kept.push_back( std::move( probe ) );
  moduleAt( use.module ).sectionAt( use.section ).appendItems( std::move( kept ) );
  return value;
}

Expander::Argument Expander::carriedBy( Frame* frame, syntax::Expression const& node )
{
  Argument const itself{
    .tree = &node, .frame = frame, .value = std::nullopt, .offset = std::nullopt, .written = &node
  };
  if ( frame == nullptr )
  {
    return itself;
  }

  // A pack's bare name is a count, which is declared, so it never stands
  // where U does; nor does a name of a `.case` that was not kept.
  MacroDefinition const& definition = moduleAt( frame->macro.module ).macroAt( frame->macro.macro );
  if ( auto const parameter = definition.parameterUses.find( &node ); parameter != definition.parameterUses.end() )
  {
    bool const pack = definition.parameters.pack && parameter->second + 1 == definition.parameters.names.size();
    return pack ? itself : frame->arguments[parameter->second];
  }
  if ( auto const bound = definition.caseUses.find( &node ); bound != definition.caseUses.end() )
  {
    CaseBinding const& binding = bound->second;
    std::optional<Matched> const& matched = frame->matched[binding.conditional];
    syntax::Pattern const& pattern =
        definition.body.conditionals()[binding.conditional].branches[binding.branch].pattern;
    bool const pack = pattern.pack && binding.slot + 1 == pattern.names.size();
    if ( matched.has_value() && matched->branch == binding.branch && !pack )
    {
      return matched->elements[binding.slot];
    }
  }
  return itself;
}

bool Expander::refuseTooDeep( Use const& use, syntax::Expression const& tree, diag::SourceSpan at )
{
  // Every tree measured here was built from a body's, which the parser held
  // to the limit, with arguments already measured standing in it — so none
  // is deeper than twice the limit, and measuring it recursively is safe.
  std::uint32_t const depth = syntax::depthOf( tree );
  if ( depth <= syntax::MAX_EXPRESSION_DEPTH )
  {
    return false;
  }
  if ( !mAbandoned )
  {
    mAbandoned = true;
    syntax::Token const outer =
        std::get<MacroUseContent>( moduleAt( use.module ).sectionAt( use.section ).chunkAt( use.chunk ).content ).name;
    report(
        diag::diagnostic( diag::DiagnosticId::EXPANDED_EXPRESSION_TOO_DEEP )
            .at( at.begin, at.length )
            .arg( "depth", static_cast<std::int64_t>( depth ) )
            .arg( "limit", static_cast<std::int64_t>( syntax::MAX_EXPRESSION_DEPTH ) )
            .note( diag::diagnostic( diag::DiagnosticId::EXPANSION_BEGAN_HERE ).at( outer.location, outer.length ) ) );
  }
  return true;
}

syntax::ExpressionPtr
Expander::clone( Use const& use, syntax::Expression const& node, Frame* frame, ModuleIndex source )
{
  if ( frame != nullptr )
  {
    MacroDefinition const& macro = moduleAt( frame->macro.module ).macroAt( frame->macro.macro );
    if ( auto const parameter = macro.parameterUses.find( &node ); parameter != macro.parameterUses.end() )
    {
      if ( macro.parameters.pack && parameter->second + 1 == macro.parameters.names.size() )
      {
        // A pack's bare name is how many elements it has: a value, and a
        // declared one, so `.if rest` is decided where it stands.
        syntax::ExpressionPtr count = syntax::makeExpression( syntax::ExpressionKind::VALUE, node.token, node.span );
        count->value = static_cast<std::int64_t>( frame->arguments.size() - macro.parameters.fixed() );
        return count;
      }
      return substitute( use, frame->arguments[parameter->second], node );
    }
    if ( auto const bound = macro.caseUses.find( &node ); bound != macro.caseUses.end() )
    {
      // A name a `.case` bound: the case was kept, or the node would lie in a
      // branch not taken and never be cloned.
      CaseBinding const& binding = bound->second;
      std::optional<Matched> const& matched = frame->matched[binding.conditional];
      syntax::Pattern const& pattern = macro.body.conditionals()[binding.conditional].branches[binding.branch].pattern;
      if ( !matched.has_value() || matched->branch != binding.branch )
      {
        return syntax::makeExpression( syntax::ExpressionKind::ERROR, node.token, node.span );
      }
      if ( pattern.pack && binding.slot + 1 == pattern.names.size() )
      {
        syntax::ExpressionPtr count = syntax::makeExpression( syntax::ExpressionKind::VALUE, node.token, node.span );
        count->value = static_cast<std::int64_t>( matched->elements.size() - pattern.fixed() );
        return count;
      }
      return substitute( use, matched->elements[binding.slot], node );
    }
  }

  syntax::ExpressionPtr copy = syntax::makeExpression( node.kind, node.token, node.span );
  copy->parenthesised = node.parenthesised;
  copy->fromRoot = node.fromRoot;
  copy->unaryOperator = node.unaryOperator;
  copy->binaryOperator = node.binaryOperator;
  copy->value = node.value;

  switch ( node.kind )
  {
  case syntax::ExpressionKind::NAME:
  case syntax::ExpressionKind::ATTRIBUTE:
  case syntax::ExpressionKind::STRING:
  case syntax::ExpressionKind::CHARACTER:
    // Read in the Module whose text it is, which is the macro's for a body
    // and the use's for an argument.
    if ( source != use.module )
    {
      moduleAt( use.module ).bindHome( copy.get(), source );
    }
    if ( std::optional<std::string_view> const scope = moduleAt( source ).scopeOf( &node ); scope.has_value() )
    {
      moduleAt( use.module ).bindScope( copy.get(), *scope );
    }
    break;
  case syntax::ExpressionKind::LOCAL_NAME:
  {
    std::optional<LabelPosition> const target = moduleAt( source ).localTarget( &node );
    if ( !target.has_value() )
    {
      break;
    }
    if ( target->section == MACRO_BODY_SECTION && frame != nullptr )
    {
      // A local of a body: its position inside the use's Chunk is known once
      // the body's end is, so the frame binds it then.
      frame->pendingLocals.emplace_back( copy.get(), target->chunk );
    }
    else
    {
      // A local of the use's own Proc, handed in as an argument: a position
      // that already exists.
      moduleAt( use.module ).bindLocal( copy.get(), *target );
    }
    break;
  }
  default:
    break;
  }

  if ( node.left != nullptr )
  {
    copy->left = clone( use, *node.left, frame, source );
  }
  if ( node.right != nullptr )
  {
    copy->right = clone( use, *node.right, frame, source );
  }
  return copy;
}

syntax::ExpressionPtr Expander::substitute( Use const& use, Argument const& argument, syntax::Expression const& at )
{
  if ( argument.value.has_value() )
  {
    // Folded: one node carrying the number, underlining what the author
    // wrote as the argument, wherever that was.
    syntax::ExpressionPtr value =
        syntax::makeExpression( syntax::ExpressionKind::VALUE, argument.tree->token, argument.tree->span );
    value->value = *argument.value;
    return value;
  }
  if ( argument.tree == nullptr )
  {
    return syntax::makeExpression( syntax::ExpressionKind::ERROR, at.token, at.span );
  }

  // The argument's text belongs to whoever wrote the use: the enclosing
  // template, whose frame is still on the stack below this one, or the use
  // Module itself.
  ModuleIndex const owner = argument.frame == nullptr ? use.module : argument.frame->macro.module;
  syntax::ExpressionPtr carried = clone( use, *argument.tree, argument.frame, owner );
  if ( !argument.offset.has_value() )
  {
    return carried;
  }

  // The sum beside what it is added to, both underlining the argument as it
  // was written where it was last handed on.
  syntax::Expression const& written = *argument.written;
  syntax::ExpressionPtr sum = syntax::makeExpression( syntax::ExpressionKind::BINARY, written.token, written.span );
  sum->binaryOperator = syntax::BinaryOperator::ADD;
  sum->left = std::move( carried );
  sum->right = syntax::makeExpression( syntax::ExpressionKind::VALUE, written.token, written.span );
  sum->right->value = *argument.offset;
  return sum;
}

std::span<Expander::Argument const>
Expander::elementsOf( Frame const& frame, MacroDefinition const& definition, syntax::Expression const& node )
{
  if ( auto const parameter = definition.parameterUses.find( &node ); parameter != definition.parameterUses.end() )
  {
    if ( definition.parameters.pack && parameter->second + 1 == definition.parameters.names.size() )
    {
      return std::span<Argument const>{ frame.arguments }.subspan( definition.parameters.fixed() );
    }
    return {};
  }
  if ( auto const bound = definition.caseUses.find( &node ); bound != definition.caseUses.end() )
  {
    CaseBinding const& binding = bound->second;
    std::optional<Matched> const& matched = frame.matched[binding.conditional];
    syntax::Pattern const& pattern =
        definition.body.conditionals()[binding.conditional].branches[binding.branch].pattern;
    if ( matched.has_value() && matched->branch == binding.branch && pattern.pack &&
         binding.slot + 1 == pattern.names.size() )
    {
      return std::span<Argument const>{ matched->elements }.subspan( pattern.fixed() );
    }
  }
  return {};
}

std::vector<syntax::ExpressionPtr>
Expander::itemsOf( Use const& use, Frame& frame, MacroDefinition const& definition, Chunk const& templated )
{
  std::vector<syntax::ExpressionPtr> items;
  for ( syntax::ExpressionPtr const& item : definition.body.itemsOf( templated ) )
  {
    if ( item->kind != syntax::ExpressionKind::SPREAD )
    {
      items.push_back( clone( use, *item, &frame, frame.macro.module ) );
      refuseTooDeep( use, *items.back(), item->span );
      continue;
    }
    for ( Argument const& element : elementsOf( frame, definition, *item ) )
    {
      items.push_back( substitute( use, element, *item ) );
    }
  }
  return items;
}

std::vector<Expander::Argument> Expander::argumentsOf(
    Use const& use, Frame& frame, MacroDefinition const& definition, Chunk const& templated, std::size_t skip )
{
  std::vector<Argument> arguments;
  for ( syntax::ExpressionPtr const& item : definition.body.itemsOf( templated ).subspan( skip ) )
  {
    if ( item->kind != syntax::ExpressionKind::SPREAD )
    {
      arguments.push_back( makeArgument( use, *item, &frame ) );
      continue;
    }
    // Handed on as they are: each was folded, or not, at the use that gave
    // it, and its text still belongs to whoever wrote that.
    std::span<Argument const> const elements = elementsOf( frame, definition, *item );
    arguments.insert( arguments.end(), elements.begin(), elements.end() );
  }
  return arguments;
}

void Expander::enter( Use const& use,
                      MacroRef macro,
                      std::vector<Argument> arguments,
                      syntax::Token at,
                      syntax::Token outer,
                      std::deque<Frame>& stack,
                      std::vector<Chunk> const& flat )
{
  if ( mAbandoned )
  {
    return;
  }
  auto const depth = static_cast<std::uint32_t>( stack.size() );
  if ( depth > MAX_EXPANSION_DEPTH )
  {
    mAbandoned = true;
    report(
        diag::diagnostic( diag::DiagnosticId::MACRO_EXPANDS_WITHOUT_END )
            .at( at.location, at.length )
            .arg( "name", std::string{ textOf( at ) } )
            .arg( "depth", static_cast<std::int64_t>( depth ) )
            .note( diag::diagnostic( diag::DiagnosticId::EXPANSION_BEGAN_HERE ).at( outer.location, outer.length ) ) );
    return;
  }
  if ( flat.size() > MAX_EXPANSION_CHUNKS )
  {
    mAbandoned = true;
    report(
        diag::diagnostic( diag::DiagnosticId::MACRO_EXPANDS_TOO_MUCH )
            .at( at.location, at.length )
            .arg( "count", static_cast<std::int64_t>( flat.size() ) )
            .note( diag::diagnostic( diag::DiagnosticId::EXPANSION_BEGAN_HERE ).at( outer.location, outer.length ) ) );
    return;
  }

  MacroDefinition const& definition = moduleAt( macro.module ).macroAt( macro.macro );
  if ( !definition.parameters.fits( arguments.size() ) )
  {
    report( diag::diagnostic( definition.parameters.pack ? diag::DiagnosticId::MACRO_ARITY_AT_LEAST
                                                         : diag::DiagnosticId::MACRO_ARITY )
                .at( at.location, at.length )
                .arg( "name", std::string{ textOf( at ) } )
                .arg( "expected", static_cast<std::int64_t>( definition.parameters.fixed() ) )
                .arg( "given", static_cast<std::int64_t>( arguments.size() ) ) );
    return;
  }

  // A nested use's arguments point at the frames below it, and a deque's
  // elements stay where they are while it grows and shrinks at the back: a
  // frame is popped only after every frame above it, so it outlives every
  // pointer to it.
  Section const& body = definition.body;
  Frame& frame =
      stack.emplace_back( Frame{ .macro = macro,
                                 .definition = &definition,
                                 .arguments = std::move( arguments ),
                                 .starts = {},
                                 .pendingLocals = {},
                                 .matched = std::vector<std::optional<Matched>>( body.conditionals().size() ),
                                 .dead = {},
                                 .next = 0 } );

  Section& target = moduleAt( use.module ).sectionAt( use.section );

  // The body's conditionals first, with this use's arguments in them, which
  // is what lets a macro choose a branch by what it was given — and decided
  // before any Chunk is cloned, so that a use in a branch not taken is never
  // expanded, which is what lets a macro use itself. The clone of a
  // condition is kept: the bindings it made are keyed by its nodes.
  auto const read = [&]( syntax::Expression const& condition )
  {
    syntax::ExpressionPtr copy = clone( use, condition, &frame, macro.module );
    refuseTooDeep( use, *copy, condition.span );
    syntax::Expression const* const cloned = copy.get();
    std::vector<syntax::ExpressionPtr> kept;
    kept.push_back( std::move( copy ) );
    target.appendItems( std::move( kept ) );
    return cloned;
  };
  frame.dead = decideAll( body.conditionals(),
                          [&]( std::size_t index, Conditional const& conditional )
                          {
                            return conditional.isMatch() ? decideMatch( frame, definition, index, conditional, at )
                                                         : decideIf( use.module, conditional, read );
                          } );
}

void Expander::instantiate(
    Use const& use, MacroRef macro, std::vector<Argument> arguments, syntax::Token at, std::vector<Chunk>& flat )
{
  std::deque<Frame> stack;
  enter( use, macro, std::move( arguments ), at, at, stack, flat );

  // Depth first, in the order the recursion it replaces had: a frame goes on
  // from where it stood once every frame a nested use pushed is done. A use
  // given up on stops only what is still to be entered; the frames under
  // way finish their bodies, and what they produce is dropped by the caller.
  Section& target = moduleAt( use.module ).sectionAt( use.section );
  while ( !stack.empty() )
  {
    Frame& frame = stack.back();
    MacroDefinition const& definition = *frame.definition;
    std::span<Chunk const> const chunks = definition.body.chunks();
    if ( frame.next == chunks.size() )
    {
      frame.starts.push_back( static_cast<std::uint32_t>( flat.size() ) );
      for ( auto const& [reference, chunk] : frame.pendingLocals )
      {
        moduleAt( use.module )
            .bindLocal(
                reference,
                LabelPosition{ .section = use.section, .chunk = use.chunk, .inner = frame.starts[chunk.value] } );
      }
      stack.pop_back();
      continue;
    }

    std::uint32_t const index = frame.next++;
    Chunk const& templated = chunks[index];
    frame.starts.push_back( static_cast<std::uint32_t>( flat.size() ) );

    if ( insideDead( frame.dead, index ) )
    {
      // A branch not taken: one Chunk of nothing per Chunk of the body, so
      // that every position the body counts in still exists.
      flat.push_back( Chunk{ .content = ReserveContent{}, .span = templated.span, .firstItem = 0, .itemCount = 0 } );
      continue;
    }

    // A `.with` of the body, either side of its statement: kept as a Chunk
    // of nothing among the use's inner Chunks, so that the closure check and
    // the check on returns see where the statement begins and ends, and
    // followed by what the side expands to, as a nested use is. The exit
    // asks the use's Section what it shows again, since the body stands
    // there — see docs/decisions/0096-panes-in-c.md.
    if ( auto const* const named = std::get_if<MacroUseContent>( &templated.content );
         named != nullptr && named->side != WithSide::NONE )
    {
      std::vector<syntax::ExpressionPtr> items = itemsOf( use, frame, definition, templated );
      syntax::Expression const* const what = items.empty() ? nullptr : items.front().get();
      target.appendItems( std::move( items ) );
      flat.push_back( Chunk{ .content = templated.content, .span = templated.span, .firstItem = 0, .itemCount = 0 } );
      auto& marker = std::get<MacroUseContent>( flat.back().content );
      mWithOuter = at;
      std::optional<WithExpansion> expansion =
          resolveWith( use, marker, what, templated.span, frame.macro.module, definition.scope );
      mWithOuter.reset();
      if ( expansion.has_value() )
      {
        enter( use, expansion->macro, std::move( expansion->arguments ), named->name, at, stack, flat );
      }
      continue;
    }

    // A nested use, in either of the forms a use arrives in: by name with a
    // list, or as an instruction the ISA does not have.
    std::optional<syntax::Token> nested;
    std::span<syntax::Token const> nestedPath;
    if ( auto const* const named = std::get_if<MacroUseContent>( &templated.content ); named != nullptr )
    {
      nested = named->name;
      nestedPath = named->path;
    }
    else if ( auto const* const instruction = std::get_if<InstructionContent>( &templated.content );
              instruction != nullptr && !isMnemonic( textOf( instruction->mnemonic ) ) )
    {
      if ( !isArgumentShape( instruction->shape ) )
      {
        report( diag::diagnostic( diag::DiagnosticId::MACRO_ARGUMENT_SHAPE )
                    .at( templated.span.begin, templated.span.length ) );
        continue;
      }
      nested = instruction->mnemonic;
    }

    if ( nested.has_value() )
    {
      if ( nestedPath.empty() && isMnemonic( textOf( *nested ) ) )
      {
        report( diag::diagnostic( diag::DiagnosticId::INSTRUCTION_GIVEN_LIST )
                    .at( nested->location, nested->length )
                    .arg( "name", std::string{ textOf( *nested ) } ) );
        continue;
      }
      std::span<syntax::ExpressionPtr const> const bodyItems = definition.body.itemsOf( templated );
      std::size_t skip = 0;
      std::optional<MacroRef> const inner = resolve( frame.macro.module,
                                                     definition.scope,
                                                     nestedPath,
                                                     *nested,
                                                     bodyItems.empty() ? nullptr : bodyItems.front().get(),
                                                     skip );
      if ( !inner.has_value() )
      {
        continue;
      }
      enter( use, *inner, argumentsOf( use, frame, definition, templated, skip ), *nested, at, stack, flat );
      continue;
    }

    // An instruction or data of the body: cloned into the use's arena, and a
    // Chunk of the inside pointing at the clones. The span is the body's,
    // which is where a finding about the instruction points.
    std::vector<syntax::ExpressionPtr> items = itemsOf( use, frame, definition, templated );
    auto const count = static_cast<std::uint32_t>( items.size() );
    std::uint32_t const first = target.appendItems( std::move( items ) );
    flat.push_back( Chunk{ .content = templated.content,
                           .span = templated.span,
                           .firstItem = first,
                           .itemCount = count,
                           .taking = templated.taking,
                           .followers = templated.followers } );
  }
}

void Expander::expandUse( Use const& use, MacroRef macro, std::vector<Argument> arguments )
{
  Section& section = moduleAt( use.module ).sectionAt( use.section );
  syntax::Token const at = std::get<MacroUseContent>( section.chunkAt( use.chunk ).content ).name;

  std::vector<Chunk> flat;
  mAbandoned = false;
  instantiate( use, macro, std::move( arguments ), at, flat );
  if ( mAbandoned )
  {
    // Nothing of a use that ran away: what it did produce is a prefix of
    // something wrong, and every Step after this would walk it.
    flat.clear();
  }
  section.expand( use.chunk, std::move( flat ) );
}

void Expander::expandSection( ModuleIndex module, SectionIndex index )
{
  Section& section = moduleAt( module ).sectionAt( index );

  // The Section's own conditionals first: a use in a branch not taken is
  // blanked here and never looked at below.
  std::vector<DeadRange> const dead =
      decideAll( section.conditionals(),
                 [&]( std::size_t, Conditional const& conditional )
                 { return decideIf( module, conditional, []( syntax::Expression const& c ) { return &c; } ); } );
  for ( DeadRange const& range : dead )
  {
    for ( std::uint32_t chunk = range.first; chunk < range.second; ++chunk )
    {
      section.blank( ChunkIndex{ chunk } );
    }
  }

  mOpenWiths.clear();

  for ( std::uint32_t chunkIndex = 0; chunkIndex < section.chunks().size(); ++chunkIndex )
  {
    Chunk& chunk = section.chunkAt( ChunkIndex{ chunkIndex } );

    // A mnemonic the ISA does not have is a use with an operand or none.
    if ( auto const* const instruction = std::get_if<InstructionContent>( &chunk.content ); instruction != nullptr )
    {
      std::string_view const name = textOf( instruction->mnemonic );
      if ( isMnemonic( name ) )
      {
        continue;
      }
      // A use from here on, whatever comes of it: no Step after this one
      // meets a mnemonic the ISA does not have. One that names no macro,
      // or has the shape of an instruction's operand, is reported and left
      // unexpanded, which is a Chunk of nothing.
      syntax::Token const mnemonic = instruction->mnemonic;
      bool const wellShaped = isArgumentShape( instruction->shape );
      chunk.content = MacroUseContent{ .path = {},
                                       .name = mnemonic,
                                       .form = WithForm::NONE,
                                       .side = WithSide::NONE,
                                       .state = std::nullopt,
                                       .window = std::nullopt,
                                       .pane = std::nullopt,
                                       .shownState = std::nullopt };

      // Reported for a name that is a macro; one that is not is reported
      // below as neither, which says more.
      if ( !wellShaped && namesMacro( module, section.scope(), name ) )
      {
        report(
            diag::diagnostic( diag::DiagnosticId::MACRO_ARGUMENT_SHAPE ).at( chunk.span.begin, chunk.span.length ) );
        continue;
      }
    }

    auto const* const use = std::get_if<MacroUseContent>( &chunk.content );
    if ( use == nullptr )
    {
      continue;
    }
    if ( use->side != WithSide::NONE )
    {
      expandWith( module, index, chunkIndex );
      continue;
    }
    std::string_view const name = textOf( use->name );
    if ( use->path.empty() && isMnemonic( name ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::INSTRUCTION_GIVEN_LIST )
                  .at( use->name.location, use->name.length )
                  .arg( "name", std::string{ name } ) );
      continue;
    }
    std::span<syntax::ExpressionPtr const> const items = section.itemsOf( chunk );
    std::size_t skip = 0;
    std::optional<MacroRef> const macro =
        resolve( module, section.scope(), use->path, use->name, items.empty() ? nullptr : items.front().get(), skip );
    if ( !macro.has_value() )
    {
      continue;
    }

    // The arguments are the use's items, whose nodes stay where they are
    // while the arena grows behind them — less the Window a role took.
    Use const where{ .module = module, .section = index, .chunk = ChunkIndex{ chunkIndex } };
    std::vector<Argument> arguments;
    for ( syntax::ExpressionPtr const& item : items.subspan( skip ) )
    {
      arguments.push_back( makeArgument( where, *item, nullptr ) );
    }
    expandUse( where, *macro, std::move( arguments ) );
  }
  checkWithReturns( module, index );
}

void Expander::reportWith( diag::Diagnostic finding ) const
{
  if ( mWithOuter.has_value() )
  {
    finding = std::move( finding ).note(
        diag::diagnostic( diag::DiagnosticId::EXPANSION_BEGAN_HERE ).at( mWithOuter->location, mWithOuter->length ) );
  }
  report( std::move( finding ) );
}

void Expander::expandWith( ModuleIndex module, SectionIndex index, std::uint32_t chunkIndex )
{
  Section& section = moduleAt( module ).sectionAt( index );
  Chunk& chunk = section.chunkAt( ChunkIndex{ chunkIndex } );
  auto& use = std::get<MacroUseContent>( chunk.content );
  Use const where{ .module = module, .section = index, .chunk = ChunkIndex{ chunkIndex } };
  std::span<syntax::ExpressionPtr const> const items = section.itemsOf( chunk );
  std::optional<WithExpansion> expansion =
      resolveWith( where, use, items.empty() ? nullptr : items.front().get(), chunk.span, module, section.scope() );
  if ( expansion.has_value() )
  {
    expandUse( where, expansion->macro, std::move( expansion->arguments ) );
  }
}

std::optional<Expander::WithExpansion> Expander::resolveWith( Use const& where,
                                                              MacroUseContent& use,
                                                              syntax::Expression const* what,
                                                              diag::SourceSpan span,
                                                              ModuleIndex from,
                                                              std::string_view scope )
{
  Section& section = moduleAt( where.module ).sectionAt( where.section );
  Target const& target = mProject->target;
  syntax::Token const at = use.name;

  // The exit shows again what was shown where the statement stands — the
  // Pane the Section is in, where that is a Pane of the Window the matching
  // entry resolved to, and the Window's base otherwise — and nothing where
  // that entry could not be resolved, which it reported. See
  // docs/decisions/0068-with-shows-again-what-was-shown.md.
  if ( use.side == WithSide::LEAVE )
  {
    std::optional<WindowIndex> window;
    if ( !mOpenWiths.empty() )
    {
      window = mOpenWiths.back();
      mOpenWiths.pop_back();
    }
    if ( !window.has_value() || !mProject->driver.has_value() )
    {
      return std::nullopt;
    }
    Driver const& driver = *mProject->driver;
    WindowIndex const which = *window;
    // A Proc that runs under a Pane shows it again, where that is a Pane of
    // this Window; every other Section is in the Window's base state, since
    // the entry refused a Section in a Pane of the Window.
    std::optional<PaneIndex> const home = section.under();
    bool const homeShown = home.has_value() && target.panes[home->value].window == which;
    use.window = window;
    std::vector<Argument> arguments;
    std::optional<syntax::Token> const paneName = section.underName();
    if ( homeShown && !target.panes[home->value].state.has_value() && paneName.has_value() )
    {
      // The Pane by name, as the entry hands its Pane: the value is the
      // solver's, and the name is the one `under PANE` wrote on the Proc.
      syntax::ExpressionPtr name = syntax::makeExpression( syntax::ExpressionKind::NAME, *paneName, span );
      syntax::Expression const* const kept = name.get();
      std::vector<syntax::ExpressionPtr> items;
      items.push_back( std::move( name ) );
      section.appendItems( std::move( items ) );
      arguments.emplace_back( makeArgument( where, *kept, nullptr ) );
    }
    else
    {
      std::optional<std::uint32_t> const state =
          homeShown ? target.panes[home->value].state : baseAcrossResidency( where.module, which, span );
      if ( !state.has_value() )
      {
        return std::nullopt;
      }
      syntax::ExpressionPtr value = syntax::makeExpression( syntax::ExpressionKind::VALUE, at, span );
      value->value = static_cast<std::int64_t>( *state );
      syntax::Expression const* const kept = value.get();
      std::vector<syntax::ExpressionPtr> items;
      items.push_back( std::move( value ) );
      section.appendItems( std::move( items ) );
      arguments.emplace_back( makeArgument( where, *kept, nullptr ) );
    }
    return WithExpansion{ .macro = MacroRef{ .module = driver.module, .macro = driver.windows[which.value].show },
                          .arguments = std::move( arguments ) };
  }

  // The entry: what was named, typed as the type check will type it — a
  // Pane, a family or its member — or a Window by its Symbol.
  std::optional<WindowIndex> window;
  std::optional<PaneIndex> pane;
  std::optional<std::uint32_t> state;
  std::string kind = "name the program does not know";
  bool bareFamily = false;
  if ( what != nullptr && what->kind == syntax::ExpressionKind::NAME )
  {
    std::optional<SymbolRef> const found = mSymbols->lookupScoped( from, scope, textOf( what->token ) );
    if ( found.has_value() && mSymbols->at( *found ).kind == SymbolKind::WINDOW )
    {
      window = std::get<WindowIndex>( mSymbols->at( *found ).value );
      kind = "window";
    }
    else if ( found.has_value() && mSymbols->at( *found ).kind != SymbolKind::PANE )
    {
      kind = std::string{ nameOf( mSymbols->at( *found ).kind ) };
    }
  }
  if ( !window.has_value() && what != nullptr )
  {
    diag::SeverityPolicy policy;
    diag::DiagnosticSink quiet{ policy };
    Merged const merged{ *mSources, *mProject, *mSymbols, *mCharsets };
    Type const type = typeOf( merged, from, *what, quiet );
    if ( type.is( syntax::ExpressionType::PANE ) )
    {
      pane = type.pane;
      window = target.panes[type.pane.value].window;
      kind = "pane";
      bareFamily = what->kind == syntax::ExpressionKind::NAME && type.paneCount > 1;
    }
  }

  // Each form takes its own kind of name: a Pane or a member for SHOW, a
  // family or a Window for AT, a Window for STATE.
  bool const fits = window.has_value() && ( ( use.form == WithForm::SHOW && pane.has_value() && !bareFamily ) ||
                                            ( use.form == WithForm::AT && ( !pane.has_value() || bareFamily ) ) ||
                                            ( use.form == WithForm::STATE && !pane.has_value() ) );
  if ( !fits )
  {
    diag::SourceSpan const named = what != nullptr ? what->span : span;
    reportWith( diag::diagnostic( diag::DiagnosticId::WITH_FORM )
                    .at( named.begin, named.length )
                    .arg( "name", std::string{ mSources->textOf( named ) } )
                    .arg( "kind", bareFamily ? std::string{ "family" } : kind ) );
    mOpenWiths.emplace_back( std::nullopt );
    return std::nullopt;
  }
  WindowIndex const found = *window;
  Window const& shown = target.windows[found.value];
  if ( use.form == WithForm::STATE && use.state.has_value() )
  {
    syntax::Token const stateToken = *use.state;
    std::string_view const name = textOf( stateToken );
    state = shown.namedStateOf( name, target.unitSets );
    if ( !state.has_value() )
    {
      reportWith( diag::diagnostic( diag::DiagnosticId::WITH_STATE_UNKNOWN )
                      .at( stateToken.location, stateToken.length )
                      .arg( "state", std::string{ name } )
                      .arg( "window", shown.name ) );
      mOpenWiths.emplace_back( std::nullopt );
      return std::nullopt;
    }
  }
  else if ( pane.has_value() && target.panes[pane->value].state.has_value() )
  {
    state = target.panes[pane->value].state;
  }

  // One state at a time: a second `.with` on the same Window over one
  // statement would hide what the first showed — through a macro body as
  // over one line, since the statement of a `.with` in the body runs while
  // the use's own `.with` shows its state.
  if ( std::ranges::find( mOpenWiths, window ) != mOpenWiths.end() )
  {
    reportWith( diag::diagnostic( diag::DiagnosticId::WITH_NESTED_SAME_WINDOW )
                    .at( span.begin, span.length )
                    .arg( "window", shown.name ) );
    mOpenWiths.emplace_back( std::nullopt );
    return std::nullopt;
  }
  // Code in a Pane of the Window cannot switch the Window: the switch would
  // take the code with it. A Proc in `fixed` declared `under PANE` does it —
  // see docs/decisions/0098-a-proc-declares-what-is-shown.md.
  std::optional<PaneIndex> const inPane = section.pane();
  bool const ownWindow = inPane.has_value() && target.panes[inPane->value].window == *window;
  bool const ownPane = ownWindow && pane == inPane && target.panes[inPane->value].count == 1;
  if ( ownWindow && !ownPane )
  {
    reportWith( diag::diagnostic( diag::DiagnosticId::WITH_FROM_OWN_WINDOW )
                    .at( span.begin, span.length )
                    .arg( "pane", target.panes[inPane->value].name )
                    .arg( "window", shown.name ) );
    mOpenWiths.emplace_back( std::nullopt );
    return std::nullopt;
  }
  // Against what is shown where the statement stands: the Pane the Proc runs
  // under, where it is one of this Window, the Window's base otherwise.
  std::optional<PaneIndex> const home = section.under();
  bool changesNothing = ownPane;
  if ( ownPane )
  {
    // The Pane's own code showing its Pane: nothing changes, and it is told.
  }
  else if ( !home.has_value() || target.panes[home->value].window != *window )
  {
    changesNothing = state.has_value() && state == baseIfOneAcrossResidency( where.module, *window );
  }
  else if ( pane.has_value() )
  {
    changesNothing = pane == home;
  }
  else
  {
    changesNothing = state.has_value() && state == target.panes[home->value].state;
  }
  if ( changesNothing )
  {
    reportWith( diag::diagnostic( diag::DiagnosticId::WITH_DOES_NOTHING )
                    .at( span.begin, span.length )
                    .arg( "window", shown.name ) );
  }
  use.window = window;
  use.pane = pane;
  use.shownState = state;
  mOpenWiths.push_back( window );

  if ( !mProject->driver.has_value() )
  {
    if ( !mRoleWithoutDriver )
    {
      mRoleWithoutDriver = true;
      report( diag::diagnostic( diag::DiagnosticId::ROLE_WITHOUT_DRIVER )
                  .at( at.location, at.length )
                  .arg( "name", std::string{ use.form == WithForm::AT ? "showAt" : "show" } ) );
    }
    return std::nullopt;
  }
  // What `show` is handed: the Pane itself, whose value the solver decides,
  // or the state's index where it is known already — a named state, or a
  // Pane pinned to one — which a driver may then do arithmetic on.
  Driver const& driver = *mProject->driver;
  WindowRoles const& roles = driver.windows[window->value];
  std::vector<Argument> arguments;
  if ( use.form == WithForm::SHOW && !state.has_value() )
  {
    arguments.emplace_back( makeArgument( where, *what, nullptr ) );
  }
  else if ( use.form != WithForm::AT )
  {
    syntax::ExpressionPtr value = syntax::makeExpression( syntax::ExpressionKind::VALUE, at, span );
    value->value = static_cast<std::int64_t>( state.value_or( 0 ) );
    syntax::Expression const* const kept = value.get();
    std::vector<syntax::ExpressionPtr> keptItems;
    keptItems.push_back( std::move( value ) );
    section.appendItems( std::move( keptItems ) );
    arguments.emplace_back( makeArgument( where, *kept, nullptr ) );
  }
  return WithExpansion{ .macro = MacroRef{ .module = driver.module,
                                           .macro = use.form == WithForm::AT ? roles.showAt : roles.show },
                        .arguments = std::move( arguments ) };
}

std::optional<std::uint32_t> Expander::baseIfOneAcrossResidency( ModuleIndex module, WindowIndex window ) const
{
  Residency const& residency = mSymbols->moduleAt( module ).residency();
  std::optional<std::uint32_t> one;
  bool any = false;
  for ( std::uint32_t phase = 0; phase < residency.phaseCount(); ++phase )
  {
    if ( !residency.includes( PhaseIndex{ phase } ) )
    {
      continue;
    }
    std::optional<std::uint32_t> const here = baseIn( *mProject, PhaseIndex{ phase }, window );
    if ( any && here != one )
    {
      return std::nullopt;
    }
    one = here;
    any = true;
  }
  return any ? one : mProject->target.windows[window.value].base;
}

std::optional<std::uint32_t>
Expander::baseAcrossResidency( ModuleIndex module, WindowIndex window, diag::SourceSpan at )
{
  (void)at;
  Window const& shown = mProject->target.windows[window.value];
  Residency const& residency = mSymbols->moduleAt( module ).residency();
  std::optional<std::uint32_t> one;
  std::optional<std::uint32_t> first;
  for ( std::uint32_t phase = 0; phase < residency.phaseCount(); ++phase )
  {
    if ( !residency.includes( PhaseIndex{ phase } ) )
    {
      continue;
    }
    std::optional<std::uint32_t> const here = baseIn( *mProject, PhaseIndex{ phase }, window );
    if ( first.has_value() && here != one )
    {
      auto const stateName = [&shown, this]( std::optional<std::uint32_t> state )
      {
        std::uint32_t index = 0;
        for ( WindowState const& candidate : shown.states )
        {
          if ( !candidate.units.has_value() && state == index )
          {
            return candidate.name;
          }
          index += candidate.units.has_value() ? mProject->target.unitSets[candidate.units->value].count : 1;
        }
        return std::string{ "none" };
      };
      reportWith( diag::diagnostic( diag::DiagnosticId::WITH_BASES_DIFFER )
                      .at( at.begin, at.length )
                      .arg( "window", shown.name )
                      .arg( "state", stateName( one ) )
                      .arg( "phase", mProject->phases.phases[*first].name.value_or( "(implicit)" ) )
                      .arg( "other", stateName( here ) )
                      .arg( "otherPhase", mProject->phases.phases[phase].name.value_or( "(implicit)" ) ) );
      return std::nullopt;
    }
    if ( !first.has_value() )
    {
      first = phase;
      one = here;
    }
  }
  // Nothing where the Window has no base: what it shows outside a `.with` is
  // undefined, so there is nothing to show again and the exit is no
  // instructions at all — see
  // docs/decisions/0217-a-window-without-a-base-is-switched-and-never-restored.md.
  return first.has_value() ? one : shown.base;
}

void Expander::checkWithReturns( ModuleIndex module, SectionIndex index )
{
  Section const& section = moduleAt( module ).sectionAt( index );
  bool underWith = false;
  for ( std::uint32_t chunkIndex = 0; chunkIndex < section.chunks().size(); ++chunkIndex )
  {
    Chunk const& chunk = section.chunks()[chunkIndex];
    auto const* const use = std::get_if<MacroUseContent>( &chunk.content );
    if ( use != nullptr && use->side == WithSide::ENTER )
    {
      // A `.with` on a Window with no base emits no exit, so a return under it
      // skips nothing and leaves the Window exactly as the model says it is:
      // switched, and whatever the last `.with` made it.
      underWith = use->window.has_value() && mProject->target.windows[use->window->value].base.has_value();
      continue;
    }
    if ( use != nullptr && use->side == WithSide::LEAVE )
    {
      underWith = false;
      continue;
    }
    if ( use == nullptr )
    {
      // The statement under the `.with` is an instruction of its own: a
      // return leaves the Window as shown exactly as one in a macro does.
      auto const* const bare = std::get_if<InstructionContent>( &chunk.content );
      if ( underWith && bare != nullptr )
      {
        std::string_view const mnemonic = textOf( bare->mnemonic );
        if ( mnemonic == "rts" || mnemonic == "rti" )
        {
          report( diag::diagnostic( diag::DiagnosticId::WITH_RETURNS )
                      .at( chunk.span.begin, chunk.span.length )
                      .arg( "mnemonic", std::string{ mnemonic } ) );
        }
      }
      continue;
    }
    // Inside the use: a `.with` of a body opens a statement of its own,
    // which the return is then reported at, with the use noted.
    std::vector<diag::SourceSpan> inner;
    for ( Chunk const& one : section.innerChunksOf( ChunkIndex{ chunkIndex } ) )
    {
      if ( auto const* const marker = std::get_if<MacroUseContent>( &one.content ); marker != nullptr )
      {
        if ( marker->side == WithSide::ENTER )
        {
          inner.push_back( one.span );
        }
        else if ( marker->side == WithSide::LEAVE && !inner.empty() )
        {
          inner.pop_back();
        }
        continue;
      }
      auto const* const instruction = std::get_if<InstructionContent>( &one.content );
      if ( instruction == nullptr || ( !underWith && inner.empty() ) )
      {
        continue;
      }
      std::string_view const mnemonic = textOf( instruction->mnemonic );
      if ( mnemonic != "rts" && mnemonic != "rti" )
      {
        continue;
      }
      if ( inner.empty() )
      {
        report( diag::diagnostic( diag::DiagnosticId::WITH_RETURNS )
                    .at( chunk.span.begin, chunk.span.length )
                    .arg( "mnemonic", std::string{ mnemonic } ) );
      }
      else
      {
        report( diag::diagnostic( diag::DiagnosticId::WITH_RETURNS )
                    .at( inner.back().begin, inner.back().length )
                    .arg( "mnemonic", std::string{ mnemonic } )
                    .note( diag::diagnostic( diag::DiagnosticId::EXPANSION_BEGAN_HERE )
                               .at( use->name.location, use->name.length ) ) );
      }
      break;
    }
  }
}

void Expander::run()
{
  for ( std::uint32_t module = 0; module < mModules.size(); ++module )
  {
    for ( std::uint32_t section = 0; section < mModules[module].sections().size(); ++section )
    {
      expandSection( ModuleIndex{ module }, SectionIndex{ section } );
    }
  }
}

} // namespace

void expand( diag::SourceManager const& sources,
             GlobalSymbols const& symbols,
             Charsets const& charsets,
             Project const& project,
             std::span<Module> modules,
             diag::DiagnosticSink& sink )
{
  Expander expander{ sources, symbols, charsets, project, modules, sink };
  expander.run();
}

} // namespace nga::model
