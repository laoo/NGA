#include "nga/model/TypeCheck.hpp"

#include "nga/model/With.hpp"

#include "nga/model/Isa.hpp"

#include "nga/model/Dispatch.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/References.hpp"
#include "nga/model/Slots.hpp"

#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model
{

namespace
{

using syntax::ExpressionType;

/// Ordering rather than equality, which is the half of comparison that relates
/// two positions to each other.
bool isOrdering( syntax::BinaryOperator op )
{
  return op == syntax::BinaryOperator::LESS || op == syntax::BinaryOperator::LESS_EQUAL ||
         op == syntax::BinaryOperator::GREATER || op == syntax::BinaryOperator::GREATER_EQUAL;
}

std::string typeName( Type const& type )
{
  return std::string{ nameOf( type.kind ) };
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
         PhaseGraph const* phases,
         Reachable const* reachable,
         Freezes* freezes,
         diag::DiagnosticSink& sink )
      : mSources( &sources ), mSymbols( &symbols ), mCharsets( &charsets ), mTarget( &target ), mPhases( phases ),
        mReachable( reachable ), mFreezes( freezes ), mSink( &sink )
  {
  }

  Type typeOf( ModuleIndex home, syntax::Expression const& node );

  /// Every expression of the program, and the rules that belong to the
  /// statement holding one rather than to the expression itself.
  void checkEverything();

private:
  Type typeOfName( ModuleIndex home, syntax::Expression const& node );
  Type typeOfLocalName( ModuleIndex home, syntax::Expression const& node );
  Type typeOfSymbol( SymbolRef where, syntax::Expression const& site );
  Type typeOfAttribute( ModuleIndex home, syntax::Expression const& node );
  void checkCharset( ModuleIndex home, syntax::Expression const& node );
  Type typeOfUnary( ModuleIndex home, syntax::Expression const& node );
  Type typeOfBinary( ModuleIndex home, syntax::Expression const& node );

  /// The Address a position in a Section has: runtime space, that Section, and
  /// the PlacementClass the Section declared — never one read back from an
  /// address the solver chose.
  [[nodiscard]] Type addressOf( ModuleIndex module, LabelPosition position ) const;

  Type expectInteger( Type const& type, syntax::Expression const& node );

  /// One of a fixed set of types, or a finding naming what was written.
  /// Answers the type either way, so that a rule about one of the allowed
  /// kinds can go on from here without typing the expression twice.
  Type expectOneOf( ModuleIndex home,
                    syntax::Expression const& node,
                    std::initializer_list<ExpressionType> allowed,
                    diag::Diagnostic complaint );

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
  void checkDispatch( ModuleIndex home,
                      SectionIndex here,
                      Chunk const& chunk,
                      std::span<syntax::ExpressionPtr const> targets );

  /// The rule on where a Reference may point, over one expression of a Chunk
  /// in `section` of `home`: every name in it that reaches memory — a Label, a
  /// Section, a Constant defined in terms of either — must reach a Module
  /// present wherever `home` is. `site` is the name as written in the Chunk,
  /// which is where the finding lands when a Constant led elsewhere.
  void checkResidency( ModuleIndex home, SectionIndex section, syntax::Expression const& node );
  void checkSlotUse( ModuleIndex home, SectionIndex section, SymbolRef slot, syntax::Expression const& site );
  void checkSlots();
  void checkReference( ModuleIndex home, SectionIndex section, SymbolRef target, syntax::Expression const& site );

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

  diag::SourceManager const* mSources;
  GlobalSymbols const* mSymbols;
  Charsets const* mCharsets;
  Target const* mTarget;
  PhaseGraph const* mPhases;

  /// Null when one expression is typed on its own, which asks nothing about
  /// where a Section may move.
  Reachable const* mReachable;
  Freezes* mFreezes;
  diag::DiagnosticSink* mSink;

  /// What a name has already been found to be. Without it a Constant used ten
  /// times would report the same bad definition ten times.
  std::unordered_map<std::uint64_t, Type> mKnown;

  /// The Constants being typed right now, so that a definition in terms of
  /// itself is one finding rather than a stack overflow.
  std::vector<SymbolRef> mVisiting;
};

std::uint64_t keyOf( SymbolRef where )
{
  return ( static_cast<std::uint64_t>( where.module.value ) << 32U ) | where.index;
}

Type Typer::addressOf( ModuleIndex module, LabelPosition position ) const
{
  Section const& section = mSymbols->moduleAt( module ).sectionAt( position.section );
  return Type::address( SectionRef{ .module = module, .section = position.section }, section.placement() );
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
  if ( node.left != nullptr && node.left->kind == syntax::ExpressionKind::NAME && textOf( node.left->token ) == "nga" )
  {
    // The tool's namespace holds the driver's roles, which are macros and
    // stand where a mnemonic does, and nothing a value could be made of.
    std::string_view const role = textOf( node.token );
    bool const known = role == "open" || role == "read" || role == "show" || role == "showAt";
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
  if ( attribute == "runtimeSectionAddress" )
  {
    Section const& section = mSymbols->moduleAt( named->module ).sectionAt( named->section );
    return Type::address( *named, section.placement() );
  }
  if ( attribute == "runtimeSectionSize" )
  {
    return Type::integer();
  }
  if ( attribute == "resident" )
  {
    // Whether the Section is present in every Phase: a fact of the Project,
    // known before any Step runs, so that `.assert vars.resident` is how
    // source states what it depends on without declaring it.
    return Type::integer();
  }

  // `storagePayloadAddress` and `storagePayloadSize` are reserved and not
  // built: nothing reads storage by itself yet, and the space tag is what
  // keeps runtime and storage apart until something does.
  report( diag::diagnostic( diag::DiagnosticId::NO_SUCH_ATTRIBUTE )
              .at( node.token.location, node.token.length )
              .arg( "name", std::string{ attribute } ) );
  return Type::unknown();
}

Type Typer::typeOfUnary( ModuleIndex home, syntax::Expression const& node )
{
  Type const operand = typeOf( home, *node.left );

  switch ( node.unaryOperator )
  {
  case syntax::UnaryOperator::LOW_BYTE:
  case syntax::UnaryOperator::HIGH_BYTE:
    // Byte extraction is what an Address is for as much as an Integer is.
    if ( !operand.isKnown() )
    {
      return Type::unknown();
    }
    if ( operand.is( ExpressionType::INTEGER ) || operand.is( ExpressionType::ADDRESS ) )
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
    return typeOf( home, *node.right );
  }
  if ( node.binaryOperator == syntax::BinaryOperator::ARM )
  {
    Type const taken = typeOf( home, *node.left );
    Type const otherwise = typeOf( home, *node.right );
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

  case syntax::BinaryOperator::MULTIPLY:
  case syntax::BinaryOperator::DIVIDE:
  case syntax::BinaryOperator::SHIFT_LEFT:
  case syntax::BinaryOperator::SHIFT_RIGHT:
  case syntax::BinaryOperator::LOGICAL_AND:
  case syntax::BinaryOperator::LOGICAL_OR:
    return expectInteger( left, node ).isKnown() && expectInteger( right, node ).isKnown() ? Type::integer()
                                                                                           : Type::unknown();
  }
  return Type::unknown();
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

void Typer::checkResidency( ModuleIndex home, SectionIndex section, syntax::Expression const& node )
{
  // The finding lands on the expression as written in the Chunk, which is
  // `node` itself, however far a Constant led from there.
  class Visitor final : public ReferenceVisitor
  {
  public:
    Visitor( Typer& typer, ModuleIndex home, SectionIndex section, syntax::Expression const& site )
        : mTyper( &typer ), mHome( home ), mSection( section ), mSite( &site )
    {
    }

    void reference( SymbolRef target ) override
    {
      if ( mTyper->mSymbols->at( target ).kind == SymbolKind::SLOT )
      {
        mTyper->checkSlotUse( mHome, mSection, target, *mSite );
        return;
      }
      mTyper->checkReference( mHome, mSection, target, *mSite );
    }

  private:
    Typer* mTyper;
    ModuleIndex mHome;
    SectionIndex mSection;
    syntax::Expression const* mSite;
  };

  Visitor visitor{ *this, home, section, node };
  walkReferences( *mSymbols, *mSources, home, node, visitor );
}

void Typer::checkBranch( ModuleIndex home, SectionIndex section, syntax::Expression const& operand )
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
                .arg( "section", mSymbols->moduleAt( where.module ).displayNameOf( where.section, *mSources ) ) );
    return;
  }
}

