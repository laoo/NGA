#include "nga/c/Walk.hpp"

#include "nga/c/Loops.hpp"

#include <cstdint>
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
/// written nowhere else in the loop. `at` is where the step's store stands.
struct Counter
{
  std::string name;
  std::size_t at = 0;
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
      writes += static_cast<std::size_t>( other != nullptr && baseOf( other->name ) == counter->name );
    }
  }
  std::set<std::string> const own = ownBytesOf( function );
  if ( writes != 1 || !own.contains( counter->name ) )
  {
    return std::nullopt;
  }
  return Counter{ .name = counter->name, .at = at };
}

/// The operand an address adds to the counter, where an instruction is such an
/// address: `add ptr flags, @i` gives `flags`, and nothing else gives anything.
std::optional<ir::Operand> baseOfAddress( ir::Instruction const& instruction, Counter const& counter )
{
  auto const* const add = std::get_if<ir::Binary>( &instruction.operation );
  if ( add == nullptr || add->op != ir::BinaryOperator::ADD || add->type != ir::Type::POINTER )
  {
    return std::nullopt;
  }
  auto const* const right = std::get_if<ir::Object>( &add->right );
  if ( right != nullptr && right->name == counter.name )
  {
    return add->left;
  }
  auto const* const left = std::get_if<ir::Object>( &add->left );
  if ( left != nullptr && left->name == counter.name )
  {
    return add->right;
  }
  return std::nullopt;
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

/// A pointer beside the counter: the base it walks from, and its byte.
struct Walker
{
  ir::Operand base;
  std::string byte;
};

void walkIn( ir::Function& function )
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
    Written const written = writtenIn( function, loop );
    std::vector<Walker> walkers;
    for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
    {
      std::vector<ir::Instruction>& instructions = function.blocks[index].instructions;
      for ( std::size_t at = 0; at < instructions.size(); )
      {
        std::optional<ir::Operand> const base = baseOfAddress( instructions[at], *counter );
        if ( !base.has_value() || !invariant( *base, written, own ) )
        {
          ++at;
          continue;
        }
        ir::Value const value = *ir::resultOf( instructions[at] );
        diag::SourceLocation const where = instructions[at].at;

        // One pointer for one base, set where the loop is entered and
        // stepped beside the counter.
        std::string byte;
        for ( Walker const& walker : walkers )
        {
          if ( sameBase( walker.base, *base ) )
          {
            byte = walker.byte;
          }
        }
        if ( byte.empty() )
        {
          byte = "__p" + std::to_string( kept++ );
          function.locals.push_back( ir::Local{ .name = byte, .type = ir::Type::POINTER, .bytes = 0 } );
          walkers.push_back( Walker{ .base = *base, .byte = byte } );

          ir::Value const start{ static_cast<std::uint32_t>( function.values.size() ) };
          function.values.push_back( ir::Type::POINTER );
          std::vector<ir::Instruction>& entry = function.blocks[loop.entry].instructions;
          entry.push_back( ir::Instruction{
              .operation = ir::Binary{ .result = start,
                                       .op = ir::BinaryOperator::ADD,
                                       .type = ir::Type::POINTER,
                                       .left = *base,
                                       .right = ir::Object{ .name = counter->name, .type = ir::Type::U16 } },
              .at = where } );
          entry.push_back( ir::Instruction{
              .operation = ir::Store{ .name = byte, .type = ir::Type::POINTER, .value = ir::Operand{ start } },
              .at = where } );

          ir::Value const stepped{ static_cast<std::uint32_t>( function.values.size() ) };
          function.values.push_back( ir::Type::POINTER );
          std::vector<ir::Instruction>& last = function.blocks[loop.last].instructions;
          auto const after = last.begin() + static_cast<std::ptrdiff_t>( counter->at ) + 1;
          last.insert(
              after,
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

        // The address is the pointer now, wherever the loop read the value.
        instructions.erase( instructions.begin() + static_cast<std::ptrdiff_t>( at ) );
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
          for ( ir::Instruction& reader : function.blocks[inside].instructions )
          {
            ir::eachOperand( reader, reread );
          }
        }
      }
    }
  }
}

} // namespace

void walkPointers( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      walkIn( *function );
    }
  }
}

} // namespace nga::c
