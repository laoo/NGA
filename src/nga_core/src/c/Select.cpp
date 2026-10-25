#include "nga/c/Select.hpp"

#include "nga/c/Ranges.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nga::c
{

namespace
{

constexpr std::string_view WIDE_MULTIPLY = "__mul16";
constexpr std::string_view BYTE_MULTIPLY = "__mul8to16";

/// The name a byte of the narrower Proc has, where the wider one's byte is
/// named: `__mul16.left` is `__mul8to16.left`, and the Proc's own name is all
/// that changes.
std::string renamed( std::string const& name )
{
  std::size_t const dot = name.find( '.' );
  return dot == std::string::npos ? std::string{ BYTE_MULTIPLY } : std::string{ BYTE_MULTIPLY } + name.substr( dot );
}

void narrowIn( ir::Function& function )
{
  std::vector<Range> const ranges = rangesOf( function );
  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      auto* const call = std::get_if<ir::Call>( &instruction.operation );
      if ( call == nullptr || call->name != WIDE_MULTIPLY || call->arguments.size() != 2 )
      {
        continue;
      }

      // Either Proc takes as many rounds as `left` has bits up to its highest
      // set one, so the operand the ranges hold smaller is `left`. A product
      // is the same either way round. Where a byte of the argument is named
      // rather than a value, the two are not the compiler's to exchange. See
      // docs/decisions/0144-a-multiply-stops-when-its-multiplier-does.md and
      // docs/decisions/0170-a-wide-multiply-stops-when-its-multiplier-does.md.
      bool const values = !call->arguments[0].from.has_value() && !call->arguments[1].from.has_value();
      if ( values && rangeOf( call->arguments[1].value, function, ranges ).max <
                         rangeOf( call->arguments[0].value, function, ranges ).max )
      {
        std::swap( call->arguments[0].value, call->arguments[1].value );
      }

      // Both operands one unsigned byte: the whole product of two of those is
      // sixteen bits, which is what the narrower Proc leaves, and its low half
      // is what a `u16` or an `i16` was going to be given either way.
      bool bytes = values;
      for ( ir::Argument const& argument : call->arguments )
      {
        bytes = bytes && rangeOf( argument.value, function, ranges ).isByte();
      }
      if ( !bytes )
      {
        continue;
      }

      call->name = std::string{ BYTE_MULTIPLY };
      call->returned = renamed( call->returned );
      for ( ir::Argument& argument : call->arguments )
      {
        argument.name = renamed( argument.name );
        argument.type = ir::Type::U8;
      }
    }
  }
}

/// An address the loop of a block computes as an array's name plus an index
/// that can only hold a byte: the array, the byte the index is, and every
/// instruction that computed the address and is read by nothing else, to go.
struct ByteIndexed
{
  std::string array;
  ir::Operand index;
  std::vector<std::size_t> spent;
};

