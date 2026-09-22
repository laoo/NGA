#include "nga/c/Fold.hpp"

#include "nga/c/Loops.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace nga::c
{

namespace
{

/// What a byte is known to hold, by the name the instructions read it under.
using Known = std::map<std::string, ir::Constant, std::less<>>;

/// A constant the text writes as a number rather than as a name: one written
/// by its name is left where it stands, since that name is what the text has
/// for it, and an operator of two of them would lose both.
bool isPlain( ir::Operand const& operand )
{
  auto const* const constant = std::get_if<ir::Constant>( &operand );
  return constant != nullptr && constant->name.empty();
}

std::int64_t valueOf( ir::Operand const& operand )
{
  return std::get<ir::Constant>( operand ).value;
}

/// Every operand of an instruction a constant may stand in, which is every
/// one but the **pointer** of a read or a write through one: `(zp),y` reads
/// the address out of the zero page, and a number is no address to read it
/// out of. Turning a read through a pointer the program knows into a read of
/// the object itself is a different rewrite and not this one.
template <typename Visit>
void eachOperandForAConstant( ir::Instruction& instruction, Visit const& visit )
{
  if ( auto* const load = std::get_if<ir::LoadIndirect>( &instruction.operation ) )
  {
    visit( load->index );
    return;
  }
  if ( auto* const store = std::get_if<ir::StoreIndirect>( &instruction.operation ) )
  {
    visit( store->index );
    visit( store->value );
    return;
  }
  ir::eachOperand( instruction, visit );
}

/// Every operand of the function, the one a BRANCH ends its block with
/// included, which belongs to the block and not to an instruction.
template <typename Visit>
void eachOperandOf( ir::Function& function, Visit const& visit )
{
  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      eachOperandForAConstant( instruction, visit );
    }
    visit( block.terminator.condition );
  }
}

/// What an operator of constants leaves, wrapped into its own type exactly as
/// the fold over the tree wraps it — see
/// docs/decisions/0175-a-constant-reaches-its-reader.md.
std::optional<std::int64_t> answerOf( ir::Binary const& binary )
{
  if ( !isPlain( binary.left ) || !isPlain( binary.right ) )
  {
    return std::nullopt;
  }
  std::int64_t const left = valueOf( binary.left );
  auto const l = static_cast<std::uint64_t>( left );
  auto const r = static_cast<std::uint64_t>( valueOf( binary.right ) );
  auto const wrap = [&binary]( std::uint64_t value )
  { return ir::wrapped( static_cast<std::int64_t>( value ), binary.type ); };

  if ( binary.op == ir::BinaryOperator::SHIFT_LEFT || binary.op == ir::BinaryOperator::SHIFT_RIGHT )
  {
    // A count past the width shifts everything out, which is what a count
    // that large means; the tree's fold reads it the same way.
    std::int64_t const count = valueOf( binary.right );
    bool const past = count < 0 || count > 62;
    if ( binary.op == ir::BinaryOperator::SHIFT_LEFT )
    {
      return past ? std::int64_t{ 0 } : wrap( l << count );
    }
    if ( !past )
    {
      return wrap( static_cast<std::uint64_t>( left >> count ) );
    }
    return left < 0 ? std::int64_t{ -1 } : std::int64_t{ 0 };
  }

  switch ( binary.op )
  {
  case ir::BinaryOperator::ADD:
    return wrap( l + r );
  case ir::BinaryOperator::SUBTRACT:
    return wrap( l - r );
  case ir::BinaryOperator::AND:
    return wrap( l & r );
  case ir::BinaryOperator::OR:
    return wrap( l | r );
  case ir::BinaryOperator::XOR:
    return wrap( l ^ r );
  default:
    return std::nullopt;
  }
}

std::optional<std::int64_t> answerOf( ir::Unary const& unary )
{
  if ( !isPlain( unary.operand ) )
  {
    return std::nullopt;
  }
  auto const operand = static_cast<std::uint64_t>( valueOf( unary.operand ) );
  if ( unary.op == ir::UnaryOperator::LOGICAL_NOT )
  {
    return operand == 0 ? std::int64_t{ 1 } : std::int64_t{ 0 };
  }
  std::uint64_t const answer = unary.op == ir::UnaryOperator::NEGATE ? ( 0 - operand ) : ~operand;
  return ir::wrapped( static_cast<std::int64_t>( answer ), unary.type );
}

/// A conversion of a constant is the constant held in the type converted to,
/// which is what widening, narrowing and reading the bits another way all
/// leave — the four the subset has are one rule here.
std::optional<std::int64_t> answerOf( ir::Convert const& convert )
{
  return isPlain( convert.operand ) ? std::optional{ ir::wrapped( valueOf( convert.operand ), convert.type ) }
                                    : std::nullopt;
}