void Typer::checkSlotUse( ModuleIndex home, SectionIndex section, SymbolRef slot, syntax::Expression const& site )
{
  // The Cell is Resident, so 0028 passes on its own; what it holds is not.
  // In a Phase where no Implementation is live the Cell keeps whatever the
  // last edge wrote, which points at memory that Phase does not hold — the
  // stray Reference 0028 refuses, one indirection later. Reported for the
  // first such Phase the referrer is present in, and once per site.
  if ( mPhases == nullptr )
  {
    return;
  }
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
                .arg( "phase", mPhases->phases[index].name.value_or( "(implicit)" ) )
                .arg( "section", mSymbols->moduleAt( home ).displayNameOf( section, *mSources ) ) );
    return;
  }
}

void Typer::checkSlots()
{
  // At most one Implementation live in any Phase: two whose Modules share a
  // Phase are reported at the later, naming the earlier and the Phase.
  if ( mPhases == nullptr )
  {
    return;
  }
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
                .arg( "phase", mPhases->phases[*shared].name.value_or( "(implicit)" ) )
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

void Typer::checkReference( ModuleIndex home, SectionIndex section, SymbolRef target, syntax::Expression const& site )
{
  // A direct Reference to a Movable Section holds it to one address across
  // the referrer's Residency — before the rule below, because a Section's own
  // Module refers to it from every Phase it is in, and that is the Reference
  // that keeps it from moving at all. A referrer Prune dropped encodes
  // nothing, and holds nothing.
  if ( mFreezes != nullptr && mReachable->includes( SectionRef{ .module = home, .section = section } ) )
  {
    Symbol const& symbol = mSymbols->at( target );
    std::optional<SectionIndex> memory;
    if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value ); label != nullptr )
    {
      memory = label->section;
    }
    if ( memory.has_value() && mSymbols->moduleAt( target.module ).sectionAt( *memory ).isMovable() )
    {
      mFreezes->freeze( SectionRef{ .module = target.module, .section = *memory },
                        mSymbols->moduleAt( home ).residency() );
    }
  }

  // A Proc has one entry, the Label at its start: from outside it nothing
  // names a position inside, whether to call it, to read a table kept there
  // or to patch an operand. What is meant to be reached from outside lives
  // in a `.section`. Own Module or not, the rule is about Sections.
  if ( auto const* const label = std::get_if<LabelPosition>( &mSymbols->at( target ).value );
       label != nullptr && label->chunk != ChunkIndex{ 0 } && !( target.module == home && label->section == section ) &&
       mSymbols->moduleAt( target.module ).sectionAt( label->section ).isProc() )
  {
    report( diag::diagnostic( diag::DiagnosticId::PROC_ENTERED_INSIDE )
                .at( site.span.begin, site.span.length )
                .arg( "symbol", std::string{ mSymbols->at( target ).name } )
                .arg( "proc", mSymbols->moduleAt( target.module ).displayNameOf( label->section, *mSources ) ) );
    return;
  }

  if ( target.module == home || mPhases == nullptr )
  {
    return;
  }

  // Sound only where the referrer's Residency lies within the target's: the
  // target is in memory whenever the referrer is. Reported for the first
  // Phase that breaks it, in declaration order, and once per site.
  Residency const& referrer = mSymbols->moduleAt( home ).residency();
  Residency const& referred = mSymbols->moduleAt( target.module ).residency();
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
                .arg( "phase", mPhases->phases[index].name.value_or( "(implicit)" ) )
                .arg( "section", mSymbols->moduleAt( home ).displayNameOf( section, *mSources ) ) );
    return;
  }
}

