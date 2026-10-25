#include "nga/c/Copies.hpp"

#include "nga/c/Liveness.hpp"
#include "nga/c/Loops.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace nga::c
{

namespace
{

/// The most reads a copy may have and still cost more than reading its object
/// in its place: a byte copied is `lda abs` and `sta zp`, seven cycles, and
/// each read of the copy is three where the object's is four, so `k` reads
/// pay `7 + 3k` against `4k` — the object wins below seven.
constexpr std::size_t MOST_READS = 6;

/// Where an operand stands: its instruction, or the BRANCH condition of its
/// block where `at` is past the last, and its place among that instruction's
/// operands as `eachOperand` walks them.
struct Site
{
  std::uint32_t block = 0;
  std::size_t at = 0;
  std::size_t operand = 0;
};

/// A copy, `store LOCAL, @OBJECT`, and the reads it is known to fill.
struct Copy
{
  std::uint32_t block = 0;
  std::size_t at = 0;
  std::string local;
  ir::Object source;
  std::vector<Site> reads;
};

/// What is known at a point: each local a copy filled, by its name, to the
/// index of that copy.
using Known = std::map<std::string, std::size_t>;

/// The block every path into this one comes through, or nothing: the one whose
/// end holds for this one's start — the straight line of
/// docs/decisions/0175-a-constant-reaches-its-reader.md.
std::vector<std::optional<std::uint32_t>> straightLine( ir::Function const& function )
{
  std::vector<std::vector<std::uint32_t>> const before = ir::predecessorsOf( function.blocks );
  std::vector<std::optional<std::uint32_t>> only( function.blocks.size() );
  for ( std::uint32_t index = 0; index < only.size(); ++index )
  {
    if ( before[index].size() == 1 && before[index].front() < index )
    {
      only[index] = before[index].front();
    }
  }
  return only;
}

/// What an instruction writes, as far as a copy is concerned: the local it
/// filled, or the object it was filled from. 0175's kill set, applied to both
/// ends of the copy.
void forgetAfter( ir::Instruction const& instruction,
                  Known& known,
                  std::vector<Copy> const& copies,
                  std::set<std::string> const& own )
{
  auto const forgetWhere = [&known, &copies]( auto const& forgets )
  {
    std::erase_if( known,
                   [&copies, &forgets]( auto const& entry )
                   { return forgets( entry.first, copies[entry.second].source.name ); } );
  };
  auto const forgetObject = [&forgetWhere]( std::string const& name )
  {
    std::string const base = ir::baseOf( name );
    forgetWhere( [&base]( std::string const& local, std::string const& source )
                 { return ir::baseOf( local ) == base || ir::baseOf( source ) == base; } );
  };
  auto const forgetReached = [&forgetWhere, &own]
  {
    forgetWhere( [&own]( std::string const&, std::string const& source )
                 { return !own.contains( ir::baseOf( source ) ); } );
  };

  std::visit(
      [&]( auto const& operation )
      {
        using Operation = std::decay_t<decltype( operation )>;
        if constexpr ( std::is_same_v<Operation, ir::Store> || std::is_same_v<Operation, ir::StoreElement> )
        {
          forgetObject( operation.name );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Copy> )
        {
          if ( operation.to.pointer.has_value() )
          {
            forgetReached();
            return;
          }
          forgetObject( operation.to.name );
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> || std::is_same_v<Operation, ir::Call> )
        {
          forgetReached();
        }
        else if constexpr ( std::is_same_v<Operation, ir::EnterWith> )
        {
          known.clear();
        }
      },
      instruction.operation );
}

/// Every copy of the function and the reads known to come from it, walked as
/// 0175 walks a constant: through a block, and on into a block entered from
/// that one alone.
std::vector<Copy> copiesOf( ir::Function& function, std::set<std::string> const& volatiles )
{
  std::set<std::string> const own = ownBytesOf( function );
  std::vector<std::optional<std::uint32_t>> const from = straightLine( function );
  std::vector<Known> atEnd( function.blocks.size() );
  std::vector<Copy> copies;

  auto const record = [&copies]( Known const& known, Site site, ir::Operand const& operand )
  {
    auto const* const object = std::get_if<ir::Object>( &operand );
    if ( object == nullptr )
    {
      return;
    }
    auto const found = known.find( object->name );
    if ( found != known.end() && copies[found->second].source.type == object->type )
    {
      copies[found->second].reads.push_back( site );
    }
  };

  for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
  {
    ir::Block& block = function.blocks[index];
    std::optional<std::uint32_t> const entered = from[index];
    Known known = entered.has_value() ? atEnd[*entered] : Known{};
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      ir::Instruction& instruction = block.instructions[at];
      std::size_t ordinal = 0;
      ir::eachOperand( instruction,
                       [&]( ir::Operand& operand )
                       { record( known, Site{ .block = index, .at = at, .operand = ordinal++ }, operand ); } );
      forgetAfter( instruction, known, copies, own );

      // A copy: a whole local of the Proc's own given an object of the same
      // type, neither of them `volatile`, and not itself.
      auto const* const store = std::get_if<ir::Store>( &instruction.operation );
      auto const* const source = store != nullptr ? std::get_if<ir::Object>( &store->value ) : nullptr;
      if ( source != nullptr && own.contains( store->name ) && source->type == store->type &&
           ir::baseOf( source->name ) != ir::baseOf( store->name ) && !volatiles.contains( store->name ) &&
           !volatiles.contains( ir::baseOf( source->name ) ) )
      {
        known[store->name] = copies.size();
        copies.push_back( Copy{ .block = index, .at = at, .local = store->name, .source = *source, .reads = {} } );
      }
    }
    if ( block.terminator.kind == ir::TerminatorKind::BRANCH )
    {
      record(
          known, Site{ .block = index, .at = block.instructions.size(), .operand = 0 }, block.terminator.condition );
    }
    atEnd[index] = std::move( known );
  }
  return copies;
}

/// The operand a site names.
ir::Operand* operandAt( ir::Function& function, Site const& site )
{
  ir::Block& block = function.blocks[site.block];
  if ( site.at == block.instructions.size() )
  {
    return &block.terminator.condition;
  }
  ir::Operand* found = nullptr;
  std::size_t ordinal = 0;
  ir::eachOperand( block.instructions[site.at],
                   [&]( ir::Operand& operand )
                   {
                     if ( ordinal++ == site.operand )
                     {
                       found = &operand;
                     }
                   } );
  return found;
}

/// Whether anything still reads the local after the copy: on in its own block
/// until something stores the whole of it again, and then what the blocks it
/// leads to are entered needing.
bool readAfter( ir::Function& function, std::vector<std::set<std::string>> const& liveIn, Copy const& copy )
{
  ir::Block& block = function.blocks[copy.block];
  std::string const base = ir::baseOf( copy.local );
  for ( std::size_t at = copy.at + 1; at < block.instructions.size(); ++at )
  {
    ir::Instruction& instruction = block.instructions[at];
    bool reads = false;
    ir::eachOperand( instruction,
                     [&base, &reads]( ir::Operand& operand )
                     {
                       auto const* const object = std::get_if<ir::Object>( &operand );
                       reads = reads || ( object != nullptr && ir::baseOf( object->name ) == base );
                     } );
    if ( !std::holds_alternative<ir::Store>( instruction.operation ) )
    {
      ir::eachName( instruction,
                    [&base, &reads]( std::string const& name ) { reads = reads || ir::baseOf( name ) == base; } );
    }
    if ( reads )
    {
      return true;
    }
    if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation );
         store != nullptr && store->name == copy.local )
    {
      return false;
    }
  }
  ir::Terminator const& end = block.terminator;
  if ( auto const* const object = std::get_if<ir::Object>( &end.condition );
       end.kind == ir::TerminatorKind::BRANCH && object != nullptr && ir::baseOf( object->name ) == base )
  {
    return true;
  }
  std::vector<std::uint32_t> next;
  switch ( end.kind )
  {
  case ir::TerminatorKind::JUMP:
    next = { end.target };
    break;
  case ir::TerminatorKind::BRANCH:
    next = { end.target, end.otherwise };
    break;
  case ir::TerminatorKind::DISPATCH:
    next = end.targets;
    break;
  case ir::TerminatorKind::RETURN:
  case ir::TerminatorKind::TRANSITION:
  case ir::TerminatorKind::FALL:
    break;
  }
  return std::ranges::any_of(
      next, [&liveIn, &copy]( std::uint32_t to ) { return to < liveIn.size() && liveIn[to].contains( copy.local ); } );
}

