#include "nga/c/Promote.hpp"

#include "nga/c/Loops.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nga::c
{

namespace
{

/// Whether nothing in the loop calls, enters a `[[with]]` block, or leaves
/// otherwise than by a branch, a jump or a return.
bool callsNothing( ir::Function const& function, Loop const& loop )
{
  for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
  {
    ir::Block const& block = function.blocks[index];
    for ( ir::Instruction const& instruction : block.instructions )
    {
      if ( std::holds_alternative<ir::Call>( instruction.operation ) ||
           std::holds_alternative<ir::EnterWith>( instruction.operation ) )
      {
        return false;
      }
    }
    if ( block.terminator.kind == ir::TerminatorKind::FALL )
    {
      return false;
    }
  }
  return true;
}

/// How many bytes `fit` lets a loop grow the text by to hold one object, where
/// the loop saves many cycles for them, and how many times over what the
/// copies cost each time it is entered a loop of known turns must save. The
/// tool's numbers, as 0177 has a word come to — see
/// docs/decisions/0303-fit-holds-an-object-where-the-cycles-pay-for-the-bytes.md.
constexpr std::size_t FIT_ALLOWANCE = 24;
constexpr std::uint64_t FIT_RETURN = 4;

/// What a byte copied in or out costs: a load of the object and a store to the
/// zero page, or the other way round.
constexpr std::uint64_t COPY_CYCLES = 7;

/// More turns than any loop of a 6502 makes, where a product of them stops.
constexpr std::uint64_t MANY = std::uint64_t{ 1 } << 40;

/// The number a constant is, where the compiler knows it.
std::optional<std::int64_t> numberOf( ir::Operand const& operand )
{
  auto const* const constant = std::get_if<ir::Constant>( &operand );
  if ( constant == nullptr || ( !constant->name.empty() && !constant->valued ) )
  {
    return std::nullopt;
  }
  return constant->value;
}

/// How many times the loop turns each time it is entered, where the lowering's
/// counted shape says so: the header tests `k < b` of a counter against a
/// number, the block the loop is entered from sets the counter to a number
/// last of what it writes, and the one store to it inside the loop is the step
/// of one in its last block.
std::optional<std::uint64_t> turnsOf( ir::Function const& function, Loop const& loop )
{
  ir::Block const& header = function.blocks[loop.header];
  if ( header.instructions.size() != 1 || header.terminator.kind != ir::TerminatorKind::BRANCH ||
       header.terminator.target != loop.header + 1 )
  {
    return std::nullopt;
  }
  auto const* const compare = std::get_if<ir::Compare>( &header.instructions[0].operation );
  auto const* const condition = std::get_if<ir::Value>( &header.terminator.condition );
  if ( compare == nullptr || condition == nullptr || condition->index != compare->result.index ||
       compare->op != ir::Comparison::LESS )
  {
    return std::nullopt;
  }
  auto const* const counted = std::get_if<ir::Object>( &compare->left );
  std::optional<std::int64_t> const bound = numberOf( compare->right );
  if ( counted == nullptr || !bound.has_value() )
  {
    return std::nullopt;
  }
  std::string const counter = ir::baseOf( counted->name );

  std::optional<std::int64_t> start;
  ir::Block const& entry = function.blocks[loop.entry];
  for ( std::size_t at = entry.instructions.size(); at-- > 0; )
  {
    ir::Instruction const& instruction = entry.instructions[at];
    if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation ) )
    {
      if ( ir::baseOf( store->name ) == counter )
      {
        if ( store->name == counted->name )
        {
          start = numberOf( store->value );
        }
        break;
      }
      continue;
    }
    if ( !std::holds_alternative<ir::Unary>( instruction.operation ) &&
         !std::holds_alternative<ir::Binary>( instruction.operation ) &&
         !std::holds_alternative<ir::Compare>( instruction.operation ) &&
         !std::holds_alternative<ir::Convert>( instruction.operation ) &&
         !std::holds_alternative<ir::Load>( instruction.operation ) &&
         !std::holds_alternative<ir::LoadIndirect>( instruction.operation ) )
    {
      // What may write the counter without naming it.
      break;
    }
  }
  if ( !start.has_value() )
  {
    return std::nullopt;
  }

  std::uint32_t stores = 0;
  bool stepped = false;
  for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
  {
    ir::Block const& block = function.blocks[index];
    for ( ir::Instruction const& instruction : block.instructions )
    {
      auto const* const store = std::get_if<ir::Store>( &instruction.operation );
      if ( store == nullptr || ir::baseOf( store->name ) != counter )
      {
        continue;
      }
      ++stores;
      auto const* const value = std::get_if<ir::Value>( &store->value );
      std::optional<std::size_t> const at =
          value != nullptr && index == loop.last ? ir::definerIn( block, *value ) : std::nullopt;
      auto const* const add = at.has_value() ? std::get_if<ir::Binary>( &block.instructions[*at].operation ) : nullptr;
      auto const* const from = add != nullptr ? std::get_if<ir::Object>( &add->left ) : nullptr;
      stepped = store->name == counted->name && add != nullptr && add->op == ir::BinaryOperator::ADD &&
                from != nullptr && from->name == counted->name && numberOf( add->right ) == std::int64_t{ 1 };
    }
  }
  if ( stores != 1 || !stepped )
  {
    return std::nullopt;
  }
  return *bound > *start ? static_cast<std::uint64_t>( *bound - *start ) : 0;
}

