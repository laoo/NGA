#include "nga/c/Wrap.hpp"

#include "nga/c/Liveness.hpp"
#include "nga/c/Loops.hpp"

#include <cstddef>
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

/// Whether an operand is the number zero, written as a number.
bool isZero( ir::Operand const& operand )
{
  auto const* const constant = std::get_if<ir::Constant>( &operand );
  return constant != nullptr && constant->name.empty() && constant->value == 0;
}

void wrapIn( ir::Function& function, std::set<std::string> const& volatiles )
{
  std::set<std::string> const own = ownBytesOf( function );
  std::uint32_t kept = 0;
  for ( Loop const& loop : loopsOf( function ) )
  {
    // The header is the test and nothing else, `k < b` of an unsigned byte or
    // pair, going into the loop's next block.
    ir::Block& header = function.blocks[loop.header];
    if ( header.instructions.size() != 1 || header.terminator.kind != ir::TerminatorKind::BRANCH ||
         header.terminator.target != loop.header + 1 )
    {
      continue;
    }
    auto* const compare = std::get_if<ir::Compare>( &header.instructions[0].operation );
    auto const* const condition = std::get_if<ir::Value>( &header.terminator.condition );
    if ( compare == nullptr || condition == nullptr || condition->index != compare->result.index ||
         compare->op != ir::Comparison::LESS || ( compare->type != ir::Type::U8 && compare->type != ir::Type::U16 ) )
    {
      continue;
    }
    auto const* const counted = std::get_if<ir::Object>( &compare->left );
    if ( counted == nullptr || !own.contains( counted->name ) || volatiles.contains( counted->name ) ||
         counted->type != compare->type )
    {
      continue;
    }
    std::string const counter = counted->name;
    ir::Type const type = compare->type;
    ir::Operand const bound = compare->right;
    Written const written = writtenIn( function, loop );
    if ( !invariant( bound, written, own ) )
    {
      continue;
    }

    // The step: the last block adds one and stores it, last, and nothing
    // else in the loop reads or writes the counter.
    ir::Block& last = function.blocks[loop.last];
    // Where it stands in the block: a pointer walked beside it is stepped
    // after it (0126).
    std::optional<std::size_t> found;
    for ( std::size_t at = 0; at + 1 < last.instructions.size(); ++at )
    {
      auto const* const candidate = std::get_if<ir::Store>( &last.instructions[at + 1].operation );
      if ( candidate != nullptr && candidate->name == counter )
      {
        found = at;
      }
    }
    if ( !found.has_value() )
    {
      continue;
    }
    std::size_t const addAt = *found;
    auto* const add = std::get_if<ir::Binary>( &last.instructions[addAt].operation );
    auto* const store = std::get_if<ir::Store>( &last.instructions[addAt + 1].operation );
    auto const* const stepped = add != nullptr ? std::get_if<ir::Object>( &add->left ) : nullptr;
    auto const* const one = add != nullptr ? std::get_if<ir::Constant>( &add->right ) : nullptr;
    auto const* const value = store != nullptr ? std::get_if<ir::Value>( &store->value ) : nullptr;
    if ( add == nullptr || store == nullptr || add->op != ir::BinaryOperator::ADD || add->type != type ||
         stepped == nullptr || stepped->name != counter || one == nullptr || !one->name.empty() || one->value != 1 ||
         store->name != counter || value == nullptr || value->index != add->result.index ||
         ir::readersOf( function, add->result ) != 1 )
    {
      continue;
    }
    bool alone = true;
    for ( std::uint32_t index = loop.header; index <= loop.last && alone; ++index )
    {
      ir::Block& block = function.blocks[index];
      for ( std::size_t at = 0; at < block.instructions.size() && alone; ++at )
      {
        bool const itself =
            ( index == loop.header && at == 0 ) || ( index == loop.last && ( at == addAt || at == addAt + 1 ) );
        if ( itself )
        {
          continue;
        }
        ir::eachName( block.instructions[at],
                      [&counter, &alone]( std::string const& name )
                      { alone = alone && ir::baseOf( name ) != ir::baseOf( counter ); } );
      }
      if ( auto const* const object = std::get_if<ir::Object>( &block.terminator.condition );
           object != nullptr && ir::baseOf( object->name ) == ir::baseOf( counter ) )
      {
        alone = false;
      }
    }
    if ( !alone )
    {
      continue;
    }

    // Started at zero right before the loop, which is what makes `0 - b` the
    // count of turns, and no turn at all for a bound of zero.
    ir::Block& entry = function.blocks[loop.entry];
    std::optional<std::size_t> startAt;
    for ( std::size_t at = entry.instructions.size(); at-- > 0; )
    {
      if ( auto const* const start = std::get_if<ir::Store>( &entry.instructions[at].operation );
           start != nullptr && ir::baseOf( start->name ) == ir::baseOf( counter ) )
      {
        if ( start->name == counter && isZero( start->value ) )
        {
          startAt = at;
        }
        break;
      }
      // What reads the start after it is set reads it as it was, and keeps
      // it: a pointer walked beside the counter starts from it (0126).
      bool writes = std::holds_alternative<ir::Copy>( entry.instructions[at].operation ) ||
                    std::holds_alternative<ir::StoreElement>( entry.instructions[at].operation );
      if ( writes )
      {
        ir::eachName( entry.instructions[at],
                      [&counter, &writes]( std::string const& name )
                      { writes = writes && ir::baseOf( name ) == ir::baseOf( counter ); } );
        if ( writes )
        {
          break;
        }
      }
    }
    if ( !startAt.has_value() )
    {
      continue;
    }

    // Nothing after the loop reads the counter, which no longer counts.
    std::vector<std::set<std::string>> const liveIn = liveInOf( function );
    bool dead = true;
    for ( std::uint32_t index = loop.header; index <= loop.last; ++index )
    {
      for ( std::uint32_t const to : ir::successorsOf( function.blocks[index].terminator ) )
      {
        if ( ( to < loop.header || to > loop.last ) && liveIn[to].contains( counter ) )
        {
          dead = false;
        }
      }
    }
    if ( !dead )
    {
      continue;
    }

    // The new counter: `0 - b` where the loop is entered, tested against zero,
    // stepped by one.
    std::string const runs = "__w" + std::to_string( kept++ );
    function.locals.push_back( ir::Local{ .name = runs, .type = type, .bytes = 0 } );
    diag::SourceLocation const where = entry.instructions[*startAt].at;
    std::int64_t const modulus = type == ir::Type::U8 ? 0x100 : 0x10000;
    if ( auto const* const number = std::get_if<ir::Constant>( &bound ); number != nullptr && number->name.empty() )
    {
      // A number: the start is one too, which a counted loop wants to see.
      entry.instructions.push_back( ir::Instruction{
          .operation = ir::Store{ .name = runs,
                                  .type = type,
                                  .value = ir::Constant{ .type = type,
                                                         .value = ( modulus - number->value ) % modulus,
                                                         .name = {},
                                                         .follower = {} } },
          .at = where } );
    }
    else
    {
      ir::Value const start{ static_cast<std::uint32_t>( function.values.size() ) };
      function.values.push_back( type );
      entry.instructions.push_back( ir::Instruction{
          .operation = ir::Binary{ .result = start,
                                   .op = ir::BinaryOperator::SUBTRACT,
                                   .type = type,
                                   .left = ir::Constant{ .type = type, .value = 0, .name = {}, .follower = {} },
                                   .right = bound },
          .at = where } );
      entry.instructions.push_back( ir::Instruction{
          .operation = ir::Store{ .name = runs, .type = type, .value = ir::Operand{ start } }, .at = where } );
    }

    compare->op = ir::Comparison::NOT_EQUAL;
    compare->left = ir::Object{ .name = runs, .type = type };
    compare->right = ir::Constant{ .type = type, .value = 0, .name = {}, .follower = {} };

    add->left = ir::Object{ .name = runs, .type = type };
    store->name = runs;

    // The step last of all, after a pointer walked beside it (0126): then the
    // test right after it may branch on what the step's own `inc` left — see
    // docs/decisions/0288-a-pair-stepped-to-its-wrap-branches-on-the-step.md.
    ir::Instruction const sum = last.instructions[addAt];
    ir::Instruction const storeOfStep = last.instructions[addAt + 1];
    last.instructions.erase( last.instructions.begin() + static_cast<std::ptrdiff_t>( addAt ),
                             last.instructions.begin() + static_cast<std::ptrdiff_t>( addAt + 2 ) );
    last.instructions.push_back( sum );
    last.instructions.push_back( storeOfStep );
  }
}

} // namespace

void countToWrap( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      wrapIn( *function, unit.volatiles );
    }
  }
}

} // namespace nga::c
