#include "nga/c/Narrow.hpp"

#include "nga/c/Ranges.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

namespace nga::c
{

namespace
{

/// Two bytes, and so a value whose high byte narrowing may learn something
/// about: an `i16` the ranges hold in 0 to 255 has a high byte of zero as a
/// `u16` does, its sign bit among it. A pointer is two unsigned bytes too, but
/// what it addresses is nobody's to fold into one byte, so it is left alone.
/// See docs/decisions/0138-a-pair-an-operand-leaves-is-the-answers.md.
bool isNarrowable( ir::Type type )
{
  return type == ir::Type::U16 || type == ir::Type::I16;
}

/// What the pass knows about each value of one function: the ranges of
/// docs/decisions/0121-what-a-value-can-hold.md, asked the one question this
/// pass has — whether a value is one unsigned byte, so that its high byte is
/// zero wherever it is read as two.
class Known
{
public:
  explicit Known( ir::Function const& function ) : mFunction( &function ), mRanges( rangesOf( function ) ) {}

  [[nodiscard]] bool highIsZero( ir::Operand const& operand ) const
  {
    return rangeOf( operand, *mFunction, mRanges ).isByte();
  }

  /// Whether a value the instructions define is one unsigned byte, which is
  /// what the sweep says of a whole operation at once.
  [[nodiscard]] bool isByte( ir::Value value ) const
  {
    return mRanges.at( value.index ).isByte();
  }

private:
  ir::Function const* mFunction;
  std::vector<Range> mRanges;
};

/// Whether a 16-bit operator may be computed in one byte: where the **result**
/// is one unsigned byte, and the low byte of the result is made from the
/// operands' low bytes alone.
///
/// The second half is what keeps a shift out however narrow its range: a bit
/// of the low byte of `a >> 8` comes from the high byte of `a`, and a narrowed
/// operator reads its operands' low bytes. Everything else here is either
/// bitwise or carries upwards only, so its low byte is `low(a) op low(b)` — and
/// where the result is in 0 to 255 it *is* that byte, whatever stands above the
/// operands. See docs/decisions/0121-what-a-value-can-hold.md.
bool narrowsToByte( ir::Binary const& binary, Known const& known )
{
  switch ( binary.op )
  {
  case ir::BinaryOperator::AND:
  case ir::BinaryOperator::OR:
  case ir::BinaryOperator::XOR:
  case ir::BinaryOperator::ADD:
  case ir::BinaryOperator::SUBTRACT:
    return known.isByte( binary.result );
  case ir::BinaryOperator::SHIFT_LEFT:
    return false;
  case ir::BinaryOperator::SHIFT_RIGHT:
  {
    // By eight or more, unsigned: the answer is the high byte shifted by the
    // rest, which the emitter reads as `x+1` — see
    // docs/decisions/0140-a-value-only-its-low-byte-is-read-of.md.
    auto const* const by = std::get_if<ir::Constant>( &binary.right );
    return !ir::isSigned( binary.type ) && by != nullptr && by->name.empty() && by->value >= 8 &&
           known.isByte( binary.result );
  }
  }
  return false;
}

/// How many times the instruction reads the value, and how many of those
/// read its low byte and nothing else: as an operand of an operator, a
/// comparison or a conversion computed in one byte — but not as the count of
/// a shift, where 256 is not 0 — or as what a store of one byte writes.
struct Reads
{
  std::uint32_t all = 0;
  std::uint32_t low = 0;
};

Reads readsOf( ir::Instruction const& instruction, ir::Value value )
{
  auto const is = [value]( ir::Operand const& operand )
  {
    auto const* const read = std::get_if<ir::Value>( &operand );
    return read != nullptr && read->index == value.index ? 1U : 0U;
  };
  Reads reads;
  ir::Instruction copy = instruction;
  ir::eachOperand( copy, [&reads, &is]( ir::Operand& operand ) { reads.all += is( operand ); } );
  std::visit(
      [&reads, &is]( auto const& operation )
      {
        using Operation = std::decay_t<decltype( operation )>;
        if constexpr ( std::is_same_v<Operation, ir::Binary> )
        {
          if ( ir::sizeOf( operation.type ) == 1 )
          {
            bool const shifts =
                operation.op == ir::BinaryOperator::SHIFT_LEFT || operation.op == ir::BinaryOperator::SHIFT_RIGHT;
            reads.low += is( operation.left ) + ( shifts ? 0U : is( operation.right ) );
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::Compare> )
        {
          if ( ir::sizeOf( operation.type ) == 1 )
          {
            reads.low += is( operation.left ) + is( operation.right );
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::Convert> )
        {
          if ( ir::sizeOf( operation.type ) == 1 )
          {
            reads.low += is( operation.operand );
          }
        }
        else if constexpr ( std::is_same_v<Operation, ir::Store> || std::is_same_v<Operation, ir::StoreElement> ||
                            std::is_same_v<Operation, ir::StoreIndirect> )
        {
          if ( ir::sizeOf( operation.type ) == 1 )
          {
            reads.low += is( operation.value );
          }
        }
      },
      instruction.operation );
  return reads;
}

/// Narrows every 16-bit `&`, `|`, `^`, `+` and `-` whose every reader takes
/// its low byte alone: that byte is made from the operands' low bytes, so the
/// operator is one byte's worth, and its operands are then read low only in
/// turn — which is why this runs until nothing more narrows. See
/// docs/decisions/0140-a-value-only-its-low-byte-is-read-of.md.
void narrowToWhatIsRead( ir::Function& function )
{
  bool changed = true;
  while ( changed )
  {
    changed = false;
    // What reads each value, each instruction counted once however often
    // it names the value.
    std::vector<Reads> reads( function.values.size() );
    std::vector<bool> branchedOn( function.values.size(), false );
    for ( ir::Block const& block : function.blocks )
    {
      for ( ir::Instruction const& instruction : block.instructions )
      {
        std::vector<std::uint32_t> seen;
        ir::Instruction copy = instruction;
        ir::eachOperand( copy,
                         [&seen]( ir::Operand& operand )
                         {
                           if ( auto const* const value = std::get_if<ir::Value>( &operand );
                                value != nullptr && std::ranges::find( seen, value->index ) == seen.end() )
                           {
                             seen.push_back( value->index );
                           }
                         } );
        for ( std::uint32_t const index : seen )
        {
          Reads const here = readsOf( instruction, ir::Value{ index } );
          reads.at( index ).all += here.all;
          reads.at( index ).low += here.low;
        }
      }
      if ( auto const* const condition = std::get_if<ir::Value>( &block.terminator.condition );
           block.terminator.kind == ir::TerminatorKind::BRANCH && condition != nullptr )
      {
        branchedOn.at( condition->index ) = true;
      }
    }

    for ( ir::Block& block : function.blocks )
    {
      for ( ir::Instruction& instruction : block.instructions )
      {
        auto* const binary = std::get_if<ir::Binary>( &instruction.operation );
        if ( binary == nullptr || !isNarrowable( binary->type ) || binary->op == ir::BinaryOperator::SHIFT_LEFT ||
             binary->op == ir::BinaryOperator::SHIFT_RIGHT )
        {
          continue;
        }
        Reads const& read = reads.at( binary->result.index );
        if ( read.all == 0 || read.low != read.all || branchedOn.at( binary->result.index ) )
        {
          continue;
        }
        binary->type = ir::Type::U8;
        function.values.at( binary->result.index ) = ir::Type::U8;
        changed = true;
      }
    }
  }
}

/// Whether two operands are the same thing read twice: one object, or one
/// value.
bool sameOperand( ir::Operand const& one, ir::Operand const& other )
{
  auto const* const object = std::get_if<ir::Object>( &one );
  auto const* const otherObject = std::get_if<ir::Object>( &other );
  if ( object != nullptr && otherObject != nullptr )
  {
    return object->name == otherObject->name && object->type == otherObject->type;
  }
  auto const* const value = std::get_if<ir::Value>( &one );
  auto const* const otherValue = std::get_if<ir::Value>( &other );
  return value != nullptr && otherValue != nullptr && value->index == otherValue->index;
}

/// `x + x` as `x << 1` for a byte: `asl` reads its operand once where the add
/// read it twice, so that a parameter summed with itself may come in `A`.
/// Not for a pair, whose shift copies it first and costs more than the add —
/// see docs/decisions/0148-a-bit-the-carry-already-holds.md.
void doubleOnce( ir::Function& function )
{
  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      auto* const binary = std::get_if<ir::Binary>( &instruction.operation );
      if ( binary != nullptr && binary->op == ir::BinaryOperator::ADD && ir::sizeOf( binary->type ) == 1 &&
           sameOperand( binary->left, binary->right ) )
      {
        binary->op = ir::BinaryOperator::SHIFT_LEFT;
        binary->right = ir::Constant{ .type = ir::Type::U8, .value = 1, .name = {}, .follower = {} };
      }
    }
  }
}

void narrow( ir::Function& function )
{
  // A dispatcher has no body, and a trampoline's is one jump.
  if ( function.blocks.empty() )
  {
    return;
  }
  doubleOnce( function );

  // What each value's high byte holds, which the ranges answer for every value
  // at once: a conversion of a byte and an operator that cannot reach past one
  // are byte-ranged by the sweep itself, so this pass learns nothing of its
  // own any more.
  Known const known{ function };

  // The operators first: one whose high byte is zero is computed in
  // one byte, and every reading of it as two gets `#0` above.
  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      if ( auto* binary = std::get_if<ir::Binary>( &instruction.operation );
           binary != nullptr && isNarrowable( binary->type ) && narrowsToByte( *binary, known ) )
      {
        binary->type = ir::Type::U8;
        function.values.at( binary->result.index ) = ir::Type::U8;
      }
      // A comparison of two values that are bytes in all but name is a
      // comparison of bytes, and needs neither the second `cmp` nor the branch
      // that joins them.
      else if ( auto* compare = std::get_if<ir::Compare>( &instruction.operation );
                compare != nullptr && isNarrowable( compare->type ) && known.highIsZero( compare->left ) &&
                known.highIsZero( compare->right ) )
      {
        compare->type = ir::Type::U8;
      }
    }
  }

