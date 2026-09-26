#pragma once

#include "nga/diag/SourceLocation.hpp"
#include "nga/model/Binding.hpp"
#include "nga/model/Charset.hpp"
#include "nga/model/Chunk.hpp"
#include "nga/model/PlacementClass.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/Operand.hpp"
#include "nga/syntax/Token.hpp"

#include <optional>
#include <vector>

namespace nga::syntax
{

/// What `.section` declared beyond a name.
///
/// The pin is an expression rather than an address: `at` completes the
/// placement, and what it completes is a declaration, so it is subject to the
/// same rule as every other operand — see docs/spec/syntax.md.
struct SectionAttributes
{
  model::PlacementClass placement = model::PlacementClass::ABSOLUTE;

  /// Null unless the placement was written `... at EXPRESSION`.
  ExpressionPtr pinnedAddress;

  /// Null unless the placement was written `... align EXPRESSION`: the
  /// Section starts at a multiple of it.
  ExpressionPtr alignment;

  /// Null unless the placement was written `... within EXPRESSION`: the
  /// Section does not cross a multiple of it.
  ExpressionPtr boundary;

  /// Written `, in NAME`: the Pane the Section is in, which the Project
  /// declares — see docs/decisions/0054-panes.md.
  std::optional<Token> pane;

  /// Written `, under NAME`, on a Proc: the Proc is in `fixed`, runs while
  /// the Pane is shown, and shows it again after every `.with` on the Pane's
  /// Window — see docs/decisions/0098-a-proc-declares-what-is-shown.md.
  std::optional<Token> under;

  /// Written `, as NAME`, on a Proc: its arguments are the Temporaries of the
  /// Proc named, a function type whose members it joins — see
  /// docs/decisions/0065-handlers.md.
  std::optional<Token> as;

  /// Written `, movable`: the Section may stand at a different address in
  /// each Phase. See the glossary.
  bool movable = false;

  /// Written `, root`: the hardware reaches the Section with no Reference
  /// from any Chunk, so Prune keeps it. See the glossary.
  bool root = false;

  /// Written `, temporary`: a Temporary, whose bytes are another's while no
  /// Section naming it is active — see docs/decisions/0058-temporary-is-an-attribute.md.
  bool temporary = false;

  /// Written `, readonly`: the Section's bytes never change at run time, and
  /// the pointers its address is taken into are read through. What the tool
  /// cannot see for itself, said where it can be held to — see
  /// docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
  bool readOnly = false;
};

/// The names an argument list is matched against: the parameters of a
/// `.macro`, or a `.case`. When `pack` is set the last name is a pack,
/// written `name...`, and takes every argument the names before it do not,
/// none included. See docs/decisions/0050-packs.md.
struct Pattern
{
  std::vector<Token> names;
  bool pack = false;

  /// How many arguments the names before the pack take.
  [[nodiscard]] std::size_t fixed() const
  {
    return pack ? names.size() - 1 : names.size();
  }

  /// Whether a list of `count` arguments fits: exactly the fixed names, or
  /// at least them when there is a pack to take the rest.
  [[nodiscard]] bool fits( std::size_t count ) const
  {
    return pack ? count >= fixed() : count == fixed();
  }
};

/// What a grammar hands its results to.
///
/// There is no syntax tree: the parser recognises a statement and calls one of
/// these, with arguments that are already typed — see
/// docs/decisions/0009-assemble-builds-model-objects.md. Every operand arrives
/// as an Expression and never as a number, so folding one early would require
/// adding an overload here, which is visible in a way that an `evaluate()`
/// buried in a parser is not.
///
/// Implementations validate. The parser reports what the text said and checks
/// only what the text alone can settle; whether a mnemonic exists, whether a
/// Section may hold what it was given, and every rule needing a Symbol belong
/// to whoever implements this.
class Builder
{
public:
  Builder() = default;
  Builder( Builder const& ) = delete;
  Builder( Builder&& ) = delete;
  Builder& operator=( Builder const& ) = delete;
  Builder& operator=( Builder&& ) = delete;
  virtual ~Builder() = default;

  /// `.section [attribute [, attribute]...]`: an anonymous Section, named
  /// by its first Label. Every statement that emits, reserves or defines a
  /// Label arrives inside a `.section`, a `.proc` or a macro body: the
  /// grammar refuses one that stands in none (`NGA0138`), so an
  /// implementation never has to invent a Section for it.
  virtual void beginSection( SectionAttributes attributes, diag::SourceSpan span ) = 0;
  virtual void endSection( diag::SourceSpan span ) = 0;

