#include "nga/c/Ranges.hpp"

#include "nga/c/Loops.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace nga::c
{

namespace
{

/// The smallest `2^k - 1` that is not below the value: the bits an `or` or an
/// `xor` of two values this large can reach.
std::int64_t lowerBitsMask( std::int64_t value )
{
  std::int64_t mask = 0;
  while ( mask < value )
  {
    mask = ( mask << 1 ) | 1;
  }
  return mask;
}

/// `c | x` and `c ^ x` of a number and a value whose bits all lie below the
/// lowest the number has: the two share no bit, so either is the sum —
/// `1024 ^ (u16)byte` is 1024 to 1279. Nothing where they may share one.
std::optional<Range> disjoint( Range left, Range right )
{
  for ( auto const [one, other] : { std::pair{ left, right }, std::pair{ right, left } } )
  {
    if ( one.min == one.max && one.min >= 0 && other.min >= 0 && ( one.min & lowerBitsMask( other.max ) ) == 0 )
    {
      return Range{ .min = one.min + other.min, .max = one.min + other.max };
    }
  }
  return std::nullopt;
}

/// The exact range, or the type's where the exact one does not fit it: a
/// result that wraps is no longer an interval, and nothing may be promised of
/// it beyond its bits.
Range fitted( Range exact, ir::Type type )
{
  Range const whole = rangeOfType( type );
  return exact.within( whole ) ? exact : whole;
}

/// The count a shift shifts by, where it is one number small enough to shift a
/// 64-bit bound with: a shift of a value by a value says nothing.
std::optional<int> shiftCount( Range right )
{
  if ( right.min != right.max || right.min < 0 || right.min > 16 )
  {
    return std::nullopt;
  }
  return static_cast<int>( right.min );
}

/// What is known of the Proc's own bytes at a point of a block: the range of
/// each byte a store of the block, or of the one block before it, has
/// written, with the type it was written as.
struct Held
{
  ir::Type type = ir::Type::U8;
  Range range;
};

using Known = std::map<std::string, Held>;

/// What a store, a store of an element or a copy leaves known: the byte
/// written whole takes the value's range, and anything else written of an
/// object forgets every byte of it.
void write( Known& known, std::string const& name )
{
  std::string const base = ir::baseOf( name );
  std::erase_if( known, [&base]( auto const& entry ) { return ir::baseOf( entry.first ) == base; } );
}

/// What the edge into a block adds, where the block before it ends by
/// branching on a comparison of one of its own bytes with a number: `c < 4`
/// taken says `c` is at most 3 there, and not taken that it is at least 4.
void refine( Known& known, ir::Block const& before, std::uint32_t into, std::set<std::string> const& own )
{
  ir::Terminator const& end = before.terminator;
  if ( end.kind != ir::TerminatorKind::BRANCH || end.target == end.otherwise || before.instructions.empty() )
  {
    return;
  }
  auto const* const compare = std::get_if<ir::Compare>( &before.instructions.back().operation );
  auto const* const condition = std::get_if<ir::Value>( &end.condition );
  if ( compare == nullptr || condition == nullptr || condition->index != compare->result.index )
  {
    return;
  }
  auto const* const object = std::get_if<ir::Object>( &compare->left );
  auto const* const bound = std::get_if<ir::Constant>( &compare->right );
  if ( object == nullptr || bound == nullptr || ( !bound->name.empty() && !bound->valued ) ||
       !own.contains( object->name ) || object->type != compare->type )
  {
    return;
  }
  Range range = rangeOfType( object->type );
  if ( auto const found = known.find( object->name ); found != known.end() && found->second.type == object->type )
  {
    range = found->second.range;
  }
  bool const taken = into == end.target;
  std::int64_t const k = bound->value;
  switch ( compare->op )
  {
  case ir::Comparison::LESS:
    range = taken ? Range{ .min = range.min, .max = std::min( range.max, k - 1 ) }
                  : Range{ .min = std::max( range.min, k ), .max = range.max };
    break;
  case ir::Comparison::GREATER_OR_EQUAL:
    range = taken ? Range{ .min = std::max( range.min, k ), .max = range.max }
                  : Range{ .min = range.min, .max = std::min( range.max, k - 1 ) };
    break;
  case ir::Comparison::EQUAL:
    range = taken ? Range{ .min = k, .max = k } : range;
    break;
  case ir::Comparison::NOT_EQUAL:
    range = taken ? range : Range{ .min = k, .max = k };
    break;
  }
  // An edge no value can take says nothing that is worth keeping.
  if ( range.min <= range.max )
  {
    known[object->name] = Held{ .type = object->type, .range = range };
  }
}

} // namespace

Range rangeOfType( ir::Type type )
{
  switch ( type )
  {
  case ir::Type::U8:
    return Range{ .min = 0, .max = 0xFF };
  case ir::Type::I8:
    return Range{ .min = -0x80, .max = 0x7F };
  case ir::Type::U16:
  case ir::Type::POINTER:
  case ir::Type::BLOCK:
    return Range{ .min = 0, .max = 0xFFFF };
  case ir::Type::I16:
    return Range{ .min = -0x8000, .max = 0x7FFF };
  case ir::Type::BOOL:
    return Range{ .min = 0, .max = 1 };
  }
  return Range{ .min = 0, .max = 0xFFFF };
}

Range rangeOfBinary( ir::BinaryOperator op, ir::Type type, Range left, Range right )
{
  switch ( op )
  {
  case ir::BinaryOperator::ADD:
    return fitted( Range{ .min = left.min + right.min, .max = left.max + right.max }, type );
  case ir::BinaryOperator::SUBTRACT:
    return fitted( Range{ .min = left.min - right.max, .max = left.max - right.min }, type );
  case ir::BinaryOperator::AND:
    // Neither operand's bits are added to, so the result is no larger than
    // the smaller of the two — where both are positive, which is what makes
    // the bits of a bound mean anything.
    if ( left.min >= 0 && right.min >= 0 )
    {
      return fitted( Range{ .min = 0, .max = std::min( left.max, right.max ) }, type );
    }
    return rangeOfType( type );
  case ir::BinaryOperator::OR:
    if ( std::optional<Range> const joined = disjoint( left, right ); joined.has_value() )
    {
      return fitted( *joined, type );
    }
    if ( left.min >= 0 && right.min >= 0 )
    {
      return fitted(
          Range{ .min = std::max( left.min, right.min ), .max = lowerBitsMask( std::max( left.max, right.max ) ) },
          type );
    }
    return rangeOfType( type );
  case ir::BinaryOperator::XOR:
    if ( std::optional<Range> const joined = disjoint( left, right ); joined.has_value() )
    {
      return fitted( *joined, type );
    }
    if ( left.min >= 0 && right.min >= 0 )
    {
      return fitted( Range{ .min = 0, .max = lowerBitsMask( std::max( left.max, right.max ) ) }, type );
    }
    return rangeOfType( type );
  case ir::BinaryOperator::SHIFT_LEFT:
    if ( std::optional<int> const by = shiftCount( right ); by.has_value() && left.min >= 0 )
    {
      return fitted( Range{ .min = left.min << *by, .max = left.max << *by }, type );
    }
    return rangeOfType( type );
  case ir::BinaryOperator::SHIFT_RIGHT:
    // Only over a value that cannot be negative: what a signed one shifts in
    // from the top is its sign, and this is not the place to decide whether
    // the subset says so.
    if ( std::optional<int> const by = shiftCount( right ); by.has_value() && left.min >= 0 )
    {
      return fitted( Range{ .min = left.min >> *by, .max = left.max >> *by }, type );
    }
    return rangeOfType( type );
  }
  return rangeOfType( type );
}

Range rangeOfUnary( ir::UnaryOperator op, ir::Type type, Range operand )
{
  switch ( op )
  {
  case ir::UnaryOperator::NEGATE:
    return fitted( Range{ .min = -operand.max, .max = -operand.min }, type );
  case ir::UnaryOperator::COMPLEMENT:
    return fitted( Range{ .min = ~operand.max, .max = ~operand.min }, type );
  case ir::UnaryOperator::LOGICAL_NOT:
    return Range{ .min = 0, .max = 1 };
  }
  return rangeOfType( type );
}

Range rangeOfConvert( ir::Type to, Range operand )
{
  // A conversion that keeps the value keeps its range; one the value does not
  // fit takes its low bytes, and what that comes to is the type's own range.
  return fitted( operand, to );
}

Range rangeOf( ir::Operand const& operand, ir::Function const& function, std::vector<Range> const& ranges )
{
  if ( auto const* const constant = std::get_if<ir::Constant>( &operand ) )
  {
    // A constant the text writes by its name is the assembler's to resolve,
    // and its type is all the compiler is promised of it — but a `const` of
    // the program's C is the value the compiler writes for it.
    if ( !constant->name.empty() && !constant->valued )
    {
      return rangeOfType( constant->type );
    }
    return Range{ .min = constant->value, .max = constant->value };
  }
  if ( auto const* const object = std::get_if<ir::Object>( &operand ) )
  {
    return rangeOfType( object->type );
  }
  ir::Value const value = std::get<ir::Value>( operand );
  return value.index < ranges.size() ? ranges[value.index] : rangeOfType( ir::typeOf( operand, function ) );
}

namespace
{

/// The union of what two ways into a block know: a byte both know, with the
/// same type, at the smallest range that holds both.
Known meet( Known const& one, Known const& other )
{
  Known both;
  for ( auto const& [name, held] : one )
  {
    auto const found = other.find( name );
    if ( found != other.end() && found->second.type == held.type )
    {
      both[name] = Held{ .type = held.type,
                         .range = Range{ .min = std::min( held.range.min, found->second.range.min ),
                                         .max = std::max( held.range.max, found->second.range.max ) } };
    }
  }
  return both;
}

/// One sweep of the function in the order of its blocks, taking as given at
/// each loop's header that its own bytes hold what they held where the loop
/// was entered — no more than that where they are not `unbounded` for that
/// header, and nothing where they are `refused` — and adding to `unbounded`
/// each such byte a store of the loop may take above it, and to `refused`
/// each one a store may take below it.
std::vector<Range> sweep( ir::Function const& function,
                          std::vector<std::vector<std::uint32_t>> const& before,
                          std::map<std::uint32_t, Loop> const& loops,
                          std::map<std::uint32_t, std::set<std::string>>& refused,
                          std::map<std::uint32_t, std::set<std::string>>& unbounded,
                          std::vector<std::optional<Range>>* bounds = nullptr )
{
  std::vector<Range> ranges( function.values.size() );
  for ( std::size_t index = 0; index < ranges.size(); ++index )
  {
    ranges[index] = rangeOfType( function.values[index] );
  }
  std::set<std::string> const own = ownBytesOf( function );

  // What each header takes as given: the byte and what it may hold.
  std::map<std::uint32_t, std::map<std::string, Range>> assumed;
  std::vector<Known> atEnd( function.blocks.size() );
  for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
  {
    ir::Block const& block = function.blocks[index];
    Known known;
    bool const forward = !before[index].empty() &&
                         std::ranges::all_of( before[index], [index]( std::uint32_t from ) { return from < index; } );
    if ( forward )
    {
      for ( std::size_t way = 0; way < before[index].size(); ++way )
      {
        std::uint32_t const from = before[index][way];
        Known edge = atEnd[from];
        refine( edge, function.blocks[from], index, own );
        known = way == 0 ? std::move( edge ) : meet( known, edge );
      }
    }
    else if ( auto const loop = loops.find( index ); loop != loops.end() )
    {
      // A loop's header, entered from before it and from its own blocks
      // jumping back: what was known on the way in, less its upper bounds
      // where a store of the loop may go above them — a bound the loop does
      // not write keeps its most, `limit = 4096 ^ seed` its high byte. See
      // docs/decisions/0290-a-high-byte-the-ranges-know-is-read-as-a-number.md.
      for ( auto const& [name, held] : atEnd[loop->second.entry] )
      {
        if ( !refused[index].contains( name ) )
        {
          Range const range{ .min = held.range.min,
                             .max = unbounded[index].contains( name ) ? rangeOfType( held.type ).max : held.range.max };
          known[name] = Held{ .type = held.type, .range = range };
          assumed[index][name] = range;
        }
      }
    }

    // A store of the byte inside a loop whose header took its range as given
    // must keep to it, or the header may not take its least, or its most.
    auto const keepsTo = [&]( std::string const& name, std::optional<Range> stored )
    {
      for ( auto const& [header, taken] : assumed )
      {
        Loop const& loop = loops.at( header );
        if ( index < loop.header || index > loop.last )
        {
          continue;
        }
        for ( auto const& [byte, range] : taken )
        {
          if ( ir::baseOf( byte ) != ir::baseOf( name ) )
          {
            continue;
          }
          if ( byte != name || !stored.has_value() || stored->min < range.min )
          {
            refused[header].insert( byte );
          }
          else if ( stored->max > range.max )
          {
            unbounded[header].insert( byte );
          }
        }
      }
    };

    auto const read = [&function, &ranges, &known]( ir::Operand const& operand )
    {
      if ( auto const* const object = std::get_if<ir::Object>( &operand ) )
      {
        if ( auto const found = known.find( object->name ); found != known.end() && found->second.type == object->type )
        {
          return found->second.range;
        }
      }
      return rangeOf( operand, function, ranges );
    };

    for ( ir::Instruction const& instruction : block.instructions )
    {
      std::visit(
          [&]( auto const& operation )
          {
            using Operation = std::decay_t<decltype( operation )>;
            if constexpr ( std::is_same_v<Operation, ir::Binary> )
            {
              ranges[operation.result.index] =
                  rangeOfBinary( operation.op, operation.type, read( operation.left ), read( operation.right ) );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Unary> )
            {
              ranges[operation.result.index] = rangeOfUnary( operation.op, operation.type, read( operation.operand ) );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Convert> )
            {
              ranges[operation.result.index] = rangeOfConvert( operation.type, read( operation.operand ) );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Store> )
            {
              Range const stored = fitted( read( operation.value ), operation.type );
              write( known, operation.name );
              keepsTo( operation.name, stored );
              if ( own.contains( operation.name ) )
              {
                known[operation.name] = Held{ .type = operation.type, .range = stored };
              }
            }
            else if constexpr ( std::is_same_v<Operation, ir::StoreElement> )
            {
              write( known, operation.name );
              keepsTo( operation.name, std::nullopt );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Compare> )
            {
              if ( bounds != nullptr )
              {
                ( *bounds )[operation.result.index] = read( operation.right );
              }
            }
            else if constexpr ( std::is_same_v<Operation, ir::Copy> )
            {
              write( known, operation.to.name );
              keepsTo( operation.to.name, std::nullopt );
            }
          },
          instruction.operation );
    }
    atEnd[index] = std::move( known );
  }
  return ranges;
}

} // namespace

namespace
{

/// The sweeps of `rangesOf` to their fixed point, the last of them also
/// answering what each comparison's right operand holds where it stands.
std::vector<Range> swept( ir::Function const& function, std::vector<std::optional<Range>>* bounds )
{
  std::vector<std::vector<std::uint32_t>> const before = ir::predecessorsOf( function.blocks );

  // The loops whose header is entered from the block before it and otherwise
  // only from the loop's own blocks, each by its header, with its last block
  // the furthest that jumps back.
  std::map<std::uint32_t, Loop> loops;
  for ( Loop const& loop : loopsOf( function ) )
  {
    auto [at, fresh] = loops.try_emplace( loop.header, loop );
    at->second.last = std::max( at->second.last, loop.last );
  }
  std::erase_if( loops,
                 [&before]( auto const& entry )
                 {
                   Loop const& loop = entry.second;
                   return !std::ranges::all_of(
                       before[loop.header],
                       [&loop]( std::uint32_t from )
                       { return from == loop.entry || ( from >= loop.header && from <= loop.last ); } );
                 } );

  // Each sweep takes back what a store showed a header may not take as given,
  // and the sweep after it is sound where none was shown: the bytes each
  // header takes as given keep to it at every turn, so they held on the way
  // in and hold ever after. A sweep only ever takes back, so this ends.
  std::map<std::uint32_t, std::set<std::string>> refused;
  std::map<std::uint32_t, std::set<std::string>> unbounded;
  while ( true )
  {
    std::map<std::uint32_t, std::set<std::string>> const was = refused;
    std::map<std::uint32_t, std::set<std::string>> const wasUnbounded = unbounded;
    std::vector<Range> ranges = sweep( function, before, loops, refused, unbounded );
    if ( refused == was && unbounded == wasUnbounded )
    {
      if ( bounds != nullptr )
      {
        bounds->assign( function.values.size(), std::nullopt );
        std::map<std::uint32_t, std::set<std::string>> again = refused;
        std::map<std::uint32_t, std::set<std::string>> againUnbounded = unbounded;
        sweep( function, before, loops, again, againUnbounded, bounds );
      }
      return ranges;
    }
  }
}

} // namespace

std::vector<Range> rangesOf( ir::Function const& function )
{
  return swept( function, nullptr );
}

std::vector<std::optional<Range>> comparedBoundsOf( ir::Function const& function )
{
  std::vector<std::optional<Range>> bounds;
  swept( function, &bounds );
  return bounds;
}

} // namespace nga::c
