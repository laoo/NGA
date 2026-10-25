#include "nga/model/TypeCheck.hpp"

#include "nga/model/With.hpp"

#include "nga/model/Isa.hpp"

#include "nga/model/Dispatch.hpp"
#include "nga/model/Evaluate.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/References.hpp"
#include "nga/model/Slots.hpp"
#include "nga/model/Storage.hpp"

#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model
{

namespace
{

using syntax::ExpressionType;

/// A count of bytes as a message says it: "1 byte", "2 bytes".
std::string bytesCounted( std::int64_t count )
{
  return std::to_string( count ) + ( count == 1 ? " byte" : " bytes" );
}

/// How a Chunk reaches what its expressions name: the kind of every
/// Reference it holds, and whether it is an instruction touching the one
/// address its single operand names, with no index and no indirection.
struct Reach
{
  ReferenceKind kind = ReferenceKind::ESCAPE;
  bool plain = false;
  syntax::Taking taking = syntax::Taking::ESCAPE;
};

/// Ordering rather than equality, which is the half of comparison that relates
/// two positions to each other.
bool isOrdering( syntax::BinaryOperator op )
{
  return op == syntax::BinaryOperator::LESS || op == syntax::BinaryOperator::LESS_EQUAL ||
         op == syntax::BinaryOperator::GREATER || op == syntax::BinaryOperator::GREATER_EQUAL;
}

/// A type as a message names it, with its article: "an address", "a pane".
std::string typeName( Type const& type )
{
  std::string_view const name = nameOf( type.kind );
  bool const vowel = !name.empty() && std::string_view{ "aeiou" }.contains( name.front() );
  return ( vowel ? "an " : "a " ) + std::string{ name };
}

/// Types one Module's expressions, following names wherever they lead.
///
/// A Constant holds an expression, so typing a name means typing what it was
/// defined as — in the Module that defined it, not the one that used it. The
/// stack is what turns `a = b + 1`, `b = a + 1` into one finding instead of a
/// stack overflow.
class Typer
{
public:
  Typer( diag::SourceManager const& sources,
         GlobalSymbols const& symbols,
         Charsets const& charsets,
         Target const& target,
         diag::DiagnosticSink& sink )
      : mSources( &sources ), mSymbols( &symbols ), mCharsets( &charsets ), mTarget( &target ), mSink( &sink )
  {
  }

  Type typeOf( ModuleIndex home, syntax::Expression const& node );

  /// What a Symbol is, typed where it was defined; `site` is the name as
  /// written, where a finding about the definition lands.
  Type typeOfSymbol( SymbolRef where, syntax::Expression const& site );

  /// A linear address where the CPU would use it as one: as an operand, or as
  /// a `.word`, which is the bytes of an address the CPU reads.
  void refuseLinear( Type const& type, syntax::Expression const& site ) const
  {
    if ( type.is( syntax::ExpressionType::ADDRESS ) && type.space == Space::LINEAR )
    {
      report( diag::diagnostic( diag::DiagnosticId::LINEAR_NOT_FOR_CPU ).at( site.span.begin, site.span.length ) );
    }
  }

  Type expectInteger( Type const& type, syntax::Expression const& node );

  /// One of a fixed set of types, or a finding naming what was written.
  /// Answers the type either way, so that a rule about one of the allowed
  /// kinds can go on from here without typing the expression twice.
  Type expectOneOf( ModuleIndex home,
                    syntax::Expression const& node,
                    std::initializer_list<ExpressionType> allowed,
                    diag::Diagnostic complaint );

  /// See `namesSettableByte`.
  [[nodiscard]] bool
  settableByte( ModuleIndex home, SymbolRef target, LabelPosition const& label, syntax::Expression const& site ) const;

  /// The opcodes the instruction at the Label `target` may hold: its own,
  /// and those of its alternatives, where `.alt` declared any.
  [[nodiscard]] std::vector<std::uint8_t> opcodesAt( SymbolRef target, LabelPosition const& label ) const;

  /// How many bytes of operand the instruction at the Label `target` has,
  /// where it is one whose operand a write from outside may set: one that
  /// goes on to the next instruction whatever its operand says.
  [[nodiscard]] std::optional<std::int64_t> settableOperandAt( SymbolRef target, LabelPosition const& label ) const;

  /// How far past the Label `target` the address `site` stands, when that is
  /// all it says; a byte taken of it, `<` or `>`, says the same.
  [[nodiscard]] std::optional<std::int64_t>
  offsetFrom( ModuleIndex home, SymbolRef target, LabelPosition const& label, syntax::Expression const& site ) const;

private:
  Type typeOfName( ModuleIndex home, syntax::Expression const& node );
  Type typeOfLocalName( ModuleIndex home, syntax::Expression const& node );
  Type typeOfAttribute( ModuleIndex home, syntax::Expression const& node );
  /// `linearFamilyAddress` where `ofFamily`, and `linearSectionAddress`
  /// where not.
  Type typeOfLinear( SectionRef named, bool ofFamily, syntax::Expression const& node );

  void checkCharset( ModuleIndex home, syntax::Expression const& node );
  Type typeOfUnary( ModuleIndex home, syntax::Expression const& node );
  Type typeOfBinary( ModuleIndex home, syntax::Expression const& node );

  /// An operation whose operands have values and that has none itself: a
  /// division by zero, or an answer the evaluator's 64 bits do not hold.
  /// Every Step after this one would read such an expression as having no
  /// value — a `.res` as a size depending on itself, a pin as nothing, an
  /// operand as zero — so it is said here, where every expression is read
  /// once and before anything leans on its value, and once per node.
  void checkComputes( ModuleIndex home, syntax::Expression const& node );

  /// The Address a position in a Section has: runtime space, that Section, and
  /// the PlacementClass the Section declared — never one read back from an
  /// address the solver chose.
  [[nodiscard]] Type addressOf( ModuleIndex module, LabelPosition position ) const;

  void report( diag::Diagnostic value ) const
  {
    mSink->add( std::move( value ) );
  }

  [[nodiscard]] std::string_view textOf( syntax::Token token ) const
  {
    return mSources->textOf( token.span() );
  }

  diag::SourceManager const* mSources;
  GlobalSymbols const* mSymbols;
  Charsets const* mCharsets;
  Target const* mTarget;
  diag::DiagnosticSink* mSink;

  /// What a name has already been found to be. Without it a Constant used ten
  /// times would report the same bad definition ten times.
  std::unordered_map<std::uint64_t, Type> mKnown;

  /// The operations already found to compute nothing, each told once.
  std::set<syntax::Expression const*> mComputesNothing;

  /// Which arm a conditional being typed takes, where its condition is
  /// decided; and how deep in an arm not taken the typing stands, where an
  /// operation computing nothing is no fault, since it is never computed.
  std::optional<bool> mArmTaken;
  std::uint32_t mUntaken = 0;

  /// The Constants being typed right now, so that a definition in terms of
  /// itself is one finding rather than a stack overflow.
  std::vector<SymbolRef> mVisiting;
};

/// Every expression of the program typed, and the rules that belong to the
/// statement holding one rather than to the expression itself: where a
/// Reference may point and in which Phases, what a Proc's Signature, a
/// dispatch, a branch and a `.alt` declare. What Trace and Place are later
/// told of who refers to whom is gathered on the way.
class Rules
{
public:
  Rules( diag::SourceManager const& sources,
         GlobalSymbols const& symbols,
         Charsets const& charsets,
         Target const& target,
         PhaseGraph const& phases,
         Reachable const& reachable,
         Referrers& referrers,
         diag::DiagnosticSink& sink )
      : mTyper( sources, symbols, charsets, target, sink ), mSources( &sources ), mSymbols( &symbols ),
        mCharsets( &charsets ), mTarget( &target ), mPhases( &phases ), mReachable( &reachable ),
        mReferrers( &referrers ), mSink( &sink )
  {
  }

  void checkEverything();

private:
  void checkModule( ModuleIndex home );

  /// A Proc's Signature against the bytes its declared variables reserve:
  /// where a `.declare` wrote a type, the reservation under it holds
  /// exactly that type — see
  /// docs/decisions/0081-a-procs-signature-is-declared.md. Where it wrote
  /// none, the shape is the answer and there is nothing to disagree with.
  void checkSignature( ModuleIndex home, Module const& module, Section const& proc );

  /// The rules on one Chunk of `section`: a Chunk of its own, or an inner
  /// Chunk of a macro use's expansion, which reaches its expressions through
  /// the same arena.
  void checkChunk( ModuleIndex home, SectionIndex here, Section const& section, Chunk const& chunk );

  /// Whether an instruction is `asl a` or one like it: the accumulator form,
  /// spelled with the operand other assemblers write and this one does not,
  /// where nothing defines an `a` it could name instead. Reported when it is.
  bool refuseAccumulatorOperand( ModuleIndex home,
                                 InstructionContent const& instruction,
                                 std::span<syntax::ExpressionPtr const> items );
  void checkDispatch( ModuleIndex home,
                      SectionIndex here,
                      Chunk const& chunk,
                      std::span<syntax::ExpressionPtr const> targets );

  /// The rule on where a Reference may point, over one expression of a Chunk
  /// in `section` of `home`: every name in it that reaches memory — a Label, a
  /// Section, a Constant defined in terms of either — must reach a Module
  /// present wherever `home` is. `site` is the name as written in the Chunk,
  /// which is where the finding lands when a Constant led elsewhere.
  ///
  /// `reach` is how the Chunk reaches what it names, which decides how far
  /// into a Proc it may.
  void checkResidency( ModuleIndex home, SectionIndex section, syntax::Expression const& node, Reach reach );
  void checkSlotUse( ModuleIndex home, SectionIndex section, SymbolRef slot, syntax::Expression const& site );
  void checkSlots();
  void checkReference(
      ModuleIndex home, SectionIndex section, SymbolRef target, syntax::Expression const& site, Reach reach );

  /// Whether `reach` may go where `site` leads in the Proc that the Label
  /// `target` stands in, from outside it.
  [[nodiscard]] bool reachesIntoProc( ModuleIndex home,
                                      SymbolRef target,
                                      LabelPosition const& label,
                                      syntax::Expression const& site,
                                      Reach reach ) const;

  /// What a `.alt` declared of the instruction it stands above: an
  /// instruction that reaches no memory, and alternatives the processor has
  /// in its form and at its length.
  void checkAlternatives( Chunk const& chunk );

  /// A store whose site is the opcode of an instruction with alternatives,
  /// and whose value the instructions before it say: that value is the
  /// instruction's own opcode or one of them.
  void checkOpcodeStore( ModuleIndex home, Section const& section, std::uint32_t at );

  /// A conditional branch reaches its own Section or one chained to it by
  /// `then`, in either direction: only there is the distance a fact of the
  /// program rather than of the layout the solver chose.
  void checkBranch( ModuleIndex home, SectionIndex section, syntax::Expression const& operand );

  void report( diag::Diagnostic value ) const
  {
    mSink->add( std::move( value ) );
  }

  [[nodiscard]] std::string_view textOf( syntax::Token token ) const
  {
    return mSources->textOf( token.span() );
  }

  Typer mTyper;
  diag::SourceManager const* mSources;
  GlobalSymbols const* mSymbols;
  Charsets const* mCharsets;
  Target const* mTarget;
  PhaseGraph const* mPhases;
  Reachable const* mReachable;
  Referrers* mReferrers;
  diag::DiagnosticSink* mSink;
};

/// `operandPlacement`, over the parts of a build a Typer holds.
OperandPlacement operandPlacementIn( diag::SourceManager const& sources,
                                     GlobalSymbols const& symbols,
                                     Charsets const& charsets,
                                     Target const& target,
                                     ModuleIndex home,
                                     syntax::Expression const& operand,
                                     syntax::OperandShape shape )
{
  // Only these three shapes have two widths to choose between. An immediate or
  // an indirect operand is one byte or two whatever it names, so nothing about
  // it has to be known here.
  if ( shape != syntax::OperandShape::DIRECT && shape != syntax::OperandShape::DIRECT_X &&
       shape != syntax::OperandShape::DIRECT_Y )
  {
    return OperandPlacement{ .placement = syntax::PlacementClass::ABSOLUTE };
  }

  diag::SeverityPolicy policy;
  diag::DiagnosticSink quiet{ policy };
  Typer asker{ sources, symbols, charsets, target, quiet };
  OperandPlacement found{ .type = asker.typeOf( home, operand ) };
  if ( found.type.is( ExpressionType::ADDRESS ) )
  {
    // Declared where the Symbol was defined, which may be a Module this one
    // never saw. Nothing here reads an address back.
    found.placement = found.type.placement;
    return found;
  }
  if ( !found.type.is( ExpressionType::INTEGER ) )
  {
    return found;
  }

  // An Integer operand has no PlacementClass to inherit, so its value decides
  // the width — and the value has to be one the author declared rather than
  // one a Step produced.
  found.value = declaredValueOf( sources, symbols, &charsets, home, operand );
  if ( found.value.has_value() && *found.value >= 0 && std::cmp_less( *found.value, ADDRESS_SPACE_END ) )
  {
    found.placement = std::cmp_less( *found.value, ZERO_PAGE_END ) ? syntax::PlacementClass::ZEROPAGE
                                                                   : syntax::PlacementClass::ABSOLUTE;
  }
  return found;
}

std::uint64_t keyOf( SymbolRef where )
{
  return ( static_cast<std::uint64_t>( where.module.value ) << 32U ) | where.index;
}

Type Typer::addressOf( ModuleIndex module, LabelPosition position ) const
{
  Section const& section = mSymbols->moduleAt( module ).sectionAt( position.section );
  // A Section in a unit set has no address the CPU could use, so the linear
  // one is what its Labels stand for — see
  // docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
  return Type::address( SectionRef{ .module = module, .section = position.section },
                        section.placement(),
                        section.unitSet().has_value() ? Space::LINEAR : Space::RUNTIME );
}

Type Typer::expectInteger( Type const& type, syntax::Expression const& node )
{
  if ( !type.isKnown() )
  {
    return Type::unknown();
  }
  if ( type.is( ExpressionType::INTEGER ) )
  {
    return Type::integer();
  }

  report( diag::diagnostic( diag::DiagnosticId::EXPECTED_INTEGER )
              .at( node.span.begin, node.span.length )
              .arg( "type", typeName( type ) ) );
  return Type::unknown();
}

Type Typer::typeOfSymbol( SymbolRef where, syntax::Expression const& site )
{
  if ( std::ranges::find( mVisiting, where ) != mVisiting.end() )
  {
    Symbol const& cyclic = mSymbols->at( where );
    report( diag::diagnostic( diag::DiagnosticId::CONSTANT_CYCLE )
                .at( site.span.begin, site.span.length )
                .arg( "symbol", std::string{ cyclic.name } ) );
    return Type::unknown();
  }

  if ( auto const known = mKnown.find( keyOf( where ) ); known != mKnown.end() )
  {
    return known->second;
  }

  Symbol const& symbol = mSymbols->at( where );
  switch ( symbol.kind )
  {
  case SymbolKind::LABEL:
    return addressOf( where.module, std::get<LabelPosition>( symbol.value ) );
  case SymbolKind::CONSTANT:
  {
    // A Constant's type is the type of what it was defined as, and that
    // expression belongs to the Module that defined it.
    auto const& value = std::get<syntax::ExpressionPtr>( symbol.value );
    if ( value == nullptr )
    {
      return Type::unknown();
    }
    mVisiting.push_back( where );
    Type const type = typeOf( where.module, *value );
    mVisiting.pop_back();
    mKnown.emplace( keyOf( where ), type );
    return type;
  }
  case SymbolKind::CHARSET:
    // A Charset names itself only as a literal's prefix, which is part of the
    // literal rather than a sub-expression. Reaching one here means it was
    // written as a value, and that has a message of its own.
    report( diag::diagnostic( diag::DiagnosticId::CHARSET_NOT_A_VALUE )
                .at( site.span.begin, site.span.length )
                .arg( "symbol", std::string{ symbol.name } ) );
    break;
  case SymbolKind::SLOT:
  {
    // What a Reference to a Slot reaches is its Cell, with the placement the
    // Slot declared — which is what decides the width of `lda (slot),y`.
    auto const& declaration = std::get<SlotDeclaration>( symbol.value );
    return declaration.cell.has_value() ? Type::address( *declaration.cell, declaration.placement ) : Type::unknown();
  }
  case SymbolKind::MACRO:
    report( diag::diagnostic( diag::DiagnosticId::MACRO_NOT_A_VALUE )
                .at( site.span.begin, site.span.length )
                .arg( "symbol", std::string{ symbol.name } ) );
    break;
  case SymbolKind::REGION:
    // A Region's address is a value the variant wrote out — a declaration
    // spelled in digits, as 0010 calls `lda $D01A` — so its name is the
    // Integer operand hardware registers have always been reached by, and
    // the width of `sta COLBK` is decided by the value as it is for `$D01A`.
    return Type::integer();
  case SymbolKind::WINDOW:
    report( diag::diagnostic( diag::DiagnosticId::WINDOW_NOT_A_VALUE )
                .at( site.span.begin, site.span.length )
                .arg( "symbol", std::string{ symbol.name } ) );
    break;
  case SymbolKind::PANE:
  {
    // A Pane's value is the state the solver chose, known after Place as an
    // address is, and the type is what keeps it out of arithmetic — see
    // docs/decisions/0054-panes.md and 0010.
    PaneIndex const pane = std::get<PaneIndex>( symbol.value );
    return Type::paneOf( pane, mTarget->panes[pane.value].count );
  }
  }
  return Type::unknown();
}

void Typer::checkCharset( ModuleIndex home, syntax::Expression const& node )
{
  // A character set is named only as a literal's prefix, and a prefix is an
  // identifier resolved like any other name — so one that names nothing is an
  // ordinary undefined symbol rather than a rule of its own.
  syntax::Quoted const quoted = syntax::quotedOf( textOf( node.token ) );
  if ( quoted.charset.empty() )
  {
    // An untranslated literal is the source bytes, so it must be ASCII: the
    // author who wrote a non-ASCII character wanted a translation and forgot
    // to say which, and emitting UTF-8 into a 6502 program is not it. Asked
    // here and not in the lexer, because the answer depends on where the
    // literal stands — the runs in a `.charset` are code points and must hold
    // whatever the font draws. See docs/decisions/0013-charset-declaration.md.
    for ( char32_t const point : syntax::codePointsOf( quoted.body ) )
    {
      if ( point < 0x80 )
      {
        continue;
      }
      report( diag::diagnostic( diag::DiagnosticId::NON_ASCII_WITHOUT_CHARSET )
                  .at( node.token.location, node.token.length )
                  .arg( "character", syntax::displayOf( point ) ) );
      return;
    }
    return;
  }

  auto const prefixLength = static_cast<std::uint32_t>( quoted.charset.size() );
  std::optional<SymbolRef> const named = mSymbols->resolveText( home, node, quoted.charset );
  if ( !named.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_SYMBOL )
                .at( node.token.location, prefixLength )
                .arg( "symbol", std::string{ quoted.charset } ) );
    return;
  }

  std::optional<CharsetRef> const where = charsetOf( *mSymbols, *named );
  if ( !where.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::NOT_A_CHARSET )
                .at( node.token.location, prefixLength )
                .arg( "symbol", std::string{ quoted.charset } ) );
    return;
  }

  // Checked here and not where the bytes are written, because a program with a
  // character its set cannot map should say so whether or not it ever reaches
  // Patch — and because Patch is the Step that must not have opinions.
  Charset const& table = mCharsets->at( *where );
  for ( char32_t const point : syntax::codePointsOf( quoted.body ) )
  {
    if ( table.byteFor( point ).has_value() )
    {
      continue;
    }
    report( diag::diagnostic( diag::DiagnosticId::CHARACTER_NOT_IN_CHARSET )
                .at( node.token.location, node.token.length )
                .arg( "charset", std::string{ quoted.charset } )
                .arg( "character", syntax::displayOf( point ) ) );
    // One finding per literal: a set missing an alphabet would otherwise
    // report once per letter.
    return;
  }
}