/// Every operator of constants made a constant, and the value it defined
/// rewritten to it wherever the function reads it. Not a `Compare`: its
/// reader is a BRANCH, and a branch whose condition is known wants the block
/// it no longer reaches to go with it.
bool foldIn( ir::Function& function )
{
  std::map<std::uint32_t, ir::Constant> answers;
  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      std::optional<std::int64_t> answer;
      ir::Value result;
      ir::Type type = ir::Type::U8;
      if ( auto const* const binary = std::get_if<ir::Binary>( &instruction.operation ) )
      {
        answer = answerOf( *binary );
        result = binary->result;
        type = binary->type;
      }
      else if ( auto const* const unary = std::get_if<ir::Unary>( &instruction.operation ) )
      {
        answer = answerOf( *unary );
        result = unary->result;
        type = unary->type;
      }
      else if ( auto const* const convert = std::get_if<ir::Convert>( &instruction.operation ) )
      {
        answer = answerOf( *convert );
        result = convert->result;
        type = convert->type;
      }
      if ( answer.has_value() )
      {
        answers.emplace( result.index, ir::Constant{ .type = type, .value = *answer, .name = {}, .follower = {} } );
      }
    }
  }
  if ( answers.empty() )
  {
    return false;
  }

  eachOperandOf( function,
                 [&answers]( ir::Operand& operand )
                 {
                   auto const* const value = std::get_if<ir::Value>( &operand );
                   if ( value == nullptr )
                   {
                     return;
                   }
                   if ( auto const found = answers.find( value->index ); found != answers.end() )
                   {
                     operand = found->second;
                   }
                 } );
  for ( ir::Block& block : function.blocks )
  {
    std::erase_if( block.instructions,
                   [&answers]( ir::Instruction const& instruction )
                   {
                     std::optional<ir::Value> const result = ir::resultOf( instruction );
                     return result.has_value() && answers.contains( result->index );
                   } );
  }
  return true;
}

/// The shift or the mask a divide of the runtime's turns out to be: what the
/// front end writes where the divisor is a constant of the expression, done
/// where the divisor turns out to be one only after a body was wrapped or a
/// store was read — 0095 and c-subset say the shape without saying where the
/// constant has to stand. Unsigned only: a signed `/` truncates towards zero
/// and wants the bias the front end writes beside the shift, and a signed `%`
/// is no mask at all.
std::optional<ir::Binary> insteadOfDividing( ir::Call const& call )
{
  static constexpr std::array<std::string_view, 4> DIVIDES{ "__udiv8", "__udiv16", "__umod8", "__umod16" };
  if ( !call.result.has_value() || call.arguments.size() != 2 ||
       std::ranges::find( DIVIDES, call.name ) == DIVIDES.end() || !isPlain( call.arguments[1].value ) )
  {
    return std::nullopt;
  }
  std::int64_t const divisor = valueOf( call.arguments[1].value );
  if ( divisor <= 0 || ( divisor & ( divisor - 1 ) ) != 0 )
  {
    return std::nullopt;
  }

  ir::Type const type = call.arguments[0].type;
  bool const quotient = call.name.starts_with( "__udiv" );
  auto const shift = static_cast<std::int64_t>( std::countr_zero( static_cast<std::uint64_t>( divisor ) ) );
  return ir::Binary{ .result = *call.result,
                     .op = quotient ? ir::BinaryOperator::SHIFT_RIGHT : ir::BinaryOperator::AND,
                     .type = type,
                     .left = call.arguments[0].value,
                     .right = quotient
                                  ? ir::Constant{ .type = ir::Type::U8, .value = shift, .name = {}, .follower = {} }
                                  : ir::Constant{ .type = type, .value = divisor - 1, .name = {}, .follower = {} } };
}

bool divideIn( ir::Function& function )
{
  bool changed = false;
  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      auto const* const call = std::get_if<ir::Call>( &instruction.operation );
      if ( call == nullptr )
      {
        continue;
      }
      if ( std::optional<ir::Binary> instead = insteadOfDividing( *call ); instead.has_value() )
      {
        instruction.operation = std::move( *instead );
        changed = true;
      }
    }
  }
  return changed;
}

