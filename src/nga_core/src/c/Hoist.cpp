#include "nga/c/Hoist.hpp"

#include "nga/c/Loops.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace nga::c
{

namespace
{

/// The instruction kinds worth moving: an operator, whose answer is one value
/// of one or two bytes and whose computing changes nothing else.
bool hoistable( ir::Instruction const& instruction, ir::Function const& function )
{
  std::optional<ir::Value> const defined = ir::resultOf( instruction );
  if ( !defined.has_value() )
  {
    return false;
  }
  ir::Type const type = function.values[defined->index];
  if ( type == ir::Type::BLOCK || type == ir::Type::POINTER )
  {
    return false;
  }
  return std::holds_alternative<ir::Binary>( instruction.operation ) ||
         std::holds_alternative<ir::Unary>( instruction.operation ) ||
         std::holds_alternative<ir::Convert>( instruction.operation );
}

/// Whether two instructions compute the same thing from the same operands, so
/// that `row * 8` written twice in one loop is kept once.
bool sameComputation( ir::Instruction const& a, ir::Instruction const& b )
{
  auto const same = []( ir::Operand const& x, ir::Operand const& y )
  {
    if ( auto const* const cx = std::get_if<ir::Constant>( &x ) )
    {
      auto const* const cy = std::get_if<ir::Constant>( &y );
      return cy != nullptr && cx->type == cy->type && cx->value == cy->value && cx->name == cy->name;
    }
    if ( auto const* const ox = std::get_if<ir::Object>( &x ) )
    {
      auto const* const oy = std::get_if<ir::Object>( &y );
      return oy != nullptr && ox->name == oy->name && ox->type == oy->type;
    }
    return false;
  };
  if ( auto const* const ba = std::get_if<ir::Binary>( &a.operation ) )
  {
    auto const* const bb = std::get_if<ir::Binary>( &b.operation );
    return bb != nullptr && ba->op == bb->op && ba->type == bb->type && same( ba->left, bb->left ) &&
           same( ba->right, bb->right );
  }
  if ( auto const* const ua = std::get_if<ir::Unary>( &a.operation ) )
  {
    auto const* const ub = std::get_if<ir::Unary>( &b.operation );
    return ub != nullptr && ua->op == ub->op && ua->type == ub->type && same( ua->operand, ub->operand );
  }
  if ( auto const* const ca = std::get_if<ir::Convert>( &a.operation ) )
  {
    auto const* const cb = std::get_if<ir::Convert>( &b.operation );
    return cb != nullptr && ca->type == cb->type && same( ca->operand, cb->operand );
  }
  return false;
}

/// What was hoisted into an entry block already: the instruction as it stands
/// there, and the byte it is kept in.
struct Hoisted
{
  std::uint32_t entry = 0;
  ir::Instruction computed;
  std::string byte;
};

void hoistIn( ir::Function& function, std::set<std::string> const& volatiles )
{
  std::set<std::string> const own = ownBytesOf( function );
  std::uint32_t kept = 0;
  std::vector<Hoisted> hoisted;
  for ( Loop const& loop : loopsOf( function ) )
  {
    // A byte that changes behind the program's back is written at every turn
    // as far as a loop can say — see docs/decisions/0151-volatile.md.
    Written written = writtenIn( function, loop );
    written.objects.insert( volatiles.begin(), volatiles.end() );
    for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
    {
      std::vector<ir::Instruction>& instructions = function.blocks[index].instructions;
      for ( std::size_t at = 0; at < instructions.size(); )
      {
        ir::Instruction& instruction = instructions[at];
        bool moves = hoistable( instruction, function );
        ir::eachOperand( instruction,
                         [&]( ir::Operand& operand ) { moves = moves && invariant( operand, written, own ); } );
        if ( !moves )
        {
          ++at;
          continue;
        }

        // Out it goes: computed where the loop is entered, kept in a byte of
        // its own, and read from that byte wherever the loop read the value —
        // or, where the same computation went out to the same entry before,
        // read from the byte that one is kept in and computed no second time.
        ir::Value const value = *ir::resultOf( instruction );
        ir::Type const type = function.values[value.index];
        std::string byte;
        for ( Hoisted const& before : hoisted )
        {
          if ( before.entry == loop.entry && sameComputation( before.computed, instruction ) )
          {
            byte = before.byte;
          }
        }
        ir::Instruction moved = std::move( instruction );
        instructions.erase( instructions.begin() + static_cast<std::ptrdiff_t>( at ) );
        if ( byte.empty() )
        {
          byte = "__h" + std::to_string( kept++ );
          function.locals.push_back( ir::Local{ .name = byte, .type = type, .bytes = 0 } );
          std::vector<ir::Instruction>& entry = function.blocks[loop.entry].instructions;
          diag::SourceLocation const where = moved.at;
          hoisted.push_back( Hoisted{ .entry = loop.entry, .computed = moved, .byte = byte } );
          entry.push_back( std::move( moved ) );
          entry.push_back( ir::Instruction{
              .operation = ir::Store{ .name = byte, .type = type, .value = ir::Operand{ value } }, .at = where } );
        }

        auto const reread = [&value, &byte, type]( ir::Operand& operand )
        {
          if ( auto const* const read = std::get_if<ir::Value>( &operand );
               read != nullptr && read->index == value.index )
          {
            operand = ir::Object{ .name = byte, .type = type };
          }
        };
        for ( std::uint32_t inside = loop.header; inside <= loop.last; ++inside )
        {
          for ( ir::Instruction& reader : function.blocks[inside].instructions )
          {
            ir::eachOperand( reader, reread );
          }
          if ( function.blocks[inside].terminator.kind == ir::TerminatorKind::BRANCH )
          {
            reread( function.blocks[inside].terminator.condition );
          }
        }
      }
    }
  }
}

} // namespace

void hoistInvariants( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      hoistIn( *function, unit.volatiles );
    }
  }
}

} // namespace nga::c
