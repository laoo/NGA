#pragma once

#include "nga/diag/SeverityPolicy.hpp"
#include "nga/diag/SourceLocation.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/Token.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace nga::syntax
{

/// What the Project grammar hands its results to.
///
/// The same division as the assembler's Builder: the grammar reports what the
/// text said and checks only what the text alone settles, and everything that
/// needs a file to exist, a path to resolve, a diagnostic identifier to be
/// real or a name to have been declared belongs to whoever implements this.
class ProjectBuilder
{
public:
  ProjectBuilder() = default;
  ProjectBuilder( ProjectBuilder const& ) = delete;
  ProjectBuilder( ProjectBuilder&& ) = delete;
  ProjectBuilder& operator=( ProjectBuilder const& ) = delete;
  ProjectBuilder& operator=( ProjectBuilder&& ) = delete;
  virtual ~ProjectBuilder() = default;

  /// One argument of a generator call, as the Project wrote it: the parameter
  /// it names and what stands after `=` — a quoted literal, an expression, or a
  /// bare word, which is a name the generator reads and no Symbol, since the
  /// Project has none. A **flag**, `root`, is the name with none of the three,
  /// and the one **positional** argument is a value with no name.
  struct GeneratorArgument
  {
    std::optional<Token> name{};
    std::optional<Token> literal{};
    std::optional<Token> word{};
    ExpressionPtr value{};

    /// What a diagnostic about this argument underlines.
    diag::SourceSpan span{};
  };

  /// One entry of a `modules` block. The alias is absent where the entry gave
  /// no `as`, and the Module is then named by the file's stem. The group is
  /// the name in the block's header, absent for a block without one: a named
  /// `modules` block is a group holding the Modules it declares.
  virtual void addModule( Token path, std::optional<Token> alias, std::optional<Token> group ) = 0;

  /// One entry of a `modules` block that calls a **generator**: a Module whose
  /// text the tool writes rather than reads — see
  /// docs/spec/generators.md. The alias and the group are the entry's, as they
  /// are for a path.
  virtual void addGeneratedModule( Token generator,
                                   std::vector<GeneratorArgument> arguments,
                                   std::optional<Token> alias,
                                   std::optional<Token> group ) = 0;

  /// The header of a `group` block or of a named `modules` block. Reached
  /// once per block, so a name written over two blocks arrives twice, and
  /// summing them is the implementation's business.
  virtual void declareGroup( Token name ) = 0;

  /// One name in a `group` block: a Module or another group, neither of
  /// which need have been declared yet.
  virtual void groupMember( Token group, Token member ) = 0;

  /// One entry of a `diagnostics` block. Whether the code names a diagnostic
  /// at all is not a question the text settles.
  virtual void setSeverity( Token code, diag::SeverityOverride action ) = 0;

  /// One entry of a `constants` block: `NAME = NUMBER`. Whether the name has
  /// been given a value already is a fact about the document, and it is
  /// reported by the implementation, which is where the first one was kept.
  virtual void addConstant( Token name, Token value ) = 0;

  /// Reads another document into this Project, at this point. Recursion, the
  /// cycle check and registering the file all belong to the implementation:
  /// the grammar has read one `include` and knows nothing about files.
  virtual void includeDocument( Token path ) = 0;

  /// The header of a `phase` block. Reached once per block, so a name written
  /// over two blocks arrives twice, and summing them is the implementation's
  /// business — as is everything the names inside refer to, which may not
  /// have been declared yet.
  virtual void declarePhase( Token name ) = 0;

  /// One name in a `needs` list of the Phase named `phase`.
  virtual void phaseNeeds( Token phase, Token module ) = 0;

  /// One name in a `then` list of the Phase named `phase`.
  virtual void phaseLeadsTo( Token phase, Token next ) = 0;

  /// `base WINDOW = STATE` in the Phase named `phase`, or in the group named
  /// `group`, which every Phase needing the group inherits — see
  /// docs/decisions/0056-a-phase-chooses-a-base.md.
  virtual void phaseBase( Token phase, Token window, Token state ) = 0;
  virtual void groupBase( Token group, Token window, Token state ) = 0;

  /// One name in a `resident` block.
  virtual void addResident( Token module ) = 0;

  /// The `entry` statement. Whether it has been given before is a fact about
  /// the document, and it is reported by the implementation because that is
  /// where the first one was kept.
  virtual void setEntry( Token phase ) = 0;

  /// The `container` statement: what the program is, and the cartridge board
  /// after it where one was written. Whether the word names a container the
  /// tool writes, whether that container takes a board, and whether one was
  /// given before, belong to the implementation.
  virtual void setContainer( Token name, std::optional<Token> board ) = 0;

  /// One name of a `containers` entry of a `target` block: what the machine
  /// takes.
  virtual void addAcceptedContainer( Token keyword, Token name ) = 0;

  /// The `cpu` entry of a `target` block: the processor the machine has.
  virtual void setCpu( Token keyword, Token name ) = 0;

  /// The `optimize` statement: what the program is optimised for. Whether the
  /// word is one of the three, and whether one was given before, belong to the
  /// implementation.
  virtual void setIntent( Token name ) = 0;

  /// `units NAME` of a `storage` block: the units are the Banks of the unit
  /// set named. Whether the set exists is not a question the text settles.
  virtual void setStorageUnits( Token keyword, Token name ) = 0;

  /// `units N` of a `storage` block: that many units, known by their
  /// numbers.
  virtual void setUnitCount( Token keyword, ExpressionPtr value ) = 0;

  /// `size` of a `storage` block: what one unit holds.
  virtual void setUnitSize( Token keyword, ExpressionPtr value ) = 0;

  /// A `panes` block opens: the Window its Panes are in, and the named state
  /// they are pinned to where the header wrote `= STATE`.
  virtual void beginPanes( Token keyword, Token window, std::optional<Token> state ) = 0;

  /// One entry of the open `panes` block: a Pane, or with a count a family
  /// of that many.
  virtual void addPane( Token name, ExpressionPtr count ) = 0;

  /// A `units` entry of a `target` block: a named set of that many Banks.
  virtual void addUnitSet( Token keyword, Token name, ExpressionPtr count ) = 0;

  /// A `window` entry of a `target` block: a name, its ranges, the names
  /// after `views` — each a unit set or a named state, which the text
  /// cannot tell apart — and the name after `base`, where there was one.
  virtual void addWindow( Token keyword,
                          Token name,
                          std::vector<std::pair<ExpressionPtr, ExpressionPtr>> ranges,
                          std::vector<Token> states,
                          std::optional<Token> base ) = 0;

  /// A `region` entry: an optional name, the two ends of its range, and the
  /// word that follows, which the grammar has only seen to be a word.
  virtual void
  addRegion( Token keyword, std::optional<Token> name, ExpressionPtr begin, ExpressionPtr end, Token property ) = 0;

  /// A `register` entry: a name, an address, and the width after the comma,
  /// absent where there was none.
  virtual void addRegister( Token keyword, Token name, ExpressionPtr address, ExpressionPtr width ) = 0;

  /// The `entry` of a `phase` block: the Label the Phase starts at, as a
  /// name or a dotted one, `one.start`, when the Label lies in a Namespace.
  virtual void phaseEntry( Token phase, std::vector<Token> label ) = 0;

  /// One entry of a `transform` block: the Section, written `module.section`
  /// or `module.space.section` for one declared in a Namespace, that the
  /// transform named applies to. Whether either name exists is not a
  /// question the text settles, and the Section's cannot be settled at all
  /// until its Module is assembled.
  virtual void applyTransform( Token transform, Token module, std::vector<Token> section ) = 0;
};

} // namespace nga::syntax