Type Typer::typeOfName( ModuleIndex home, syntax::Expression const& node )
{
  std::string_view const name = textOf( node.token );
  std::optional<SymbolRef> const where = mSymbols->resolveText( home, node, name );
  if ( !where.has_value() )
  {
    report( diag::diagnostic( mSymbols->isNamespace( name ) ? diag::DiagnosticId::NAMESPACE_NOT_A_VALUE
                                                            : diag::DiagnosticId::UNKNOWN_SYMBOL )
                .at( node.span.begin, node.span.length )
                .arg( "symbol", std::string{ name } )
                .arg( "name", std::string{ name } ) );
    return Type::unknown();
  }
  return typeOfSymbol( *where, node );
}

Type Typer::typeOfLocalName( ModuleIndex home, syntax::Expression const& node )
{
  // Assemble bound this to a position, or reported that it could not. Either
  // way there is nothing here that knows about directions.
  std::optional<LabelPosition> const target = mSymbols->moduleAt( home ).localTarget( &node );
  if ( !target.has_value() )
  {
    return Type::unknown();
  }
  return addressOf( home, *target );
}

Type Typer::typeOfAttribute( ModuleIndex home, syntax::Expression const& node )
{
  // A name stands for a Section when it is one, or when it is the Label at
  // the start of an anonymous one; anything else on the left is typed, and
  // is a Section or an error.
  std::optional<SectionRef> named;
  bool const toolNamespace =
      node.left != nullptr && node.left->kind == syntax::ExpressionKind::NAME && textOf( node.left->token ) == "nga";
  // And the entry Phase's entry, which the stub a program that returns is
  // started by jumps to, under a name only the tool's own Modules use — see
  // docs/decisions/0229-no-window-has-a-base.md.
  bool const entry = textOf( node.token ) == "entry" && mSymbols->moduleAt( home ).name().starts_with( "nga." );
  bool const cell = textOf( node.token ) == "unit" || textOf( node.token ) == "offset" || entry;
  if ( toolNamespace && cell && !mSymbols->find( "nga." + std::string{ textOf( node.token ) } ).has_value() )
  {
    // The cells are bound only where a driver was resolved. The routine uses
    // them whether or not one was, and a program without one has been told so
    // where the driver is resolved or by its Container; the program's own code
    // is told here, as it is told about a role.
    if ( !mSymbols->moduleAt( home ).name().starts_with( "nga." ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::ROLE_WITHOUT_DRIVER )
                  .at( node.span.begin, node.span.length )
                  .arg( "name", std::string{ textOf( node.token ) } ) );
    }
    return Type::unknown();
  }
  if ( toolNamespace && !cell )
  {
    // The tool's namespace holds the driver's roles, which are macros and
    // stand where a mnemonic does, and nothing a value could be made of —
    // except the two cells the stream stands in, which Merge bound to the
    // driver's labels and which resolve below as any dotted name does.
    std::string_view const role = textOf( node.token );
    std::optional<DriverRoleNames> const shape = driverRoleNamed( role );
    bool const known = shape == DriverRoleNames::MACRO || shape == DriverRoleNames::WINDOW_AND_MACRO;
    report( diag::diagnostic( known ? diag::DiagnosticId::MACRO_NOT_A_VALUE : diag::DiagnosticId::NO_SUCH_TOOL_NAME )
                .at( node.span.begin, node.span.length )
                .arg( "symbol", "nga." + std::string{ role } )
                .arg( "name", std::string{ role } ) );
    return Type::unknown();
  }
  // A dotted name that is a Symbol — a member of a Namespace — before an
  // attribute of what stands left of the dot.
  if ( std::optional<std::string> const dotted = syntax::dottedNameOf( *mSources, node ); dotted.has_value() )
  {
    if ( std::optional<SymbolRef> const where = mSymbols->resolveText( home, syntax::leftmostOf( node ), *dotted );
         where.has_value() )
    {
      return typeOfSymbol( *where, node );
    }
  }
  if ( node.left != nullptr )
  {
    if ( std::optional<std::string> const receiver = syntax::dottedNameOf( *mSources, *node.left );
         receiver.has_value() )
    {
      if ( std::optional<SymbolRef> const where =
               mSymbols->resolveText( home, syntax::leftmostOf( *node.left ), *receiver );
           where.has_value() )
      {
        named = sectionNamedBy( *mSymbols, *where );
      }
    }
  }
  if ( !named.has_value() )
  {
    // Only a Label reaches the attributes of the Section it stands in; the
    // type is what the message calls whatever else stood there.
    Type const receiver = typeOf( home, *node.left );
    if ( !receiver.isKnown() )
    {
      return Type::unknown();
    }
    report( diag::diagnostic( diag::DiagnosticId::NOT_A_SECTION )
                .at( node.span.begin, node.span.length )
                .arg( "type", typeName( receiver ) ) );
    return Type::unknown();
  }

  std::string_view const attribute = textOf( node.token );
  std::optional<SectionAttribute> const which = sectionAttributeNamed( attribute );
  if ( which == SectionAttribute::RUNTIME_SECTION_ADDRESS )
  {
    Module const& module = mSymbols->moduleAt( named->module );
    Section const& section = module.sectionAt( named->section );
    if ( std::optional<UnitSetIndex> const set = section.unitSet(); set.has_value() )
    {
      report( diag::diagnostic( diag::DiagnosticId::IN_UNITS_NO_RUNTIME )
                  .at( node.token.location, node.token.length )
                  .arg( "section", module.displayNameOf( named->section ) )
                  .arg( "units", mTarget->unitSets[set->value].name ) );
      return Type::unknown();
    }
    return Type::address( *named, section.placement() );
  }
  if ( which == SectionAttribute::RUNTIME_SECTION_SIZE )
  {
    return Type::integer();
  }
  if ( which == SectionAttribute::RESIDENT )
  {
    // Whether the Section is present in every Phase: a fact of the Project,
    // known before any Step runs, so that `.assert vars.resident` is how
    // source states what it depends on without declaring it.
    return Type::integer();
  }
  if ( which == SectionAttribute::LINEAR_SECTION_ADDRESS || which == SectionAttribute::LINEAR_FAMILY_ADDRESS )
  {
    return typeOfLinear( *named, *which == SectionAttribute::LINEAR_FAMILY_ADDRESS, node );
  }

  // `storagePayloadAddress` and `storagePayloadSize` are reserved and not
  // built: nothing reads storage by itself yet, and the space tag is what
  // keeps runtime and storage apart until something does.
  report( diag::diagnostic( diag::DiagnosticId::NO_SUCH_ATTRIBUTE )
              .at( node.token.location, node.token.length )
              .arg( "name", std::string{ attribute } ) );
  return Type::unknown();
}

