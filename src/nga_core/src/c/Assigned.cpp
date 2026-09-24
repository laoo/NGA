#include "nga/c/Assigned.hpp"

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

/// One `static` local this analysis is about, and the bytes of it.
struct Candidate
{
  std::string name;
  std::string written;
  std::uint32_t bytes = 1;
};

std::string baseOf( std::string const& name )
{
  return name.substr( 0, name.find( '+' ) );
}

std::uint32_t offsetIn( std::string const& name )
{
  std::string::size_type const plus = name.find( '+' );
  if ( plus == std::string::npos )
  {
    return 0;
  }
  return static_cast<std::uint32_t>( std::stoul( name.substr( plus + 1 ) ) );
}

/// The `static` locals of a function: its own objects that are neither a local
/// array nor given a value. An array is left out because a store to an element
/// writes a byte no analysis here can name, and one given a value has nothing
/// to report.
std::vector<Candidate> candidatesOf( ir::Function const& function )
{
  std::vector<Candidate> candidates;
  for ( ir::Global const& object : function.sections )
  {
    if ( object.isTemporary || object.count.has_value() || object.value.has_value() )
    {
      continue;
    }
    std::uint32_t const bytes = ir::sizeOf( object.type );
    if ( bytes == 0 || bytes > 2 )
    {
      continue;
    }
    candidates.push_back( Candidate{
        .name = object.name, .written = object.written.empty() ? object.name : object.written, .bytes = bytes } );
  }
  return candidates;
}

