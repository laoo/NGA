#include "nga/c/Fill.hpp"

#include "nga/c/Loops.hpp"

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

/// The name the runtime gives the Proc, and its arguments as it declares them.
constexpr std::string_view FILL = "__fill";

/// Whether an operand reads the same at every turn of a loop that writes
/// memory through a pointer: a constant, or a byte of the Proc's own that the
/// loop does not write, which no pointer reaches — see
/// docs/decisions/0150-a-pointer-does-not-reach-the-procs-own-scalars.md.
bool steady( ir::Operand const& operand,
             std::string const& counter,
             std::set<std::string> const& own,
             std::set<std::string> const& volatiles )
{
  if ( std::holds_alternative<ir::Constant>( operand ) )
  {
    return true;
  }
  auto const* const object = std::get_if<ir::Object>( &operand );
  return object != nullptr && object->name != counter && own.contains( object->name ) &&
         !volatiles.contains( object->name );
}

/// Whether the block steps the counter by one, as a 16-bit add and its store,
/// and does nothing else.
bool stepsByOne( ir::Block const& block, std::string const& counter )
{
  if ( block.instructions.size() != 2 || block.terminator.kind != ir::TerminatorKind::JUMP )
  {
    return false;
  }
  auto const* const add = std::get_if<ir::Binary>( &block.instructions[0].operation );
  auto const* const store = std::get_if<ir::Store>( &block.instructions[1].operation );
  if ( add == nullptr || store == nullptr || add->op != ir::BinaryOperator::ADD || add->type != ir::Type::U16 ||
       store->name != counter || store->type != ir::Type::U16 )
  {
    return false;
  }
  auto const* const read = std::get_if<ir::Object>( &add->left );
  auto const* const by = std::get_if<ir::Constant>( &add->right );
  auto const* const stored = std::get_if<ir::Value>( &store->value );
  return read != nullptr && read->name == counter && by != nullptr && by->name.empty() && by->value == 1 &&
         stored != nullptr && stored->index == add->result.index;
}

void fillIn( ir::Function& function, std::set<std::string> const& volatiles )
{
  std::set<std::string> const own = ownBytesOf( function );
  for ( Loop const& loop : loopsOf( function ) )
  {
    // The shape the lowering writes for such a `for`: a header that is the
    // test and nothing else, one block of body, and the step.
    std::uint32_t const header = loop.header;
    std::uint32_t const body = header + 1;
    std::uint32_t const step = loop.last;
    if ( step != body + 1 )
    {
      continue;
    }
    ir::Block const& test = function.blocks[header];
    if ( test.instructions.size() != 1 || test.terminator.kind != ir::TerminatorKind::BRANCH ||
         test.terminator.target != body || test.terminator.otherwise <= step )
    {
      continue;
    }
    auto const* const compare = std::get_if<ir::Compare>( &test.instructions[0].operation );
    auto const* const condition = std::get_if<ir::Value>( &test.terminator.condition );
    if ( compare == nullptr || condition == nullptr || condition->index != compare->result.index ||
         compare->op != ir::Comparison::LESS || compare->type != ir::Type::U16 )
    {
      continue;
    }
    auto const* const counted = std::get_if<ir::Object>( &compare->left );
    if ( counted == nullptr || !own.contains( counted->name ) || volatiles.contains( counted->name ) )
    {
      continue;
    }
    std::string const counter = counted->name;
    ir::Operand const bound = compare->right;
    if ( !steady( bound, counter, own, volatiles ) || !stepsByOne( function.blocks[step], counter ) )
    {
      continue;
    }

    // The body: the element's address, the base plus the counter, and one
    // byte stored there.
    ir::Block& fills = function.blocks[body];
    if ( fills.instructions.size() != 2 || fills.terminator.kind != ir::TerminatorKind::JUMP ||
         fills.terminator.target != step )
    {
      continue;
    }
    auto const* const address = std::get_if<ir::Binary>( &fills.instructions[0].operation );
    auto const* const store = std::get_if<ir::StoreIndirect>( &fills.instructions[1].operation );
    if ( address == nullptr || store == nullptr || address->op != ir::BinaryOperator::ADD ||
         address->type != ir::Type::POINTER || store->type != ir::Type::U8 || store->isVolatile )
    {
      continue;
    }
    auto const* const through = std::get_if<ir::Value>( &store->pointer );
    auto const* const at = std::get_if<ir::Constant>( &store->index );
    auto const* const added = std::get_if<ir::Object>( &address->right );
    if ( through == nullptr || through->index != address->result.index || at == nullptr || !at->name.empty() ||
         at->value != 0 || added == nullptr || added->name != counter ||
         !steady( address->left, counter, own, volatiles ) || !steady( store->value, counter, own, volatiles ) )
    {
      continue;
    }

    // Where the test first holds, the bytes from the counter up to the bound
    // are filled at once, and the counter left where the loop would have left
    // it: at the bound, which it reaches by ones from below.
    ir::Value const start = address->result;
    ir::Value const remaining{ static_cast<std::uint32_t>( function.values.size() ) };
    function.values.push_back( ir::Type::U16 );
    diag::SourceLocation const where = fills.instructions[1].at;
    ir::Operand const value = store->value;
    fills.instructions.resize( 1 );
    fills.instructions.push_back(
        ir::Instruction{ .operation = ir::Binary{ .result = remaining,
                                                  .op = ir::BinaryOperator::SUBTRACT,
                                                  .type = ir::Type::U16,
                                                  .left = bound,
                                                  .right = ir::Object{ .name = counter, .type = ir::Type::U16 } },
                         .at = where } );
    std::string const name{ FILL };
    fills.instructions.push_back( ir::Instruction{
        .operation =
            ir::Call{
                .name = name,
                .arguments = { ir::Argument{
                                   .name = name + ".to", .type = ir::Type::POINTER, .value = ir::Operand{ start } },
                               ir::Argument{
                                   .name = name + ".count", .type = ir::Type::U16, .value = ir::Operand{ remaining } },
                               ir::Argument{ .name = name + ".value", .type = ir::Type::U8, .value = value } } },
        .at = where } );
    fills.instructions.push_back( ir::Instruction{
        .operation = ir::Store{ .name = counter, .type = ir::Type::U16, .value = bound }, .at = where } );

    // The step is gone with the turns: what was the loop's last block is now
    // the way out.
    ir::Block& stepped = function.blocks[step];
    stepped.instructions.clear();
    stepped.terminator.target = test.terminator.otherwise;
  }
}

} // namespace

void fillLoops( ir::Unit& unit, bool speed )
{
  if ( !speed )
  {
    return;
  }
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* const function = std::get_if<ir::Function>( &definition ) )
    {
      fillIn( *function, unit.volatiles );
    }
  }
}

} // namespace nga::c