/// A linear address is a Pane's Section's, in a Bank of a set that declares
/// where its Banks lie in the medium — and a family's member holds the
/// Section once each, so the address in its first member has a name of its
/// own. See docs/decisions/0251-a-bank-has-a-linear-address.md.
Type Typer::typeOfLinear( SectionRef named, bool ofFamily, syntax::Expression const& node )
{
  Module const& module = mSymbols->moduleAt( named.module );
  Section const& section = module.sectionAt( named.section );
  std::string const name = module.displayNameOf( named.section );
  auto const refusal = [&node, &name]( diag::DiagnosticId id )
  { return diag::diagnostic( id ).at( node.token.location, node.token.length ).arg( "section", name ); };

  // A Section in a unit set is one buffer of the medium, never a family's.
  if ( std::optional<UnitSetIndex> const set = section.unitSet(); set.has_value() )
  {
    if ( ofFamily )
    {
      report( refusal( diag::DiagnosticId::LINEAR_NOT_FAMILY ).arg( "pane", mTarget->unitSets[set->value].name ) );
      return Type::unknown();
    }
    return Type::address( named, section.placement(), Space::LINEAR );
  }
  std::optional<PaneIndex> const paneIndex = section.pane();
  if ( !paneIndex.has_value() )
  {
    report( refusal( diag::DiagnosticId::LINEAR_NOT_IN_PANE ) );
    return Type::unknown();
  }
  Pane const& pane = mTarget->panes[paneIndex->value];
  if ( pane.state.has_value() )
  {
    report( refusal( diag::DiagnosticId::LINEAR_PANE_PINNED ).arg( "pane", pane.name ) );
    return Type::unknown();
  }
  std::optional<UnitSetIndex> const set = mTarget->unitSetShownBy( pane.window );
  if ( !set.has_value() || !mTarget->unitSets[set->value].linear.has_value() )
  {
    report( refusal( diag::DiagnosticId::LINEAR_UNITS_NOT_LINEAR )
                .arg( "pane", pane.name )
                .arg( "units", set.has_value() ? mTarget->unitSets[set->value].name : std::string{} ) );
    return Type::unknown();
  }
  bool const family = pane.count > 1;
  if ( family && !ofFamily )
  {
    report( refusal( diag::DiagnosticId::LINEAR_FAMILY ).arg( "pane", pane.name ) );
    return Type::unknown();
  }
  if ( !family && ofFamily )
  {
    report( refusal( diag::DiagnosticId::LINEAR_NOT_FAMILY ).arg( "pane", pane.name ) );
    return Type::unknown();
  }
  return Type::address( named, section.placement(), Space::LINEAR );
}

Type Typer::typeOfUnary( ModuleIndex home, syntax::Expression const& node )
{
  Type const operand = typeOf( home, *node.left );

  switch ( node.unaryOperator )
  {
  case syntax::UnaryOperator::LOW_BYTE:
  case syntax::UnaryOperator::HIGH_BYTE:
    // Byte extraction is what an Address is for as much as an Integer is —
    // and a Pane's, whose state of a Window of more states than a byte counts
    // is two bytes, handed to the driver one in `X` and one in `Y`: see
    // docs/decisions/0228-a-driver-names-the-cells-its-stream-stands-in.md.
    if ( !operand.isKnown() )
    {
      return Type::unknown();
    }
    if ( operand.is( ExpressionType::INTEGER ) || operand.is( ExpressionType::ADDRESS ) ||
         operand.is( ExpressionType::PANE ) )
    {
      return Type::integer();
    }
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_INTEGER )
                .at( node.span.begin, node.span.length )
                .arg( "type", typeName( operand ) ) );
    return Type::unknown();

  case syntax::UnaryOperator::NEGATE:
  case syntax::UnaryOperator::COMPLEMENT:
  case syntax::UnaryOperator::NOT:
    // The distance between two positions is a quantity; the negation of one
    // position is not.
    if ( operand.is( ExpressionType::PANE ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::PANE_NOT_A_NUMBER ).at( node.span.begin, node.span.length ) );
      return Type::unknown();
    }
    checkComputes( home, node );
    return expectInteger( operand, node );
  }
  return Type::unknown();
}

