#include "nga/model/Evaluate.hpp"

#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <variant>
#include <vector>

namespace nga::model
{

namespace
{

// Arithmetic on the 64 bits an expression is evaluated in, where an answer
// those bits do not hold is no answer, as a division by zero is none: each
// checked before it is done, since a signed overflow is undefined and a
// division of the least value by -1 traps on x86. Written out rather than
// through a compiler's builtins, which MSVC does not have.
constexpr std::int64_t LEAST = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t MOST = std::numeric_limits<std::int64_t>::max();

std::optional<std::int64_t> sumOf( std::int64_t a, std::int64_t b )
{
  if ( ( b > 0 && a > MOST - b ) || ( b < 0 && a < LEAST - b ) )
  {
    return std::nullopt;
  }
  return a + b;
}

std::optional<std::int64_t> differenceOf( std::int64_t a, std::int64_t b )
{
  if ( ( b < 0 && a > MOST + b ) || ( b > 0 && a < LEAST + b ) )
  {
    return std::nullopt;
  }
  return a - b;
}

std::optional<std::int64_t> productOf( std::int64_t a, std::int64_t b )
{
  if ( a == 0 || b == 0 )
  {
    return 0;
  }
  // The bound the other factor is held to, by the signs of the two: the
  // quotient of the extreme the product would pass by the factor.
  bool fits = false;
  if ( a > 0 )
  {
    fits = b > 0 ? a <= MOST / b : b >= LEAST / a;
  }
  else
  {
    fits = b > 0 ? a >= LEAST / b : b >= MOST / a;
  }
  return fits ? std::optional{ a * b } : std::nullopt;
}

std::optional<std::int64_t> quotientOf( std::int64_t a, std::int64_t b )
{
  if ( b == 0 || ( a == LEAST && b == -1 ) )
  {
    return std::nullopt;
  }
  return a / b;
}

/// An intermediate result. An address is kept as its Section and an offset
/// into it, so that `dataEnd - data` is a number after Size, before any
/// address exists — which is exactly what the subtraction rule promises.
struct Value
{
  std::int64_t number = 0;
  std::optional<SectionRef> section;

  [[nodiscard]] bool isAddress() const
  {
    return section.has_value();
  }
};

class Evaluator
{
public:
  Evaluator( diag::SourceManager const& sources,
             GlobalSymbols const& symbols,
             Charsets const* charsets,
             ValueSource& known )
      : mSources( &sources ), mSymbols( &symbols ), mCharsets( charsets ), mKnown( &known )
  {
  }

  std::optional<Value> evaluate( ModuleIndex home, syntax::Expression const& node );

  /// An address resolved to a number, which needs Place to have run.
  std::optional<std::int64_t> asNumber( std::optional<Value> const& value );

private:
  std::optional<Value> evaluateName( ModuleIndex home, syntax::Expression const& node );
  std::optional<Value> evaluateSymbol( SymbolRef where );
  std::optional<Value> evaluatePosition( ModuleIndex module, LabelPosition position );
  std::optional<Value> evaluateAttribute( ModuleIndex home, syntax::Expression const& node );
  std::optional<Value> evaluateUnary( ModuleIndex home, syntax::Expression const& node );
  std::optional<Value> evaluateBinary( ModuleIndex home, syntax::Expression const& node );

  /// The byte a prefixed character literal produces. Nothing when the prefix
  /// names no character set or the set does not map the character, both of
  /// which the type check has reported.
  std::optional<Value> evaluateCharacter( ModuleIndex home, syntax::Expression const& node );

  diag::SourceManager const* mSources;
  GlobalSymbols const* mSymbols;
  Charsets const* mCharsets;
  ValueSource* mKnown;