void Typer::checkChunk( ModuleIndex home, SectionIndex here, Section const& section, Chunk const& chunk )
{
  std::span<syntax::ExpressionPtr const> const items = section.itemsOf( chunk );

  // Every expression a Chunk encodes is a Reference where it names memory,
  // and the rule about where one may point is asked of each. An assertion
  // is not a Chunk and encodes nothing, so it may look anywhere.
  for ( syntax::ExpressionPtr const& item : items )
  {
    checkResidency( home, here, *item );
  }

  if ( auto const* const instruction = std::get_if<InstructionContent>( &chunk.content ); instruction != nullptr )
  {
    bool const branch = opcodeOf( textOf( instruction->mnemonic ), AddressingMode::RELATIVE ).has_value();
    for ( syntax::ExpressionPtr const& operand : items )
    {
      Type const type = expectOneOf( home,
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
      expectInteger( typeOf( home, *size ), *size );
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
    expectOneOf(
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
  }
}

/// A Dispatch goes to one of the positions it names and they are positions of
/// the Section it stands in: that is what makes where it goes known, and it
/// is what tells this statement from `.own`, whose table reaches other
/// Sections and pays for it in liveness — see
/// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
void Typer::checkDispatch( ModuleIndex home,
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
    Visitor( Typer& typer, ModuleIndex home, SectionIndex here, syntax::Expression const& site )
        : mTyper( &typer ), mHome( home ), mHere( here ), mSite( &site )
    {
    }

    void reference( SymbolRef target ) override
    {
      auto const* const label = std::get_if<LabelPosition>( &mTyper->mSymbols->at( target ).value );
      if ( label != nullptr && target.module == mHome && label->section == mHere )
      {
        return;
      }
      // A Slot, a Section of another Module, a Constant that folded to a
      // number: none of them is a position of this Section.
      refuse( std::string{ mTyper->mSymbols->at( target ).name } );
    }

    void localReference( LabelPosition target ) override
    {
      // A local label is of the Proc it is written in, so this holds by
      // construction; it is asked because the rule is the statement's and not
      // the scope's.
      if ( target.section != mHere )
      {
        refuse( std::string{ mTyper->mSources->textOf( mSite->span ) } );
      }
    }

  private:
    void refuse( std::string name )
    {
      mTyper->report(
          diag::diagnostic( diag::DiagnosticId::DISPATCH_LEAVES_SECTION )
              .at( mSite->span.begin, mSite->span.length )
              .arg( "symbol", std::move( name ) )
              .arg( "section", mTyper->mSymbols->moduleAt( mHome ).displayNameOf( mHere, *mTyper->mSources ) ) );
    }

    Typer* mTyper;
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
    if ( !expectOneOf( home,
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

void Typer::checkSignature( ModuleIndex home, Module const& module, Section const& proc )
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
                    .arg( "want", std::to_string( declared.reservedBytes() ) )
                    .arg( "got", std::to_string( *reserved ) ) );
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
}

void Typer::checkModule( ModuleIndex home )
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
        typeOfSymbol( *where, *std::get<syntax::ExpressionPtr>( symbol.value ) );
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
        expectInteger( typeOf( home, *declared ), *declared );
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
        }
        continue;
      }
      checkChunk( home, here, section, chunk );
    }
  }

  for ( Assertion const& assertion : module.assertions() )
  {
    expectInteger( typeOf( home, *assertion.condition ), *assertion.condition );
  }
}