Type Typer::typeOfBinary( ModuleIndex home, syntax::Expression const& node )
{
  // The conditional is typed before the rest, because its two halves are not
  // operands of one operation: the condition is a quantity and the arms are
  // whatever they both are. Both arms are typed however the condition falls,
  // since a type error is a fact about what is written, not about what is
  // chosen.
  if ( node.binaryOperator == syntax::BinaryOperator::SELECT )
  {
    expectInteger( typeOf( home, *node.left ), *node.left );
    std::optional<std::int64_t> const decided = declaredValueOf( *mSources, *mSymbols, mCharsets, home, *node.left );
    mArmTaken = decided.has_value() ? std::optional{ *decided != 0 } : std::nullopt;
    return typeOf( home, *node.right );
  }
  if ( node.binaryOperator == syntax::BinaryOperator::ARM )
  {
    std::optional<bool> const chosen = std::exchange( mArmTaken, std::nullopt );
    auto const armOf = [&]( syntax::Expression const& arm, bool first )
    {
      bool const untaken = chosen.has_value() && *chosen != first;
      mUntaken += untaken ? 1U : 0U;
      Type const type = typeOf( home, arm );
      mUntaken -= untaken ? 1U : 0U;
      return type;
    };
    Type const taken = armOf( *node.left, true );
    Type const otherwise = armOf( *node.right, false );
    if ( !taken.isKnown() || !otherwise.isKnown() )
    {
      return Type::unknown();
    }
    if ( taken.kind != otherwise.kind )
    {
      report( diag::diagnostic( diag::DiagnosticId::SELECT_ARMS_DIFFER )
                  .at( node.span.begin, node.span.length )
                  .arg( "type", typeName( taken ) )
                  .arg( "other", typeName( otherwise ) ) );
      return Type::unknown();
    }

    // A String and a Section have no operators, and a conditional is not the
    // place to give them one: what would be chosen could stand nowhere the
    // chosen value is used.
    if ( !taken.is( syntax::ExpressionType::INTEGER ) && !taken.is( syntax::ExpressionType::ADDRESS ) )
    {
      return expectInteger( taken, *node.left );
    }

    // Two Addresses of one Section keep it, since what is chosen lies in that
    // Section either way. Of two Sections there is no Section to carry, and
    // an Address without one cannot be subtracted or given a width, so it is
    // refused rather than invented.
    if ( taken.is( syntax::ExpressionType::ADDRESS ) &&
         ( taken.section != otherwise.section || taken.space != otherwise.space ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::SELECT_ARMS_UNRELATED ).at( node.span.begin, node.span.length ) );
      return Type::unknown();
    }
    return taken;
  }

  Type const left = typeOf( home, *node.left );
  Type const right = typeOf( home, *node.right );
  if ( !left.isKnown() || !right.isKnown() )
  {
    return Type::unknown();
  }
  // After the operands, so that one computing nothing is told before the
  // operation over it, which then has an operand with no value and says no
  // more.
  checkComputes( home, node );

  // A Pane admits one operation: a declared Integer added to a family
  // selects a member, its count allowed as one past the last, as the end of
  // a range is. Everything else is a number about the layout the solver
  // chose — see docs/decisions/0054-panes.md.
  if ( left.is( ExpressionType::PANE ) || right.is( ExpressionType::PANE ) )
  {
    Type const pane = left.is( ExpressionType::PANE ) ? left : right;
    syntax::Expression const& offset = left.is( ExpressionType::PANE ) ? *node.right : *node.left;
    Type const other = left.is( ExpressionType::PANE ) ? right : left;
    if ( node.binaryOperator != syntax::BinaryOperator::ADD || !other.is( ExpressionType::INTEGER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::PANE_NOT_A_NUMBER ).at( node.span.begin, node.span.length ) );
      return Type::unknown();
    }
    std::optional<std::int64_t> const index = declaredValueOf( *mSources, *mSymbols, mCharsets, home, offset );
    if ( !index.has_value() || *index < 0 || std::cmp_greater( *index + pane.member, pane.paneCount ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::PANE_MEMBER_OUT_OF_RANGE )
                  .at( node.span.begin, node.span.length )
                  .arg( "name", std::string{ mTarget->panes[pane.pane.value].name } )
                  .arg( "count", static_cast<std::int64_t>( pane.paneCount ) )
                  .arg( "index", index.value_or( -1 ) + pane.member ) );
      return Type::unknown();
    }
    Type member = pane;
    member.member = pane.member + static_cast<std::uint32_t>( *index );
    return member;
  }

  bool const leftIsAddress = left.is( ExpressionType::ADDRESS );
  bool const rightIsAddress = right.is( ExpressionType::ADDRESS );

  // Two addresses are related when a difference or an ordering between them is
  // a fact about the program rather than about the layout the solver happened
  // to choose.
  auto const related = [&left, &right] { return left.section == right.section && left.space == right.space; };

  switch ( node.binaryOperator )
  {
  case syntax::BinaryOperator::SELECT:
  case syntax::BinaryOperator::ARM:
    // Both are typed above, before the operands of an ordinary operation are.
    return Type::unknown();

  case syntax::BinaryOperator::ADD:
    if ( leftIsAddress && rightIsAddress )
    {
      report( diag::diagnostic( diag::DiagnosticId::ADDRESSES_NOT_ADDABLE ).at( node.span.begin, node.span.length ) );
      return Type::unknown();
    }
    if ( leftIsAddress || rightIsAddress )
    {
      Type const address = leftIsAddress ? left : right;
      Type const offset = leftIsAddress ? right : left;
      return expectInteger( offset, node ).isKnown() ? address : Type::unknown();
    }
    return expectInteger( left, node ).isKnown() && expectInteger( right, node ).isKnown() ? Type::integer()
                                                                                           : Type::unknown();

  case syntax::BinaryOperator::SUBTRACT:
    if ( leftIsAddress && rightIsAddress )
    {
      if ( !related() )
      {
        report( diag::diagnostic( diag::DiagnosticId::ADDRESSES_NOT_RELATED ).at( node.span.begin, node.span.length ) );
        return Type::unknown();
      }
      // Known after Size, before any address exists, because it is a sum of
      // Chunk sizes.
      return Type::integer();
    }
    if ( leftIsAddress )
    {
      return expectInteger( right, node ).isKnown() ? left : Type::unknown();
    }
    if ( rightIsAddress )
    {
      report( diag::diagnostic( diag::DiagnosticId::ADDRESS_SUBTRACTED_FROM_INTEGER )
                  .at( node.span.begin, node.span.length ) );
      return Type::unknown();
    }
    return expectInteger( left, node ).isKnown() && expectInteger( right, node ).isKnown() ? Type::integer()
                                                                                           : Type::unknown();

  case syntax::BinaryOperator::BITWISE_AND:
  case syntax::BinaryOperator::BITWISE_OR:
  case syntax::BinaryOperator::BITWISE_XOR:
  {
    // An Address with an Integer yields an Integer, so that `buffer & $FF00`
    // is a page base and `(buffer & $FF) == 0` is an alignment check.
    Type const other = leftIsAddress ? right : left;
    if ( leftIsAddress && rightIsAddress )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_INTEGER )
                  .at( node.span.begin, node.span.length )
                  .arg( "type", typeName( right ) ) );
      return Type::unknown();
    }
    if ( leftIsAddress || rightIsAddress )
    {
      return expectInteger( other, node ).isKnown() ? Type::integer() : Type::unknown();
    }
    return expectInteger( left, node ).isKnown() && expectInteger( right, node ).isKnown() ? Type::integer()
                                                                                           : Type::unknown();
  }

  case syntax::BinaryOperator::EQUAL:
  case syntax::BinaryOperator::NOT_EQUAL:
  case syntax::BinaryOperator::LESS:
  case syntax::BinaryOperator::LESS_EQUAL:
  case syntax::BinaryOperator::GREATER:
  case syntax::BinaryOperator::GREATER_EQUAL:
    if ( leftIsAddress && rightIsAddress )
    {
      // Relating two positions to each other is a fact about the program only
      // where both are in one Section. Ordering follows the subtraction rule
      // for that reason; equality does not need it.
      if ( isOrdering( node.binaryOperator ) && !related() )
      {
        report( diag::diagnostic( diag::DiagnosticId::ADDRESSES_NOT_RELATED ).at( node.span.begin, node.span.length ) );
        return Type::unknown();
      }
      return Type::integer();
    }
    if ( leftIsAddress || rightIsAddress )
    {
      // An address against a number is the question `.assert` exists to ask,
      // and a hardcoded address is a declaration spelled in digits.
      return expectInteger( leftIsAddress ? right : left, node ).isKnown() ? Type::integer() : Type::unknown();
    }
    return expectInteger( left, node ).isKnown() && expectInteger( right, node ).isKnown() ? Type::integer()
                                                                                           : Type::unknown();

  case syntax::BinaryOperator::SHIFT_RIGHT:
    // An Address shifted right yields an Integer, for the reason `&` does:
    // the bytes of a linear address beyond the two `<` and `>` reach — see
    // docs/decisions/0251-a-bank-has-a-linear-address.md.
    if ( leftIsAddress )
    {
      return expectInteger( right, node ).isKnown() ? Type::integer() : Type::unknown();
    }
    return expectInteger( left, node ).isKnown() && expectInteger( right, node ).isKnown() ? Type::integer()
                                                                                           : Type::unknown();

  case syntax::BinaryOperator::MULTIPLY:
  case syntax::BinaryOperator::DIVIDE:
  case syntax::BinaryOperator::SHIFT_LEFT:
  case syntax::BinaryOperator::LOGICAL_AND:
  case syntax::BinaryOperator::LOGICAL_OR:
    return expectInteger( left, node ).isKnown() && expectInteger( right, node ).isKnown() ? Type::integer()
                                                                                           : Type::unknown();
  }
  return Type::unknown();
}

void Typer::checkComputes( ModuleIndex home, syntax::Expression const& node )
{
  if ( mUntaken > 0 || mComputesNothing.contains( &node ) )
  {
    return;
  }
  auto const valueOf = [this, home]( syntax::Expression const& one )
  { return declaredValueOf( *mSources, *mSymbols, mCharsets, home, one ); };

  // A divisor of zero is the fault whatever is divided, an address included.
  bool const divides =
      node.kind == syntax::ExpressionKind::BINARY && node.binaryOperator == syntax::BinaryOperator::DIVIDE;
  if ( divides && node.right != nullptr && valueOf( *node.right ) == std::optional<std::int64_t>{ 0 } )
  {
    mComputesNothing.insert( &node );
    report( diag::diagnostic( diag::DiagnosticId::DIVISION_BY_ZERO )
                .at( node.right->span.begin, node.right->span.length ) );
    return;
  }
  // Otherwise only where every operand has a value: one that has none is
  // missing for a reason of its own, said where that reason is.
  bool const operandsKnown = ( node.left == nullptr || valueOf( *node.left ).has_value() ) &&
                             ( node.right == nullptr || valueOf( *node.right ).has_value() );
  if ( operandsKnown && !valueOf( node ).has_value() )
  {
    mComputesNothing.insert( &node );
    report( diag::diagnostic( diag::DiagnosticId::VALUE_PAST_64_BITS ).at( node.span.begin, node.span.length ) );
  }
}

Type Typer::typeOf( ModuleIndex home, syntax::Expression const& node )
{
  switch ( node.kind )
  {
  case syntax::ExpressionKind::ERROR:
    // Already reported on. Saying anything here is how one mistake becomes a
    // column of findings.
    return Type::unknown();
  case syntax::ExpressionKind::NUMBER:
  case syntax::ExpressionKind::VALUE:
    return Type::integer();
  case syntax::ExpressionKind::SPREAD:
    // Replaced by Expand wherever it named a pack; one left standing did
    // not, and Assemble has said so.
    return Type::unknown();
  case syntax::ExpressionKind::CHARACTER:
    checkCharset( home, node );
    return Type::integer();
  case syntax::ExpressionKind::STRING:
    checkCharset( home, node );
    return Type::string();
  case syntax::ExpressionKind::NAME:
    return typeOfName( home, node );
  case syntax::ExpressionKind::LOCAL_NAME:
    return typeOfLocalName( home, node );
  case syntax::ExpressionKind::ATTRIBUTE:
    return typeOfAttribute( home, node );
  case syntax::ExpressionKind::UNARY:
    return typeOfUnary( home, node );
  case syntax::ExpressionKind::BINARY:
    return typeOfBinary( home, node );
  }
  return Type::unknown();
}