/// What `add ptr ARRAY, %v` addresses where `%v` fits a byte — looked through
/// the widening the lowering wrapped the index in, and through the doubling of
/// an index into an array of 16-bit elements, whose byte must then be at most
/// 127 so that the doubled one fits `X` too.
std::optional<ByteIndexed> byteIndexedBy( ir::Function& function,
                                          ir::Block const& block,
                                          std::size_t addAt,
                                          std::uint32_t elementBytes,
                                          std::vector<Range> const& ranges )
{
  auto const* const add = std::get_if<ir::Binary>( &block.instructions[addAt].operation );
  if ( add == nullptr || add->op != ir::BinaryOperator::ADD || add->type != ir::Type::POINTER ||
       ir::readersOf( function, add->result ) != 1 )
  {
    return std::nullopt;
  }
  auto const* array = std::get_if<ir::Constant>( &add->left );
  ir::Operand const* offset = &add->right;
  if ( array == nullptr )
  {
    array = std::get_if<ir::Constant>( &add->right );
    offset = &add->left;
  }
  if ( array == nullptr || array->name.empty() || array->value != 0 || array->type != ir::Type::POINTER )
  {
    return std::nullopt;
  }
  ByteIndexed found{ .array = array->name, .index = {}, .spent = { addAt } };

  // An array of 16-bit elements: the index doubled first, `shl ptr %u, 1`.
  ir::Operand const* index = offset;
  std::int64_t atMost = 0xFF;
  if ( elementBytes == 2 )
  {
    auto const* const doubled = std::get_if<ir::Value>( index );
    std::optional<std::size_t> const at = doubled != nullptr ? ir::definerIn( block, *doubled ) : std::nullopt;
    if ( !at.has_value() )
    {
      return std::nullopt;
    }
    auto const* const shift = std::get_if<ir::Binary>( &block.instructions[*at].operation );
    auto const* const by = shift != nullptr ? std::get_if<ir::Constant>( &shift->right ) : nullptr;
    if ( shift == nullptr || shift->op != ir::BinaryOperator::SHIFT_LEFT || by == nullptr || by->value != 1 ||
         ir::readersOf( function, shift->result ) != 1 )
    {
      return std::nullopt;
    }
    found.spent.push_back( *at );
    index = &shift->left;
    atMost = 0x7F;
  }
  else if ( elementBytes != 1 )
  {
    return std::nullopt;
  }

  // The index itself: a byte, or a byte the lowering widened, or a wider
  // value the ranges prove a byte, which is read as its low byte.
  Range const range = rangeOf( *index, function, ranges );
  if ( range.min < 0 || range.max > atMost )
  {
    return std::nullopt;
  }
  auto const* const value = std::get_if<ir::Value>( index );
  if ( value != nullptr )
  {
    if ( std::optional<std::size_t> const at = ir::definerIn( block, *value ); at.has_value() )
    {
      auto const* const widened = std::get_if<ir::Convert>( &block.instructions[*at].operation );
      if ( widened != nullptr && ir::sizeOf( ir::typeOf( widened->operand, function ) ) == 1 &&
           ir::readersOf( function, *value ) == 1 )
      {
        found.index = widened->operand;
        found.spent.push_back( *at );
        return found;
      }
    }
  }
  if ( ir::sizeOf( ir::typeOf( *index, function ) ) != 1 )
  {
    return std::nullopt;
  }
  found.index = *index;
  return found;
}

void narrowIndexesIn( ir::Function& function )
{
  std::vector<Range> const ranges = rangesOf( function );
  for ( ir::Block& block : function.blocks )
  {
    std::vector<bool> spent( block.instructions.size(), false );
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      ir::Instruction& instruction = block.instructions[at];
      ir::Operand const* pointer = nullptr;
      ir::Operand const* index = nullptr;
      std::uint32_t elementBytes = 0;
      // Not through a pointer to `volatile`, which an element of an array by
      // its name would no longer say.
      if ( auto const* const load = std::get_if<ir::LoadIndirect>( &instruction.operation );
           load != nullptr && !load->isVolatile )
      {
        pointer = &load->pointer;
        index = &load->index;
        elementBytes = ir::sizeOf( load->type );
      }
      else if ( auto const* const store = std::get_if<ir::StoreIndirect>( &instruction.operation );
                store != nullptr && !store->isVolatile )
      {
        pointer = &store->pointer;
        index = &store->index;
        elementBytes = ir::sizeOf( store->type );
      }
      auto const* const zero = index != nullptr ? std::get_if<ir::Constant>( index ) : nullptr;
      auto const* const address = pointer != nullptr ? std::get_if<ir::Value>( pointer ) : nullptr;
      if ( zero == nullptr || zero->value != 0 || !zero->name.empty() || address == nullptr )
      {
        continue;
      }
      std::optional<std::size_t> const addAt = ir::definerIn( block, *address );
      std::optional<ByteIndexed> const found =
          addAt.has_value() ? byteIndexedBy( function, block, *addAt, elementBytes, ranges ) : std::nullopt;
      if ( !found.has_value() )
      {
        continue;
      }
      if ( auto const* const load = std::get_if<ir::LoadIndirect>( &instruction.operation ) )
      {
        instruction.operation = ir::Load{ .result = load->result,
                                          .type = load->type,
                                          .name = found->array,
                                          .index = found->index,
                                          .scaled = false,
                                          .high = {} };
      }
      else
      {
        auto const& store = std::get<ir::StoreIndirect>( instruction.operation );
        instruction.operation = ir::StoreElement{
          .name = found->array, .type = store.type, .index = found->index, .value = store.value, .scaled = false
        };
      }
      for ( std::size_t const gone : found->spent )
      {
        spent[gone] = true;
      }
    }
    std::size_t index = 0;
    std::erase_if( block.instructions, [&spent, &index]( ir::Instruction const& ) { return spent[index++]; } );
  }
}

