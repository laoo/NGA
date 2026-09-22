#include "nga/c/Result.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace nga::c
{

namespace
{

/// The object a name stands for, without the offset a byte of it carries:
/// `__0made+1` is `__0made`.
std::string baseOf( std::string const& name )
{
  return name.substr( 0, name.find( '+' ) );
}

/// The bytes a local holds, and the bytes the result does: a `struct` counts
/// its own, and everything else the bytes of its type.
std::uint32_t bytesOf( ir::Type type, std::uint32_t blockBytes )
{
  return type == ir::Type::BLOCK ? blockBytes : ir::sizeOf( type );
}

/// A write of the result: a `return` of a value, or the copy a `return` of a
/// `struct` is. Where the write copies one object whole, `from` is that
/// object's name.
struct Write
{
  std::uint32_t block = 0;
  std::size_t at = 0;
  std::string from{};
};

/// Every write of the result, or nothing where one of them is not the last
/// thing the function does. The merge rests on that: from the first write on,
/// the local's bytes are the result's, and a function that went on running
/// could read the local it had just overwritten.
std::optional<std::vector<Write>> writesOfResult( ir::Function& function )
{
  std::vector<Write> writes;
  std::uint32_t mentions = 0;
  for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
  {
    ir::Block& block = function.blocks[index];
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      ir::Instruction& instruction = block.instructions[at];
      ir::eachName( instruction,
                    [&mentions]( std::string& name )
                    {
                      if ( baseOf( name ) == ir::RESULT )
                      {
                        ++mentions;
                      }
                    } );

      Write write{ .block = index, .at = at, .from = {} };
      bool isWrite = false;
      if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation );
           store != nullptr && store->name == ir::RESULT )
      {
        isWrite = true;
        if ( auto const* const object = std::get_if<ir::Object>( &store->value ); object != nullptr )
        {
          write.from = object->name;
        }
      }
      else if ( auto const* const copy = std::get_if<ir::Copy>( &instruction.operation );
                copy != nullptr && copy->to.name == ir::RESULT && !copy->to.pointer.has_value() &&
                copy->to.offset == 0 )
      {
        isWrite = true;
        if ( !copy->from.pointer.has_value() && copy->from.offset == 0 )
        {
          write.from = copy->from.name;
        }
      }
      if ( !isWrite )
      {
        continue;
      }

      // The write must be the last instruction of a block that returns. Every
      // one the lowering writes is — `return x;` stores and leaves — and a
      // write the function outlives would have to be looked at much harder.
      if ( at + 1 != block.instructions.size() || block.terminator.kind != ir::TerminatorKind::RETURN )
      {
        return std::nullopt;
      }
      writes.push_back( std::move( write ) );
    }
  }

  // Nothing else may name the result: every mention of it is one of the
  // writes, so the function reads nothing of what it is about to give away.
  if ( mentions != writes.size() )
  {
    return std::nullopt;
  }
  return writes;
}

/// Whether the function hands its bytes to Procs of its own, which name them
/// through it — `f.__0i` — and which this pass does not see.
bool hasSwitch( ir::Function const& function )
{
  for ( ir::Block const& block : function.blocks )
  {
    for ( ir::Instruction const& instruction : block.instructions )
    {
      if ( std::holds_alternative<ir::Switch>( instruction.operation ) )
      {
        return true;
      }
    }
  }
  return false;
}

/// Every name of the object `chosen` in the function becomes the result's,
/// or the other way about, and what were copies between the two become the
/// one byte written from itself and are not written at all.
void renameInto( ir::Function& function, std::string const& from, std::string_view to )
{
  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      ir::eachName( instruction,
                    [&from, to]( std::string& name )
                    {
                      if ( baseOf( name ) == from )
                      {
                        name.replace( 0, from.size(), to );
                      }
                    } );
    }
    std::erase_if( block.instructions,
                   [to]( ir::Instruction const& instruction )
                   {
                     if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation ) )
                     {
                       auto const* const object = std::get_if<ir::Object>( &store->value );
                       return store->name == to && object != nullptr && object->name == to;
                     }
                     if ( auto const* const copy = std::get_if<ir::Copy>( &instruction.operation ) )
                     {
                       return copy->from.name == to && copy->to.name == to && !copy->from.pointer.has_value() &&
                              !copy->to.pointer.has_value() && copy->from.offset == copy->to.offset;
                     }
                     return false;
                   } );
  }
}

