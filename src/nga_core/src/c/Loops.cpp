#include "nga/c/Loops.hpp"

#include <algorithm>
#include <optional>
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

std::string baseOf( std::string const& name )
{
  return name.substr( 0, name.find( '+' ) );
}

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

Written writtenIn( ir::Function const& function, Loop const& loop )
{
  Written written;
  for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
  {
    for ( ir::Instruction const& instruction : function.blocks[index].instructions )
    {
      std::visit(
          [&written]( auto const& operation )
          {
            using Operation = std::decay_t<decltype( operation )>;
            if constexpr ( std::is_same_v<Operation, ir::Store> || std::is_same_v<Operation, ir::StoreElement> )
            {
              written.objects.insert( baseOf( operation.name ) );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Copy> )
            {
              written.objects.insert( baseOf( operation.to.name ) );
              written.anything = written.anything || operation.to.pointer.has_value();
            }
            else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> || std::is_same_v<Operation, ir::Call> ||
                                std::is_same_v<Operation, ir::Switch> || std::is_same_v<Operation, ir::EnterWith> )
            {
              written.anything = true;
            }
          },
          instruction.operation );
    }
  }
  return written;
}

std::optional<std::uint32_t> onlyPredecessorOf( ir::Function const& function, std::uint32_t index )
{
  std::optional<std::uint32_t> found;
  for ( std::uint32_t other = 0; other < function.blocks.size(); ++other )
  {
    ir::Terminator const& terminator = function.blocks[other].terminator;
    bool const goes = ( terminator.kind == ir::TerminatorKind::JUMP && terminator.target == index ) ||
                      ( terminator.kind == ir::TerminatorKind::BRANCH &&
                        ( terminator.target == index || terminator.otherwise == index ) );
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
  std::string const base = baseOf( object->name );
  if ( written.objects.contains( base ) )
  {
    return false;
  }
  return !written.anything || own.contains( base );
}

} // namespace nga::c
