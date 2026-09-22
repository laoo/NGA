#include "nga/c/Reuse.hpp"

#include "nga/c/Loops.hpp"

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

/// The instruction of a block that defines a value, by its position.
std::optional<std::size_t> definerIn( ir::Block const& block, ir::Value value )
{
  for ( std::size_t at = 0; at < block.instructions.size(); ++at )
  {
    std::optional<ir::Value> const defined = ir::resultOf( block.instructions[at] );
    if ( defined.has_value() && defined->index == value.index )
    {
      return at;
    }
  }
  return std::nullopt;
}

/// What an operand is computed from, spelled so that two computations of the
/// same thing from the same objects spell the same: a constant by its type,
/// value and name, an object by its name, and a value by the operator that
/// defines it in the block over what its own operands spell. Nothing where a
/// value is defined by anything but an operator — a load, a call — since two
/// of those need not give the same. The names read are gathered as it goes,
/// so that a store to any of them can be found.
std::optional<std::string> spelling( ir::Block const& block, ir::Operand const& operand, std::set<std::string>& reads )
{
  if ( auto const* const constant = std::get_if<ir::Constant>( &operand ) )
  {
    if ( !constant->name.empty() )
    {
      reads.insert( constant->name );
    }
    return "c" + std::to_string( static_cast<int>( constant->type ) ) + ":" + std::to_string( constant->value ) + ":" +
           constant->name;
  }
  if ( auto const* const object = std::get_if<ir::Object>( &operand ) )
  {
    reads.insert( baseOf( object->name ) );
    return "o" + std::to_string( static_cast<int>( object->type ) ) + ":" + object->name;
  }
  ir::Value const value = std::get<ir::Value>( operand );
  std::optional<std::size_t> const at = definerIn( block, value );
  if ( !at.has_value() )
  {
    return std::nullopt;
  }
  ir::Instruction const& instruction = block.instructions[*at];
  if ( auto const* const binary = std::get_if<ir::Binary>( &instruction.operation ) )
  {
    std::optional<std::string> const left = spelling( block, binary->left, reads );
    std::optional<std::string> const right = spelling( block, binary->right, reads );
    if ( !left.has_value() || !right.has_value() )
    {
      return std::nullopt;
    }
    return "b" + std::to_string( static_cast<int>( binary->op ) ) + ":" +
           std::to_string( static_cast<int>( binary->type ) ) + "(" + *left + "," + *right + ")";
  }
  if ( auto const* const convert = std::get_if<ir::Convert>( &instruction.operation ) )
  {
    std::optional<std::string> const inner = spelling( block, convert->operand, reads );
    if ( !inner.has_value() )
    {
      return std::nullopt;
    }
    return "v" + std::to_string( static_cast<int>( convert->type ) ) + "(" + *inner + ")";
  }
  return std::nullopt;
}

/// A read through a pointer the block computes: what it reads spelled out,
/// and the names that spelling reads.
struct Read
{
  std::size_t at = 0;
  ir::Value result;
  ir::Type type = ir::Type::U8;
  std::string spelled;
  std::set<std::string> reads;
};

std::optional<Read> readAt( ir::Block const& block, std::size_t at )
{
  auto const* const load = std::get_if<ir::LoadIndirect>( &block.instructions[at].operation );
  // A read through a pointer to `volatile` is one read of its own — see
  // docs/decisions/0151-volatile.md.
  if ( load == nullptr || load->isVolatile || !std::holds_alternative<ir::Value>( load->pointer ) )
  {
    return std::nullopt;
  }
  Read read{ .at = at, .result = load->result, .type = load->type, .spelled = {}, .reads = {} };
  std::optional<std::string> const pointer = spelling( block, load->pointer, read.reads );
  std::optional<std::string> const index = spelling( block, load->index, read.reads );
  if ( !pointer.has_value() || !index.has_value() )
  {
    return std::nullopt;
  }
  read.spelled = "r" + std::to_string( static_cast<int>( load->type ) ) + "[" + *pointer + "][" + *index + "]";
  return read;
}