  // Then what only its low byte is read of, which the ranges do not see: the
  // `crc ^ b` of `(crc ^ b) & 255` may reach 65535 and is read as one byte.
  narrowToWhatIsRead( function );

  // A widening of a byte that is already zero above is no work at all: every
  // reading of the value becomes a reading of what was widened, and the emitter
  // writes `#0` for its high byte. The instruction then defines nothing and
  // goes, which is also the scratch byte it was going to take.
  // Held by value: the instructions these came from are erased below, so a
  // pointer into one would not outlive the rewrite.
  std::vector<std::optional<ir::Operand>> instead( function.values.size() );
  for ( ir::Block const& block : function.blocks )
  {
    for ( ir::Instruction const& instruction : block.instructions )
    {
      // A byte `&` with 255 is the byte: the mask the narrowing above leaves
      // over what it made one byte.
      if ( auto const* const binary = std::get_if<ir::Binary>( &instruction.operation );
           binary != nullptr && binary->op == ir::BinaryOperator::AND && ir::sizeOf( binary->type ) == 1 )
      {
        for ( auto const& [kept, mask] :
              { std::pair{ &binary->left, &binary->right }, std::pair{ &binary->right, &binary->left } } )
        {
          auto const* const constant = std::get_if<ir::Constant>( mask );
          if ( constant != nullptr && constant->name.empty() && constant->value == 0xFF &&
               std::holds_alternative<ir::Value>( *kept ) && ir::sizeOf( ir::typeOf( *kept, function ) ) == 1 )
          {
            instead.at( binary->result.index ) = *kept;
            break;
          }
        }
        continue;
      }
      auto const* const convert = std::get_if<ir::Convert>( &instruction.operation );
      if ( convert == nullptr )
      {
        continue;
      }
      ir::Type const from = ir::typeOf( convert->operand, function );

      // A conversion to the type the operand already has converts nothing —
      // but only over a value, which is already in a byte of its own. Over an
      // object or a named constant it is what puts the thing somewhere, and
      // the text writes a name differently where it stands for a value than
      // where it stands for an address.
      bool const same = from == convert->type && std::holds_alternative<ir::Value>( convert->operand );
      bool const widensNothing =
          isNarrowable( convert->type ) && known.highIsZero( convert->operand ) && ir::sizeOf( from ) == 1;
      if ( same || widensNothing )
      {
        instead.at( convert->result.index ) = convert->operand;
      }
    }
  }

  // Followed to its end: what stands instead of a value may itself be one
  // that goes, as `(u8)(x & 255)` is the mask's operand by way of the mask.
  auto const rewrite = [&instead]( ir::Operand& operand )
  {
    for ( auto const* value = std::get_if<ir::Value>( &operand );
          value != nullptr && instead.at( value->index ).has_value();
          value = std::get_if<ir::Value>( &operand ) )
    {
      operand = *instead.at( value->index );
    }
  };

  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      ir::eachOperand( instruction, rewrite );
    }
    if ( block.terminator.kind == ir::TerminatorKind::BRANCH )
    {
      rewrite( block.terminator.condition );
    }
  }

  for ( ir::Block& block : function.blocks )
  {
    std::erase_if( block.instructions,
                   [&instead]( ir::Instruction const& instruction )
                   {
                     std::optional<ir::Value> const defined = ir::resultOf( instruction );
                     return defined.has_value() && instead.at( defined->index ).has_value();
                   } );
  }
}

} // namespace

void narrowRanges( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    if ( auto* function = std::get_if<ir::Function>( &definition ) )
    {
      narrow( *function );
    }
  }
}

} // namespace nga::c