Type Typer::expectOneOf( ModuleIndex home,
                         syntax::Expression const& node,
                         std::initializer_list<ExpressionType> allowed,
                         diag::Diagnostic complaint )
{
  Type const type = typeOf( home, node );
  if ( !type.isKnown() || std::ranges::find( allowed, type.kind ) != allowed.end() )
  {
    return type;
  }
  report( std::move( complaint ).at( node.span.begin, node.span.length ).arg( "type", typeName( type ) ) );
  return Type::unknown();
}

void Rules::checkResidency( ModuleIndex home, SectionIndex section, syntax::Expression const& node, Reach reach )
{
  // The finding lands on the expression as written in the Chunk, which is
  // `node` itself, however far a Constant led from there.
  class Visitor final : public ReferenceVisitor
  {
  public:
    Visitor( Rules& rules, ModuleIndex home, SectionIndex section, syntax::Expression const& site, Reach reach )
        : mRules( &rules ), mHome( home ), mSection( section ), mSite( &site ), mReach( reach )
    {
    }

    void reference( SymbolRef target ) override
    {
      if ( mRules->mSymbols->at( target ).kind == SymbolKind::SLOT )
      {
        mRules->checkSlotUse( mHome, mSection, target, *mSite );
        return;
      }
      mRules->checkReference( mHome, mSection, target, *mSite, mReach );
    }

  private:
    Rules* mRules;
    ModuleIndex mHome;
    SectionIndex mSection;
    syntax::Expression const* mSite;
    Reach mReach;
  };

  Visitor visitor{ *this, home, section, node, reach };
  walkReferences( *mSymbols, *mSources, home, node, visitor );
}

std::optional<std::int64_t> Typer::offsetFrom( ModuleIndex home,
                                               SymbolRef target,
                                               LabelPosition const& label,
                                               syntax::Expression const& site ) const
{
  if ( label.inner.has_value() )
  {
    return std::nullopt;
  }
  // A byte of an address is that address, half of it: `<(worker - 1)` is
  // where an `rts` goes, as `worker - 1` is.
  syntax::Expression const* whole = &site;
  if ( whole->kind == syntax::ExpressionKind::UNARY && whole->left != nullptr &&
       ( whole->unaryOperator == syntax::UnaryOperator::LOW_BYTE ||
         whole->unaryOperator == syntax::UnaryOperator::HIGH_BYTE ) )
  {
    whole = whole->left.get();
  }
  return offsetPast( *mSources,
                     *mSymbols,
                     mCharsets,
                     home,
                     *whole,
                     SectionRef{ .module = target.module, .section = label.section },
                     label.chunk );
}

void Rules::checkAlternatives( Chunk const& chunk )
{
  if ( chunk.alternatives.empty() )
  {
    return;
  }
  syntax::Token const first = chunk.alternatives.front();
  auto const* const instruction = std::get_if<InstructionContent>( &chunk.content );
  if ( instruction == nullptr )
  {
    report( diag::diagnostic( diag::DiagnosticId::ALT_NEEDS_INSTRUCTION ).at( first.location, first.length ) );
    return;
  }
  // No operand, or an immediate one: the instruction reaches no memory, and
  // nothing of that shape is a jump, a call or a branch, so every
  // alternative goes on to the next instruction or returns.
  std::string_view const mnemonic = textOf( instruction->mnemonic );
  syntax::OperandShape const shape = instruction->shape;
  if ( shape != syntax::OperandShape::NONE && shape != syntax::OperandShape::IMMEDIATE )
  {
    report( diag::diagnostic( diag::DiagnosticId::ALT_REACHES_MEMORY )
                .at( chunk.span.begin, chunk.span.length )
                .arg( "mnemonic", std::string{ mnemonic } ) );
    return;
  }
  std::optional<AddressingMode> const mode =
      isMnemonic( mnemonic ) ? modeFor( mnemonic, shape, syntax::PlacementClass::ABSOLUTE ) : std::nullopt;
  if ( !mode.has_value() )
  {
    // The instruction itself is Size's to report.
    return;
  }
  for ( syntax::Token const& alternative : chunk.alternatives )
  {
    std::string_view const text = textOf( alternative );
    std::optional<AddressingMode> const its =
        isMnemonic( text ) ? modeFor( text, shape, syntax::PlacementClass::ABSOLUTE ) : std::nullopt;
    if ( !its.has_value() || sizeOf( *its ) != sizeOf( *mode ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::ALT_NO_SUCH_FORM )
                  .at( alternative.location, alternative.length )
                  .arg( "alternative", std::string{ text } )
                  .arg( "mnemonic", std::string{ mnemonic } ) );
      continue;
    }
    if ( mTarget->cpu != Cpu::WDC65SC02 && needs65sc02( text, *its ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::INSTRUCTION_NEEDS_CPU )
                  .at( alternative.location, alternative.length )
                  .arg( "mnemonic", std::string{ text } )
                  .arg( "needs", std::string{ nameOf( Cpu::WDC65SC02 ) } )
                  .arg( "cpu", std::string{ nameOf( mTarget->cpu ) } ) );
    }
  }
}

bool Typer::settableByte( ModuleIndex home,
                          SymbolRef target,
                          LabelPosition const& label,
                          syntax::Expression const& site ) const
{
  std::optional<std::int64_t> const offset = offsetFrom( home, target, label, site );
  if ( !offset.has_value() )
  {
    return false;
  }
  if ( *offset == 0 )
  {
    bool const start =
        label.chunk == ChunkIndex{ 0 } && mSymbols->moduleAt( target.module ).sectionAt( label.section ).isProc();
    return !start && !opcodesAt( target, label ).empty();
  }
  std::optional<std::int64_t> const bytes = settableOperandAt( target, label );
  return bytes.has_value() && *offset >= 1 && *offset <= *bytes;
}

std::vector<std::uint8_t> Typer::opcodesAt( SymbolRef target, LabelPosition const& label ) const
{
  std::vector<std::uint8_t> opcodes;
  if ( label.inner.has_value() )
  {
    return opcodes;
  }
  Section const& section = mSymbols->moduleAt( target.module ).sectionAt( label.section );
  if ( label.chunk.value >= section.chunks().size() )
  {
    return opcodes;
  }
  Chunk const& chunk = section.chunks()[label.chunk.value];
  auto const* const instruction = std::get_if<InstructionContent>( &chunk.content );
  if ( instruction == nullptr || chunk.alternatives.empty() )
  {
    return opcodes;
  }
  auto const add = [&]( std::string_view mnemonic )
  {
    if ( !isMnemonic( mnemonic ) )
    {
      return;
    }
    if ( std::optional<AddressingMode> const mode =
             modeFor( mnemonic, instruction->shape, syntax::PlacementClass::ABSOLUTE );
         mode.has_value() )
    {
      if ( std::optional<std::uint8_t> const opcode = opcodeOf( mnemonic, *mode ); opcode.has_value() )
      {
        opcodes.push_back( *opcode );
      }
    }
  };
  add( textOf( instruction->mnemonic ) );
  for ( syntax::Token const& alternative : chunk.alternatives )
  {
    add( textOf( alternative ) );
  }
  return opcodes;
}

void Rules::checkOpcodeStore( ModuleIndex home, Section const& section, std::uint32_t at )
{
  Chunk const& chunk = section.chunks()[at];
  auto const* const instruction = std::get_if<InstructionContent>( &chunk.content );
  std::span<syntax::ExpressionPtr const> const items = section.itemsOf( chunk );
  if ( instruction == nullptr || instruction->shape != syntax::OperandShape::DIRECT || items.size() != 1 )
  {
    return;
  }
  std::string_view const store = textOf( instruction->mnemonic );
  if ( store != "sta" && store != "stx" && store != "sty" )
  {
    return;
  }

  // What the register holds is what a load of it from an immediate left,
  // where only stores stand between: they change no register. Whatever
  // else stands there, or a label others may come in at, the value is not
  // known here and the declaration is taken at its word. Read on the way
  // in, the value is one this store writes, so a finding about it is never
  // wrong, whatever other paths reach the store.
  std::string const load = std::string{ "ld" } + store.back();
  std::optional<std::int64_t> value;
  for ( std::uint32_t before = at; before-- > 0; )
  {
    Chunk const& earlier = section.chunks()[before];
    auto const* const previous = std::get_if<InstructionContent>( &earlier.content );
    if ( previous == nullptr )
    {
      break;
    }
    std::string_view const text = textOf( previous->mnemonic );
    if ( text == "sta" || text == "stx" || text == "sty" || text == "stz" )
    {
      continue;
    }
    if ( text == load && previous->shape == syntax::OperandShape::IMMEDIATE && section.itemsOf( earlier ).size() == 1 )
    {
      value = declaredValueOf( *mSources, *mSymbols, mCharsets, home, *section.itemsOf( earlier ).front() );
    }
    break;
  }
  if ( !value.has_value() )
  {
    return;
  }

  class Labels final : public ReferenceVisitor
  {
  public:
    explicit Labels( GlobalSymbols const& symbols ) : mSymbols( &symbols ) {}

    void reference( SymbolRef target ) override
    {
      if ( std::holds_alternative<LabelPosition>( mSymbols->at( target ).value ) )
      {
        found.push_back( target );
      }
    }

    std::vector<SymbolRef> found;

  private:
    GlobalSymbols const* mSymbols;
  };

  Labels labels{ *mSymbols };
  walkReferences( *mSymbols, *mSources, home, *items.front(), labels );
  for ( SymbolRef const target : labels.found )
  {
    auto const& label = std::get<LabelPosition>( mSymbols->at( target ).value );
    std::vector<std::uint8_t> const opcodes = mTyper.opcodesAt( target, label );
    if ( opcodes.empty() || mTyper.offsetFrom( home, target, label, *items.front() ) != 0 )
    {
      continue;
    }
    if ( std::ranges::find( opcodes, *value ) != opcodes.end() )
    {
      continue;
    }
    Section const& patched = mSymbols->moduleAt( target.module ).sectionAt( label.section );
    auto const& written = std::get<InstructionContent>( patched.chunks()[label.chunk.value].content );
    report( diag::diagnostic( diag::DiagnosticId::ALT_OPCODE_NOT_DECLARED )
                .at( items.front()->span.begin, items.front()->span.length )
                .arg( "value", *value )
                .arg( "mnemonic", std::string{ textOf( written.mnemonic ) } ) );
  }
}