/// Whether an instruction may change what a read that reads `names` would
/// give: a store to one of them, or anything that writes memory it does not
/// name.
bool writesAny( ir::Instruction const& instruction, std::set<std::string> const& names )
{
  return std::visit(
      [&names]( auto const& operation )
      {
        using Operation = std::decay_t<decltype( operation )>;
        if constexpr ( std::is_same_v<Operation, ir::Store> || std::is_same_v<Operation, ir::StoreElement> )
        {
          return names.contains( baseOf( operation.name ) );
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> || std::is_same_v<Operation, ir::Copy> ||
                            std::is_same_v<Operation, ir::Call> || std::is_same_v<Operation, ir::Switch> ||
                            std::is_same_v<Operation, ir::EnterWith> )
        {
          return true;
        }
        else
        {
          return false;
        }
      },
      instruction.operation );
}

void reuseIn( ir::Function& function )
{
  std::uint32_t kept = 0;
  for ( std::uint32_t index = 1; index < function.blocks.size(); ++index )
  {
    std::optional<std::uint32_t> const before = onlyPredecessorOf( function, index );
    if ( !before.has_value() || *before >= index )
    {
      continue;
    }

    // What the block before read and still holds good at its end: each read
    // through a computed address with nothing after it that writes what it
    // read.
    ir::Block const& earlier = function.blocks[*before];
    std::vector<Read> held;
    for ( std::size_t at = 0; at < earlier.instructions.size(); ++at )
    {
      std::optional<Read> read = readAt( earlier, at );
      if ( !read.has_value() )
      {
        continue;
      }
      bool written = false;
      for ( std::size_t after = at + 1; after < earlier.instructions.size(); ++after )
      {
        written = written || writesAny( earlier.instructions[after], read->reads );
      }
      if ( !written )
      {
        held.push_back( std::move( *read ) );
      }
    }
    if ( held.empty() )
    {
      continue;
    }

    // The same read again, before anything in this block writes what it reads:
    // the byte the first was kept in stands for it.
    ir::Block& block = function.blocks[index];
    for ( std::size_t at = 0; at < block.instructions.size(); )
    {
      std::optional<Read> const again = readAt( block, at );
      Read const* first = nullptr;
      for ( Read const& candidate : held )
      {
        if ( again.has_value() && candidate.spelled == again->spelled && candidate.type == again->type )
        {
          first = &candidate;
        }
      }
      bool written = false;
      for ( std::size_t between = 0; between < at && first != nullptr; ++between )
      {
        written = written || writesAny( block.instructions[between], first->reads );
      }
      if ( first == nullptr || written )
      {
        ++at;
        continue;
      }

      // The first read is kept in a byte of the Proc's own, stored right
      // after it — where a 16-bit read then writes straight into that byte.
      std::string const byte = "__c" + std::to_string( kept++ );
      function.locals.push_back( ir::Local{ .name = byte, .type = first->type, .bytes = 0 } );
      std::vector<ir::Instruction>& instructions = function.blocks[*before].instructions;
      diag::SourceLocation const where = instructions[first->at].at;
      instructions.insert(
          instructions.begin() + static_cast<std::ptrdiff_t>( first->at ) + 1,
          ir::Instruction{ .operation =
                               ir::Store{ .name = byte, .type = first->type, .value = ir::Operand{ first->result } },
                           .at = where } );
      for ( Read& shifted : held )
      {
        shifted.at += static_cast<std::size_t>( shifted.at > first->at );
      }

      // The second read goes, and what read it reads the byte. Its address,
      // now read by nothing, is what the dead-store pass takes out.
      ir::Value const gone = again->result;
      block.instructions.erase( block.instructions.begin() + static_cast<std::ptrdiff_t>( at ) );
      auto const reread = [&gone, &byte, type = first->type]( ir::Operand& operand )
      {
        if ( auto const* const read = std::get_if<ir::Value>( &operand ); read != nullptr && read->index == gone.index )
        {
          operand = ir::Object{ .name = byte, .type = type };
        }
      };
      for ( ir::Instruction& reader : block.instructions )
      {
        ir::eachOperand( reader, reread );
      }
      if ( block.terminator.kind == ir::TerminatorKind::BRANCH )
      {
        reread( block.terminator.condition );
      }
    }
  }
}

} // namespace

void reuseLoads( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      reuseIn( *function );
    }
  }
}

} // namespace nga::c