/// The one of `candidates` copied into the result on the most ways out, and of
/// those the one written first, so that the choice does not turn on the order
/// the blocks happen to stand in. Empty where none is copied into it at all,
/// which is where there is nothing to save.
std::string chosenOf( std::vector<std::string> const& candidates, std::vector<Write> const& writes )
{
  std::string chosen;
  std::size_t best = 0;
  for ( std::string const& candidate : candidates )
  {
    std::size_t copies = 0;
    for ( Write const& write : writes )
    {
      copies += static_cast<std::size_t>( write.from == candidate );
    }
    if ( copies > best )
    {
      best = copies;
      chosen = candidate;
    }
  }
  return chosen;
}

/// Whether a local or a parameter is of the result's own shape.
bool fitsResult( ir::Local const& local, ir::Type result, std::uint32_t resultBytes )
{
  return local.type == result && bytesOf( local.type, local.bytes ) == bytesOf( result, resultBytes );
}

void mergeIn( ir::Function& function )
{
  if ( !function.result.has_value() || hasSwitch( function ) )
  {
    return;
  }
  std::optional<std::vector<Write>> const writes = writesOfResult( function );
  if ( !writes.has_value() )
  {
    return;
  }

  // What the result could be given: a byte of the Proc's own of the result's
  // own shape, or a `struct` past four bytes, which is a Temporary Section of
  // the Proc rather than a byte of it — see
  // docs/decisions/0086-a-struct-by-value.md. Never one that is `static` or
  // has its address taken, which is no Temporary and outlives the call.
  std::uint32_t const wanted = bytesOf( *function.result, function.resultBytes );
  std::vector<std::string> candidates;
  for ( ir::Local const& local : function.locals )
  {
    if ( fitsResult( local, *function.result, function.resultBytes ) )
    {
      candidates.push_back( local.name );
    }
  }
  for ( ir::Global const& section : function.sections )
  {
    if ( section.isTemporary && *function.result == ir::Type::BLOCK && section.count.value_or( 0 ) == wanted &&
         section.elements.empty() && section.stripes.empty() && section.pane.empty() && section.implements.empty() )
    {
      candidates.push_back( section.name );
    }
  }

  std::string const chosen = chosenOf( candidates, *writes );
  if ( chosen.empty() )
  {
    return;
  }

  // What the copies became: the result written from itself, which is the
  // whole of what this step takes out.
  renameInto( function, chosen, ir::RESULT );

  std::erase_if( function.locals, [&chosen]( ir::Local const& local ) { return local.name == chosen; } );
  std::erase_if( function.sections, [&chosen]( ir::Global const& section ) { return section.name == chosen; } );
}

/// The parameter a function returns through, or empty where it returns
/// through `__ret` as before. Nothing is rewritten here: the caller of this
/// must know every function's answer before it may change any of them, since
/// a call written in one unit reads a byte chosen in another.
std::string returnsThrough( ir::Function& function )
{
  if ( !function.result.has_value() || function.parameters.empty() || function.isTrampoline || hasSwitch( function ) )
  {
    return {};
  }

  // A member's result is its function type's byte and its callers reach it
  // through the type, and a Proc that implements a Slot is called through a
  // Cell that another Phase may fill with another definition: neither can
  // promise where its answer lies. See docs/decisions/0065-handlers.md and
  // docs/decisions/0031-slots.md.
  if ( !function.memberOf.empty() || !function.implements.empty() )
  {
    return {};
  }

  std::optional<std::vector<Write>> const writes = writesOfResult( function );
  if ( !writes.has_value() )
  {
    return {};
  }
  std::vector<std::string> candidates;
  for ( ir::Local const& parameter : function.parameters )
  {
    if ( fitsResult( parameter, *function.result, function.resultBytes ) )
    {
      candidates.push_back( parameter.name );
    }
  }
  return chosenOf( candidates, *writes );
}

