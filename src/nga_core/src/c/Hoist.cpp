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
/// of one or two bytes and whose computing changes nothing else, and a call of
/// the runtime's arithmetic, which is one too — see
/// docs/decisions/0304-a-product-a-loop-computes-the-same-is-computed-before-it.md.
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
  if ( auto const* const call = std::get_if<ir::Call>( &instruction.operation ) )
  {
    return isArithmetic( *call ) && call->with.empty();
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
  if ( auto const* const call = std::get_if<ir::Call>( &a.operation ) )
  {
    auto const* const other = std::get_if<ir::Call>( &b.operation );
    return other != nullptr && call->name == other->name && call->arguments.size() == other->arguments.size() &&
           std::ranges::equal( call->arguments,
                               other->arguments,
                               [&same]( ir::Argument const& x, ir::Argument const& y )
                               { return x.name == y.name && x.type == y.type && same( x.value, y.value ); } );
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

void hoistIn( ir::Function& function, std::set<std::string> const& volatiles, Effects const& effects )
{
  std::set<std::string> own = ownBytesOf( function );
  std::uint32_t kept = 0;
  std::vector<Hoisted> hoisted;
  for ( Loop const& loop : loopsOf( function ) )
  {
    // A byte that changes behind the program's back is written at every turn
    // as far as a loop can say — see docs/decisions/0151-volatile.md.
    Written written = writtenIn( function, loop, &effects );
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

        // A byte widened is that byte read as two, `#0` above: its readers
        // read the byte itself, and nothing goes out — so that what is
        // computed from it may, and what reads it knows its high byte is zero.
        if ( auto const* const convert = std::get_if<ir::Convert>( &instruction.operation ) )
        {
          auto const* const object = std::get_if<ir::Object>( &convert->operand );
          if ( object != nullptr && ir::sizeOf( object->type ) == 1 && !ir::isSigned( object->type ) &&
               ir::sizeOf( convert->type ) == 2 && !ir::isSigned( convert->type ) )
          {
            ir::Value const widened = convert->result;
            ir::Operand const byte = *object;
            instructions.erase( instructions.begin() + static_cast<std::ptrdiff_t>( at ) );
            for ( std::uint32_t inside = loop.header; inside <= loop.last; ++inside )
            {
              ir::eachOperand( function.blocks[inside],
                               [&widened, &byte]( ir::Operand& operand )
                               {
                                 if ( auto const* const read = std::get_if<ir::Value>( &operand );
                                      read != nullptr && read->index == widened.index )
                                 {
                                   operand = byte;
                                 }
                               } );
            }
            continue;
          }
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
          // A byte of the Proc's own like any other, so what is computed
          // from it is invariant across a store through a pointer too.
          own.insert( byte );
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
          ir::eachOperand( function.blocks[inside], reread );
        }
      }
    }
  }
}

} // namespace

void hoistInvariants( ir::Unit& unit, Effects const& effects )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      hoistIn( *function, unit.volatiles, effects );
    }
  }
}

} // namespace nga::c