std::optional<std::int64_t> Typer::settableOperandAt( SymbolRef target, LabelPosition const& label ) const
{
  if ( label.inner.has_value() )
  {
    return std::nullopt;
  }
  Section const& section = mSymbols->moduleAt( target.module ).sectionAt( label.section );
  if ( label.chunk.value >= section.chunks().size() )
  {
    return std::nullopt;
  }
  Chunk const& chunk = section.chunks()[label.chunk.value];
  auto const* const instruction = std::get_if<InstructionContent>( &chunk.content );
  std::span<syntax::ExpressionPtr const> const items = section.itemsOf( chunk );
  if ( instruction == nullptr || items.size() != 1 || containsError( *items.front() ) )
  {
    return std::nullopt;
  }
  // An operand that decides where control goes is not a value: a jump, a
  // call and a branch go wherever it says, and a return has none.
  std::string_view const mnemonic = textOf( instruction->mnemonic );
  if ( !isMnemonic( mnemonic ) || flowOf( mnemonic ) != ControlFlow::NEXT )
  {
    return std::nullopt;
  }

  // The width Size gives it. An Integer that gives none is Size's to report,
  // and is taken here at the wider, so that one operand is one finding.
  syntax::OperandShape const shape = instruction->shape;
  OperandPlacement const found =
      operandPlacementIn( *mSources, *mSymbols, *mCharsets, *mTarget, target.module, *items.front(), shape );
  if ( !found.placement.has_value() && !found.type.is( ExpressionType::INTEGER ) )
  {
    return std::nullopt;
  }
  // Trace takes a direct operand at its word: a store into a Temporary it
  // names ends the value before, and one that went elsewhere would leave
  // that value read where Trace let another share its bytes.
  if ( found.type.is( ExpressionType::ADDRESS ) &&
       mSymbols->moduleAt( found.type.section.module ).sectionAt( found.type.section.section ).isTemporary() )
  {
    return std::nullopt;
  }
  std::optional<AddressingMode> const mode =
      modeFor( mnemonic, shape, found.placement.value_or( syntax::PlacementClass::ABSOLUTE ) );
  if ( !mode.has_value() )
  {
    return std::nullopt;
  }
  return std::int64_t{ sizeOf( *mode ) } - 1;
}

bool Rules::reachesIntoProc(
    ModuleIndex home, SymbolRef target, LabelPosition const& label, syntax::Expression const& site, Reach reach ) const
{
  // What reads changes nothing there, so a read reaches anything. Control
  // comes in at the start and nowhere else, and an address taken is one
  // that control may come in through: the start, or the byte before it
  // that an `rts` goes past. A write sets an operand in place, of an
  // instruction that goes on to the next whatever its operand says, and an
  // opcode where `.alt` declared what else it may be; and the address of
  // such a byte may be taken as data, to write it through — see
  // docs/decisions/0238-the-address-of-a-byte-the-program-may-write-is-data.md,
  // docs/decisions/0231-a-proc-is-read-anywhere-entered-at-its-start-and-written-in-an-immediate.md,
  // docs/decisions/0232-a-write-from-outside-sets-any-operand-that-is-not-a-jump.md
  // and docs/decisions/0237-an-instruction-declares-the-opcodes-it-may-be-given.md.
  bool const atStart = label.chunk == ChunkIndex{ 0 } && !label.inner.has_value();
  switch ( reach.kind )
  {
  case ReferenceKind::READ:
    return true;
  case ReferenceKind::WRITE:
  case ReferenceKind::READ_WRITE:
  {
    if ( !reach.plain || site.kind == syntax::ExpressionKind::UNARY )
    {
      return false;
    }
    std::optional<std::int64_t> const offset = mTyper.offsetFrom( home, target, label, site );
    if ( offset == 0 )
    {
      return !mTyper.opcodesAt( target, label ).empty();
    }
    std::optional<std::int64_t> const bytes = mTyper.settableOperandAt( target, label );
    return bytes.has_value() && offset.has_value() && *offset >= 1 && *offset <= *bytes;
  }
  case ReferenceKind::CALL:
  case ReferenceKind::JUMP:
    return atStart && mTyper.offsetFrom( home, target, label, site ) == 0;
  case ReferenceKind::ESCAPE:
  {
    std::optional<std::int64_t> const offset = mTyper.offsetFrom( home, target, label, site );
    if ( atStart && ( offset == 0 || offset == -1 ) )
    {
      return true;
    }
    // The address of a byte the program may write is data, and only as
    // data: under `.own` or `.root` somebody would follow it in.
    return reach.taking == syntax::Taking::ESCAPE && mTyper.settableByte( home, target, label, site );
  }
  }
  return false;
}

void Rules::checkBranch( ModuleIndex home, SectionIndex section, syntax::Expression const& operand )
{
  class Targets final : public ReferenceVisitor
  {
  public:
    explicit Targets( GlobalSymbols const& symbols ) : mSymbols( &symbols ) {}

    void reference( SymbolRef target ) override
    {
      Symbol const& symbol = mSymbols->at( target );
      if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value ); label != nullptr )
      {
        found.emplace_back( target, SectionRef{ .module = target.module, .section = label->section } );
      }
    }

    std::vector<std::pair<SymbolRef, SectionRef>> found;

  private:
    GlobalSymbols const* mSymbols;
  };

  Targets targets{ *mSymbols };
  walkReferences( *mSymbols, *mSources, home, operand, targets );

  Module const& module = mSymbols->moduleAt( home );
  auto const chained = [&module]( SectionIndex from, SectionIndex to )
  {
    for ( std::optional<SectionIndex> at = module.sectionAt( from ).next(); at.has_value();
          at = module.sectionAt( *at ).next() )
    {
      if ( *at == to )
      {
        return true;
      }
    }
    return false;
  };

  for ( auto const& [symbol, where] : targets.found )
  {
    if ( where.module == home &&
         ( where.section == section || chained( section, where.section ) || chained( where.section, section ) ) )
    {
      continue;
    }
    report( diag::diagnostic( diag::DiagnosticId::BRANCH_ACROSS_SECTIONS )
                .at( operand.span.begin, operand.span.length )
                .arg( "symbol", std::string{ mSymbols->at( symbol ).name } )
                .arg( "section", mSymbols->moduleAt( where.module ).displayNameOf( where.section ) ) );
    return;
  }
}

void Rules::checkSlotUse( ModuleIndex home, SectionIndex section, SymbolRef slot, syntax::Expression const& site )
{
  // The Cell is Resident, so 0028 passes on its own; what it holds is not.
  // In a Phase where no Implementation is live the Cell keeps whatever the
  // last edge wrote, which points at memory that Phase does not hold — the
  // stray Reference 0028 refuses, one indirection later. Reported for the
  // first such Phase the referrer is present in, and once per site.
  Residency const& referrer = mSymbols->moduleAt( home ).residency();
  for ( std::uint32_t index = 0; index < referrer.phaseCount(); ++index )
  {
    PhaseIndex const phase{ index };
    if ( !referrer.includes( phase ) || implementationIn( *mSymbols, slot, phase ).has_value() )
    {
      continue;
    }
    report( diag::diagnostic( diag::DiagnosticId::SLOT_NOT_IMPLEMENTED_IN_PHASE )
                .at( site.span.begin, site.span.length )
                .arg( "slot", std::string{ mSymbols->at( slot ).name } )
                .arg( "phase", mPhases->phases[index].displayName() )
                .arg( "section", mSymbols->moduleAt( home ).displayNameOf( section ) ) );
    return;
  }
}

void Rules::checkSlots()
{
  // At most one Implementation live in any Phase: two whose Modules share a
  // Phase are reported at the later, naming the earlier and the Phase.
  for ( SymbolRef const slot : slotsOf( mSymbols->modules() ) )
  {
    std::span<ResolvedImplementation const> const implementations = mSymbols->implementationsOf( slot );

    // An Implementation in a Pane: the Cell would hold an address in the
    // Window and nothing would show the state, which is the rule of
    // `.with` one indirection later. A `bank` Binding that carries the
    // state is the queue's — see docs/open-questions.md.
    for ( ResolvedImplementation const& implementation : implementations )
    {
      Symbol const& symbol = mSymbols->at( implementation.target );
      std::optional<SectionIndex> section;
      if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value ); label != nullptr )
      {
        section = label->section;
      }
      if ( !section.has_value() )
      {
        continue;
      }
      Section const& holder = mSymbols->moduleAt( implementation.target.module ).sectionAt( *section );
      if ( !holder.paneName().has_value() )
      {
        continue;
      }
      report( diag::diagnostic( diag::DiagnosticId::IMPLEMENTATION_IN_PANE )
                  .at( implementation.span.begin, implementation.span.length )
                  .arg( "symbol", std::string{ symbol.name } )
                  .arg( "slot", std::string{ mSymbols->at( slot ).name } )
                  .arg( "pane", std::string{ mSources->textOf( holder.paneName()->span() ) } ) );
    }

    for ( std::size_t later = 1; later < implementations.size(); ++later )
    {
      for ( std::size_t earlier = 0; earlier < later; ++earlier )
      {
        Residency const& one = mSymbols->moduleAt( implementations[earlier].target.module ).residency();
        Residency const& two = mSymbols->moduleAt( implementations[later].target.module ).residency();
        std::optional<std::uint32_t> shared;
        for ( std::uint32_t index = 0; index < std::min( one.phaseCount(), two.phaseCount() ); ++index )
        {
          if ( one.includes( PhaseIndex{ index } ) && two.includes( PhaseIndex{ index } ) )
          {
            shared = index;
            break;
          }
        }
        if ( !shared.has_value() )
        {
          continue;
        }
        report(
            diag::diagnostic( diag::DiagnosticId::SLOT_IMPLEMENTED_TWICE )
                .at( implementations[later].span.begin, implementations[later].span.length )
                .arg( "slot", std::string{ mSymbols->at( slot ).name } )
                .arg( "phase", mPhases->phases[*shared].displayName() )
                .arg( "symbol", std::string{ mSymbols->at( implementations[later].target ).name } )
                .arg( "other", std::string{ mSymbols->at( implementations[earlier].target ).name } )
                .note( diag::diagnostic( diag::DiagnosticId::OTHER_IMPLEMENTATION )
                           .at( implementations[earlier].span.begin, implementations[earlier].span.length )
                           .arg( "symbol", std::string{ mSymbols->at( implementations[earlier].target ).name } ) ) );
        break;
      }
    }
  }
}