void Typer::checkEverything()
{
  for ( std::uint32_t index = 0; index < mSymbols->modules().size(); ++index )
  {
    checkModule( ModuleIndex{ index } );
  }
  checkSlots();
}

} // namespace

Type typeOf( Merged const& build, ModuleIndex home, syntax::Expression const& node, diag::DiagnosticSink& sink )
{
  // One expression's type asks nothing about Residency, so no Phases.
  Typer typer{ build.sources(), build.symbols(), build.charsets(), build.target(), nullptr, nullptr, nullptr, sink };
  return typer.typeOf( home, node );
}

Freezes::Freezes( std::span<Module const> modules )
{
  mByModule.resize( modules.size() );
  mKeepOut.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    mKeepOut[module].resize( modules[module].sections().size() );
    mByModule[module].resize( modules[module].sections().size() );
  }
}

void Freezes::freeze( SectionRef where, Residency const& across )
{
  mByModule[where.module.value][where.section.value].push_back( across );
}

std::span<Residency const> Freezes::of( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value];
}

void Freezes::keepOut( SectionRef where, WindowIndex window )
{
  std::vector<WindowIndex>& windows = mKeepOut[where.module.value][where.section.value];
  if ( std::ranges::find( windows, window ) == windows.end() )
  {
    windows.push_back( window );
  }
}

std::span<WindowIndex const> Freezes::keptOutOf( SectionRef where ) const
{
  return mKeepOut[where.module.value][where.section.value];
}

Freezes checkTypes( Pruned const& build, diag::DiagnosticSink& sink )
{
  Freezes freezes{ build.modules() };
  Typer typer{ build.sources(), build.symbols(),    build.charsets(), build.target(),
               &build.phases(), &build.reachable(), &freezes,         sink };
  typer.checkEverything();
  checkWiths( build, freezes, sink );
  return freezes;
}

} // namespace nga::model