/// A division of the runtime and the remainder beside it, which divides the
/// same way and leaves the quotient in its own `dividend` — the bytes it was
/// handed the dividend in, shifted out at the top as the quotient's bits come
/// in at the bottom.
struct Division
{
  std::string_view quotient;
  std::string_view remainder;
};

constexpr std::array<Division, 2> DIVISIONS{ { { .quotient = "__udiv8", .remainder = "__umod8" },
                                               { .quotient = "__udiv16", .remainder = "__umod16" } } };

/// Which division a call is, and whether it asks for the quotient; no division
/// where it is a call of anything else.
struct Divides
{
  Division const* division = nullptr;
  bool quotient = false;
};

Divides dividesBy( ir::Call const& call )
{
  if ( !call.result.has_value() || call.arguments.size() != 2 )
  {
    return {};
  }
  for ( Division const& division : DIVISIONS )
  {
    if ( call.name == division.quotient || call.name == division.remainder )
    {
      return Divides{ .division = &division, .quotient = call.name == division.quotient };
    }
  }
  return {};
}

/// Whether two operands read the same: one value, one object, or one constant.
bool sameOperand( ir::Operand const& left, ir::Operand const& right )
{
  if ( left.index() != right.index() )
  {
    return false;
  }
  if ( auto const* const value = std::get_if<ir::Value>( &left ) )
  {
    return value->index == std::get<ir::Value>( right ).index;
  }
  if ( auto const* const object = std::get_if<ir::Object>( &left ) )
  {
    return object->name == std::get<ir::Object>( right ).name && object->type == std::get<ir::Object>( right ).type;
  }
  auto const& number = std::get<ir::Constant>( left );
  auto const& other = std::get<ir::Constant>( right );
  return number.value == other.value && number.name == other.name;
}

/// The objects a call's arguments read, by the names their bytes are known by.
std::vector<std::string> objectsReadBy( ir::Call const& call )
{
  std::vector<std::string> objects;
  for ( ir::Argument const& argument : call.arguments )
  {
    if ( auto const* const object = std::get_if<ir::Object>( &argument.value ) )
    {
      objects.push_back( ir::baseOf( object->name ) );
    }
  }
  return objects;
}

/// Whether the instruction may write an object the call read: a store to one
/// of them, or anything that writes what it cannot name — through a pointer,
/// a copy of bytes, a call, or the entry of a `[[with]]` block.
bool mayWrite( ir::Instruction const& instruction, std::vector<std::string> const& objects )
{
  if ( objects.empty() )
  {
    return false;
  }
  auto const named = [&objects]( std::string const& name )
  { return std::ranges::find( objects, ir::baseOf( name ) ) != objects.end(); };
  if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation ) )
  {
    return named( store->name );
  }
  if ( auto const* const element = std::get_if<ir::StoreElement>( &instruction.operation ) )
  {
    return named( element->name );
  }
  return std::holds_alternative<ir::Call>( instruction.operation ) ||
         std::holds_alternative<ir::StoreIndirect>( instruction.operation ) ||
         std::holds_alternative<ir::Copy>( instruction.operation ) ||
         std::holds_alternative<ir::EnterWith>( instruction.operation );
}