/// Every stretch of blocks a jump back closes, from the block it goes back to
/// through the one it goes back from: a loop whatever shape it has, which is
/// what a question of how often something runs wants.
std::vector<std::pair<std::uint32_t, std::uint32_t>> cyclesOf( ir::Function const& function )
{
  std::vector<std::pair<std::uint32_t, std::uint32_t>> cycles;
  for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
  {
    for ( std::uint32_t const to : ir::successorsOf( function.blocks[index].terminator ) )
    {
      if ( to <= index )
      {
        cycles.emplace_back( to, index );
      }
    }
  }
  return cycles;
}

/// Whether the loop saves many cycles for the object each time it is entered,
/// and is entered seldom enough that the bytes are spent once for many turns.
/// Where the turns of the loop and of every loop inside it that names the
/// object are known, each access is counted as often as it runs, and the sum
/// must be several times what the copies cost. Where they are not, the loop
/// must be the function's outermost, in a function no loop calls.
bool savesMany( ir::Function& function,
                std::vector<Loop> const& loops,
                Loop const& loop,
                std::string const& global,
                std::uint64_t copies,
                std::set<std::string> const& often )
{
  // A loop every way through which leaves, `break` on both sides of an `if`,
  // runs once and turns no more however often it is entered.
  std::vector<bool> reached( function.blocks.size(), false );
  std::vector<std::uint32_t> pending{ loop.header };
  reached[loop.header] = true;
  bool goesBack = false;
  while ( !pending.empty() )
  {
    std::uint32_t const index = pending.back();
    pending.pop_back();
    for ( std::uint32_t const to : ir::successorsOf( function.blocks[index].terminator ) )
    {
      goesBack = goesBack || to == loop.header;
      if ( to > loop.header && to <= loop.last && !reached[to] )
      {
        reached[to] = true;
        pending.push_back( to );
      }
    }
  }
  if ( !goesBack )
  {
    return false;
  }

  std::uint64_t accesses = 0;
  bool known = true;
  for ( std::uint32_t index = loop.header; index <= loop.last && known; ++index )
  {
    ir::Block& block = function.blocks[index];
    std::uint64_t named = 0;
    auto const count = [&named, &global]( auto& holder )
    {
      bool names = false;
      ir::eachName( holder,
                    [&names, &global]( std::string const& name ) { names = names || ir::baseOf( name ) == global; } );
      named += names ? 1 : 0;
    };
    for ( ir::Instruction& instruction : block.instructions )
    {
      count( instruction );
    }
    count( block.terminator.condition );
    if ( named == 0 )
    {
      continue;
    }
    std::uint64_t runs = 1;
    for ( Loop const& around : loops )
    {
      if ( around.header < loop.header || around.last > loop.last || index < around.header || index > around.last )
      {
        continue;
      }
      std::optional<std::uint64_t> const turns = turnsOf( function, around );
      if ( !turns.has_value() )
      {
        known = false;
        break;
      }
      runs = std::min( runs * std::max<std::uint64_t>( *turns, 1 ), MANY );
    }
    accesses = std::min( accesses + ( named * runs ), MANY );
  }
  if ( known )
  {
    return accesses >= FIT_RETURN * copies;
  }

  if ( often.contains( function.name ) )
  {
    return false;
  }
  return std::ranges::none_of( cyclesOf( function ),
                               [&loop]( auto const& cycle )
                               {
                                 return cycle.first <= loop.header && loop.last <= cycle.second &&
                                        ( cycle.first != loop.header || cycle.second != loop.last );
                               } );
}