void Rules::checkReference(
    ModuleIndex home, SectionIndex section, SymbolRef target, syntax::Expression const& site, Reach reach )
{
  // Where a Section is named, its bytes are seen, so it is hidden nowhere
  // there. A referrer Prune dropped names nothing.
  if ( mReachable->includes( SectionRef{ .module = home, .section = section } ) )
  {
    Symbol const& symbol = mSymbols->at( target );
    if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value ); label != nullptr )
    {
      mReferrers->add( SectionRef{ .module = target.module, .section = label->section },
                       mSymbols->moduleAt( home ).residency() );
    }
  }

  // From outside a Proc, how far in a Reference may go depends on what it
  // does there. Own Module or not, the rule is about Sections.
  if ( auto const* const label = std::get_if<LabelPosition>( &mSymbols->at( target ).value );
       label != nullptr && !( target.module == home && label->section == section ) &&
       mSymbols->moduleAt( target.module ).sectionAt( label->section ).isProc() &&
       !reachesIntoProc( home, target, *label, site, reach ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::PROC_ENTERED_INSIDE )
                .at( site.span.begin, site.span.length )
                .arg( "proc", mSymbols->moduleAt( target.module ).displayNameOf( label->section ) ) );
    return;
  }

  if ( target.module == home )
  {
    return;
  }

  // Sound only where the referrer's Residency lies within the target's: the
  // target is in memory whenever the referrer is. Reported for the first
  // Phase that breaks it, in declaration order, and once per site.
  //
  // Where bytes written once are held beyond their Module's Residency — in a
  // program without storage, every Section with bytes — a Reference that
  // only touches those bytes may reach them wherever they are held. One that
  // makes code run there may not: the code would run in a Phase where what
  // it names in turn was never promised, so a call, a jump and an address of
  // code followed in keep to the Residency. An address of data the hardware
  // follows — a display list under `.root` — is read, and is bytes — see
  // docs/decisions/0239-bytes-held-beyond-their-residency-are-reached-where-they-are-held.md.
  Residency const& referrer = mSymbols->moduleAt( home ).residency();
  Residency referred = mSymbols->moduleAt( target.module ).residency();
  if ( auto const* const label = std::get_if<LabelPosition>( &mSymbols->at( target ).value ); label != nullptr )
  {
    Section const& held = mSymbols->moduleAt( target.module ).sectionAt( label->section );
    bool const code = std::ranges::any_of( held.chunks(),
                                           []( Chunk const& chunk )
                                           { return std::holds_alternative<InstructionContent>( chunk.content ); } );
    bool const runsThere = reach.kind == ReferenceKind::CALL || reach.kind == ReferenceKind::JUMP ||
                           ( reach.kind == ReferenceKind::ESCAPE && reach.taking != syntax::Taking::ESCAPE && code );
    if ( !runsThere )
    {
      referred = heldAcross( *mPhases, *mTarget, held, referred );
    }
  }
  for ( std::uint32_t index = 0; index < referrer.phaseCount(); ++index )
  {
    PhaseIndex const phase{ index };
    if ( !referrer.includes( phase ) || referred.includes( phase ) )
    {
      continue;
    }
    report( diag::diagnostic( diag::DiagnosticId::CROSS_RESIDENCY_REFERENCE )
                .at( site.span.begin, site.span.length )
                .arg( "symbol", std::string{ mSymbols->at( target ).name } )
                .arg( "phase", mPhases->phases[index].displayName() )
                .arg( "section", mSymbols->moduleAt( home ).displayNameOf( section ) ) );
    return;
  }
}

bool Rules::refuseAccumulatorOperand( ModuleIndex home,
                                      InstructionContent const& instruction,
                                      std::span<syntax::ExpressionPtr const> items )
{
  if ( instruction.shape != syntax::OperandShape::DIRECT || items.size() != 1 ||
       items[0]->kind != syntax::ExpressionKind::NAME )
  {
    return false;
  }
  syntax::Expression const& operand = *items[0];
  std::string_view const name = textOf( operand.token );
  if ( name != "a" || mSymbols->resolveText( home, operand, name ).has_value() || mSymbols->isNamespace( name ) )
  {
    return false;
  }

  // An instruction with an accumulator form and a form on memory: the four
  // shifts, and `inc` and `dec` of the 65SC02, whose processor is checked
  // when the instruction is encoded as for any of its own.
  std::string_view const mnemonic = textOf( instruction.mnemonic );
  if ( !isMnemonic( mnemonic ) || !opcodeOf( mnemonic, AddressingMode::IMPLIED ).has_value() ||
       !opcodeOf( mnemonic, AddressingMode::ZERO_PAGE ).has_value() )
  {
    return false;
  }
  report( diag::diagnostic( diag::DiagnosticId::ACCUMULATOR_OPERAND )
              .at( operand.span.begin, operand.span.length )
              .arg( "mnemonic", std::string{ mnemonic } ) );
  return true;
}

void Rules::checkChunk( ModuleIndex home, SectionIndex here, Section const& section, Chunk const& chunk )
{
  std::span<syntax::ExpressionPtr const> const items = section.itemsOf( chunk );
  auto const* const instruction = std::get_if<InstructionContent>( &chunk.content );

  Reach const reach{ .kind = referenceKindOf( *mSources, chunk ),
                     .plain = instruction != nullptr && instruction->shape == syntax::OperandShape::DIRECT &&
                              items.size() == 1,
                     .taking = chunk.taking };

  // Every expression a Chunk encodes is a Reference where it names memory,
  // and the rule about where one may point is asked of each. An assertion
  // is not a Chunk and encodes nothing, so it may look anywhere.
  for ( syntax::ExpressionPtr const& item : items )
  {
    checkResidency( home, here, *item, reach );
  }

  if ( instruction != nullptr )
  {
    if ( refuseAccumulatorOperand( home, *instruction, items ) )
    {
      return;
    }
    bool const branch = opcodeOf( textOf( instruction->mnemonic ), AddressingMode::RELATIVE ).has_value();
    for ( syntax::ExpressionPtr const& operand : items )
    {
      Type const type = mTyper.expectOneOf( home,
                                            *operand,
                                            { ExpressionType::INTEGER, ExpressionType::ADDRESS, ExpressionType::PANE },
                                            diag::diagnostic( diag::DiagnosticId::OPERAND_TYPE ) );
      // A Pane is asked for its value and never reached as memory: an
      // immediate, and no other operand.
      if ( type.is( ExpressionType::PANE ) && instruction->shape != syntax::OperandShape::IMMEDIATE )
      {
        report( diag::diagnostic( diag::DiagnosticId::PANE_NOT_AN_ADDRESS )
                    .at( operand->span.begin, operand->span.length ) );
      }
      mTyper.refuseLinear( type, *operand );
      if ( branch )
      {
        checkBranch( home, here, *operand );
      }
    }
    return;
  }

  if ( std::holds_alternative<ReserveContent>( chunk.content ) )
  {
    for ( syntax::ExpressionPtr const& size : items )
    {
      mTyper.expectInteger( mTyper.typeOf( home, *size ), *size );
    }
    return;
  }

  if ( std::holds_alternative<DispatchContent>( chunk.content ) )
  {
    checkDispatch( home, here, chunk, items );
    return;
  }

  if ( !std::holds_alternative<DataContent>( chunk.content ) )
  {
    // The Target's own kinds — a Transition, a table, the Cell — carry no
    // expressions to check.
    return;
  }

  // `.byte` takes Integer or String, `.word` takes Integer or Address. A
  // String in `.word` is an ordinary type error rather than a rule of the
  // directive.
  bool const isByte = std::get<DataContent>( chunk.content ).width == syntax::DataWidth::BYTE;
  for ( syntax::ExpressionPtr const& item : items )
  {
    Type const type = mTyper.expectOneOf(
        home,
        *item,
        isByte ? std::initializer_list<ExpressionType>{ ExpressionType::INTEGER,
                                                        ExpressionType::STRING,
                                                        ExpressionType::PANE }
               : std::initializer_list<ExpressionType>{ ExpressionType::INTEGER,
                                                        ExpressionType::ADDRESS,
                                                        ExpressionType::PANE },
        diag::diagnostic( diag::DiagnosticId::DATA_ITEM_TYPE )
            .arg( "directive", isByte ? "byte" : "word" )
            .arg( "allowed", isByte ? "an integer, a string or a pane" : "an integer, an address or a pane" ) );
    mTyper.refuseLinear( type, *item );
  }
}

/// A Dispatch goes to one of the positions it names and they are positions of
/// the Section it stands in: that is what makes where it goes known, and it
/// is what tells this statement from `.own`, whose table reaches other
/// Sections and pays for it in liveness — see
/// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
void Rules::checkDispatch( ModuleIndex home,
                           SectionIndex here,
                           Chunk const& chunk,
                           std::span<syntax::ExpressionPtr const> targets )
{
  if ( targets.size() > MOST_DISPATCH_TARGETS )
  {
    report( diag::diagnostic( diag::DiagnosticId::DISPATCH_TOO_MANY_TARGETS )
                .at( chunk.span.begin, chunk.span.length )
                .arg( "count", static_cast<std::int64_t>( targets.size() ) )
                .arg( "most", static_cast<std::int64_t>( MOST_DISPATCH_TARGETS ) ) );
  }

  class Visitor final : public ReferenceVisitor
  {
  public:
    Visitor( Rules& rules, ModuleIndex home, SectionIndex here, syntax::Expression const& site )
        : mRules( &rules ), mHome( home ), mHere( here ), mSite( &site )
    {
    }

    void reference( SymbolRef target ) override
    {
      auto const* const label = std::get_if<LabelPosition>( &mRules->mSymbols->at( target ).value );
      if ( label != nullptr && target.module == mHome && label->section == mHere )
      {
        return;
      }
      // A Slot, a Section of another Module, a Constant that folded to a
      // number: none of them is a position of this Section.
      refuse( std::string{ mRules->mSymbols->at( target ).name } );
    }

    void localReference( LabelPosition target ) override
    {
      // A local label is of the Proc it is written in, so this holds by
      // construction; it is asked because the rule is the statement's and not
      // the scope's.
      if ( target.section != mHere )
      {
        refuse( std::string{ mRules->mSources->textOf( mSite->span ) } );
      }
    }

  private:
    void refuse( std::string name )
    {
      mRules->report( diag::diagnostic( diag::DiagnosticId::DISPATCH_LEAVES_SECTION )
                          .at( mSite->span.begin, mSite->span.length )
                          .arg( "symbol", std::move( name ) )
                          .arg( "section", mRules->mSymbols->moduleAt( mHome ).displayNameOf( mHere ) ) );
    }

    Rules* mRules;
    ModuleIndex mHome;
    SectionIndex mHere;
    syntax::Expression const* mSite;
  };

  for ( syntax::ExpressionPtr const& target : targets )
  {
    if ( containsError( *target ) )
    {
      continue;
    }
    // An address and nothing else: a number is an address no name stands
    // for, and the rule below is about a name.
    if ( !mTyper
              .expectOneOf( home,
                            *target,
                            { ExpressionType::ADDRESS },
                            diag::diagnostic( diag::DiagnosticId::DATA_ITEM_TYPE )
                                .arg( "directive", "dispatch" )
                                .arg( "allowed", "a position of this section" ) )
              .is( ExpressionType::ADDRESS ) )
    {
      continue;
    }
    Visitor visitor{ *this, home, here, *target };
    walkReferences( *mSymbols, *mSources, home, *target, visitor );
  }
}

