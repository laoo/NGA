#include "nga/c/Loops.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>

namespace nga::c
{

namespace
{

/// Whether a block's terminator goes to `target`.
bool goesTo( ir::Terminator const& terminator, std::uint32_t target )
{
  switch ( terminator.kind )
  {
  case ir::TerminatorKind::JUMP:
    return terminator.target == target;
  case ir::TerminatorKind::BRANCH:
    return terminator.target == target || terminator.otherwise == target;
  case ir::TerminatorKind::DISPATCH:
    return std::ranges::find( terminator.targets, target ) != terminator.targets.end();
  case ir::TerminatorKind::RETURN:
  case ir::TerminatorKind::TRANSITION:
  case ir::TerminatorKind::FALL:
    return false;
  }
  return false;
}

} // namespace

std::vector<Loop> loopsOf( ir::Function const& function )
{
  std::vector<Loop> loops;
  for ( std::uint32_t last = 0; last < function.blocks.size(); ++last )
  {
    ir::Terminator const& terminator = function.blocks[last].terminator;
    std::optional<std::uint32_t> header;
    if ( terminator.kind == ir::TerminatorKind::JUMP && terminator.target <= last )
    {
      header = terminator.target;
    }
    else if ( terminator.kind == ir::TerminatorKind::BRANCH )
    {
      if ( terminator.target <= last )
      {
        header = terminator.target;
      }
      else if ( terminator.otherwise <= last )
      {
        header = terminator.otherwise;
      }
    }
    if ( !header.has_value() || *header == 0 )
    {
      continue;
    }
    ir::Block const& entry = function.blocks[*header - 1];
    if ( entry.terminator.kind != ir::TerminatorKind::JUMP || entry.terminator.target != *header )
    {
      continue;
    }
    bool enteredElsewhere = false;
    for ( std::uint32_t other = 0; other < function.blocks.size(); ++other )
    {
      bool const outside = other < *header - 1 || other > last;
      enteredElsewhere = enteredElsewhere || ( outside && goesTo( function.blocks[other].terminator, *header ) );
    }
    if ( !enteredElsewhere )
    {
      loops.push_back( Loop{ .header = *header, .last = last, .entry = *header - 1 } );
    }
  }
  // Outermost first: an inner loop's header comes after its outer's, and what
  // is invariant in the outer one goes straight to the outer entry, once.
  std::ranges::sort( loops, []( Loop const& a, Loop const& b ) { return a.header < b.header; } );
  return loops;
}

bool isArithmetic( ir::Call const& call )
{
  static constexpr std::array<std::string_view, 11> ARITHMETIC{
    "__mul8",   "__mul16", "__mul8to16", "__udiv8", "__udiv16", "__umod8",
    "__umod16", "__sdiv8", "__sdiv16",   "__smod8", "__smod16",
  };
  return std::ranges::find( ARITHMETIC, call.name ) != ARITHMETIC.end();
}

namespace
{

/// What one instruction writes, added to what is known: a call of a function
/// `effects` summarises adds that function's writes, and without `effects` a
/// store through a pointer is taken to write anything.
void noteWrites( ir::Instruction const& instruction, Writes& writes, Effects const* effects )
{
  std::visit(
      [&writes, effects]( auto const& operation )
      {
        using Operation = std::decay_t<decltype( operation )>;
        auto const pointer = [&writes, effects]
        {
          writes.throughPointer = true;
          writes.anything = writes.anything || effects == nullptr;
        };
        if constexpr ( std::is_same_v<Operation, ir::Store> || std::is_same_v<Operation, ir::StoreElement> )
        {
          writes.objects.insert( ir::baseOf( operation.name ) );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Copy> )
        {
          if ( !operation.to.name.empty() )
          {
            writes.objects.insert( ir::baseOf( operation.to.name ) );
          }
          if ( operation.to.pointer.has_value() )
          {
            pointer();
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> )
        {
          pointer();
        }
        else if constexpr ( std::is_same_v<Operation, ir::Call> )
        {
          if ( isArithmetic( operation ) )
          {
            return;
          }
          if ( effects == nullptr )
          {
            writes.anything = true;
            return;
          }
          auto const found = effects->functions.find( operation.name );
          if ( found == effects->functions.end() )
          {
            writes.anything = true;
            return;
          }
          writes.objects.insert( found->second.objects.begin(), found->second.objects.end() );
          writes.throughPointer = writes.throughPointer || found->second.throughPointer;
          writes.anything = writes.anything || found->second.anything;
        }
        else if constexpr ( std::is_same_v<Operation, ir::EnterWith> )
        {
          writes.anything = true;
        }
      },
      instruction.operation );
}

} // namespace

Written writtenIn( ir::Function const& function, Loop const& loop, Effects const* effects )
{
  Writes writes;
  for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
  {
    for ( ir::Instruction const& instruction : function.blocks[index].instructions )
    {
      noteWrites( instruction, writes, effects );
    }
  }
  return Written{ .objects = std::move( writes.objects ),
                  .anything = writes.anything,
                  .throughPointer = writes.throughPointer,
                  .addressed = effects != nullptr ? &effects->addressed : nullptr };
}

void addressesIn( ir::Unit const& unit, std::set<std::string>& taken )
{
  auto const note = [&taken]( ir::Constant const& constant )
  {
    if ( !constant.name.empty() )
    {
      taken.insert( ir::baseOf( constant.name ) );
    }
  };
  for ( ir::Definition const& definition : unit.definitions )
  {
    if ( auto const* const object = std::get_if<ir::Global>( &definition ) )
    {
      if ( object->value.has_value() )
      {
        note( *object->value );
      }
      for ( ir::Constant const& element : object->elements )
      {
        note( element );
      }
      for ( auto const& stripe : object->stripes )
      {
        for ( ir::Constant const& element : stripe.second )
        {
          note( element );
        }
      }
    }
    else if ( auto const* const named = std::get_if<ir::NamedConstant>( &definition ) )
    {
      note( named->value );
    }
    else if ( auto const* const function = std::get_if<ir::Function>( &definition ) )
    {
      for ( ir::Global const& section : function->sections )
      {
        for ( ir::Constant const& element : section.elements )
        {
          note( element );
        }
      }
      for ( ir::Block const& block : function->blocks )
      {
        for ( ir::Instruction const& instruction : block.instructions )
        {
          ir::eachOperand( instruction,
                           [&note]( ir::Operand const& operand )
                           {
                             if ( auto const* const given = std::get_if<ir::Constant>( &operand ) )
                             {
                               note( *given );
                             }
                           } );
        }
        if ( auto const* const condition = std::get_if<ir::Constant>( &block.terminator.condition ) )
        {
          note( *condition );
        }
      }
    }
  }
}

Effects effectsOf( std::span<std::optional<ir::Unit> const> units )
{
  Effects effects;
  // Per function, by name, what its own instructions write and whom it
  // calls; two `static` functions of one name in two files are taken
  // together, which errs towards writing more.
  std::map<std::string, std::set<std::string>, std::less<>> callees;
  for ( std::optional<ir::Unit> const& each : units )
  {
    if ( !each.has_value() )
    {
      continue;
    }
    addressesIn( *each, effects.addressed );
    for ( ir::Definition const& definition : each->definitions )
    {
      auto const* const function = std::get_if<ir::Function>( &definition );
      if ( function == nullptr )
      {
        continue;
      }
      Writes& writes = effects.functions[function->name];
      std::set<std::string> const own = ownBytesOf( *function );
      for ( ir::Block const& block : function->blocks )
      {
        for ( ir::Instruction const& instruction : block.instructions )
        {
          if ( auto const* const call = std::get_if<ir::Call>( &instruction.operation );
               call != nullptr && !isArithmetic( *call ) )
          {
            callees[function->name].insert( call->name );
            continue;
          }
          Writes mine;
          noteWrites( instruction, mine, &effects );
          for ( std::string const& object : mine.objects )
          {
            if ( !own.contains( object ) )
            {
              writes.objects.insert( object );
            }
          }
          writes.throughPointer = writes.throughPointer || mine.throughPointer;
          writes.anything = writes.anything || mine.anything;
        }
      }
    }
  }
  // What a callee writes, its caller writes, to a fixed point; a callee
  // nothing here summarises writes anything.
  for ( bool changed = true; changed; )
  {
    changed = false;
    for ( auto const& [caller, called] : callees )
    {
      Writes& writes = effects.functions[caller];
      for ( std::string const& callee : called )
      {
        auto const found = effects.functions.find( callee );
        Writes const from = found != effects.functions.end()
                                ? found->second
                                : Writes{ .objects = {}, .throughPointer = false, .anything = true };
        std::size_t const before = writes.objects.size();
        writes.objects.insert( from.objects.begin(), from.objects.end() );
        bool const pointer = writes.throughPointer || from.throughPointer;
        bool const anything = writes.anything || from.anything;
        changed = changed || writes.objects.size() != before || pointer != writes.throughPointer ||
                  anything != writes.anything;
        writes.throughPointer = pointer;
        writes.anything = anything;
      }
    }
  }
  return effects;
}

std::optional<std::uint32_t> onlyPredecessorOf( ir::Function const& function, std::uint32_t index )
{
  std::optional<std::uint32_t> found;
  for ( std::uint32_t other = 0; other < function.blocks.size(); ++other )
  {
    bool const goes = goesTo( function.blocks[other].terminator, index );
    if ( goes && found.has_value() )
    {
      return std::nullopt;
    }
    found = goes ? std::optional{ other } : found;
  }
  return found;
}

std::set<std::string> ownBytesOf( ir::Function const& function )
{
  std::set<std::string> own;
  for ( ir::Local const& local : function.locals )
  {
    own.insert( local.name );
  }
  for ( ir::Local const& parameter : function.parameters )
  {
    own.insert( parameter.name );
  }
  return own;
}

bool invariant( ir::Operand const& operand, Written const& written, std::set<std::string> const& own )
{
  if ( std::holds_alternative<ir::Constant>( operand ) )
  {
    return true;
  }
  auto const* const object = std::get_if<ir::Object>( &operand );
  if ( object == nullptr )
  {
    // A value is defined in its own block, which is in the loop: what it
    // reads was looked at before it, and where that was invariant the value
    // is a byte of the Proc's own by now.
    return false;
  }
  std::string const base = ir::baseOf( object->name );
  if ( written.throughPointer && !written.anything && written.addressed != nullptr && !own.contains( base ) &&
       written.addressed->contains( base ) )
  {
    return false;
  }
  if ( written.objects.contains( base ) )
  {
    return false;
  }
  return !written.anything || own.contains( base );
}

} // namespace nga::c