/// Every Operand an instruction holds, whatever it does with it. Both readers
/// below are written over this one walk, so the shape of an instruction is
/// known in one place.
template <typename Each>
void visitOperands( ir::Instruction const& instruction, Each each )
{
  std::visit(
      [&]( auto const& operation )
      {
        using Operation = std::decay_t<decltype( operation )>;
        // A `switch` holds the value it tests where a store holds the value it
        // writes, and this walk is about the operand either way.
        if constexpr ( std::is_same_v<Operation, ir::Store> || std::is_same_v<Operation, ir::Switch> )
        {
          each( operation.value );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Unary> || std::is_same_v<Operation, ir::Convert> )
        {
          each( operation.operand );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Binary> || std::is_same_v<Operation, ir::Compare> )
        {
          each( operation.left );
          each( operation.right );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Load> )
        {
          each( operation.index );
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreElement> )
        {
          each( operation.index );
          each( operation.value );
        }
        else if constexpr ( std::is_same_v<Operation, ir::LoadIndirect> )
        {
          each( operation.pointer );
          each( operation.index );
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> )
        {
          each( operation.pointer );
          each( operation.index );
          each( operation.value );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Copy> )
        {
          if ( operation.from.pointer.has_value() )
          {
            each( *operation.from.pointer );
          }
          if ( operation.to.pointer.has_value() )
          {
            each( *operation.to.pointer );
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::Call> )
        {
          for ( ir::Argument const& argument : operation.arguments )
          {
            each( argument.value );
            if ( argument.from.has_value() && argument.from->pointer.has_value() )
            {
              each( *argument.from->pointer );
            }
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::EnterWith> )
        {
          if ( operation.index.has_value() )
          {
            each( *operation.index );
          }
        }
      },
      instruction.operation );
}

/// Every byte an instruction reads and every byte it writes whole, by the name
/// the text spells it: its Operands, and the names an instruction holds outside
/// one.
///
/// A `switch` is read here as reading its operand alone, where
/// `Liveness.cpp` reads it as using every byte of the Proc. The two are not
/// inconsistent: liveness asks what a byte **may** be needed for and is safe
/// where it assumes a read, and this asks what **must** already be written and
/// would report correct code for the same assumption. A case is a block of this
/// function since
/// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md, so the
/// flow already carries what a case writes.
template <typename Read, typename Write>
void visitBytes( ir::Instruction const& instruction, Read read, Write write )
{
  visitOperands( instruction,
                 [&read]( ir::Operand const& value )
                 {
                   if ( auto const* const object = std::get_if<ir::Object>( &value ) )
                   {
                     read( object->name );
                   }
                 } );
  std::visit(
      [&]( auto const& operation )
      {
        using Operation = std::decay_t<decltype( operation )>;
        if constexpr ( std::is_same_v<Operation, ir::Store> )
        {
          write( operation.name );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Load> || std::is_same_v<Operation, ir::StoreElement> )
        {
          read( operation.name );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Copy> )
        {
          if ( !operation.from.pointer.has_value() )
          {
            read( operation.from.name );
          }
          if ( !operation.to.pointer.has_value() )
          {
            write( operation.to.name );
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::Call> )
        {
          for ( ir::Argument const& argument : operation.arguments )
          {
            if ( argument.from.has_value() && !argument.from->pointer.has_value() )
            {
              read( argument.from->name );
            }
          }
        }
      },
      instruction.operation );
}

/// Whether the address of a candidate is taken anywhere in the function, which
/// a `Constant` naming it is. A write through such a pointer is a write this
/// analysis cannot see, so the candidate is left alone.
bool addressTaken( ir::Function const& function, std::string const& name )
{
  for ( ir::Block const& block : function.blocks )
  {
    for ( ir::Instruction const& instruction : block.instructions )
    {
      bool taken = false;
      visitOperands( instruction,
                     [&]( ir::Operand const& value )
                     {
                       auto const* const constant = std::get_if<ir::Constant>( &value );
                       if ( constant != nullptr && !constant->name.empty() && baseOf( constant->name ) == name )
                       {
                         taken = true;
                       }
                     } );
      if ( taken )
      {
        return true;
      }
    }
  }
  return false;
}

std::vector<bool> reachableFrom( ir::Function const& function )
{
  std::vector<bool> seen( function.blocks.size(), false );
  if ( function.blocks.empty() )
  {
    return seen;
  }
  std::vector<std::uint32_t> pending{ 0 };
  seen[0] = true;
  while ( !pending.empty() )
  {
    std::uint32_t const block = pending.back();
    pending.pop_back();
    for ( std::uint32_t const next : ir::successorsOf( function.blocks[block].terminator ) )
    {
      if ( next < seen.size() && !seen[next] )
      {
        seen[next] = true;
        pending.push_back( next );
      }
    }
  }
  return seen;
}

} // namespace

void checkStaticsAreAssigned( ir::Unit const& unit, diag::DiagnosticSink& sink )
{
  for ( ir::Definition const& definition : unit.definitions )
  {
    auto const* const function = std::get_if<ir::Function>( &definition );
    if ( function == nullptr || function->blocks.empty() )
    {
      continue;
    }

    std::vector<Candidate> candidates = candidatesOf( *function );
    std::erase_if( candidates, [&]( Candidate const& one ) { return addressTaken( *function, one.name ); } );
    if ( candidates.empty() )
    {
      continue;
    }

    // A bit per byte of each candidate, so that a pair is answered a byte at a
    // time and a program that writes one byte and reads the other is told.
    auto const bitOf = [&candidates]( std::string const& spelled ) -> std::optional<std::uint32_t>
    {
      std::string const base = baseOf( spelled );
      std::uint32_t bit = 0;
      for ( Candidate const& one : candidates )
      {
        if ( one.name == base )
        {
          std::uint32_t const offset = offsetIn( spelled );
          return offset < one.bytes ? std::optional{ bit + offset } : std::nullopt;
        }
        bit += one.bytes;
      }
      return std::nullopt;
    };
    std::uint32_t everyBit = 0;
    for ( Candidate const& one : candidates )
    {
      everyBit = ( everyBit << one.bytes ) | ( ( 1U << one.bytes ) - 1U );
    }

    std::vector<bool> const reachable = reachableFrom( *function );
    std::vector<std::uint32_t> writes( function->blocks.size(), 0 );
    for ( std::size_t index = 0; index < function->blocks.size(); ++index )
    {
      for ( ir::Instruction const& instruction : function->blocks[index].instructions )
      {
        visitBytes(
            instruction,
            []( std::string const& ) {},
            [&]( std::string const& name )
            {
              if ( std::optional<std::uint32_t> const bit = bitOf( name ) )
              {
                writes[index] |= 1U << *bit;
              }
            } );
      }
    }

    // Which bytes every path from the entry has written, block by block: the
    // intersection over the predecessors, to a fixed point. Unreachable blocks
    // stand at every byte written, so that code no call enters reports nothing.
    std::vector<std::vector<std::uint32_t>> predecessors( function->blocks.size() );
    for ( std::uint32_t index = 0; index < function->blocks.size(); ++index )
    {
      for ( std::uint32_t const next : ir::successorsOf( function->blocks[index].terminator ) )
      {
        if ( next < predecessors.size() )
        {
          predecessors[next].push_back( index );
        }
      }
    }
    std::vector<std::uint32_t> written( function->blocks.size(), everyBit );
    std::vector<std::uint32_t> entering( function->blocks.size(), everyBit );
    entering[0] = 0;
    for ( bool changed = true; changed; )
    {
      changed = false;
      for ( std::size_t index = 0; index < function->blocks.size(); ++index )
      {
        std::uint32_t before = index == 0 ? 0U : everyBit;
        if ( index != 0 )
        {
          for ( std::uint32_t const previous : predecessors[index] )
          {
            before &= written[previous];
          }
        }
        std::uint32_t const after = before | writes[index];
        if ( before != entering[index] || after != written[index] )
        {
          entering[index] = before;
          written[index] = after;
          changed = true;
        }
      }
    }

    std::set<std::string> reported;
    auto const say = [&]( std::string const& name, diag::SourceLocation where, std::uint32_t held )
    {
      std::optional<std::uint32_t> const bit = bitOf( name );
      if ( !bit.has_value() || ( held & ( 1U << *bit ) ) != 0 )
      {
        return;
      }
      std::string const base = baseOf( name );
      if ( !reported.insert( base ).second )
      {
        return;
      }
      auto const which =
          std::ranges::find_if( candidates, [&base]( Candidate const& one ) { return one.name == base; } );
      sink.add( diag::diagnostic( diag::DiagnosticId::C_STATIC_READ_BEFORE_WRITTEN )
                    .at( where, 0 )
                    .arg( "name", which->written ) );
    };

    for ( std::size_t index = 0; index < function->blocks.size(); ++index )
    {
      if ( !reachable[index] )
      {
        continue;
      }
      std::uint32_t held = entering[index];
      for ( ir::Instruction const& instruction : function->blocks[index].instructions )
      {
        visitBytes(
            instruction,
            [&]( std::string const& name ) { say( name, instruction.at, held ); },
            [&]( std::string const& name )
            {
              if ( std::optional<std::uint32_t> const bit = bitOf( name ) )
              {
                held |= 1U << *bit;
              }
            } );
      }
      // A BRANCH reads its `bool` where the block ends, and an `if` over an
      // object reads it there and in no instruction of its own.
      ir::Terminator const& terminator = function->blocks[index].terminator;
      if ( terminator.kind == ir::TerminatorKind::BRANCH )
      {
        if ( auto const* const object = std::get_if<ir::Object>( &terminator.condition ) )
        {
          say( object->name, terminator.at, held );
        }
      }
    }
  }
}

} // namespace nga::c