void Rules::checkSignature( ModuleIndex home, Module const& module, Section const& proc )
{
  auto holds = [&]( Declared const& declared )
  {
    if ( ( !declared.type.has_value() && declared.place.empty() ) || !declared.temporary.has_value() )
    {
      return;
    }
    Section const& temporary = module.sectionAt( *declared.temporary );
    if ( temporary.chunks().empty() )
    {
      return;
    }
    std::span<syntax::ExpressionPtr const> const items = temporary.itemsOf( temporary.chunks().front() );
    if ( items.empty() )
    {
      return;
    }
    // A size that names something no Step has produced yet is not a number
    // here, and is nothing this rule can hold a type to.
    std::optional<std::int64_t> const reserved =
        declaredValueOf( *mSources, *mSymbols, mCharsets, home, *items.front() );
    if ( !declared.place.empty() )
    {
      // The reservation holds the bytes the place keeps in memory, and those
      // alone: see docs/decisions/0145-an-argument-in-a-register.md.
      if ( reserved.has_value() && !std::cmp_equal( *reserved, declared.reservedBytes() ) )
      {
        report( diag::diagnostic( diag::DiagnosticId::DECLARED_PLACE_BYTES )
                    .at( declared.span.begin, declared.span.length )
                    .arg( "place", declared.place )
                    .arg( "want", bytesCounted( std::int64_t{ declared.reservedBytes() } ) )
                    .arg( "got", bytesCounted( *reserved ) ) );
      }
      return;
    }
    if ( !reserved.has_value() || std::cmp_equal( *reserved, declared.type->bytes() ) )
    {
      return;
    }
    report( diag::diagnostic( diag::DiagnosticId::DECLARED_TYPE_BYTES )
                .at( declared.type->span.begin, declared.type->span.length )
                .arg( "type", spellingOf( *declared.type ) )
                .arg( "want", std::to_string( declared.type->bytes() ) )
                .arg( "got", std::to_string( *reserved ) ) );
  };

  for ( Declared const& argument : proc.signature().arguments )
  {
    holds( argument );
  }
  if ( proc.signature().result.has_value() )
  {
    holds( *proc.signature().result );
  }

  // A result declared to be another Proc's names the reservation that Proc
  // returns in, whole in memory — see
  // docs/decisions/0278-a-result-may-be-another-procs-result.md.
  if ( std::optional<Declared> const& result = proc.signature().result;
       result.has_value() && !result->elsewhere.empty() )
  {
    std::optional<SymbolRef> const named = mSymbols->lookup( home, result->elsewhere );
    std::string_view what;
    if ( !named.has_value() )
    {
      what = "names nothing this module sees";
    }
    else if ( !declaredResultAt( *mSymbols, *named ).has_value() )
    {
      what = "is no reservation a proc declares its result in";
    }
    if ( !what.empty() )
    {
      report( diag::diagnostic( diag::DiagnosticId::DECLARED_RESULT_ELSEWHERE )
                  .at( result->elsewhereSpan.begin, result->elsewhereSpan.length )
                  .arg( "name", result->elsewhere )
                  .arg( "what", std::string{ what } ) );
    }
  }
}

void Rules::checkModule( ModuleIndex home )
{
  Module const& module = mSymbols->moduleAt( home );

  // A Constant nothing uses is still a definition, and a bad one is still
  // wrong. Typing them all is also what fills the memo before any use site
  // reaches them.
  for ( Symbol const& symbol : module.symbols().symbols() )
  {
    if ( symbol.kind == SymbolKind::CONSTANT )
    {
      std::optional<SymbolRef> const where = mSymbols->lookup( home, symbol.name );
      if ( where.has_value() )
      {
        mTyper.typeOfSymbol( *where, *std::get<syntax::ExpressionPtr>( symbol.value ) );
      }
    }
  }

  for ( std::uint32_t index = 0; index < module.sections().size(); ++index )
  {
    Section const& section = module.sections()[index];
    SectionIndex const here{ index };
    for ( syntax::Expression const* declared : { section.pinnedAddress(), section.alignment(), section.boundary() } )
    {
      if ( declared != nullptr )
      {
        mTyper.expectInteger( mTyper.typeOf( home, *declared ), *declared );
      }
    }

    if ( section.isProc() )
    {
      checkSignature( home, module, section );
    }

    for ( std::uint32_t chunkIndex = 0; chunkIndex < section.chunks().size(); ++chunkIndex )
    {
      Chunk const& chunk = section.chunks()[chunkIndex];
      if ( std::holds_alternative<MacroUseContent>( chunk.content ) )
      {
        // The use's own items are the arguments, which nothing encodes: what
        // is encoded is the expansion, where each argument stands cloned.
        for ( Chunk const& inner : section.innerChunksOf( ChunkIndex{ chunkIndex } ) )
        {
          checkChunk( home, here, section, inner );
          checkAlternatives( inner );
        }
        checkAlternatives( chunk );
        continue;
      }
      checkChunk( home, here, section, chunk );
      checkAlternatives( chunk );
      checkOpcodeStore( home, section, chunkIndex );
    }
  }

  for ( Assertion const& assertion : module.assertions() )
  {
    mTyper.expectInteger( mTyper.typeOf( home, *assertion.condition ), *assertion.condition );
  }
}

void Rules::checkEverything()
{
  for ( std::uint32_t index = 0; index < mSymbols->modules().size(); ++index )
  {
    checkModule( ModuleIndex{ index } );
  }
  checkSlots();
}

} // namespace

OperandPlacement
operandPlacement( Merged const& build, ModuleIndex home, syntax::Expression const& operand, syntax::OperandShape shape )
{
  return operandPlacementIn( build.sources(), build.symbols(), build.charsets(), build.target(), home, operand, shape );
}

Type typeOf( Merged const& build, ModuleIndex home, syntax::Expression const& node, diag::DiagnosticSink& sink )
{
  // One expression's type asks nothing about Residency, so no Phases.
  Typer typer{ build.sources(), build.symbols(), build.charsets(), build.target(), sink };
  return typer.typeOf( home, node );
}

bool takenAsData( Merged const& build, ModuleIndex home, SymbolRef target, syntax::Expression const& site )
{
  auto const* const label = std::get_if<LabelPosition>( &build.symbols().at( target ).value );
  if ( label == nullptr )
  {
    return false;
  }
  // Only among code is there a question: an address into a Section of data
  // alone is data already, and one into a Temporary is that Temporary
  // escaping, which 0034 has its own word for.
  Section const& section = build.symbols().moduleAt( target.module ).sectionAt( label->section );
  bool const holdsCode = std::ranges::any_of( section.chunks(),
                                              []( Chunk const& chunk )
                                              { return std::holds_alternative<InstructionContent>( chunk.content ); } );
  if ( holdsCode && !section.isProc() && !label->inner.has_value() && label->chunk.value < section.chunks().size() )
  {
    ChunkContent const& content = section.chunks()[label->chunk.value].content;
    if ( std::holds_alternative<DataContent>( content ) || std::holds_alternative<ReserveContent>( content ) )
    {
      return true;
    }
  }
  return namesSettableByte( build, home, target, site );
}

bool namesSettableByte( Merged const& build, ModuleIndex home, SymbolRef target, syntax::Expression const& site )
{
  auto const* const label = std::get_if<LabelPosition>( &build.symbols().at( target ).value );
  if ( label == nullptr )
  {
    return false;
  }
  diag::SeverityPolicy policy;
  diag::DiagnosticSink quiet{ policy };
  Typer const typer{ build.sources(), build.symbols(), build.charsets(), build.target(), quiet };
  return typer.settableByte( home, target, *label, site );
}

Referrers::Referrers( std::span<Module const> modules )
{
  mByModule.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    mByModule[module].assign( modules[module].sections().size(),
                              Residency{ modules[module].residency().phaseCount() } );
  }
}

void Referrers::add( SectionRef where, Residency const& from )
{
  Residency& named = mByModule[where.module.value][where.section.value];
  for ( std::uint32_t phase = 0; phase < from.phaseCount() && phase < named.phaseCount(); ++phase )
  {
    if ( from.includes( PhaseIndex{ phase } ) )
    {
      named.add( PhaseIndex{ phase } );
    }
  }
}

Residency const& Referrers::of( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value];
}

namespace
{

/// The cells the driver's stream stands in hold what the routine writes into
/// them: a unit as wide as the Target's and an offset of two. Measured off the
/// reservation under the label, as a `.declare arg` without a type is, which is
/// why this waits for a Step that can evaluate the reservation's size — see
/// docs/decisions/0228-a-driver-names-the-cells-its-stream-stands-in.md.
void checkDriverCells( Pruned const& build, diag::DiagnosticSink& sink )
{
  std::optional<Driver> const& driver = build.project().driver;
  if ( !driver.has_value() || !driver->streams )
  {
    return;
  }
  GlobalSymbols const& symbols = build.symbols();
  Module const& module = symbols.moduleAt( driver->module );
  std::int64_t const unitBytes = build.project().target.unitBytes();
  for ( auto const& [name, symbol, want] :
        { std::tuple{ std::string_view{ "unit" }, driver->unit, unitBytes },
          std::tuple{ std::string_view{ "offset" }, driver->offset, std::int64_t{ 2 } } } )
  {
    auto const role =
        std::ranges::find_if( module.driverRoles(), [name]( DriverRole const& one ) { return one.role == name; } );
    auto const* const label = std::get_if<LabelPosition>( &module.symbols().symbols()[symbol].value );
    if ( role == module.driverRoles().end() || label == nullptr )
    {
      continue;
    }
    Section const& section = module.sectionAt( label->section );
    std::optional<std::int64_t> reserved;
    if ( !label->inner.has_value() && label->chunk.value < section.chunks().size() )
    {
      Chunk const& chunk = section.chunks()[label->chunk.value];
      std::span<syntax::ExpressionPtr const> const items = section.itemsOf( chunk );
      if ( std::holds_alternative<ReserveContent>( chunk.content ) && !items.empty() )
      {
        reserved = declaredValueOf( build.sources(), symbols, &build.charsets(), driver->module, *items.front() );
        if ( !reserved.has_value() )
        {
          // A size no Step has produced yet: nothing this rule can hold.
          continue;
        }
      }
    }
    // How wide a unit's number is, storage says; without it there is no
    // routine to write one and nothing to hold the cell to — see
    // docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.
    if ( reserved == want || ( name == "unit" && !build.project().target.hasStorage() ) )
    {
      continue;
    }
    sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_CELL_BYTES )
                  .at( role->labelSpan.begin, role->labelSpan.length )
                  .arg( "role", std::string{ name } )
                  .arg( "got",
                        reserved.has_value() ? "a reservation of " + bytesCounted( *reserved )
                                             : std::string{ "no reservation" } )
                  .arg( "want", bytesCounted( want ) ) );
  }
}

} // namespace

Referrers checkTypes( Pruned const& build, diag::DiagnosticSink& sink )
{
  Referrers referrers{ build.modules() };
  Rules rules{ build.sources(), build.symbols(),   build.charsets(), build.target(),
               build.phases(),  build.reachable(), referrers,        sink };
  rules.checkEverything();
  checkWiths( build, sink );
  checkDriverCells( build, sink );
  return referrers;
}

} // namespace nga::model
