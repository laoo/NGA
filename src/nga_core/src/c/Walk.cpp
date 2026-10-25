#include "nga/c/Walk.hpp"

#include "nga/c/Loops.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace nga::c
{

namespace
{

/// The loop's counter, where it has one this pass can follow: a 16-bit byte
/// of the Proc's own, stepped by one at the end of the loop's last block and
/// written nowhere else in the loop.
struct Counter
{
  std::string name;
};

std::optional<Counter> counterOf( ir::Function const& function, Loop const& loop )
{
  ir::Block const& last = function.blocks[loop.last];
  if ( last.instructions.size() < 2 )
  {
    return std::nullopt;
  }
  std::size_t const at = last.instructions.size() - 1;
  auto const* const store = std::get_if<ir::Store>( &last.instructions[at].operation );
  auto const* const add = std::get_if<ir::Binary>( &last.instructions[at - 1].operation );
  if ( store == nullptr || add == nullptr || add->op != ir::BinaryOperator::ADD || add->type != ir::Type::U16 ||
       store->type != ir::Type::U16 || !std::holds_alternative<ir::Value>( store->value ) ||
       std::get<ir::Value>( store->value ).index != add->result.index )
  {
    return std::nullopt;
  }
  auto const* const counter = std::get_if<ir::Object>( &add->left );
  auto const* const by = std::get_if<ir::Constant>( &add->right );
  if ( counter == nullptr || counter->name != store->name || by == nullptr || !by->name.empty() || by->value != 1 )
  {
    return std::nullopt;
  }

  // One step and no other writing: a counter written twice in the loop is not
  // one a pointer could keep beside.
  std::size_t writes = 0;
  for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
  {
    for ( ir::Instruction const& instruction : function.blocks[index].instructions )
    {
      auto const* const other = std::get_if<ir::Store>( &instruction.operation );
      writes += static_cast<std::size_t>( other != nullptr && ir::baseOf( other->name ) == counter->name );
    }
  }
  std::set<std::string> const own = ownBytesOf( function );
  if ( writes != 1 || !own.contains( counter->name ) )
  {
    return std::nullopt;
  }
  return Counter{ .name = counter->name };
}

/// What an address adds to the counter: its base, and what is added to the
/// counter before it, where the address is `base + (offset + i)`; and the
/// instructions of the block that computed that sum, which go once nothing
/// else reads them.
struct Address
{
  ir::Operand base;
  std::optional<ir::Operand> offset;
  std::vector<std::size_t> spent;
};

/// Whether an operand is the counter, read as itself.
bool isCounter( ir::Operand const& operand, Counter const& counter )
{
  auto const* const object = std::get_if<ir::Object>( &operand );
  return object != nullptr && object->name == counter.name;
}

/// The address an instruction is, where it is one this pass walks: `add ptr
/// flags, @i` gives `flags`; `add ptr frame, %v`, `%v` the sum `row + i` read
/// as an address, gives `frame` and `row`. Nothing else gives anything.
std::optional<Address> addressOf( ir::Function const& function,
                                  ir::Block const& block,
                                  ir::Instruction const& instruction,
                                  Counter const& counter )
{
  auto const* const add = std::get_if<ir::Binary>( &instruction.operation );
  if ( add == nullptr || add->op != ir::BinaryOperator::ADD || add->type != ir::Type::POINTER )
  {
    return std::nullopt;
  }
  if ( isCounter( add->right, counter ) )
  {
    return Address{ .base = add->left, .offset = std::nullopt, .spent = {} };
  }
  if ( isCounter( add->left, counter ) )
  {
    return Address{ .base = add->right, .offset = std::nullopt, .spent = {} };
  }

  // The base on the left, the sum read as an address on the right, as the
  // lowering writes `t[e]`.
  auto const* const read = std::get_if<ir::Value>( &add->right );
  std::optional<std::size_t> const convertAt = read != nullptr ? ir::definerIn( block, *read ) : std::nullopt;
  auto const* const convert =
      convertAt.has_value() ? std::get_if<ir::Convert>( &block.instructions[*convertAt].operation ) : nullptr;
  auto const* const summed = convert != nullptr ? std::get_if<ir::Value>( &convert->operand ) : nullptr;
  std::optional<std::size_t> const sumAt = summed != nullptr ? ir::definerIn( block, *summed ) : std::nullopt;
  auto const* const sum =
      sumAt.has_value() ? std::get_if<ir::Binary>( &block.instructions[*sumAt].operation ) : nullptr;
  if ( !convertAt.has_value() || !sumAt.has_value() || convert == nullptr || convert->type != ir::Type::POINTER ||
       sum == nullptr || sum->op != ir::BinaryOperator::ADD || sum->type != ir::Type::U16 )
  {
    return std::nullopt;
  }
  std::size_t const convertedAt = *convertAt;
  std::size_t const summedAt = *sumAt;
  std::optional<ir::Operand> offset;
  if ( isCounter( sum->right, counter ) )
  {
    offset = sum->left;
  }
  else if ( isCounter( sum->left, counter ) )
  {
    offset = sum->right;
  }
  if ( !offset.has_value() || std::holds_alternative<ir::Value>( *offset ) )
  {
    return std::nullopt;
  }
  Address found{ .base = add->left, .offset = offset, .spent = {} };
  if ( ir::readersOf( function, *read ) == 1 )
  {
    found.spent.push_back( convertedAt );
    if ( ir::readersOf( function, *summed ) == 1 )
    {
      found.spent.push_back( summedAt );
    }
  }
  return found;
}

bool sameBase( ir::Operand const& a, ir::Operand const& b )
{
  if ( auto const* const ca = std::get_if<ir::Constant>( &a ) )
  {
    auto const* const cb = std::get_if<ir::Constant>( &b );
    return cb != nullptr && ca->name == cb->name && ca->value == cb->value && ca->type == cb->type;
  }
  if ( auto const* const oa = std::get_if<ir::Object>( &a ) )
  {
    auto const* const ob = std::get_if<ir::Object>( &b );
    return ob != nullptr && oa->name == ob->name;
  }
  return false;
}

/// A pointer beside the counter: the base it walks from, what is added to the
/// counter before the base where something is, and its byte.
struct Walker
{
  ir::Operand base;
  std::optional<ir::Operand> offset;
  std::string byte;
};

void walkIn( ir::Function& function, Effects const& effects )
{
  std::set<std::string> const own = ownBytesOf( function );
  std::uint32_t kept = 0;
  for ( Loop const& loop : loopsOf( function ) )
  {
    std::optional<Counter> const counter = counterOf( function, loop );
    if ( !counter.has_value() )
    {
      continue;
    }
    Written const written = writtenIn( function, loop, &effects );
    std::vector<Walker> walkers;
    for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
    {
      std::vector<ir::Instruction>& instructions = function.blocks[index].instructions;
      for ( std::size_t at = 0; at < instructions.size(); )
      {
        std::optional<Address> const address =
            addressOf( function, function.blocks[index], instructions[at], *counter );
        if ( !address.has_value() || !invariant( address->base, written, own ) ||
             ( address->offset.has_value() && !invariant( *address->offset, written, own ) ) )
        {
          ++at;
          continue;
        }
        ir::Operand const* const base = &address->base;
        ir::Value const value = *ir::resultOf( instructions[at] );
        diag::SourceLocation const where = instructions[at].at;

        // One pointer for one base, set where the loop is entered and
        // stepped beside the counter.
        std::string byte;
        for ( Walker const& walker : walkers )
        {
          bool const sameOffset = walker.offset.has_value() == address->offset.has_value() &&
                                  ( !walker.offset.has_value() || sameBase( *walker.offset, *address->offset ) );
          if ( sameBase( walker.base, *base ) && sameOffset )
          {
            byte = walker.byte;
          }
        }
        if ( byte.empty() )
        {
          byte = "__p" + std::to_string( kept++ );
          function.locals.push_back( ir::Local{ .name = byte, .type = ir::Type::POINTER, .bytes = 0 } );
          walkers.push_back( Walker{ .base = *base, .offset = address->offset, .byte = byte } );

          std::vector<ir::Instruction>& entry = function.blocks[loop.entry].instructions;
          // The counter, and what is added to it first where something is.
          ir::Operand from = ir::Object{ .name = counter->name, .type = ir::Type::U16 };
          if ( address->offset.has_value() )
          {
            ir::Value const summed{ static_cast<std::uint32_t>( function.values.size() ) };
            function.values.push_back( ir::Type::U16 );
            entry.push_back( ir::Instruction{ .operation = ir::Binary{ .result = summed,
                                                                       .op = ir::BinaryOperator::ADD,
                                                                       .type = ir::Type::U16,
                                                                       .left = *address->offset,
                                                                       .right = from },
                                              .at = where } );
            from = summed;
          }
          ir::Value const start{ static_cast<std::uint32_t>( function.values.size() ) };
          function.values.push_back( ir::Type::POINTER );
          entry.push_back( ir::Instruction{ .operation = ir::Binary{ .result = start,
                                                                     .op = ir::BinaryOperator::ADD,
                                                                     .type = ir::Type::POINTER,
                                                                     .left = *base,
                                                                     .right = from },
                                            .at = where } );
          entry.push_back( ir::Instruction{
              .operation = ir::Store{ .name = byte, .type = ir::Type::POINTER, .value = ir::Operand{ start } },
              .at = where } );

          ir::Value const stepped{ static_cast<std::uint32_t>( function.values.size() ) };
          function.values.push_back( ir::Type::POINTER );
          // Beside the step, which is the counter's one store in the loop:
          // found where it now stands, since an address of the last block
          // already turned into a pointer has gone from before it.
          std::vector<ir::Instruction>& last = function.blocks[loop.last].instructions;
          auto const step = std::ranges::find_if( last,
                                                  [&counter]( ir::Instruction const& one )
                                                  {
                                                    auto const* const store = std::get_if<ir::Store>( &one.operation );
                                                    return store != nullptr && store->name == counter->name;
                                                  } );
          last.insert(
              step + 1,
              { ir::Instruction{
                    .operation =
                        ir::Binary{ .result = stepped,
                                    .op = ir::BinaryOperator::ADD,
                                    .type = ir::Type::POINTER,
                                    .left = ir::Object{ .name = byte, .type = ir::Type::POINTER },
                                    .right =
                                        ir::Constant{ .type = ir::Type::U16, .value = 1, .name = {}, .follower = {} } },
                    .at = where },
                ir::Instruction{
                    .operation = ir::Store{ .name = byte, .type = ir::Type::POINTER, .value = ir::Operand{ stepped } },
                    .at = where } } );
        }

        // The address is the pointer now, wherever the loop read the value;
        // and the sum it was computed from goes where nothing else reads it.
        std::vector<std::size_t> gone = address->spent;
        gone.push_back( at );
        std::ranges::sort( gone, std::greater<>{} );
        for ( std::size_t const one : gone )
        {
          instructions.erase( instructions.begin() + static_cast<std::ptrdiff_t>( one ) );
        }
        at -= gone.size() - 1;
        auto const reread = [&value, &byte]( ir::Operand& operand )
        {
          if ( auto const* const read = std::get_if<ir::Value>( &operand );
               read != nullptr && read->index == value.index )
          {
            operand = ir::Object{ .name = byte, .type = ir::Type::POINTER };
          }
        };
        for ( std::uint32_t inside = loop.header; inside <= loop.last; ++inside )
        {
          ir::eachOperand( function.blocks[inside], reread );
        }
      }
    }
  }
}

} // namespace

void walkPointers( ir::Unit& unit, Effects const& effects )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      walkIn( *function, effects );
    }
  }
}

} // namespace nga::c