  /// `.proc NAME [, attribute]...`: a Section of its own, absolute, with NAME
  /// a Label at its start. The attributes are a `.section`'s less the
  /// placement class, which the grammar has already held to `absolute`.
  virtual void beginProc( Token name, SectionAttributes attributes, diag::SourceSpan span ) = 0;
  /// `.endp [ then NAME ]`: closes the Proc, and with `then` says control
  /// falls through into NAME, a Proc of the same Module the solver places
  /// immediately after this one. Resolved at the end of the Module, since
  /// NAME may be defined further down.
  virtual void endProc( std::optional<Token> then, diag::SourceSpan span ) = 0;

  virtual void defineLabel( Token name, diag::SourceSpan span ) = 0;
  virtual void defineConstant( Token name, ExpressionPtr value, diag::SourceSpan span ) = 0;

  virtual void emitInstruction( Token mnemonic, OperandShape shape, ExpressionPtr operand, diag::SourceSpan span ) = 0;
  virtual void emitData( DataWidth width, std::vector<ExpressionPtr> items, diag::SourceSpan span ) = 0;

  /// Space that occupies addresses and emits nothing. The size is an expression
  /// rather than a count, so `.res screenWidth * 8` is ordinary usage.
  virtual void reserve( ExpressionPtr size, diag::SourceSpan span ) = 0;

  /// `NAME .ztemp SIZE` or `NAME .temp SIZE`: a Temporary of its own, on the
  /// zero page or off it — a Section holding one reservation, with NAME a
  /// Label at its start. The name arrives here and not through `defineLabel`,
  /// because it names the new Section's start and not the next Chunk of the
  /// current one.
  virtual void
  reserveTemporary( Token name, ExpressionPtr size, model::PlacementClass placement, diag::SourceSpan span ) = 0;

  /// `.transition NAME`: enters the Phase named, and never returns. Whether the
  /// name is a Phase is the Project's to say, at the end of Assemble — see
  /// docs/decisions/0019-transition-mechanism.md.
  virtual void transition( Token phase, diag::SourceSpan span ) = 0;

  /// `.dispatch TARGET [, TARGET]...`: control goes to one of the positions
  /// named, chosen by the value in `A`. Written over several lines it is one
  /// statement, the rows after the first continuing it, so an implementation
  /// is handed each row and joins them; whether a target is a position of the
  /// Section the statement stands in is not a question the text settles. See
  /// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
  virtual void dispatch( std::vector<ExpressionPtr> targets, diag::SourceSpan span ) = 0;

  /// `.macro NAME PATTERN`: opens a macro body, which holds instructions
  /// and data and nothing else, and which is assembled once into a template
  /// that Expand instantiates at each use. See docs/decisions/0043-macros.md.
  virtual void beginMacro( Token name, Pattern parameters, diag::SourceSpan span ) = 0;
  virtual void endMacro( diag::SourceSpan span ) = 0;

  /// `.match PACK`, `.case PATTERN`, `.endmatch`: a conditional decided by
  /// how many elements the pack has, the first case whose pattern fits being
  /// kept and its names bound to the elements. Inside a macro body alone.
  /// See docs/decisions/0050-packs.md.
  virtual void beginMatch( Token subject, diag::SourceSpan span ) = 0;
  virtual void beginCase( Pattern pattern, diag::SourceSpan span ) = 0;
  virtual void endMatch( diag::SourceSpan span ) = 0;

  /// `NAME expr, expr [, expr]...`: a statement with a list of operands, which
  /// no instruction has, so it is a macro use — or an instruction given too
  /// many, which is not a question the text settles. A use with one operand
  /// or none arrives through `emitInstruction`, since the grammar cannot
  /// tell it from an instruction and does not try. `SPACE.NAME [expr, ...]`
  /// is a use however many operands it has, since no instruction is
  /// qualified; `space` is the name before the dot.
  virtual void
  useMacro( std::vector<Token> path, Token name, std::vector<ExpressionPtr> arguments, diag::SourceSpan span ) = 0;

  /// `.namespace NAME[.NAME]...`: opens a Namespace, or one inside another
  /// per segment, which the matching `.endnamespace` closes whole. The
  /// grammar holds it to the top level; what it does to names is the
  /// implementation's — see docs/decisions/0045-namespaces.md.
  virtual void beginNamespace( std::vector<Token> path, diag::SourceSpan span ) = 0;
  virtual void endNamespace( diag::SourceSpan span ) = 0;

  /// `.if EXPR`: the Chunks that follow belong to this branch, and whether
  /// they are the ones kept is decided once every Module's Symbols are
  /// known. The grammar has refused everything but a statement that emits.
  virtual void beginConditional( ExpressionPtr condition, diag::SourceSpan span ) = 0;

  /// `.elsif EXPR`, or `.else` with no condition: this branch ends and the
  /// next begins.
  virtual void nextBranch( ExpressionPtr condition, diag::SourceSpan span ) = 0;