/// Where a call reads its result from, for the functions that moved theirs.
using Moved = std::map<std::string, std::string, std::less<>>;

/// The functions of one unit, by name: a `static` one is its unit's alone, so
/// a name is looked up in its own unit before the program.
void gather( ir::Unit& unit, Moved& everywhere, Moved& here )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    auto* const function = std::get_if<ir::Function>( &definition );
    if ( function == nullptr )
    {
      continue;
    }
    std::string const byte = returnsThrough( *function );
    if ( byte.empty() )
    {
      continue;
    }
    renameInto( *function, std::string{ ir::RESULT }, byte );
    function->resultByte = byte;
    ( function->isStatic ? here : everywhere ).emplace( function->name, function->name + "." + byte );
  }
}

/// What a name in `moved` says, or nothing: a `static` function is its own
/// unit's, so its unit is asked first.
std::string const* lookIn( Moved const& everywhere, Moved const& here, std::string const& name )
{
  if ( auto const own = here.find( name ); own != here.end() )
  {
    return &own->second;
  }
  if ( auto const all = everywhere.find( name ); all != everywhere.end() )
  {
    return &all->second;
  }
  return nullptr;
}

/// Every call of a function that moved its result told where to read it, and
/// every name of that function's old `__ret` — which is how the members of a
/// `struct` a call returns are reached — made the name of the byte it lies in
/// now. A call that already names a byte, one of an assembler's Proc or a
/// member's through its type, is left as it is.
void tell( ir::Unit& unit, Moved const& everywhere, Moved const& here )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    auto* const function = std::get_if<ir::Function>( &definition );
    if ( function == nullptr )
    {
      continue;
    }
    for ( ir::Block& block : function->blocks )
    {
      for ( ir::Instruction& instruction : block.instructions )
      {
        if ( auto* const call = std::get_if<ir::Call>( &instruction.operation );
             call != nullptr && call->returned.empty() )
        {
          if ( std::string const* const byte = lookIn( everywhere, here, call->name ) )
          {
            call->returned = *byte;
          }
        }
        ir::eachName( instruction,
                      [&everywhere, &here]( std::string& name )
                      {
                        std::string const base = baseOf( name );
                        std::size_t const dot = base.rfind( '.' );
                        if ( dot == std::string::npos || base.substr( dot + 1 ) != ir::RESULT )
                        {
                          return;
                        }
                        if ( std::string const* const byte = lookIn( everywhere, here, base.substr( 0, dot ) ) )
                        {
                          name.replace( 0, base.size(), *byte );
                        }
                      } );
      }
    }
  }
}

} // namespace

void returnThroughParameters( std::span<std::optional<ir::Unit>> units )
{
  // Decided first, over every unit, and told to the callers after: a call
  // written in one unit reads a byte chosen in another.
  Moved everywhere;
  std::vector<Moved> ownStatics( units.size() );
  std::size_t index = 0;
  for ( std::optional<ir::Unit>& unit : units )
  {
    if ( unit.has_value() )
    {
      gather( *unit, everywhere, ownStatics[index] );
    }
    ++index;
  }
  index = 0;
  for ( std::optional<ir::Unit>& unit : units )
  {
    if ( unit.has_value() )
    {
      tell( *unit, everywhere, ownStatics[index] );
    }
    ++index;
  }
}

void mergeReturnedLocals( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      mergeIn( *function );
    }
  }
}

} // namespace nga::c