void propagateIn( ir::Function& function, std::set<std::string> const& volatiles )
{
  if ( function.blocks.empty() )
  {
    return;
  }
  std::set<std::string> const followed = liveBytesOf( function );
  std::vector<Copy> const copies = copiesOf( function, volatiles );
  for ( Copy const& copy : copies )
  {
    // A byte liveness does not follow is one whose readers it cannot vouch
    // for; and a copy read more often than it costs is left as it is.
    if ( copy.reads.empty() || copy.reads.size() > MOST_READS || !followed.contains( copy.local ) )
    {
      continue;
    }
    std::vector<ir::Operand> was;
    for ( Site const& site : copy.reads )
    {
      ir::Operand* const operand = operandAt( function, site );
      was.push_back( *operand );
      ir::Object instead = copy.source;
      instead.type = std::get<ir::Object>( *operand ).type;
      *operand = instead;
    }

    // Every reader taken, or none: a reader left behind keeps the copy, and
    // the reads moved to the object would then only cost.
    if ( readAfter( function, liveInOf( function ), copy ) )
    {
      for ( std::size_t index = 0; index < copy.reads.size(); ++index )
      {
        *operandAt( function, copy.reads[index] ) = was[index];
      }
    }
  }
}

} // namespace

void propagateCopies( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      propagateIn( *function, unit.volatiles );
    }
  }
}

} // namespace nga::c