/// What the instruction writes, of the names a constant may be remembered
/// under: the object a store names, or every name that is no byte of the
/// Proc's own where it writes through a pointer or calls.
void forgetAfter( ir::Instruction const& instruction, Known& known, std::set<std::string> const& own )
{
  auto const forgetObject = [&known]( std::string const& name )
  {
    std::string const base = baseOf( name );
    std::erase_if( known, [&base]( auto const& entry ) { return baseOf( entry.first ) == base; } );
  };
  auto const forgetReached = [&known, &own]
  { std::erase_if( known, [&own]( auto const& entry ) { return !own.contains( baseOf( entry.first ) ); } ); };
  auto const forgetAll = [&known] { known.clear(); };

  std::visit(
      [&forgetObject, &forgetReached, &forgetAll]( auto const& operation )
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
          // A call and a store through a pointer reach everything but the
          // Proc's own bytes, which nothing else has a name for — 0150.
          forgetReached();
        }
        else if constexpr ( std::is_same_v<Operation, ir::Switch> || std::is_same_v<Operation, ir::EnterWith> )
        {
          // And these reach the Proc's own bytes as well, which is the whole
          // difference: a `switch`'s cases are Procs of their own that name
          // them through the function (0083), and a `[[with]]` block may be a
          // Proc of its own besides changing which byte an address is (0096,
          // 0098). Nothing known before one of them is known after it.
          forgetAll();
        }
      },
      instruction.operation );
}

/// The block every path into this one comes through, or nothing: the one the
/// constants known at its end still hold in, which is what `reuseLoads` takes
/// a straight line to be.
std::vector<std::optional<std::uint32_t>> straightLine( ir::Function const& function )
{
  std::vector<std::uint32_t> count( function.blocks.size(), 0 );
  std::vector<std::optional<std::uint32_t>> only( function.blocks.size() );
  for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
  {
    ir::Terminator const& end = function.blocks[index].terminator;
    if ( end.kind != ir::TerminatorKind::JUMP && end.kind != ir::TerminatorKind::BRANCH )
    {
      continue;
    }
    for ( std::uint32_t target : end.kind == ir::TerminatorKind::BRANCH
                                     ? std::vector<std::uint32_t>{ end.target, end.otherwise }
                                     : std::vector<std::uint32_t>{ end.target } )
    {
      if ( target < count.size() )
      {
        ++count[target];
        only[target] = index;
      }
    }
  }
  for ( std::uint32_t index = 0; index < only.size(); ++index )
  {
    // Entered from more than one place, or from one that stands after it and
    // so has not been walked when this one is.
    std::optional<std::uint32_t> const single = only[index];
    if ( count[index] != 1 || !single.has_value() || *single >= index )
    {
      only[index].reset();
    }
  }
  return only;
}

bool propagateIn( ir::Function& function, std::set<std::string> const& volatiles )
{
  std::set<std::string> const own = ownBytesOf( function );
  std::vector<std::optional<std::uint32_t>> const from = straightLine( function );
  std::vector<Known> atEnd( function.blocks.size() );
  bool changed = false;

  for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
  {
    std::optional<std::uint32_t> const entered = from[index];
    Known known = entered.has_value() ? atEnd[*entered] : Known{};
    for ( ir::Instruction& instruction : function.blocks[index].instructions )
    {
      eachOperandForAConstant( instruction,
                               [&known, &changed]( ir::Operand& operand )
                               {
                                 auto const* const object = std::get_if<ir::Object>( &operand );
                                 if ( object == nullptr )
                                 {
                                   return;
                                 }
                                 auto const found = known.find( object->name );
                                 if ( found != known.end() && found->second.type == object->type )
                                 {
                                   operand = found->second;
                                   changed = true;
                                 }
                               } );
      forgetAfter( instruction, known, own );
      if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation );
           store != nullptr && std::holds_alternative<ir::Constant>( store->value ) &&
           !volatiles.contains( baseOf( store->name ) ) )
      {
        ir::Constant held = std::get<ir::Constant>( store->value );
        held.type = store->type;
        known.insert_or_assign( store->name, std::move( held ) );
      }
    }

    // A BRANCH reads its condition where the block ends, after every
    // instruction of it has run.
    if ( auto* const object = std::get_if<ir::Object>( &function.blocks[index].terminator.condition ) )
    {
      auto const found = known.find( object->name );
      if ( found != known.end() && found->second.type == object->type )
      {
        function.blocks[index].terminator.condition = found->second;
        changed = true;
      }
    }
    atEnd[index] = std::move( known );
  }
  return changed;
}

} // namespace

void foldConstants( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    auto* const function = std::get_if<ir::Function>( &definition );
    if ( function == nullptr )
    {
      continue;
    }

    // Each makes the other's next site: a constant read where a byte was read
    // is an operand a fold may want, and a folded operator is a constant a
    // store may carry further.
    for ( bool again = true; again; )
    {
      bool const propagated = propagateIn( *function, unit.volatiles );
      bool const divided = divideIn( *function );
      bool const folded = foldIn( *function );
      again = propagated || divided || folded;
    }
  }
}

} // namespace nga::c