  /// `.endif`: the last branch ends.
  virtual void endConditional( diag::SourceSpan span ) = 0;

  /// Makes a Symbol visible outside its Module. It speaks about a name rather
  /// than about what has been built so far, so it may arrive before the
  /// definition it names.
  virtual void exportSymbol( Token name, diag::SourceSpan span ) = 0;

  /// `.slot NAME, binding [, placement]`: a Symbol with one live definition
  /// per Phase, reached through a Cell. See docs/decisions/0031-slots.md.
  virtual void
  declareSlot( Token name, model::Binding binding, model::PlacementClass placement, diag::SourceSpan span ) = 0;

  /// `.implements SLOT, SYMBOL`: this Module's SYMBOL fills SLOT wherever the
  /// Module is present.
  virtual void implement( Token slot, Token symbol, diag::SourceSpan span ) = 0;

  /// A whole `.charset` block, entries and all. Unlike a Section or a Proc this
  /// is one call rather than a pair, because the block holds entries rather
  /// than statements: the grammar reads it to its closer before anything else
  /// can happen, so there is no open construct for an implementation to track.
  virtual void declareCharset( Token name,
                               std::optional<model::CharsetBase> base,
                               std::vector<model::CharsetEntry> entries,
                               diag::SourceSpan span ) = 0;

  /// A condition checked after Place. Its expression is the one that cannot be
  /// folded early even in principle.
  virtual void addAssertion( ExpressionPtr condition, diag::SourceSpan span ) = 0;

  /// `.transform FORMAT LABEL`: this Module's LABEL decodes the format named,
  /// a Payload's stored form into the bytes the Section runs as. Whether the
  /// format is one the tool encodes, and whether LABEL is defined here, are
  /// not questions the text settles.
  virtual void declareTransform( Token format, Token label, diag::SourceSpan span ) = 0;

  /// `.driver ROLE NAME [NAME]`: this Module is the storage driver, and the
  /// names are where it provides ROLE — a macro, or a Window and a macro,
  /// or a Window alone for `stream`. Which roles exist and how many names
  /// each takes is not a question the text settles.
  virtual void declareDriverRole( Token role, std::vector<Token> names, diag::SourceSpan span ) = 0;

  /// `.with NAME`, `.with NAME, x` or `.with NAME = STATE`: the statement
  /// that follows runs with what NAME names shown, and the Window's base
  /// shown again after it. Held until that statement arrives; whether NAME
  /// is a Pane, a family or a Window is not a question the text settles.
  virtual void with( model::WithForm form, ExpressionPtr what, std::optional<Token> state, diag::SourceSpan span ) = 0;

  /// Nothing the pending `.with` lines could apply to followed, which has
  /// been reported: they are dropped.
  virtual void dropWiths() = 0;

  /// `.own` or `.root`: who follows an address the instruction or data on
  /// the line below takes — this Section alone, or the hardware — see
  /// docs/decisions/0060-own.md and docs/decisions/0061-root-at-the-taking.md.
  /// Held until that statement arrives.
  virtual void taking( model::Taking kind, std::vector<Token> followers, diag::SourceSpan span ) = 0;

  /// Nothing the pending `.own` or `.root` could apply to followed, which
  /// has been reported: it is dropped.
  virtual void dropTaking() = 0;

  /// `.declare arg` or `.declare ret`: what the `.ztemp` on the line below
  /// is to the Proc that holds it — one of its arguments, in the order the
  /// declarations stand in, or its result — and the type its bytes hold,
  /// where one was written. Held until that `.ztemp` arrives; see
  /// docs/decisions/0081-a-procs-signature-is-declared.md. Or where its bytes
  /// are, `place`, a register's letter or `m` for each from the low one, and
  /// where no byte is `m` there is no `.ztemp` to wait for — see
  /// docs/decisions/0145-an-argument-in-a-register.md.
  virtual void declare( model::Declaring what,
                        std::optional<model::DeclaredType> type,
                        std::string place,
                        diag::SourceSpan span ) = 0;

  /// No `.ztemp` followed the pending `.declare`, which has been reported:
  /// it is dropped.
  virtual void dropDeclaration() = 0;

  /// `.off CODE`: silences the warning CODE names on the statement that
  /// follows, whose first token is at `statement` — absent when nothing
  /// followed, which is reported as silencing nothing. Whether the code
  /// names a diagnostic is not a question the text settles.
  virtual void silence( Token code, std::optional<diag::SourceLocation> statement, diag::SourceSpan span ) = 0;

  /// `.source "PATH", LINE`: the text from here to the next `.source` came
  /// from that line of that file. `line` is a NUMBER token; the builder
  /// reads and checks its value.
  virtual void markSource( Token path, Token line, diag::SourceSpan span ) = 0;
};

} // namespace nga::syntax