void promoteIn( ir::Function& function,
                Promotable const& promotable,
                std::set<std::string> const& often,
                Intent intent )
{
  if ( !function.withs.empty() || !function.pane.empty() || !function.under.empty() )
  {
    return;
  }

  // A name the function gives a byte, a `static` local or a `const` of its
  // own is that one and not the object at file scope it shadows: a parameter
  // `u16 n` beside a global `u8 n` is the parameter wherever the body says
  // `n`.
  std::set<std::string> shadowed = ownBytesOf( function );
  for ( ir::Global const& section : function.sections )
  {
    shadowed.insert( section.name );
  }
  for ( ir::NamedConstant const& constant : function.constants )
  {
    shadowed.insert( constant.name );
  }
  auto const holds = [&promotable, &shadowed]( std::string const& base )
  { return promotable.contains( base ) && !shadowed.contains( base ); };

  // Where each block is entered from.
  std::vector<std::vector<std::uint32_t>> predecessors( function.blocks.size() );
  for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
  {
    for ( std::uint32_t const to : ir::successorsOf( function.blocks[index].terminator ) )
    {
      predecessors[to].push_back( index );
    }
  }

  std::vector<Loop> const loops = loopsOf( function );

  // Whether a block runs again and again in the innermost loop that holds it:
  // it goes on to a block of that loop rather than only out of it. A block that
  // returns, or only leaves, runs once for the loop's run, and what it names
  // is not worth a byte.
  auto const repeats = [&function]( std::uint32_t block, Loop const& innermost )
  {
    return std::ranges::any_of( ir::successorsOf( function.blocks[block].terminator ),
                                [&innermost]( std::uint32_t to )
                                { return to >= innermost.header && to <= innermost.last; } );
  };

  // What a loop declined for its bytes, which no loop inside it takes either:
  // an inner loop is entered at every turn of the outer one, and pays the
  // copies at each, where its own few turns may not pay them back.
  std::vector<std::pair<Loop const*, std::string>> declined;

  std::uint32_t made = 0;
  for ( Loop const& loop : loops )
  {
    if ( !callsNothing( function, loop ) )
    {
      continue;
    }
    auto const inside = [&loop]( std::uint32_t block ) { return block >= loop.header && block <= loop.last; };

    // The loops nested in this one, and per block the innermost of them, or
    // this one, that holds it. An object pays its byte where some block that
    // repeats names it in a loop nested in this one, where it is read many
    // times for every time this loop is entered, or where this loop is the
    // innermost and repeats it itself.
    std::vector<Loop const*> innermost( function.blocks.size(), &loop );
    bool nests = false;
    for ( Loop const& other : loops )
    {
      if ( &other != &loop && other.header >= loop.header && other.last <= loop.last )
      {
        nests = true;
        for ( std::uint32_t index = other.header; index <= other.last; ++index )
        {
          if ( innermost[index]->last - innermost[index]->header >= other.last - other.header )
          {
            innermost[index] = &other;
          }
        }
      }
    }

    // The ways out: the blocks they lead to, each entered from the loop
    // alone, so that what the loop held goes back at its start; and the
    // returns and transitions, before which it goes back where they stand.
    std::set<std::uint32_t> exits;
    std::vector<std::uint32_t> leaving;
    bool closed = true;
    for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
    {
      ir::Terminator const& end = function.blocks[index].terminator;
      if ( end.kind == ir::TerminatorKind::RETURN || end.kind == ir::TerminatorKind::TRANSITION )
      {
        leaving.push_back( index );
      }
      for ( std::uint32_t const to : ir::successorsOf( end ) )
      {
        if ( inside( to ) )
        {
          continue;
        }
        exits.insert( to );
        for ( std::uint32_t const from : predecessors[to] )
        {
          closed = closed && inside( from );
        }
      }
    }
    if ( !closed )
    {
      continue;
    }

    // What the loop names of each object, whether it writes it, and whether a
    // block that repeats names it where that pays.
    std::map<std::string, bool> named;
    std::set<std::string> pays;
    for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
    {
      ir::Block& block = function.blocks[index];
      bool const counts = ( innermost[index] != &loop || !nests ) && repeats( index, *innermost[index] );
      for ( ir::Instruction& instruction : block.instructions )
      {
        ir::eachName( instruction,
                      [&named, &pays, &holds, counts]( std::string const& name )
                      {
                        if ( holds( ir::baseOf( name ) ) )
                        {
                          named.try_emplace( ir::baseOf( name ), false );
                          if ( counts )
                          {
                            pays.insert( ir::baseOf( name ) );
                          }
                        }
                      } );
        if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation );
             store != nullptr && holds( ir::baseOf( store->name ) ) )
        {
          named[ir::baseOf( store->name )] = true;
        }
      }
      ir::eachName( block.terminator.condition,
                    [&named, &holds]( std::string const& name )
                    {
                      if ( holds( ir::baseOf( name ) ) )
                      {
                        named.try_emplace( ir::baseOf( name ), false );
                      }
                    } );
    }

    for ( auto const& [global, written] : named )
    {
      bool const inDeclined = std::ranges::any_of(
          declined,
          [&loop, &global]( auto const& entry )
          { return entry.second == global && entry.first->header <= loop.header && loop.last <= entry.first->last; } );
      if ( !pays.contains( global ) || inDeclined )
      {
        continue;
      }

      // The bytes it costs: a load and a store a byte in, and as many out on
      // every way out where the loop writes it; against what it saves, which
      // is counted low, since the emitter reads less than the IR names: every
      // instruction that names it reads or writes it at least once, and each
      // such access is a byte shorter on the zero page.
      std::uint32_t const size = ir::sizeOf( promotable.at( global ) );
      std::size_t const ways = written ? exits.size() + leaving.size() : 0;
      std::size_t const cost = std::size_t{ 5 } * size * ( 1 + ways );
      std::size_t saved = 0;
      for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
      {
        ir::Block& block = function.blocks[index];
        for ( ir::Instruction& instruction : block.instructions )
        {
          bool names = false;
          ir::eachName( instruction,
                        [&names, &global]( std::string const& name )
                        { names = names || ir::baseOf( name ) == global; } );
          saved += names ? 1 : 0;
        }
        bool names = false;
        ir::eachName( block.terminator.condition,
                      [&names, &global]( std::string const& name ) { names = names || ir::baseOf( name ) == global; } );
        saved += names ? 1 : 0;
      }
      // Where the copies cost more bytes than the accesses save: `speed`
      // holds it all the same, `size` never, and `fit` for a few bytes where
      // the loop saves many cycles for them.
      bool const takes = saved >= cost || intent == Intent::SPEED ||
                         ( intent == Intent::FIT && cost - saved <= FIT_ALLOWANCE &&
                           savesMany( function, loops, loop, global, COPY_CYCLES * size * ( 1 + ways ), often ) );
      if ( !takes )
      {
        declined.emplace_back( &loop, global );
        continue;
      }
      std::optional<ir::Type> const type = promotable.at( global );
      std::string held = "__g" + std::to_string( made++ );
      function.locals.push_back(
          ir::Local{ .name = held, .type = *type, .bytes = 0, .place = {}, .placement = ir::Placement::ZEROPAGE } );
      auto const rename = [&global, &held]( std::string& name )
      {
        if ( ir::baseOf( name ) == global )
        {
          name.replace( 0, global.size(), held );
        }
      };
      for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
      {
        ir::Block& block = function.blocks[index];
        for ( ir::Instruction& instruction : block.instructions )
        {
          ir::eachName( instruction, rename );
        }
        ir::eachName( block.terminator.condition, rename );
      }

      ir::Block& entry = function.blocks[loop.entry];
      diag::SourceLocation const at = entry.terminator.at;
      entry.instructions.push_back( ir::Instruction{
          .operation = ir::Store{ .name = held, .type = *type, .value = ir::Object{ .name = global, .type = *type } },
          .at = at } );
      if ( !written )
      {
        continue;
      }
      ir::Instruction const back{
        .operation = ir::Store{ .name = global, .type = *type, .value = ir::Object{ .name = held, .type = *type } },
        .at = at
      };
      for ( std::uint32_t const exit : exits )
      {
        std::vector<ir::Instruction>& instructions = function.blocks[exit].instructions;
        instructions.insert( instructions.begin(), back );
      }
      for ( std::uint32_t const index : leaving )
      {
        function.blocks[index].instructions.push_back( back );
      }
    }
  }
}

} // namespace