  /// Guards a Constant defined in terms of itself, which the type check has
  /// already reported.
  std::vector<SymbolRef> mVisiting;
};

std::optional<std::int64_t> Evaluator::asNumber( std::optional<Value> const& value )
{
  if ( !value.has_value() )
  {
    return std::nullopt;
  }
  if ( !value->section.has_value() )
  {
    return value->number;
  }

  std::optional<std::int64_t> const base = mKnown->addressOf( *value->section );
  if ( !base.has_value() )
  {
    return std::nullopt;
  }
  return *base + value->number;
}

std::optional<Value> Evaluator::evaluatePosition( ModuleIndex module, LabelPosition position )
{
  SectionRef const where{ .module = module, .section = position.section };
  std::optional<std::int64_t> const offset = mKnown->offsetOf( where, position.chunk, position.inner );
  if ( !offset.has_value() )
  {
    return std::nullopt;
  }
  return Value{ .number = *offset, .section = where };
}

std::optional<Value> Evaluator::evaluateName( ModuleIndex home, syntax::Expression const& node )
{
  std::string_view const name = mSources->textOf( node.token.span() );
  std::optional<SymbolRef> const where = mSymbols->resolveText( home, node, name );
  return where.has_value() ? evaluateSymbol( *where ) : std::nullopt;
}

std::optional<Value> Evaluator::evaluateSymbol( SymbolRef where )
{
  if ( std::ranges::find( mVisiting, where ) != mVisiting.end() )
  {
    return std::nullopt;
  }

  Symbol const& symbol = mSymbols->at( where );
  switch ( symbol.kind )
  {
  case SymbolKind::LABEL:
    return evaluatePosition( where.module, std::get<LabelPosition>( symbol.value ) );
  case SymbolKind::CONSTANT:
  {
    auto const& value = std::get<syntax::ExpressionPtr>( symbol.value );
    if ( value == nullptr )
    {
      return std::nullopt;
    }
    mVisiting.push_back( where );
    std::optional<Value> const result = evaluate( where.module, *value );
    mVisiting.pop_back();
    return result;
  }
  case SymbolKind::SLOT:
  {
    auto const& declaration = std::get<SlotDeclaration>( symbol.value );
    if ( !declaration.cell.has_value() )
    {
      return std::nullopt;
    }
    return evaluatePosition(
        declaration.cell->module,
        LabelPosition{ .section = declaration.cell->section, .chunk = ChunkIndex{ 0 }, .inner = std::nullopt } );
  }
  case SymbolKind::REGION:
    return Value{ .number = std::get<RegionValue>( symbol.value ).address, .section = std::nullopt };
  case SymbolKind::PANE:
  {
    // The state the solver chose, known after Place and never declared: a
    // Pane's name is no declared value, so nothing decided at Expand can
    // turn on it — see docs/decisions/0054-panes.md.
    std::optional<std::int64_t> const state = mKnown->stateOfPane( std::get<PaneIndex>( symbol.value ) );
    return state.has_value() ? std::optional{ Value{ .number = *state, .section = std::nullopt } } : std::nullopt;
  }
  case SymbolKind::CHARSET:
  case SymbolKind::WINDOW:
  case SymbolKind::MACRO:
    break;
  }
  return std::nullopt;
}

std::optional<Value> Evaluator::evaluateAttribute( ModuleIndex home, syntax::Expression const& node )
{
  std::string_view const attribute = mSources->textOf( node.token.span() );

  // A dotted name that is a Symbol, a member of a Namespace, before an
  // attribute of what stands left of the dot.
  if ( std::optional<std::string> const dotted = syntax::dottedNameOf( *mSources, node ); dotted.has_value() )
  {
    if ( std::optional<SymbolRef> const member = mSymbols->resolveText( home, syntax::leftmostOf( node ), *dotted );
         member.has_value() )
    {
      return evaluateSymbol( *member );
    }
  }
  if ( node.left == nullptr )
  {
    return std::nullopt;
  }
  std::optional<std::string> const receiver = syntax::dottedNameOf( *mSources, *node.left );
  if ( !receiver.has_value() )
  {
    return std::nullopt;
  }
  std::optional<SymbolRef> const where = mSymbols->resolveText( home, syntax::leftmostOf( *node.left ), *receiver );
  if ( !where.has_value() )
  {
    return std::nullopt;
  }

  std::optional<SectionRef> const named = sectionNamedBy( *mSymbols, *where );
  if ( !named.has_value() )
  {
    return std::nullopt;
  }

  SectionRef const section = *named;
  std::optional<SectionAttribute> const which = sectionAttributeNamed( attribute );
  if ( which == SectionAttribute::RUNTIME_SECTION_SIZE )
  {
    std::optional<std::int64_t> const size = mKnown->sizeOfSection( section );
    return size.has_value() ? std::optional{ Value{ .number = *size, .section = std::nullopt } } : std::nullopt;
  }
  if ( which == SectionAttribute::RUNTIME_SECTION_ADDRESS )
  {
    return Value{ .number = 0, .section = section };
  }
  if ( which == SectionAttribute::RESIDENT )
  {
    // Read from the Module and never from a Step's result: Residency is what
    // the Project derived, and it exists before anything has been sized.
    bool const resident = mSymbols->moduleAt( section.module ).residency().isAll();
    return Value{ .number = resident ? 1 : 0, .section = std::nullopt };
  }
  if ( which == SectionAttribute::LINEAR_SECTION_ADDRESS || which == SectionAttribute::LINEAR_FAMILY_ADDRESS )
  {
    // A number and not a position of a Section: the medium's address, which
    // no runtime address is added to.
    std::optional<std::int64_t> const linear = mKnown->linearAddressOf( section );
    return linear.has_value() ? std::optional{ Value{ .number = *linear, .section = std::nullopt } } : std::nullopt;
  }
  return std::nullopt;
}

std::optional<Value> Evaluator::evaluateUnary( ModuleIndex home, syntax::Expression const& node )
{
  std::optional<Value> const operand = evaluate( home, *node.left );

  switch ( node.unaryOperator )
  {
  case syntax::UnaryOperator::LOW_BYTE:
  case syntax::UnaryOperator::HIGH_BYTE:
  {
    // Byte extraction of an address needs the address, so it waits for Place.
    std::optional<std::int64_t> const number = asNumber( operand );
    if ( !number.has_value() )
    {
      return std::nullopt;
    }
    std::int64_t const shifted = node.unaryOperator == syntax::UnaryOperator::LOW_BYTE ? *number : *number >> 8U;
    return Value{ .number = shifted & 0xFF, .section = std::nullopt };
  }
  default:
    break;
  }

  if ( !operand.has_value() || operand->isAddress() )
  {
    return std::nullopt;
  }

  switch ( node.unaryOperator )
  {
  case syntax::UnaryOperator::NEGATE:
    if ( operand->number == LEAST )
    {
      return std::nullopt;
    }
    return Value{ .number = -operand->number, .section = std::nullopt };
  case syntax::UnaryOperator::COMPLEMENT:
    return Value{ .number = ~operand->number, .section = std::nullopt };
  case syntax::UnaryOperator::NOT:
    return Value{ .number = operand->number == 0 ? 1 : 0, .section = std::nullopt };
  default:
    return std::nullopt;
  }
}

std::optional<Value> Evaluator::evaluateBinary( ModuleIndex home, syntax::Expression const& node )
{
  // The conditional evaluates its condition and then one arm, never both.
  // That is what lets `flag ? size : 4` have a value where the arm not taken
  // has none — and, since a declared value is evaluation against a source
  // that knows nothing, what makes such an expression a declared value by the
  // rule already written rather than by an exception to it.
  if ( node.binaryOperator == syntax::BinaryOperator::SELECT )
  {
    std::optional<Value> const condition = evaluate( home, *node.left );
    std::optional<std::int64_t> const decided = asNumber( condition );
    if ( !decided.has_value() )
    {
      return std::nullopt;
    }
    syntax::Expression const& arms = *node.right;
    return evaluate( home, *decided != 0 ? *arms.left : *arms.right );
  }

  std::optional<Value> const left = evaluate( home, *node.left );
  std::optional<Value> const right = evaluate( home, *node.right );
  if ( !left.has_value() || !right.has_value() )
  {
    return std::nullopt;
  }

  auto const plain = []( std::int64_t number ) { return Value{ .number = number, .section = std::nullopt }; };
  auto const plainOf = [&plain]( std::optional<std::int64_t> number ) -> std::optional<Value>
  { return number.has_value() ? std::optional{ plain( *number ) } : std::nullopt; };
  auto const at = []( std::optional<SectionRef> section, std::optional<std::int64_t> number ) -> std::optional<Value>
  { return number.has_value() ? std::optional{ Value{ .number = *number, .section = section } } : std::nullopt; };

  if ( node.binaryOperator == syntax::BinaryOperator::ADD && left->isAddress() != right->isAddress() )
  {
    Value const& address = left->isAddress() ? *left : *right;
    Value const& offset = left->isAddress() ? *right : *left;
    return at( address.section, sumOf( address.number, offset.number ) );
  }

  if ( node.binaryOperator == syntax::BinaryOperator::SUBTRACT )
  {
    if ( left->isAddress() && right->isAddress() )
    {
      // The distance between two positions in one Section is a sum of Chunk
      // sizes, so it exists after Size and needs no address at all.
      if ( left->section != right->section )
      {
        return std::nullopt;
      }
      return plainOf( differenceOf( left->number, right->number ) );
    }
    if ( left->isAddress() )
    {
      return at( left->section, differenceOf( left->number, right->number ) );
    }
  }

  std::optional<std::int64_t> const a = asNumber( left );
  std::optional<std::int64_t> const b = asNumber( right );
  if ( !a.has_value() || !b.has_value() )
  {
    return std::nullopt;
  }

  switch ( node.binaryOperator )
  {
  case syntax::BinaryOperator::ADD:
    return plainOf( sumOf( *a, *b ) );
  case syntax::BinaryOperator::SUBTRACT:
    return plainOf( differenceOf( *a, *b ) );
  case syntax::BinaryOperator::MULTIPLY:
    return plainOf( productOf( *a, *b ) );
  case syntax::BinaryOperator::DIVIDE:
    // A division by zero has no value to give, and neither has one whose
    // answer 64 bits do not hold. It deserves a finding of its own, and will
    // have one when a Step exists that can carry it.
    return plainOf( quotientOf( *a, *b ) );
  case syntax::BinaryOperator::SELECT:
  case syntax::BinaryOperator::ARM:
    // The conditional is answered above, and an arm pair is reached only
    // through it.
    return std::nullopt;
  case syntax::BinaryOperator::SHIFT_LEFT:
    return plain( *a << static_cast<std::uint64_t>( *b & 63 ) );
  case syntax::BinaryOperator::SHIFT_RIGHT:
    return plain( *a >> static_cast<std::uint64_t>( *b & 63 ) );
  case syntax::BinaryOperator::BITWISE_AND:
    return plain( *a & *b );
  case syntax::BinaryOperator::BITWISE_OR:
    return plain( *a | *b );
  case syntax::BinaryOperator::BITWISE_XOR:
    return plain( *a ^ *b );
  case syntax::BinaryOperator::EQUAL:
    return plain( *a == *b ? 1 : 0 );
  case syntax::BinaryOperator::NOT_EQUAL:
    return plain( *a != *b ? 1 : 0 );
  case syntax::BinaryOperator::LESS:
    return plain( *a < *b ? 1 : 0 );
  case syntax::BinaryOperator::LESS_EQUAL:
    return plain( *a <= *b ? 1 : 0 );
  case syntax::BinaryOperator::GREATER:
    return plain( *a > *b ? 1 : 0 );
  case syntax::BinaryOperator::GREATER_EQUAL:
    return plain( *a >= *b ? 1 : 0 );
  case syntax::BinaryOperator::LOGICAL_AND:
    return plain( *a != 0 && *b != 0 ? 1 : 0 );
  case syntax::BinaryOperator::LOGICAL_OR:
    return plain( *a != 0 || *b != 0 ? 1 : 0 );
  }
  return std::nullopt;
}

std::optional<Value> Evaluator::evaluateCharacter( ModuleIndex home, syntax::Expression const& node )
{
  std::string_view const text = mSources->textOf( node.token.span() );
  syntax::Quoted const quoted = syntax::quotedOf( text );
  if ( quoted.charset.empty() )
  {
    std::optional<std::int64_t> const value = syntax::plainCharacterValueOf( text );
    return value.has_value() ? std::optional{ Value{ .number = *value, .section = std::nullopt } } : std::nullopt;
  }

  if ( mCharsets == nullptr )
  {
    // Resolving a character set is what produces the tables, so a declaration
    // written in terms of one has nothing to read here — and 0013 refuses to
    // make that work rather than making it circular.
    return std::nullopt;
  }

  std::optional<SymbolRef> const named = mSymbols->resolveText( home, node, quoted.charset );
  if ( !named.has_value() )
  {
    return std::nullopt;
  }
  std::optional<CharsetRef> const where = charsetOf( *mSymbols, *named );
  if ( !where.has_value() )
  {
    return std::nullopt;
  }

  std::vector<char32_t> const points = syntax::codePointsOf( quoted.body );
  if ( points.size() != 1 )
  {
    return std::nullopt;
  }
  std::optional<std::uint8_t> const byte = mCharsets->at( *where ).byteFor( points.front() );
  return byte.has_value() ? std::optional{ Value{ .number = *byte, .section = std::nullopt } } : std::nullopt;
}

std::optional<Value> Evaluator::evaluate( ModuleIndex home, syntax::Expression const& node )
{
  switch ( node.kind )
  {
  case syntax::ExpressionKind::NUMBER:
  {
    std::optional<std::int64_t> const value = syntax::numericValueOf( mSources->textOf( node.token.span() ) );
    return value.has_value() ? std::optional{ Value{ .number = *value, .section = std::nullopt } } : std::nullopt;
  }
  case syntax::ExpressionKind::VALUE:
    return Value{ .number = node.value, .section = std::nullopt };
  case syntax::ExpressionKind::SPREAD:
    return std::nullopt;
  case syntax::ExpressionKind::CHARACTER:
    return evaluateCharacter( home, node );
  case syntax::ExpressionKind::NAME:
    return evaluateName( home, node );
  case syntax::ExpressionKind::LOCAL_NAME:
  {
    std::optional<LabelPosition> const target = mSymbols->moduleAt( home ).localTarget( &node );
    return target.has_value() ? evaluatePosition( home, *target ) : std::nullopt;
  }
  case syntax::ExpressionKind::ATTRIBUTE:
    return evaluateAttribute( home, node );
  case syntax::ExpressionKind::UNARY:
    return evaluateUnary( home, node );
  case syntax::ExpressionKind::BINARY:
    return evaluateBinary( home, node );
  case syntax::ExpressionKind::ERROR:
  case syntax::ExpressionKind::STRING:
    break;
  }
  return std::nullopt;
}

class NothingKnown final : public ValueSource
{
};

} // namespace

ValueSource& nothingIsKnown()
{
  // It answers nothing, so sharing one is safe however many threads ask.
  static NothingKnown nothing;
  return nothing;
}

std::optional<std::int64_t> evaluate( diag::SourceManager const& sources,
                                      GlobalSymbols const& symbols,
                                      Charsets const* charsets,
                                      ModuleIndex home,
                                      syntax::Expression const& node,
                                      ValueSource& known )
{
  Evaluator evaluator{ sources, symbols, charsets, known };
  return evaluator.asNumber( evaluator.evaluate( home, node ) );
}

std::optional<std::int64_t> declaredValueOf( diag::SourceManager const& sources,
                                             GlobalSymbols const& symbols,
                                             Charsets const* charsets,
                                             ModuleIndex home,
                                             syntax::Expression const& node )
{
  return evaluate( sources, symbols, charsets, home, node, nothingIsKnown() );
}

std::optional<std::int64_t> offsetPast( diag::SourceManager const& sources,
                                        GlobalSymbols const& symbols,
                                        Charsets const* charsets,
                                        ModuleIndex home,
                                        syntax::Expression const& node,
                                        SectionRef where,
                                        ChunkIndex chunk )
{
  /// A source that gives the one Label an address and nothing else an
  /// address at all, so that an expression naming anything beside it says
  /// nothing.
  class AtLabel final : public ValueSource
  {
  public:
    AtLabel( SectionRef where, ChunkIndex chunk, std::int64_t address )
        : mWhere( where ), mChunk( chunk ), mAddress( address )
    {
    }

    [[nodiscard]] std::optional<std::int64_t> addressOf( SectionRef where ) override
    {
      return where == mWhere ? std::optional{ mAddress } : std::nullopt;
    }

    [[nodiscard]] std::optional<std::int64_t>
    offsetOf( SectionRef where, ChunkIndex chunk, std::optional<std::uint32_t> inner ) override
    {
      return where == mWhere && chunk == mChunk && !inner.has_value() ? std::optional{ std::int64_t{ 0 } }
                                                                      : std::nullopt;
    }

  private:
    SectionRef mWhere;
    ChunkIndex mChunk;
    std::int64_t mAddress;
  };

  constexpr std::int64_t shift = 0x10000;
  AtLabel low{ where, chunk, 0 };
  AtLabel high{ where, chunk, shift };
  std::optional<std::int64_t> const atLow = evaluate( sources, symbols, charsets, home, node, low );
  std::optional<std::int64_t> const atHigh = evaluate( sources, symbols, charsets, home, node, high );
  if ( !atLow.has_value() || !atHigh.has_value() || *atHigh - *atLow != shift )
  {
    return std::nullopt;
  }
  return atLow;
}

} // namespace nga::model
