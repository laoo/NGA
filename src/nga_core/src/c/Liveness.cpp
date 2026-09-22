#include "nga/c/Liveness.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>

namespace nga::c
{

namespace
{

/// The name an operand reads as an object, whole or in part: `__0q+1` reads
/// `__0q`. Nothing for a constant or a value.
std::optional<std::string> objectRead( ir::Operand const& operand )
{
  auto const* const object = std::get_if<ir::Object>( &operand );
  if ( object == nullptr )
  {
    return std::nullopt;
  }
  return object->name.substr( 0, object->name.find( '+' ) );
}

std::string baseOf( std::string const& name )
{
  return name.substr( 0, name.find( '+' ) );
}

/// The bytes of the Proc's own that liveness is about, by name.
std::set<std::string> ownBytesOf( ir::Function const& function )
{
  std::set<std::string> own;
  for ( ir::Local const& local : function.locals )
  {
    own.insert( local.name );
  }
  if ( function.memberOf.empty() )
  {
    for ( ir::Local const& parameter : function.parameters )
    {
      own.insert( parameter.name );
    }
  }
  return own;
}

/// Every name an instruction reads, given to `use`, and the one it writes
/// whole, given to `def` — a store to part of an object writes nothing whole,
/// so it is a use of the rest and a def of none. A call touches only the
/// callee's bytes; a `switch` reads every byte of ours, since its Procs name
/// them through the function.
template <typename Use, typename Def>
void visitAccesses( ir::Instruction const& instruction, std::set<std::string> const& own, Use use, Def def )
{
  auto const read = [&use]( ir::Operand const& operand )
  {
    if ( std::optional<std::string> const name = objectRead( operand ) )
    {
      use( *name );
    }
  };
  std::visit(
      [&]( auto const& operation )
      {
        using Operation = std::decay_t<decltype( operation )>;
        if constexpr ( std::is_same_v<Operation, ir::Store> )
        {
          read( operation.value );
          if ( !operation.name.contains( '+' ) )
          {
            def( operation.name );
          }
          else
          {
            use( baseOf( operation.name ) );
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::Unary> || std::is_same_v<Operation, ir::Convert> )
        {
          read( operation.operand );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Binary> || std::is_same_v<Operation, ir::Compare> )
        {
          read( operation.left );
          read( operation.right );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Load> )
        {
          use( baseOf( operation.name ) );
          read( operation.index );
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreElement> )
        {
          use( baseOf( operation.name ) );
          read( operation.index );
          read( operation.value );
        }
        else if constexpr ( std::is_same_v<Operation, ir::LoadIndirect> )
        {
          read( operation.pointer );
          read( operation.index );
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> )
        {
          read( operation.pointer );
          read( operation.index );
          read( operation.value );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Copy> )
        {
          // Both ends, whole: a copy into a local writes it, but a partial one
          // cannot be told from a whole one here, so it is a use.
          use( baseOf( operation.from.name ) );
          use( baseOf( operation.to.name ) );
          if ( operation.from.pointer.has_value() )
          {
            read( *operation.from.pointer );
          }
          if ( operation.to.pointer.has_value() )
          {
            read( *operation.to.pointer );
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::Call> )
        {
          for ( ir::Argument const& argument : operation.arguments )
          {
            read( argument.value );
            if ( argument.from.has_value() )
            {
              use( baseOf( argument.from->name ) );
              if ( argument.from->pointer.has_value() )
              {
                read( *argument.from->pointer );
              }
            }
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::Switch> )
        {
          read( operation.value );
          for ( std::string const& name : own )
          {
            use( name );
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::EnterWith> )
        {
          if ( operation.index.has_value() )
          {
            read( *operation.index );
          }
        }
      },
      instruction.operation );
}

/// What a block reads before it writes, and what it writes whole — the two
/// sets a backward pass needs of it.
struct BlockAccess
{
  std::set<std::string> upwardUses;
  std::set<std::string> defs;
};

BlockAccess accessOf( ir::Block const& block, std::set<std::string> const& own )
{
  BlockAccess access;
  auto const use = [&access, &own]( std::string const& name )
  {
    if ( own.contains( name ) && !access.defs.contains( name ) )
    {
      access.upwardUses.insert( name );
    }
  };
  auto const def = [&access, &own]( std::string const& name )
  {
    if ( own.contains( name ) )
    {
      access.defs.insert( name );
    }
  };
  for ( ir::Instruction const& instruction : block.instructions )
  {
    visitAccesses( instruction, own, use, def );
  }
  if ( block.terminator.kind == ir::TerminatorKind::BRANCH )
  {
    if ( std::optional<std::string> const name = objectRead( block.terminator.condition ) )
    {
      use( *name );
    }
  }
  return access;
}

std::vector<std::uint32_t> successorsOf( ir::Terminator const& terminator )
{
  switch ( terminator.kind )
  {
  case ir::TerminatorKind::JUMP:
    return { terminator.target };
  case ir::TerminatorKind::BRANCH:
    return { terminator.target, terminator.otherwise };
  case ir::TerminatorKind::DISPATCH:
    return terminator.targets;
  case ir::TerminatorKind::RETURN:
  case ir::TerminatorKind::TRANSITION:
  case ir::TerminatorKind::FALL:
    // Nothing of the Proc's own outlives the call: the caller wrote the
    // parameters and reads the result, which is not among them.
    return {};
  }
  return {};
}

} // namespace

std::vector<std::set<std::string>> liveInOf( ir::Function const& function )
{
  std::set<std::string> const own = ownBytesOf( function );
  std::vector<BlockAccess> access;
  access.reserve( function.blocks.size() );
  for ( ir::Block const& block : function.blocks )
  {
    access.push_back( accessOf( block, own ) );
  }

  // Backwards to a fixed point: what a block needs alive on entry is what it
  // reads before writing, and what its successors need that it does not
  // write. Blocks are numbered in layout order, so walking them from the last
  // settles most edges in one round and a loop in one more.
  std::vector<std::set<std::string>> liveIn( function.blocks.size() );
  bool changed = true;
  while ( changed )
  {
    changed = false;
    for ( std::size_t index = function.blocks.size(); index-- > 0; )
    {
      std::set<std::string> in = access[index].upwardUses;
      for ( std::uint32_t const next : successorsOf( function.blocks[index].terminator ) )
      {
        if ( next >= liveIn.size() )
        {
          continue;
        }
        for ( std::string const& name : liveIn[next] )
        {
          if ( !access[index].defs.contains( name ) )
          {
            in.insert( name );
          }
        }
      }
      if ( in != liveIn[index] )
      {
        liveIn[index] = std::move( in );
        changed = true;
      }
    }
  }
  return liveIn;
}

namespace
{

/// `store x, V` followed at once by `store y, @x`: the second reads `V`, and
/// the first is then read by nothing that follows it. Only a byte, and only
/// the very next instruction — the emitter keeps a value in `A` for one step
/// and no further, and a wider value is written straight into its object.
/// Not where `x` is volatile, which need not read back what was written to
/// it, nor where `V` reads a volatile byte, which would then be read twice —
/// see docs/decisions/0151-volatile.md.
void forwardStores( ir::Function& function, std::set<std::string> const& volatiles )
{
  auto const isVolatile = [&volatiles]( std::string const& name ) { return volatiles.contains( baseOf( name ) ); };
  for ( ir::Block& block : function.blocks )
  {
    for ( std::size_t at = 0; at + 1 < block.instructions.size(); ++at )
    {
      auto const* const first = std::get_if<ir::Store>( &block.instructions[at].operation );
      auto* const second = std::get_if<ir::Store>( &block.instructions[at + 1].operation );
      if ( first == nullptr || second == nullptr || ir::sizeOf( first->type ) != 1 || ir::sizeOf( second->type ) != 1 ||
           first->name.contains( '+' ) )
      {
        continue;
      }
      auto const* const read = std::get_if<ir::Object>( &second->value );
      auto const* const source = std::get_if<ir::Object>( &first->value );
      if ( read != nullptr && read->name == first->name && ir::sizeOf( read->type ) == 1 &&
           !isVolatile( first->name ) && ( source == nullptr || !isVolatile( source->name ) ) )
      {
        second->value = first->value;
      }
    }
  }
}

/// Whether a value is read by any instruction or terminator of the function.
bool isRead( ir::Function const& function, ir::Value value )
{
  auto const reads = [value]( ir::Operand const& operand )
  {
    auto const* const other = std::get_if<ir::Value>( &operand );
    return other != nullptr && other->index == value.index;
  };
  for ( ir::Block const& block : function.blocks )
  {
    for ( ir::Instruction const& instruction : block.instructions )
    {
      bool found = false;
      std::visit(
          [&]( auto const& operation )
          {
            using Operation = std::decay_t<decltype( operation )>;
            if constexpr ( std::is_same_v<Operation, ir::Store> || std::is_same_v<Operation, ir::Switch> )
            {
              found = reads( operation.value );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Unary> || std::is_same_v<Operation, ir::Convert> )
            {
              found = reads( operation.operand );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Binary> || std::is_same_v<Operation, ir::Compare> )
            {
              found = reads( operation.left ) || reads( operation.right );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Load> )
            {
              found = reads( operation.index );
            }
            else if constexpr ( std::is_same_v<Operation, ir::StoreElement> )
            {
              found = reads( operation.index ) || reads( operation.value );
            }
            else if constexpr ( std::is_same_v<Operation, ir::LoadIndirect> )
            {
              found = reads( operation.pointer ) || reads( operation.index );
            }
            else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> )
            {
              found = reads( operation.pointer ) || reads( operation.index ) || reads( operation.value );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Copy> )
            {
              found = ( operation.from.pointer.has_value() && reads( *operation.from.pointer ) ) ||
                      ( operation.to.pointer.has_value() && reads( *operation.to.pointer ) );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Call> )
            {
              found = std::ranges::any_of( operation.arguments,
                                           [&reads]( ir::Argument const& argument )
                                           {
                                             return reads( argument.value ) ||
                                                    ( argument.from.has_value() && argument.from->pointer.has_value() &&
                                                      reads( *argument.from->pointer ) );
                                           } );
            }
            else if constexpr ( std::is_same_v<Operation, ir::EnterWith> )
            {
              found = operation.index.has_value() && reads( *operation.index );
            }
          },
          instruction.operation );
      if ( found )
      {
        return true;
      }
    }
    if ( block.terminator.kind == ir::TerminatorKind::BRANCH && reads( block.terminator.condition ) )
    {
      return true;
    }
  }
  return false;
}

/// Whether an instruction only computes: dropping it where its value is read
/// by nothing changes what the program does not at all. A read of an element
/// of a volatile array is a read of the machine, and stays.
bool onlyComputes( ir::Instruction const& instruction, std::set<std::string> const& volatiles )
{
  if ( auto const* const load = std::get_if<ir::Load>( &instruction.operation );
       load != nullptr && volatiles.contains( baseOf( load->name ) ) )
  {
    return false;
  }
  if ( auto const* const indirect = std::get_if<ir::LoadIndirect>( &instruction.operation );
       indirect != nullptr && indirect->isVolatile )
  {
    return false;
  }
  return std::holds_alternative<ir::Unary>( instruction.operation ) ||
         std::holds_alternative<ir::Binary>( instruction.operation ) ||
         std::holds_alternative<ir::Compare>( instruction.operation ) ||
         std::holds_alternative<ir::Convert>( instruction.operation ) ||
         std::holds_alternative<ir::Load>( instruction.operation ) ||
         std::holds_alternative<ir::LoadIndirect>( instruction.operation );
}

void dropIn( ir::Function& function, std::set<std::string> const& volatiles )
{
  if ( function.blocks.empty() )
  {
    return;
  }
  forwardStores( function, volatiles );

  std::set<std::string> const own = ownBytesOf( function );
  std::vector<std::set<std::string>> const liveIn = liveInOf( function );

  // Each block backwards, carrying what is live: a store to a byte of ours
  // that is not is dropped, and a store that stands makes its byte dead
  // above it, where a read makes it live.
  for ( ir::Block& block : function.blocks )
  {
    std::set<std::string> live;
    for ( std::uint32_t const next : successorsOf( block.terminator ) )
    {
      if ( next < liveIn.size() )
      {
        live.insert( liveIn[next].begin(), liveIn[next].end() );
      }
    }
    if ( block.terminator.kind == ir::TerminatorKind::BRANCH )
    {
      if ( std::optional<std::string> const name = objectRead( block.terminator.condition ) )
      {
        live.insert( *name );
      }
    }

    std::vector<bool> dead( block.instructions.size(), false );
    for ( std::size_t at = block.instructions.size(); at-- > 0; )
    {
      ir::Instruction const& instruction = block.instructions[at];
      auto const* const store = std::get_if<ir::Store>( &instruction.operation );
      if ( store != nullptr && own.contains( store->name ) && !live.contains( store->name ) )
      {
        dead[at] = true;
        continue;
      }
      visitAccesses(
          instruction,
          own,
          [&live]( std::string const& name ) { live.insert( name ); },
          [&live]( std::string const& name ) { live.erase( name ); } );
    }
    std::size_t kept = 0;
    std::erase_if( block.instructions, [&dead, &kept]( ir::Instruction const& ) { return dead[kept++]; } );
  }

  // What fed a dropped store, and now feeds nothing: gone too, until nothing
  // more goes. A call stays whatever becomes of its result.
  bool dropped = true;
  while ( dropped )
  {
    dropped = false;
    for ( ir::Block& block : function.blocks )
    {
      std::size_t const before = block.instructions.size();
      std::erase_if( block.instructions,
                     [&function, &volatiles]( ir::Instruction const& instruction )
                     {
                       std::optional<ir::Value> const defined = ir::resultOf( instruction );
                       return defined.has_value() && onlyComputes( instruction, volatiles ) &&
                              !isRead( function, *defined );
                     } );
      dropped = dropped || block.instructions.size() != before;
    }
  }
}

} // namespace

void dropDeadStores( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* function = std::get_if<ir::Function>( &definition ) )
    {
      dropIn( *function, unit.volatiles );
    }
  }
}

} // namespace nga::c