Promotable promotableGlobals( std::span<std::optional<ir::Unit> const> units )
{
  std::set<std::string> taken;
  std::map<std::string, std::uint32_t> defined;
  Promotable candidates;
  for ( std::optional<ir::Unit> const& each : units )
  {
    if ( !each.has_value() )
    {
      continue;
    }
    ir::Unit const& unit = *each;
    addressesIn( unit, taken );
    for ( ir::Definition const& definition : unit.definitions )
    {
      auto const* const object = std::get_if<ir::Global>( &definition );
      if ( object == nullptr )
      {
        continue;
      }
      ++defined[object->name];
      bool const scalar = !object->count.has_value() && object->type != ir::Type::BLOCK && !object->isTemporary;
      // One the program put on the zero page is read there already.
      if ( scalar && object->pane.empty() && object->implements.empty() &&
           object->placement != ir::Placement::ZEROPAGE && !unit.volatiles.contains( object->name ) )
      {
        candidates.emplace( object->name, object->type );
      }
    }
  }
  Promotable promotable;
  for ( auto const& [name, type] : candidates )
  {
    if ( !taken.contains( name ) && defined[name] == 1 )
    {
      promotable.emplace( name, type );
    }
  }
  return promotable;
}

std::set<std::string> enteredOften( std::span<std::optional<ir::Unit> const> units )
{
  // By name: two `static` functions of one name in two files are taken
  // together, which errs towards often.
  std::set<std::string> often;
  std::map<std::string, std::set<std::string>> callees;
  for ( std::optional<ir::Unit> const& each : units )
  {
    if ( !each.has_value() )
    {
      continue;
    }
    addressesIn( *each, often );
    for ( ir::Definition const& definition : each->definitions )
    {
      auto const* const function = std::get_if<ir::Function>( &definition );
      if ( function == nullptr )
      {
        continue;
      }
      std::vector<std::pair<std::uint32_t, std::uint32_t>> const cycles = cyclesOf( *function );
      for ( std::uint32_t index = 0; index < function->blocks.size(); ++index )
      {
        bool const repeated = std::ranges::any_of(
            cycles, [index]( auto const& cycle ) { return cycle.first <= index && index <= cycle.second; } );
        for ( ir::Instruction const& instruction : function->blocks[index].instructions )
        {
          if ( auto const* const call = std::get_if<ir::Call>( &instruction.operation ) )
          {
            callees[function->name].insert( call->name );
            if ( repeated )
            {
              often.insert( call->name );
            }
          }
        }
      }
    }
  }
  std::vector<std::string> pending( often.begin(), often.end() );
  while ( !pending.empty() )
  {
    std::string const name = pending.back();
    pending.pop_back();
    for ( std::string const& callee : callees[name] )
    {
      if ( often.insert( callee ).second )
      {
        pending.push_back( callee );
      }
    }
  }
  return often;
}

void promoteGlobals( ir::Unit& unit, Promotable const& promotable, std::set<std::string> const& often, Intent intent )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      promoteIn( *function, promotable, often, intent );
    }
  }
}

} // namespace nga::c
