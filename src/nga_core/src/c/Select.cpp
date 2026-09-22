#include "nga/c/Select.hpp"

#include "nga/c/Ranges.hpp"

#include <cstdint>
#include <optional>
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

/// How many instructions of the function read a value.
std::size_t readersOf( ir::Function& function, ir::Value value )
{
  std::size_t readers = 0;
  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      ir::eachOperand( instruction,
                       [&readers, value]( ir::Operand& operand )
                       {
                         auto const* const read = std::get_if<ir::Value>( &operand );
                         readers += static_cast<std::size_t>( read != nullptr && read->index == value.index );
                       } );
    }
  }
  return readers;
}

/// The instruction of a block that defines a value, by its position, or
/// nothing where the value is defined elsewhere.
std::optional<std::size_t> definerIn( ir::Block const& block, ir::Value value )
{
  for ( std::size_t at = 0; at < block.instructions.size(); ++at )
  {
    std::optional<ir::Value> const defined = ir::resultOf( block.instructions[at] );
    if ( defined.has_value() && defined->index == value.index )
    {
      return at;
    }
  }
  return std::nullopt;
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
       readersOf( function, add->result ) != 1 )
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
    std::optional<std::size_t> const at = doubled != nullptr ? definerIn( block, *doubled ) : std::nullopt;
    if ( !at.has_value() )
    {
      return std::nullopt;
    }
    auto const* const shift = std::get_if<ir::Binary>( &block.instructions[*at].operation );
    auto const* const by = shift != nullptr ? std::get_if<ir::Constant>( &shift->right ) : nullptr;
    if ( shift == nullptr || shift->op != ir::BinaryOperator::SHIFT_LEFT || by == nullptr || by->value != 1 ||
         readersOf( function, shift->result ) != 1 )
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
    if ( std::optional<std::size_t> const at = definerIn( block, *value ); at.has_value() )
    {
      auto const* const widened = std::get_if<ir::Convert>( &block.instructions[*at].operation );
      if ( widened != nullptr && ir::sizeOf( ir::typeOf( widened->operand, function ) ) == 1 &&
           readersOf( function, *value ) == 1 )
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
      std::optional<std::size_t> const addAt = definerIn( block, *address );
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

} // namespace nga::c