void shareIn( ir::Function& function, std::set<std::string> const& volatiles )
{
  for ( ir::Block& block : function.blocks )
  {
    // The divisions no other has answered yet, by where they stand, while
    // nothing has written what they read; and each first of two, with the
    // second it answers.
    std::vector<std::size_t> open;
    std::map<std::size_t, std::size_t> pairs;
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      ir::Instruction const& instruction = block.instructions[at];
      auto const* const call = std::get_if<ir::Call>( &instruction.operation );
      Divides const divides = call != nullptr ? dividesBy( *call ) : Divides{};
      if ( divides.division == nullptr )
      {
        std::erase_if( open,
                       [&]( std::size_t const other )
                       {
                         return mayWrite( instruction,
                                          objectsReadBy( std::get<ir::Call>( block.instructions[other].operation ) ) );
                       } );
        continue;
      }
      // Two reads of a `volatile` object may read two numbers.
      if ( std::ranges::any_of( objectsReadBy( *call ),
                                [&volatiles]( std::string const& name ) { return volatiles.contains( name ); } ) )
      {
        continue;
      }
      auto const answer =
          std::ranges::find_if( open,
                                [&]( std::size_t const other )
                                {
                                  auto const& earlier = std::get<ir::Call>( block.instructions[other].operation );
                                  Divides const was = dividesBy( earlier );
                                  return was.division == divides.division && was.quotient != divides.quotient &&
                                         earlier.arguments[0].type == call->arguments[0].type &&
                                         sameOperand( earlier.arguments[0].value, call->arguments[0].value ) &&
                                         sameOperand( earlier.arguments[1].value, call->arguments[1].value );
                                } );
      if ( answer != open.end() )
      {
        pairs.emplace( *answer, at );
        open.erase( answer );
      }
      else
      {
        // The runtime writes its own bytes and nothing else, so a division
        // ends no other that is open.
        open.push_back( at );
      }
    }
    if ( pairs.empty() )
    {
      continue;
    }

    // The remainder's call where the first of the two stood, and the
    // quotient read from its `dividend`: where the second stood, if the
    // remainder came first and nothing is called between, since the bytes are
    // the Proc's own and only a call of it, or of anything sharing them, writes
    // them; at once otherwise, the quotient's readers standing before the
    // second or a call standing between.
    std::map<std::size_t, std::vector<ir::Instruction>> instead;
    std::set<std::size_t> dropped;
    for ( auto const& [first, second] : pairs )
    {
      bool const quotientFirst = dividesBy( std::get<ir::Call>( block.instructions[first].operation ) ).quotient;
      bool const calledBetween = std::any_of( block.instructions.begin() + static_cast<std::ptrdiff_t>( first ) + 1,
                                              block.instructions.begin() + static_cast<std::ptrdiff_t>( second ),
                                              []( ir::Instruction const& between )
                                              {
                                                return std::holds_alternative<ir::Call>( between.operation ) ||
                                                       std::holds_alternative<ir::EnterWith>( between.operation );
                                              } );
      ir::Instruction& remainder = block.instructions[quotientFirst ? second : first];
      std::optional<ir::Value> const result =
          std::get<ir::Call>( block.instructions[quotientFirst ? first : second].operation ).result;
      if ( !result.has_value() )
      {
        continue;
      }
      ir::Value const quotient = *result;
      ir::Type const type = function.values.at( quotient.index );
      bool const later = !quotientFirst && !calledBetween;
      ir::Instruction quotientRead{
        .operation =
            ir::Convert{ .result = quotient,
                         .type = type,
                         .operand = ir::Object{ .name = std::get<ir::Call>( remainder.operation ).name + ".dividend",
                                                .type = type } },
        .at = block.instructions[later ? second : first].at
      };
      if ( later )
      {
        instead[second].push_back( std::move( quotientRead ) );
        continue;
      }
      diag::SourceLocation const at = block.instructions[first].at;
      instead[first].push_back( ir::Instruction{ .operation = std::move( remainder.operation ), .at = at } );
      instead[first].push_back( std::move( quotientRead ) );
      dropped.insert( second );
    }
    std::vector<ir::Instruction> rebuilt;
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      if ( dropped.contains( at ) )
      {
        continue;
      }
      if ( auto const found = instead.find( at ); found != instead.end() )
      {
        std::ranges::move( found->second, std::back_inserter( rebuilt ) );
        continue;
      }
      rebuilt.push_back( std::move( block.instructions[at] ) );
    }
    block.instructions = std::move( rebuilt );
  }
}

} // namespace

void narrowIndexes( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      narrowIndexesIn( *function );
    }
  }
}

void narrowRuntimeCalls( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      narrowIn( *function );
    }
  }
}

void shareDivisions( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      shareIn( *function, unit.volatiles );
    }
  }
}

} // namespace nga::c
