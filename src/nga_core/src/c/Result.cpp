#include "nga/c/Result.hpp"

#include <cstddef>
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
                      if ( ir::baseOf( name ) == ir::RESULT )
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

/// Every name of the object `chosen` in the function becomes the result's,
/// or the other way about, and what were copies between the two become the
/// one byte written from itself and are not written at all.
void renameInto( ir::Function& function, std::string const& from, std::string_view to )
{
  auto const rename = [&from, to]( std::string& name )
  {
    if ( ir::baseOf( name ) == from )
    {
      name.replace( 0, from.size(), to );
    }
  };
  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      ir::eachName( instruction, rename );
    }
    ir::eachName( block.terminator.condition, rename );
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
  if ( !function.result.has_value() )
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
  if ( !function.result.has_value() || function.parameters.empty() || function.isTrampoline )
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
                        std::string const base = ir::baseOf( name );
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

/// Every function of the program by the name a call writes it with, a
/// `static` one under its own unit, which is asked first.
struct Functions
{
  std::map<std::string, ir::Function*, std::less<>> everywhere;
  std::vector<std::map<std::string, ir::Function*, std::less<>>> own;

  [[nodiscard]] ir::Function* find( std::size_t unit, std::string const& name ) const
  {
    if ( auto const found = own[unit].find( name ); found != own[unit].end() )
    {
      return found->second;
    }
    auto const found = everywhere.find( name );
    return found != everywhere.end() ? found->second : nullptr;
  }
};

/// Whether nothing a function does after it writes its result can write
/// another function's: every name of the result stands in a block that
/// returns, after the last call in it. So the result may lie in a byte some
/// call writes, as long as that call is not made in between.
bool writesResultLast( ir::Function& function )
{
  for ( ir::Block& block : function.blocks )
  {
    std::optional<std::size_t> lastCall;
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      if ( std::holds_alternative<ir::Call>( block.instructions[at].operation ) )
      {
        lastCall = at;
      }
    }
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      bool names = false;
      ir::eachName( block.instructions[at],
                    [&names]( std::string const& name ) { names = names || ir::baseOf( name ) == ir::RESULT; } );
      if ( names &&
           ( block.terminator.kind != ir::TerminatorKind::RETURN || ( lastCall.has_value() && *lastCall > at ) ) )
      {
        return false;
      }
    }
  }
  return true;
}

/// Whether a function's result may be another's, or another's its: a scalar
/// in memory every caller reaches by the function's own name.
bool sharable( ir::Function const& function )
{
  return function.result.has_value() && *function.result != ir::Type::BLOCK && function.memberOf.empty() &&
         function.implements.empty() && !function.isTrampoline && function.pane.empty() && function.under.empty() &&
         function.withs.empty() && function.then.empty();
}

/// The function whose result this one's may be: the callee of the most
/// `return g(...)`, the call's result stored to the function's and read by
/// nothing else, of the same type — see
/// docs/decisions/0278-a-result-may-be-another-procs-result.md.
ir::Function* sharedWith( ir::Function& function, std::size_t unit, Functions const& functions )
{
  if ( !sharable( function ) || !function.resultByte.empty() || !writesResultLast( function ) )
  {
    return nullptr;
  }
  std::map<ir::Function*, std::uint32_t> returned;
  for ( ir::Block const& block : function.blocks )
  {
    for ( std::size_t at = 0; at + 1 < block.instructions.size(); ++at )
    {
      auto const* const call = std::get_if<ir::Call>( &block.instructions[at].operation );
      auto const* const store = std::get_if<ir::Store>( &block.instructions[at + 1].operation );
      auto const* const value = store != nullptr ? std::get_if<ir::Value>( &store->value ) : nullptr;
      if ( call == nullptr || !call->result.has_value() || value == nullptr || value->index != call->result->index ||
           store->name != ir::RESULT || ir::readersOf( function, *call->result ) != 1 )
      {
        continue;
      }
      ir::Function* const callee = functions.find( unit, call->name );
      if ( callee != nullptr && callee != &function && sharable( *callee ) && callee->result == function.result )
      {
        ++returned[callee];
      }
    }
  }
  ir::Function* chosen = nullptr;
  std::uint32_t most = 0;
  for ( auto const& [callee, count] : returned )
  {
    if ( count > most || ( count == most && chosen != nullptr && callee->name < chosen->name ) )
    {
      chosen = callee;
      most = count;
    }
  }
  return chosen;
}

/// Every function whose result is another's told so, its stores to `__ret`
/// made stores to that byte, and the store after the call it shares with
/// dropped: the call has left the value there. The callers learn where to
/// read from `tell`, through `everywhere` and `here`.
void share( std::span<std::optional<ir::Unit>> units, Moved& everywhere, std::vector<Moved>& ownStatics )
{
  Functions functions;
  functions.own.resize( units.size() );
  for ( std::size_t unit = 0; unit < units.size(); ++unit )
  {
    std::optional<ir::Unit>& one = units[unit];
    if ( !one.has_value() )
    {
      continue;
    }
    for ( ir::Definition& definition : one->definitions )
    {
      if ( auto* const function = std::get_if<ir::Function>( &definition );
           function != nullptr && !function->blocks.empty() )
      {
        ( function->isStatic ? functions.own[unit] : functions.everywhere ).emplace( function->name, function );
      }
    }
  }

  // Decided over the bodies as they are, and only then resolved: the
  // function shared with may share with another in turn, and the byte is
  // the last one's. There is no cycle, since nothing calls itself.
  std::vector<std::pair<std::size_t, ir::Function*>> sharing;
  std::map<ir::Function const*, ir::Function*> with;
  for ( std::size_t unit = 0; unit < units.size(); ++unit )
  {
    std::optional<ir::Unit>& one = units[unit];
    if ( !one.has_value() )
    {
      continue;
    }
    for ( ir::Definition& definition : one->definitions )
    {
      auto* const function = std::get_if<ir::Function>( &definition );
      if ( function == nullptr )
      {
        continue;
      }
      if ( ir::Function* const callee = sharedWith( *function, unit, functions ) )
      {
        sharing.emplace_back( unit, function );
        with[function] = callee;
      }
    }
  }
  auto const last = [&with]( ir::Function* function )
  {
    while ( with.contains( function ) )
    {
      function = with.at( function );
    }
    return function;
  };

  for ( auto const& [unit, function] : sharing )
  {
    ir::Function* const target = last( function );
    target->resultShared = true;
    std::string const byte =
        target->name + "." + ( target->resultByte.empty() ? std::string{ ir::RESULT } : target->resultByte );
    function->resultElsewhere = byte;
    ( function->isStatic ? ownStatics[unit] : everywhere ).insert_or_assign( function->name, byte );

    ir::Function* const callee = with.at( function );
    for ( ir::Block& block : function->blocks )
    {
      for ( std::size_t at = 0; at + 1 < block.instructions.size(); ++at )
      {
        auto* const call = std::get_if<ir::Call>( &block.instructions[at].operation );
        auto const* const store = std::get_if<ir::Store>( &block.instructions[at + 1].operation );
        auto const* const value = store != nullptr ? std::get_if<ir::Value>( &store->value ) : nullptr;
        if ( call != nullptr && call->name == callee->name && call->result.has_value() && value != nullptr &&
             value->index == call->result->index && store->name == ir::RESULT )
        {
          call->result.reset();
          block.instructions.erase( block.instructions.begin() + static_cast<std::ptrdiff_t>( at + 1 ) );
        }
      }
    }
    renameInto( *function, std::string{ ir::RESULT }, byte );
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
  share( units, everywhere, ownStatics );
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
