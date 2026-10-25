#include "nga/c/Write.hpp"

#include "nga/c/Front.hpp"
#include "nga/c/Lines.hpp"
#include "nga/c/Liveness.hpp"
#include "nga/c/Loops.hpp"
#include "nga/c/Ranges.hpp"
#include "nga/c/Tracker.hpp"
#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nga::c::detail
{

bool isValue( ir::Operand const& operand, ir::Value value )
{
  auto const* found = std::get_if<ir::Value>( &operand );
  return found != nullptr && found->index == value.index;
}

namespace
{

bool isShiftByValue( ir::Binary const& binary )
{
  return ( binary.op == ir::BinaryOperator::SHIFT_LEFT || binary.op == ir::BinaryOperator::SHIFT_RIGHT ) &&
         !std::holds_alternative<ir::Constant>( binary.right );
}

/// Whether an instruction takes `value` from `A`: as the operand it loads
/// first, or as either operand of one whose operands may trade places, or as
/// any argument of a call. A shift by a value loads its count first, and takes
/// nothing from `A`.
bool takesFromA( ir::Instruction const& instruction, ir::Value value )
{
  if ( auto const* store = std::get_if<ir::Store>( &instruction.operation ) )
  {
    return isValue( store->value, value );
  }
  // A call writes the argument it finds in `A` first.
  if ( auto const* call = std::get_if<ir::Call>( &instruction.operation ) )
  {
    return std::ranges::any_of( call->arguments,
                                [value]( ir::Argument const& argument ) { return isValue( argument.value, value ); } );
  }
  if ( auto const* unary = std::get_if<ir::Unary>( &instruction.operation ) )
  {
    return isValue( unary->operand, value );
  }
  // A 16-bit operator carries a byte at a time and reads both operands from
  // memory, so it takes nothing from `A`. Until narrowing, every operand of one
  // was two bytes and so never in `A` anyway; a narrowed operand is one byte
  // and would be, which is why the width is asked about here and not only the
  // operand — see docs/decisions/0108-a-known-zero-high-byte.md.
  if ( auto const* binary = std::get_if<ir::Binary>( &instruction.operation ) )
  {
    if ( ir::sizeOf( binary->type ) == 2 )
    {
      return false;
    }
    bool const commutes = binary->op == ir::BinaryOperator::ADD || binary->op == ir::BinaryOperator::AND ||
                          binary->op == ir::BinaryOperator::OR || binary->op == ir::BinaryOperator::XOR;
    return ( !isShiftByValue( *binary ) && isValue( binary->left, value ) ) ||
           ( commutes && isValue( binary->right, value ) );
  }
  if ( auto const* compare = std::get_if<ir::Compare>( &instruction.operation ) )
  {
    if ( ir::sizeOf( compare->type ) == 2 )
    {
      return false;
    }
    bool const commutes = compare->op == ir::Comparison::EQUAL || compare->op == ir::Comparison::NOT_EQUAL;
    return isValue( compare->left, value ) || ( commutes && isValue( compare->right, value ) );
  }
  if ( auto const* convert = std::get_if<ir::Convert>( &instruction.operation ) )
  {
    return isValue( convert->operand, value );
  }
  // A load moves its index from `A` into `X`; a store of an element loads its
  // index into `X` first, so only an 8-bit value it stores is in `A`.
  if ( auto const* load = std::get_if<ir::Load>( &instruction.operation ) )
  {
    return isValue( load->index, value );
  }
  if ( auto const* element = std::get_if<ir::StoreElement>( &instruction.operation ) )
  {
    return ir::sizeOf( element->type ) == 1 && isValue( element->value, value );
  }
  // Through a pointer the same way, the index in `Y`.
  if ( auto const* indirect = std::get_if<ir::LoadIndirect>( &instruction.operation ) )
  {
    return isValue( indirect->index, value );
  }
  if ( auto const* indirect = std::get_if<ir::StoreIndirect>( &instruction.operation ) )
  {
    return ir::sizeOf( indirect->type ) == 1 && isValue( indirect->value, value );
  }
  // The entry of a `[[with]]` block moves its index from `A` into `X`.
  if ( auto const* with = std::get_if<ir::EnterWith>( &instruction.operation ) )
  {
    return with->index.has_value() && isValue( *with->index, value );
  }
  return false;
}

/// A loop whose counter the emitter keeps in `X` for as long as it runs, so
/// that the byte it otherwise lives in is read at no turn of the loop — the
/// second step of the order
/// docs/decisions/0062-a-subset-of-c.md#findings-and-the-order-the-allocator-grows-in
/// fixed, decided in docs/decisions/0109-an-induction-variable-in-x.md.
struct CountedLoop
{
  /// Where the test stands, which is also where the loop is entered.
  std::uint32_t header = 0;

  /// Where the counter is stepped, which is the last block of the loop.
  std::uint32_t step = 0;

  /// The block whose `store` sets the counter going, and which falls into the
  /// header.
  std::uint32_t preheader = 0;

  /// Where the loop leaves, which is the header's other branch and the only
  /// way out: the counter is put back into its byte there, once, rather than
  /// at every turn of the test.
  std::uint32_t exit = 0;

  /// Whether it is put back at all: where something after the loop may read
  /// the byte, which for a byte liveness does not follow — a global's — is
  /// always. The exit is then entered from the header alone, since a way
  /// around the loop would find `X` holding nothing of the counter's.
  bool writesBack = false;

  /// The local the counter is, and what it starts at.
  std::string counter;
  std::int64_t from = 0;

  /// Whether its step is `-1`, `dex`, rather than `+1` — see
  /// docs/decisions/0262-a-counter-stepped-down-counts-in-x.md.
  bool down = false;

  /// The most the counter can be inside the loop, where the test says: one
  /// less than a constant it is held below, or 254 where it is held below a
  /// byte whose value only the run knows. Nothing where the test says nothing
  /// of the kind, as `!=` does not.
  std::optional<std::int64_t> atMost{};

  /// The values that are the counter plus a constant, used only as an index,
  /// which the address takes instead: `t[i + 1]` is `t+1,x`. By the value's
  /// index, to the constant. Only where `atMost` says the sum cannot wrap,
  /// since a `u8` sum does and an address does not.
  std::vector<std::pair<std::uint32_t, std::int64_t>> folded{};

  [[nodiscard]] bool holds( std::uint32_t block ) const
  {
    return block >= header && block <= step;
  }

  [[nodiscard]] std::optional<std::int64_t> offsetOf( ir::Operand const& index ) const
  {
    auto const* const value = std::get_if<ir::Value>( &index );
    if ( value == nullptr )
    {
      return std::nullopt;
    }
    for ( auto const& [defined, by] : folded )
    {
      if ( defined == value->index )
      {
        return by;
      }
    }
    return std::nullopt;
  }
};

/// Whether an operand is that local read as a value.
/// The pointer an address that keeps its low byte in `Y` is read through:
/// its low byte zero for as long as a loop that uses it runs, its high byte
/// the address's — see docs/decisions/0266-an-address-keeps-its-low-byte-in-y.md.
constexpr std::string_view PAGE = "__z";

bool isLocal( ir::Operand const& operand, std::string const& name )
{
  auto const* const object = std::get_if<ir::Object>( &operand );
  return object != nullptr && object->name == name;
}

/// Whether the instruction may run while `X` holds the counter instead of its
/// byte.
///
/// Two things would break it. One wants `X` for something of its own: a `jsr`,
/// which leaves it to the callee, a `switch` or a block's entry, which put
/// their own value there, an element two bytes wide whose index is doubled on
/// the way in, or a copy long enough to run its own loop. A byte element whose
/// index is not the counter is read through `Y`, which nothing else under the
/// loop holds from one instruction to the next — see
/// docs/decisions/0133-a-second-index-goes-through-y.md. The other **reads the counter's byte**, which is stale
/// for as long as the register stands for it — `buffer[i] = i + i` reads it
/// twice, once as an index and once as a value, and only the first is `X`.
///
/// One more reading is allowed: the counter plus a constant, where the sum is
/// only ever an index and cannot wrap, which the address takes instead of the
/// register — `t[i + 1]` is `t+1,x`. `folded` names such sums.
bool mayRunUnder( ir::Instruction const& instruction,
                  std::string const& counter,
                  std::span<std::pair<std::uint32_t, std::int64_t> const> folded )
{
  return std::visit(
      [&counter, folded]( auto const& operation ) -> bool
      {
        using Operation = std::decay_t<decltype( operation )>;
        auto const reads = [&counter]( ir::Operand const& operand ) { return isLocal( operand, counter ); };
        auto const isFolded = [folded]( ir::Operand const& operand )
        {
          auto const* const value = std::get_if<ir::Value>( &operand );
          return value != nullptr &&
                 std::ranges::any_of( folded, [value]( auto const& pair ) { return pair.first == value->index; } );
        };

        if constexpr ( std::is_same_v<Operation, ir::Call> || std::is_same_v<Operation, ir::Switch> ||
                       std::is_same_v<Operation, ir::EnterWith> )
        {
          return false;
        }
        else if constexpr ( std::is_same_v<Operation, ir::Load> )
        {
          // An index that is not the counter goes through `Y` instead.
          return !operation.scaled && ir::sizeOf( operation.type ) == 1;
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreElement> )
        {
          return !operation.scaled && ir::sizeOf( operation.type ) == 1 && !reads( operation.value );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Store> )
        {
          return operation.name != counter && !reads( operation.value );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Copy> )
        {
          return operation.bytes <= 4;
        }
        else if constexpr ( std::is_same_v<Operation, ir::Unary> || std::is_same_v<Operation, ir::Convert> )
        {
          return !reads( operation.operand );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Binary> )
        {
          // The sum the address carries; or the counter as the operand `A`
          // is loaded with, which `txa` takes from `X` — on the left, or on
          // the right of an operator that commutes, where the other operand
          // is in memory and not in `A`.
          if ( isFolded( ir::Operand{ operation.result } ) ||
               ( !reads( operation.left ) && !reads( operation.right ) ) )
          {
            return true;
          }
          bool const commutes = operation.op == ir::BinaryOperator::ADD || operation.op == ir::BinaryOperator::AND ||
                                operation.op == ir::BinaryOperator::OR || operation.op == ir::BinaryOperator::XOR;
          bool const shifts =
              operation.op == ir::BinaryOperator::SHIFT_LEFT || operation.op == ir::BinaryOperator::SHIFT_RIGHT;
          ir::Operand const& other = reads( operation.left ) ? operation.right : operation.left;
          return ir::sizeOf( operation.type ) == 1 && ( reads( operation.left ) || commutes ) && !reads( other ) &&
                 !std::holds_alternative<ir::Value>( other ) &&
                 ( !shifts || std::holds_alternative<ir::Constant>( other ) );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Compare> )
        {
          return !reads( operation.left ) && !reads( operation.right );
        }
        else if constexpr ( std::is_same_v<Operation, ir::LoadIndirect> )
        {
          return !reads( operation.pointer ) && !reads( operation.index );
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> )
        {
          return !reads( operation.pointer ) && !reads( operation.index ) && !reads( operation.value );
        }
        else
        {
          return true;
        }
      },
      instruction.operation );
}

/// What the header tests, where it tests a byte counter.
struct CounterTest
{
  std::string counter;
  std::optional<std::int64_t> atMost;
  std::optional<std::int64_t> atLeast{};

  /// The number a `!=` test holds the counter apart from, where it is one.
  std::optional<std::int64_t> apartFrom{};

  /// A signed counter held at or above zero, which only a counter stepped
  /// down may be.
  bool signedAtLeastZero = false;
};

/// The counter the header tests, where it tests one against a constant, an
/// object, or a value the header computed. `cpx` takes any byte that is not in
/// `A`; a value the header computed is in a scratch byte for `<` and `>=`,
/// since neither commutes, and may be in `A` for `==` and `!=`, which do.
std::optional<CounterTest> testedCounter( ir::Block const& header )
{
  if ( header.terminator.kind != ir::TerminatorKind::BRANCH || header.instructions.empty() )
  {
    return std::nullopt;
  }
  auto const* const compare = std::get_if<ir::Compare>( &header.instructions.back().operation );
  if ( compare == nullptr || ir::sizeOf( compare->type ) != 1 )
  {
    return std::nullopt;
  }
  auto const* const object = std::get_if<ir::Object>( &compare->left );
  if ( object == nullptr )
  {
    return std::nullopt;
  }
  // Signed, only `>= 0`: against zero the sign is the answer, which `dex`
  // leaves in `N` — see docs/decisions/0190-the-sign-is-the-answer.md.
  if ( ir::isSigned( compare->type ) )
  {
    auto const* const zero = std::get_if<ir::Constant>( &compare->right );
    if ( compare->op != ir::Comparison::GREATER_OR_EQUAL || zero == nullptr || !zero->name.empty() || zero->value != 0 )
    {
      return std::nullopt;
    }
    return CounterTest{ .counter = object->name,
                        .atMost = std::nullopt,
                        .atLeast = std::nullopt,
                        .apartFrom = std::nullopt,
                        .signedAtLeastZero = true };
  }
  bool const ordered = compare->op == ir::Comparison::LESS || compare->op == ir::Comparison::GREATER_OR_EQUAL;
  if ( std::holds_alternative<ir::Value>( compare->right ) && !ordered )
  {
    return std::nullopt;
  }

  // What the counter is held to while the loop runs: the header's branch goes
  // into the body where its test holds, so `<` holds the counter below the
  // bound and `>=` at it or above, up to 255 and the wrap a `u8` sum makes.
  auto const* const bound = std::get_if<ir::Constant>( &compare->right );
  bool const known = bound != nullptr && bound->name.empty();
  std::optional<std::int64_t> atMost;
  std::optional<std::int64_t> atLeast;
  if ( compare->op == ir::Comparison::LESS )
  {
    atMost = known ? bound->value - 1 : 254;
  }
  else if ( compare->op == ir::Comparison::GREATER_OR_EQUAL && known )
  {
    atLeast = bound->value;
  }
  std::optional<std::int64_t> apartFrom;
  if ( compare->op == ir::Comparison::NOT_EQUAL && known )
  {
    apartFrom = bound->value;
  }
  return CounterTest{
    .counter = object->name, .atMost = atMost, .atLeast = atLeast, .apartFrom = apartFrom, .signedAtLeastZero = false
  };
}

/// The step of the block, where it steps the counter by one up or down and
/// does nothing else: `+1` or `-1`.
std::optional<int> stepOf( ir::Block const& block, std::string const& counter )
{
  if ( block.instructions.size() != 2 )
  {
    return std::nullopt;
  }
  auto const* const sum = std::get_if<ir::Binary>( &block.instructions[0].operation );
  auto const* const store = std::get_if<ir::Store>( &block.instructions[1].operation );
  if ( sum == nullptr || store == nullptr ||
       ( sum->op != ir::BinaryOperator::ADD && sum->op != ir::BinaryOperator::SUBTRACT ) ||
       ir::sizeOf( sum->type ) != 1 || !isLocal( sum->left, counter ) )
  {
    return std::nullopt;
  }
  auto const* const by = std::get_if<ir::Constant>( &sum->right );
  if ( by == nullptr || by->value != 1 || store->name != counter ||
       !std::holds_alternative<ir::Value>( store->value ) ||
       std::get<ir::Value>( store->value ).index != sum->result.index )
  {
    return std::nullopt;
  }
  return sum->op == ir::BinaryOperator::ADD ? 1 : -1;
}

/// What the block before the loop starts the counter at, where it is a
/// constant and the block falls straight into the header.
std::optional<std::int64_t> startsAt( ir::Block const& block, std::string const& counter, std::uint32_t header )
{
  if ( block.terminator.kind != ir::TerminatorKind::JUMP || block.terminator.target != header )
  {
    return std::nullopt;
  }
  // The last store of the counter sets the loop going, and what stands after
  // it in the block runs with `X` holding the counter already: it may not want
  // `X` for anything else, nor read the counter's byte.
  for ( std::size_t at = block.instructions.size(); at-- > 0; )
  {
    auto const* const store = std::get_if<ir::Store>( &block.instructions[at].operation );
    if ( store == nullptr || store->name != counter )
    {
      continue;
    }
    for ( std::size_t later = at + 1; later < block.instructions.size(); ++later )
    {
      if ( !mayRunUnder( block.instructions[later], counter, {} ) )
      {
        return std::nullopt;
      }
    }
    auto const* const from = std::get_if<ir::Constant>( &store->value );
    return from != nullptr && ir::sizeOf( store->type ) == 1 ? std::optional{ from->value } : std::nullopt;
  }
  return std::nullopt;
}

/// The loops of a function whose counter may live in `X`.
///
/// The shape looked for is the one Lowering writes for a `for` or a `while`
/// over a byte: a block that sets the counter and falls into a header that
/// tests it, a body, and a last block that steps it and jumps back. Anything
/// else is left alone — this is an allocator and not a prover.
/// Whether every reader of the value is among these instructions and reads it
/// as the index of a byte element, and as nothing else.
bool readOnlyAsIndex( ir::Function const& function, std::span<ir::Instruction const> after, ir::Value value )
{
  std::uint32_t indexes = 0;
  for ( ir::Instruction const& instruction : after )
  {
    auto const* const load = std::get_if<ir::Load>( &instruction.operation );
    auto const* const store = std::get_if<ir::StoreElement>( &instruction.operation );
    bool const loads =
        load != nullptr && isValue( load->index, value ) && !load->scaled && ir::sizeOf( load->type ) == 1;
    bool const stores = store != nullptr && isValue( store->index, value ) && !store->scaled &&
                        ir::sizeOf( store->type ) == 1 && !isValue( store->value, value );
    indexes += loads || stores ? 1U : 0U;
  }
  return indexes > 0 && indexes == ir::readersOf( function, value );
}

std::vector<CountedLoop> countedLoopsOf( ir::Function const& function,
                                         std::set<std::string> const& volatiles,
                                         std::vector<std::set<std::string>> const& liveIn )
{
  std::vector<std::vector<std::uint32_t>> const before = ir::predecessorsOf( function.blocks );
  std::set<std::string> const followed = liveBytesOf( function );
  std::set<std::string> const own = ownBytesOf( function );
  std::vector<CountedLoop> found;
  for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
  {
    ir::Block const& block = function.blocks[index];
    if ( block.terminator.kind != ir::TerminatorKind::JUMP || block.terminator.target > index )
    {
      continue;
    }
    std::uint32_t const header = block.terminator.target;
    if ( header == 0 )
    {
      continue;
    }

    std::optional<CounterTest> const test = testedCounter( function.blocks[header] );
    std::optional<int> const step = test.has_value() ? stepOf( block, test->counter ) : std::nullopt;
    if ( !step.has_value() )
    {
      continue;
    }
    bool const down = *step < 0;
    // A `volatile` counter is stepped in its byte at every turn — see
    // docs/decisions/0151-volatile.md.
    std::string const& counter = test->counter;
    if ( volatiles.contains( counter ) )
    {
      continue;
    }
    // A byte that is not the Proc's own may be written through a pointer in
    // the loop, which `X` would not see and the `stx` on the way out would
    // overwrite — see docs/decisions/0262-a-counter-stepped-down-counts-in-x.md.
    if ( !own.contains( counter ) &&
         writtenIn( function, Loop{ .header = header, .last = index, .entry = header - 1 } ).anything )
    {
      continue;
    }
    std::optional<std::int64_t> const from = startsAt( function.blocks[header - 1], counter, header );
    if ( !from.has_value() )
    {
      continue;
    }
    // A signed counter held at or above zero counts down from a start that
    // is: its sign then turns only where it leaves.
    if ( test->signedAtLeastZero && ( !down || *from < 0 || *from > 127 ) )
    {
      continue;
    }
    // The block that set it going is the only way in from outside: another
    // would enter with `X` holding nothing of the counter's.
    if ( !std::ranges::all_of( before[header],
                               [header, index]( std::uint32_t way )
                               { return way == header - 1 || ( way >= header && way <= index ); } ) )
    {
      continue;
    }

    // What the counter holds in the body: from its start, which it only ever
    // steps up from, to one below a bound it is held below — or, held apart
    // from zero by `!=` from a start above it, to 255, the step after which
    // wraps it to zero and out. See
    // docs/decisions/0135-a-loop-run-up-to-its-wrap.md.
    std::optional<std::int64_t> atMost = test->atMost;
    std::optional<std::int64_t> atLeast;
    if ( test->atLeast.has_value() )
    {
      atLeast = std::max( *test->atLeast, *from );
    }
    if ( std::optional<std::int64_t> const apart = test->apartFrom; apart.has_value() )
    {
      if ( *apart == 0 && *from > 0 )
      {
        atMost = 0xFF;
      }
      else if ( *from < *apart )
      {
        atMost = *apart - 1;
      }
    }
    if ( atMost.has_value() && *from <= *atMost )
    {
      atLeast = from;
    }
    // Stepped down, it holds its start down to the least the test lets
    // through, where no wrap stands between: one above a number it is held
    // apart from, zero where that number is 255 or where it is held below a
    // bound above its start, and a bound it is held at or above. See
    // docs/decisions/0262-a-counter-stepped-down-counts-in-x.md.
    if ( down )
    {
      atMost.reset();
      atLeast.reset();
      std::optional<std::int64_t> least;
      if ( test->apartFrom.has_value() )
      {
        if ( *test->apartFrom == 0xFF )
        {
          least = 0;
        }
        else if ( *test->apartFrom < *from )
        {
          least = *test->apartFrom + 1;
        }
      }
      else if ( test->atLeast.has_value() && *test->atLeast >= 1 && *test->atLeast <= *from )
      {
        least = *test->atLeast;
      }
      else if ( test->atMost.has_value() && *from <= *test->atMost )
      {
        least = 0;
      }
      if ( least.has_value() )
      {
        atMost = from;
        atLeast = least;
      }
    }

    // The sums of the counter and a constant, and the counter less one, read
    // only as indexes, where what the counter holds keeps the sum from
    // wrapping: those an address may carry instead of `X`, `t+1,x` and
    // `t-240,x`.
    std::vector<std::pair<std::uint32_t, std::int64_t>> folded;
    for ( std::uint32_t at = header; at <= index; ++at )
    {
      std::span<ir::Instruction const> const body{ function.blocks[at].instructions };
      for ( std::size_t here = 0; here < body.size(); ++here )
      {
        auto const* const sum = std::get_if<ir::Binary>( &body[here].operation );
        if ( sum == nullptr || ir::sizeOf( sum->type ) != 1 || !isLocal( sum->left, counter ) ||
             ( sum->op != ir::BinaryOperator::ADD && sum->op != ir::BinaryOperator::SUBTRACT ) )
        {
          continue;
        }
        auto const* const by = std::get_if<ir::Constant>( &sum->right );
        if ( by == nullptr || !by->name.empty() || by->value < 0 )
        {
          continue;
        }
        bool const adds = sum->op == ir::BinaryOperator::ADD;
        bool const fits =
            adds ? atMost.has_value() && *atMost + by->value <= 0xFF : atLeast.has_value() && *atLeast - by->value >= 0;
        if ( fits && readOnlyAsIndex( function, body.subspan( here + 1 ), sum->result ) )
        {
          folded.emplace_back( sum->result.index, adds ? by->value : -by->value );
        }
      }
    }

    // The counter must be the only thing that reads it, and nothing between
    // the header and the step may want `X` for anything else. The header's own
    // test and the step's own two instructions are read above and stand apart.
    bool safe = true;
    for ( std::uint32_t at = header; at <= index && safe; ++at )
    {
      std::span<ir::Instruction const> body{ function.blocks[at].instructions };
      if ( at == header )
      {
        body = body.first( body.size() - 1 );
      }
      else if ( at == index )
      {
        continue;
      }
      for ( ir::Instruction const& instruction : body )
      {
        safe = safe && mayRunUnder( instruction, counter, folded );
      }

      // The header's other branch is the way out, and it is the only one: a
      // `break` would leave by a second edge and want the counter put back
      // there too, which this first cut does not write.
      ir::Terminator const& ends = function.blocks[at].terminator;
      if ( ends.kind == ir::TerminatorKind::BRANCH && isLocal( ends.condition, counter ) )
      {
        safe = false;
      }
      if ( at == header )
      {
        continue;
      }
      if ( ends.kind == ir::TerminatorKind::RETURN || ends.kind == ir::TerminatorKind::TRANSITION ||
           ends.kind == ir::TerminatorKind::DISPATCH )
      {
        // A dispatch reaches its table through `X`, so the counter cannot be
        // there while one runs — as its `switch` instruction already says.
        safe = false;
        continue;
      }
      // A `jump` names one block and leaves `otherwise` at zero, so only a
      // branch has two to check.
      safe = safe && ends.target >= header && ends.target <= index;
      if ( ends.kind == ir::TerminatorKind::BRANCH )
      {
        safe = safe && ends.otherwise >= header && ends.otherwise <= index;
      }
    }

    std::uint32_t const exit = function.blocks[header].terminator.otherwise;
    bool const writesBack =
        exit < liveIn.size() && ( !followed.contains( counter ) || liveIn[exit].contains( counter ) );
    if ( writesBack && before[exit] != std::vector<std::uint32_t>{ header } )
    {
      safe = false;
    }
    if ( safe && exit > index )
    {
      found.push_back( CountedLoop{ .header = header,
                                    .step = index,
                                    .preheader = header - 1,
                                    .exit = exit,
                                    .writesBack = writesBack,
                                    .counter = counter,
                                    .from = *from,
                                    .down = down,
                                    .atMost = atMost,
                                    .folded = folded } );
    }
  }

  // `X` holds one counter, so two loops that overlap cannot both have it. The
  // inner one keeps it: it is the one whose turns are many, and a `for` inside
  // a `for` runs its body as often as the two counts multiplied.
  std::ranges::sort(
      found, []( CountedLoop const& a, CountedLoop const& b ) { return a.step - a.header < b.step - b.header; } );
  std::vector<CountedLoop> kept;
  for ( CountedLoop const& loop : found )
  {
    bool const overlaps = std::ranges::any_of( kept,
                                               [&loop]( CountedLoop const& taken )
                                               { return loop.holds( taken.header ) || taken.holds( loop.header ); } );
    if ( !overlaps )
    {
      kept.push_back( loop );
    }
  }
  return kept;
}

/// A loop whose pair counter the emitter keeps in `Y`, its low byte, and in
/// `X`, its high byte, for as long as it runs: the counter is stepped by a
/// number or an object the loop does not write, read by nothing but its test
/// and an element's address, and dead where the loop leaves. Its bytes are
/// not read or written at any turn — see
/// docs/decisions/0291-a-pair-counter-lives-in-y-and-x.md.
struct ResidentPair
{
  std::uint32_t preheader = 0;
  std::uint32_t header = 0;

  /// The block that steps the counter and jumps back, the loop's last.
  std::uint32_t step = 0;

  /// Where the loop leaves, its header's other branch and the only way out.
  std::uint32_t exit = 0;

  /// The instruction of the preheader from which the counter is in the
  /// registers: the store that sets it going, or the operator before it whose
  /// value that store takes.
  std::size_t from = 0;

  std::string counter;

  [[nodiscard]] bool holds( std::uint32_t block ) const
  {
    return block >= preheader && block <= step;
  }
};

std::vector<ResidentPair> residentPairsOf( ir::Function const& function,
                                           std::set<std::string> const& volatiles,
                                           std::vector<std::set<std::string>> const& liveIn,
                                           std::vector<CountedLoop> const& counted )
{
  std::vector<ResidentPair> found;
  if ( !function.withs.empty() )
  {
    return found;
  }
  std::vector<std::vector<std::uint32_t>> const before = ir::predecessorsOf( function.blocks );
  std::set<std::string> const own = ownBytesOf( function );
  for ( std::uint32_t step = 0; step < function.blocks.size(); ++step )
  {
    ir::Block const& last = function.blocks[step];
    std::uint32_t const header = last.terminator.target;
    if ( last.terminator.kind != ir::TerminatorKind::JUMP || header > step || header == 0 || header + 1 >= step )
    {
      continue;
    }

    // The header is its test and nothing else, of the counter against what
    // the loop does not write, and goes into the block after it.
    ir::Block const& head = function.blocks[header];
    auto const* const test =
        head.instructions.size() == 1 ? std::get_if<ir::Compare>( &head.instructions.front().operation ) : nullptr;
    auto const* const counter = test != nullptr ? std::get_if<ir::Object>( &test->left ) : nullptr;
    if ( counter == nullptr || ir::sizeOf( test->type ) != 2 || ir::sizeOf( counter->type ) != 2 ||
         !own.contains( counter->name ) || volatiles.contains( counter->name ) ||
         head.terminator.kind != ir::TerminatorKind::BRANCH || !isValue( head.terminator.condition, test->result ) )
    {
      continue;
    }
    std::string const& name = counter->name;
    bool const intoTarget = head.terminator.target == header + 1;
    std::uint32_t const exit = intoTarget ? head.terminator.otherwise : head.terminator.target;
    if ( ( !intoTarget && head.terminator.otherwise != header + 1 ) || exit <= step ||
         ( exit < liveIn.size() && liveIn[exit].contains( name ) ) )
    {
      continue;
    }
    Loop const loop{ .header = header, .last = step, .entry = header - 1 };
    Written const written = writtenIn( function, loop );
    if ( isLocal( test->right, name ) || !invariant( test->right, written, own ) )
    {
      continue;
    }

    // The step: the counter plus or less what the loop does not write,
    // stored back, and nothing else.
    auto const* const sum =
        last.instructions.size() == 2 ? std::get_if<ir::Binary>( &last.instructions[0].operation ) : nullptr;
    auto const* const back =
        last.instructions.size() == 2 ? std::get_if<ir::Store>( &last.instructions[1].operation ) : nullptr;
    if ( sum == nullptr || back == nullptr || ir::sizeOf( sum->type ) != 2 ||
         ( sum->op != ir::BinaryOperator::ADD && sum->op != ir::BinaryOperator::SUBTRACT ) ||
         !isLocal( sum->left, name ) || isLocal( sum->right, name ) || !invariant( sum->right, written, own ) ||
         back->name != name || !isValue( back->value, sum->result ) || ir::readersOf( function, sum->result ) != 1 )
    {
      continue;
    }

    // The body reads the counter only as an element's address, the very next
    // instruction reading or writing that byte through it, calls nothing,
    // and goes nowhere but on inside the loop.
    bool safe = true;
    for ( std::uint32_t at = header + 1; at < step && safe; ++at )
    {
      ir::Block const& block = function.blocks[at];
      for ( std::size_t here = 0; here < block.instructions.size() && safe; ++here )
      {
        ir::Instruction const& instruction = block.instructions[here];
        if ( std::holds_alternative<ir::Call>( instruction.operation ) ||
             std::holds_alternative<ir::Switch>( instruction.operation ) ||
             std::holds_alternative<ir::EnterWith>( instruction.operation ) )
        {
          safe = false;
          continue;
        }
        auto const* const address = std::get_if<ir::Binary>( &instruction.operation );
        if ( address != nullptr && address->op == ir::BinaryOperator::ADD && address->type == ir::Type::POINTER &&
             std::holds_alternative<ir::Constant>( address->left ) && isLocal( address->right, name ) )
        {
          ir::Instruction const* const next =
              here + 1 < block.instructions.size() ? &block.instructions[here + 1] : nullptr;
          auto const* const load = next != nullptr ? std::get_if<ir::LoadIndirect>( &next->operation ) : nullptr;
          auto const* const store = next != nullptr ? std::get_if<ir::StoreIndirect>( &next->operation ) : nullptr;
          bool const through =
              ( load != nullptr && isValue( load->pointer, address->result ) && ir::sizeOf( load->type ) == 1 ) ||
              ( store != nullptr && isValue( store->pointer, address->result ) && ir::sizeOf( store->type ) == 1 );
          safe = through && ir::readersOf( function, address->result ) == 1;
          continue;
        }
        ir::eachOperand( instruction,
                         [&safe, &name]( ir::Operand const& operand ) { safe = safe && !isLocal( operand, name ); } );
        // Nor written: the step is its only store.
        auto const* const store = std::get_if<ir::Store>( &instruction.operation );
        auto const* const element = std::get_if<ir::StoreElement>( &instruction.operation );
        auto const* const copy = std::get_if<ir::Copy>( &instruction.operation );
        safe = safe && ( store == nullptr || ir::baseOf( store->name ) != name ) &&
               ( element == nullptr || ir::baseOf( element->name ) != name ) &&
               ( copy == nullptr || ir::baseOf( copy->to.name ) != name );
      }
      ir::Terminator const& end = block.terminator;
      safe = safe && ( end.kind == ir::TerminatorKind::JUMP || end.kind == ir::TerminatorKind::BRANCH ) &&
             end.target > header && end.target <= step &&
             ( end.kind != ir::TerminatorKind::BRANCH || ( end.otherwise > header && end.otherwise <= step ) );
    }
    if ( !safe || !std::ranges::all_of( before[header],
                                        [header, step]( std::uint32_t way )
                                        { return way == header - 1 || ( way >= header && way <= step ); } ) )
    {
      continue;
    }

    // The preheader sets the counter going: from its last store of it, or
    // the operator right before that whose value the store takes, every
    // instruction of the preheader is the loop's as far as the registers go.
    std::vector<ir::Instruction> const& entry = function.blocks[header - 1].instructions;
    std::optional<std::size_t> from;
    for ( std::size_t at = 0; at < entry.size(); ++at )
    {
      if ( auto const* const store = std::get_if<ir::Store>( &entry[at].operation );
           store != nullptr && store->name == name )
      {
        from = at;
        auto const* const value = std::get_if<ir::Value>( &store->value );
        std::optional<ir::Value> const made = at > 0 ? ir::resultOf( entry[at - 1] ) : std::nullopt;
        if ( value != nullptr && made.has_value() && made->index == value->index )
        {
          from = at - 1;
        }
      }
    }
    if ( !from.has_value() || std::ranges::any_of( counted,
                                                   [header, step]( CountedLoop const& other )
                                                   {
                                                     return ( other.header <= step && other.step >= header - 1 ) ||
                                                            other.exit == header - 1 || other.preheader == step;
                                                   } ) )
    {
      continue;
    }
    found.push_back( ResidentPair{
        .preheader = header - 1, .header = header, .step = step, .exit = exit, .from = *from, .counter = name } );
  }

  // Two cannot overlap: the registers hold one pair. The inner one keeps them.
  std::ranges::sort(
      found, []( ResidentPair const& a, ResidentPair const& b ) { return a.step - a.header < b.step - b.header; } );
  std::vector<ResidentPair> kept;
  for ( ResidentPair const& loop : found )
  {
    if ( std::ranges::none_of( kept,
                               [&loop]( ResidentPair const& taken )
                               { return loop.holds( taken.preheader ) || taken.holds( loop.preheader ); } ) )
    {
      kept.push_back( loop );
    }
  }
  return kept;
}

/// An operator the 6502 does in place on a byte of memory: a step of one up
/// or down, or a shift of one place — `inc x`, `dec x`, `asl x`, `lsr x` —
/// where the result goes back to the object it came from and nowhere else.
/// See docs/decisions/0112-an-operator-in-place.md.
struct InPlace
{
  enum class Kind : std::uint8_t
  {
    INCREMENT,
    DECREMENT,
    LEFT,
    RIGHT,
  };
  Kind kind = Kind::INCREMENT;
  std::string object;
  bool wide = false;

  /// An element of an array rather than an object: the index, which goes into
  /// `X`, since `inc`, `dec`, `asl` and `lsr` reach `abs,x` — see
  /// docs/decisions/0159-an-operator-in-place-on-an-element.md. Whether the
  /// index is already counted in bytes is `scaled`.
  std::optional<ir::Operand> index{};
  bool scaled = false;
};

/// Whether two operands read the same: an index the element is reached by
/// before an operator and after it.
bool sameOperand( ir::Operand const& left, ir::Operand const& right )
{
  if ( left.index() != right.index() )
  {
    return false;
  }
  if ( auto const* const value = std::get_if<ir::Value>( &left ) )
  {
    return value->index == std::get<ir::Value>( right ).index;
  }
  if ( auto const* const object = std::get_if<ir::Object>( &left ) )
  {
    return object->name == std::get<ir::Object>( right ).name && object->type == std::get<ir::Object>( right ).type;
  }
  auto const& number = std::get<ir::Constant>( left );
  auto const& other = std::get<ir::Constant>( right );
  return number.value == other.value && number.name == other.name;
}

/// Reads the instruction at `at` and the one after it as an operator that may
/// be done in place, or nothing. The store after it must be the value's only
/// reader: a value two stores read — `x = x + 1; y = x;` after
/// docs/decisions/0111 — would be nowhere once the operator wrote memory and
/// not `A`. A signed shift right is arithmetic and `lsr` is not.
/// The operator a step or a shift of one place is done by, on the byte
/// itself, or nothing where the operator is none of them.
std::optional<InPlace::Kind> inPlaceKind( ir::Binary const& binary )
{
  switch ( binary.op )
  {
  case ir::BinaryOperator::ADD:
    return InPlace::Kind::INCREMENT;
  case ir::BinaryOperator::SUBTRACT:
    return InPlace::Kind::DECREMENT;
  case ir::BinaryOperator::SHIFT_LEFT:
    return InPlace::Kind::LEFT;
  case ir::BinaryOperator::SHIFT_RIGHT:
    // A signed shift right is arithmetic, and `lsr` is not.
    return ir::isSigned( binary.type ) ? std::nullopt : std::optional{ InPlace::Kind::RIGHT };
  case ir::BinaryOperator::AND:
  case ir::BinaryOperator::OR:
  case ir::BinaryOperator::XOR:
    return std::nullopt;
  }
  return std::nullopt;
}

/// An element of an array stepped or shifted where it lies: the read at `at`,
/// the operator after it and the store after that, all of one element, which
/// `inc t,x` does whole — see
/// docs/decisions/0159-an-operator-in-place-on-an-element.md.
std::optional<InPlace> elementInPlaceAt( ir::Function const& function, ir::Block const& block, std::size_t at )
{
  if ( at + 2 >= block.instructions.size() )
  {
    return std::nullopt;
  }
  auto const* const load = std::get_if<ir::Load>( &block.instructions[at].operation );
  auto const* const binary = std::get_if<ir::Binary>( &block.instructions[at + 1].operation );
  auto const* const store = std::get_if<ir::StoreElement>( &block.instructions[at + 2].operation );
  if ( load == nullptr || binary == nullptr || store == nullptr || ir::sizeOf( load->type ) != 1 ||
       !load->high.empty() || !store->high.empty() || load->name != store->name || load->scaled != store->scaled ||
       !sameOperand( load->index, store->index ) )
  {
    return std::nullopt;
  }
  auto const* const by = std::get_if<ir::Constant>( &binary->right );
  if ( !isValue( binary->left, load->result ) || by == nullptr || !by->name.empty() || by->value != 1 ||
       binary->type != load->type || store->type != load->type || !isValue( store->value, binary->result ) ||
       ir::readersOf( function, load->result ) != 1 || ir::readersOf( function, binary->result ) != 1 )
  {
    return std::nullopt;
  }
  std::optional<InPlace::Kind> const kind = inPlaceKind( *binary );
  if ( !kind.has_value() )
  {
    return std::nullopt;
  }
  return InPlace{ .kind = *kind, .object = load->name, .wide = false, .index = load->index, .scaled = load->scaled };
}

std::optional<InPlace> inPlaceAt( ir::Function const& function, ir::Block const& block, std::size_t at )
{
  if ( std::optional<InPlace> element = elementInPlaceAt( function, block, at ); element.has_value() )
  {
    return element;
  }
  if ( at + 1 >= block.instructions.size() )
  {
    return std::nullopt;
  }
  auto const* const binary = std::get_if<ir::Binary>( &block.instructions[at].operation );
  auto const* const store = std::get_if<ir::Store>( &block.instructions[at + 1].operation );
  if ( binary == nullptr || store == nullptr )
  {
    return std::nullopt;
  }
  auto const* const object = std::get_if<ir::Object>( &binary->left );
  auto const* const by = std::get_if<ir::Constant>( &binary->right );
  auto const* const stored = std::get_if<ir::Value>( &store->value );
  if ( object == nullptr || by == nullptr || !by->name.empty() || by->value != 1 || stored == nullptr ||
       stored->index != binary->result.index || store->name != object->name || store->type != binary->type ||
       object->type != binary->type )
  {
    return std::nullopt;
  }

  std::optional<InPlace::Kind> const kind = inPlaceKind( *binary );

  // The store must be the only reader of the value.
  if ( !kind.has_value() || ir::readersOf( function, binary->result ) != 1 )
  {
    return std::nullopt;
  }
  return InPlace{ .kind = *kind, .object = object->name, .wide = ir::sizeOf( binary->type ) == 2 };
}

/// Whether a value is two bytes, which never live in `A`.
bool isWide( ir::Function const& function, ir::Value value )
{
  return ir::sizeOf( function.values.at( value.index ) ) == 2;
}

/// The blocks a terminator names in the text, given which block is written
/// after its own: flow into that one is written as nothing.
std::vector<std::uint32_t> jumpsOf( ir::Terminator const& terminator, std::optional<std::uint32_t> next )
{
  switch ( terminator.kind )
  {
  case ir::TerminatorKind::RETURN:
  case ir::TerminatorKind::TRANSITION:
  case ir::TerminatorKind::FALL:
    return {};
  case ir::TerminatorKind::JUMP:
    if ( next == terminator.target )
    {
      return {};
    }
    return { terminator.target };
  case ir::TerminatorKind::BRANCH:
    if ( next == terminator.otherwise )
    {
      return { terminator.target };
    }
    if ( next == terminator.target )
    {
      return { terminator.otherwise };
    }
    return { terminator.target, terminator.otherwise };
  case ir::TerminatorKind::DISPATCH:
    // Every target stands in the table, the one written next included: the
    // statement names where it goes and never falls through.
    return terminator.targets;
  }
  return {};
}

/// The Jcc taken where a condition holds, and the one taken where it does not.
/// A pair masked below its high byte, read by the next operator a byte at a
/// time: what it is masked from, and the low byte of the mask — see
/// docs/decisions/0285-a-pair-halved-and-doubled-is-its-low-bit-cleared.md.
struct MaskedLow
{
  ir::Operand source;
  std::int64_t mask = 0xFF;
};

struct Jumps
{
  std::string_view whenTrue;
  std::string_view whenFalse;
};

/// Where a loop that tests where it jumps back goes from its step block: into
/// the body while the counter is below the bound, and out to the exit
/// otherwise — see docs/decisions/0128-a-loop-tests-where-it-jumps-back.md.
struct Rotated
{
  std::uint32_t body = 0;
  std::uint32_t exit = 0;

  /// A loop of any kind, its header's test written again whole: not a
  /// counted loop's `cpx`, which 0128 writes against `X` — see
  /// docs/decisions/0156-every-loop-tests-where-it-jumps-back.md.
  bool general = false;
};

/// What the Writer of a unit and the writer of one function share: where
/// the text is read from, what it is written for, and the definitions both
/// write — an object, a constant, a byte of a Proc's own — each into a text
/// of its own, with the declarations in it that may go.
class Output
{
public:
  /// What was written.
  [[nodiscard]] std::string const& text() const
  {
    return mText;
  }

  /// The declarations that go where nothing names them, in the order they
  /// were written: a local a counted loop moved into `X`
  /// ([0109](docs/decisions/0109-an-induction-variable-in-x.md)), one whose
  /// store was dropped as dead (0111), a scratch byte the emitter found
  /// another place for (0114 to 0116). See
  /// docs/decisions/0118-a-byte-nothing-names-is-not-declared.md.
  [[nodiscard]] std::vector<Declared> const& declared() const
  {
    return mDeclared;
  }

protected:
  Output( diag::SourceManager const& sources, std::string_view sourcePath, Cpu cpu, Intent intent )
      : mSources( &sources ), mSourcePath( syntax::quotedLiteral( sourcePath ) ), mCpu( cpu ), mIntent( intent )
  {
  }

  /// One `.source` per statement of the input, ahead of what it became —
  /// never one per instruction. See docs/decisions/0063-source-marks.md.
  void mark( diag::SourceLocation at, std::string_view indent )
  {
    mText.append( indent )
        .append( ".source " )
        .append( mSourcePath )
        .append( ", " )
        .append( std::to_string( mSources->expand( at ).line ) )
        .append( "\n" );
  }

  /// A line of data, written as it stands: it touches no register, so a
  /// function's Tracker has nothing in it to note.
  void datum( std::string_view text )
  {
    mText.append( INDENT ).append( text ).append( "\n" );
  }

  /// A definition after the first is set off by a blank line.
  void separate()
  {
    if ( !mText.empty() )
    {
      mText += '\n';
    }
  }

  /// A Section of its own per object, named by its Label, so that Place moves
  /// and Prune drops each apart — see
  /// docs/decisions/0074-a-section-is-named-by-its-labels.md.
  void global( ir::Global const& object, std::string_view indent = "" )
  {
    if ( !object.stripes.empty() )
    {
      stripes( object, indent );
      return;
    }
    mark( object.at, indent );
    // A pointer lies on the zero page, where `(zp),y` reads through it, and so
    // does an object `[[placement(zeropage)]]` asked for — see
    // docs/decisions/0210-placement-is-declared-in-c-too.md.
    std::string attributes;
    bool const onPageZero = ( object.type == ir::Type::POINTER && !object.count.has_value() ) ||
                            object.placement == ir::Placement::ZEROPAGE;
    if ( onPageZero )
    {
      attributes = "zeropage";
    }
    if ( !object.pane.empty() )
    {
      attributes += ( attributes.empty() ? "" : ", " ) + ( "in " + object.pane );
    }
    // What `const` becomes where it is bytes rather than a Constant: the word
    // that lets the solver put it in ROM although its address is taken, which
    // `.own` alone never says — see
    // docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
    // Not over the zero page, which the word does not go with, and not over a
    // Section of reservations, which has no bytes for it to be about.
    if ( object.isConst && !onPageZero && ( !object.elements.empty() || object.value.has_value() ) )
    {
      attributes += ( attributes.empty() ? "" : ", " );
      attributes += "readonly";
    }
    mText.append( ".section" )
        .append( attributes.empty() ? "" : " " + attributes )
        .append( "\n" )
        .append( object.name )
        .append( "\n" );
    if ( !object.isStatic )
    {
      mText.append( ".export " ).append( object.name ).append( "\n" );
    }
    if ( std::optional<std::uint32_t> const count = object.count; count.has_value() )
    {
      if ( object.elements.empty() )
      {
        datum( ".res " + std::to_string( *count * ir::sizeOf( object.type ) ) );
      }
      // A line holds elements of one width, eight at most: the members of a
      // `struct` may be of two.
      for ( std::size_t at = 0; at < object.elements.size(); )
      {
        std::uint32_t const width = ir::sizeOf( object.elements[at].type );
        std::string items;
        std::size_t element = at;
        while ( element < object.elements.size() && element < at + TABLE_LINE &&
                ir::sizeOf( object.elements[element].type ) == width )
        {
          items.append( element == at ? "" : ", " ).append( valueOf( object.elements[element] ) );
          ++element;
        }
        datum( std::string{ width == 1 ? ".byte " : ".word " } + items );
        at = element;
      }
    }
    else if ( std::optional<ir::Constant> const value = object.value; value.has_value() )
    {
      datum( std::string{ ir::sizeOf( object.type ) == 1 ? ".byte " : ".word " } + valueOf( *value ) );
    }
    else
    {
      datum( ".res " + std::to_string( ir::sizeOf( object.type ) ) );
    }
    mText.append( ".ends\n" );
    implements( object.implements, object.name );
  }

  /// What fills a Slot wherever this Module is present: the Label of the
  /// definition the attribute stood on — see docs/decisions/0031-slots.md.
  void implements( std::string const& slot, std::string const& name )
  {
    if ( slot.empty() )
    {
      return;
    }
    mText.append( ".implements " ).append( slot ).append( ", " ).append( name ).append( "\n" );
  }

  /// A striped array: a Section per stripe, its Label in the array's
  /// Namespace, under the Namespaces of its member's path, and a Temporary
  /// there for a local array whose bytes are its function's only while it
  /// runs — see docs/decisions/0089-a-stripe-is-a-section.md.
  void stripes( ir::Global const& object, std::string_view indent )
  {
    for ( auto const& [path, elements] : object.stripes )
    {
      mark( object.at, indent );
      mText.append( ".namespace " ).append( object.name );
      for ( std::size_t part = 0; part + 1 < path.size(); ++part )
      {
        mText.append( "." ).append( path[part] );
      }
      if ( object.isTemporary )
      {
        mText.append( "\n" )
            .append( path.back() )
            .append( " .temp " )
            .append( std::to_string( object.count.value_or( 0 ) ) )
            .append( "\n.endns\n" );
        continue;
      }
      std::string attributes = object.pane.empty() ? "" : "in " + object.pane;
      if ( object.isConst && !elements.empty() )
      {
        attributes += ( attributes.empty() ? "" : ", " );
        attributes += "readonly";
      }
      mText.append( attributes.empty() ? "\n.section\n" : "\n.section " + attributes + "\n" )
          .append( path.back() )
          .append( "\n" );
      if ( !object.isStatic )
      {
        mText.append( ".export " ).append( path.back() ).append( "\n" );
      }
      if ( elements.empty() )
      {
        datum( ".res " + std::to_string( object.count.value_or( 0 ) ) );
      }
      for ( std::size_t at = 0; at < elements.size(); at += TABLE_LINE )
      {
        std::string items;
        for ( std::size_t element = at; element < std::min( elements.size(), at + TABLE_LINE ); ++element )
        {
          items.append( element == at ? "" : ", " ).append( valueOf( elements[element] ) );
        }
        datum( ".byte " + items );
      }
      mText.append( ".ends\n.endns\n" );
    }
  }

  /// A constant as a data item or a Constant's definition writes it: by the
  /// name of the `const` it is, or as its value, signed where its type is.
  static std::string valueOf( ir::Constant const& constant )
  {
    return constant.name.empty() ? std::to_string( constant.value ) : constant.name;
  }

  /// A `struct` or a `union` as a Namespace of its name holding a Constant per
  /// member, its offset, each exported, so that the assembler reads
  /// `Point.y` — see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
  void members( ir::Aggregate const& aggregate )
  {
    mark( aggregate.at, "" );
    mText.append( ".namespace " ).append( aggregate.name ).append( "\n" );
    std::string names;
    for ( auto const& [name, offset] : aggregate.members )
    {
      names.append( names.empty() ? "" : ", " ).append( name );
    }
    mText.append( ".export " ).append( names ).append( "\n" );
    for ( auto const& [name, offset] : aggregate.members )
    {
      mText.append( name ).append( " = " ).append( std::to_string( offset ) ).append( "\n" );
    }
    mText.append( ".endns\n" );
  }

  /// A `const` given a constant as a Constant of the assembler, exported unless
  /// it is `static` — see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
  void namedConstant( ir::NamedConstant const& constant, std::string_view indent )
  {
    if ( indent.empty() )
    {
      mark( constant.at, "" );
    }
    mText.append( constant.name ).append( " = " ).append( valueOf( constant.value ) ).append( "\n" );
    if ( !constant.isStatic )
    {
      mText.append( ".export " ).append( constant.name ).append( "\n" );
    }
  }

  /// An `enum struct` as a Namespace of its name holding a Constant per
  /// enumerator, so that the assembler reads `Color.RED` — see
  /// docs/decisions/0078-switch-over-an-enum-struct.md#the-type.
  void enumeration( ir::Enumeration const& type )
  {
    mark( type.at, "" );
    if ( type.isScoped )
    {
      mText.append( ".namespace " ).append( type.name ).append( "\n" );
    }
    std::string names;
    for ( std::string const& enumerator : type.enumerators )
    {
      names.append( names.empty() ? "" : ", " ).append( enumerator );
    }
    mText.append( ".export " ).append( names ).append( "\n" );
    for ( std::size_t index = 0; index < type.enumerators.size(); ++index )
    {
      mText.append( type.enumerators[index] ).append( " = " ).append( std::to_string( index ) ).append( "\n" );
    }
    if ( type.isScoped )
    {
      mText.append( ".endns\n" );
    }
  }

  /// A byte or bytes of a Proc: a `.ztemp`, or for a `struct` or a `union` of
  /// more than four bytes a `.temp` — see docs/decisions/0086-a-struct-by-value.md.
  /// `mayGo` where nothing outside the text requires it: a local, against a
  /// parameter or a result, which the caller names whether this Proc writes
  /// it or not.
  void temporary( std::string const& name,
                  ir::Type type,
                  std::uint32_t bytes,
                  bool mayGo,
                  ir::Placement placement = ir::Placement::ZEROPAGE )
  {
    std::uint32_t const size = type == ir::Type::BLOCK ? bytes : ir::sizeOf( type );
    // A block of more than a few bytes lies off the zero page whatever was
    // asked: an instruction reaches none of it in two bytes. A pointer lies on
    // it whatever was asked, since `(zp),y` reads through it — see
    // docs/decisions/0210-placement-is-declared-in-c-too.md.
    bool const off = size > SMALL_BLOCK || ( placement == ir::Placement::ABSOLUTE && type != ir::Type::POINTER );
    std::size_t const begin = mText.size();
    mText.append( name ).append( off ? " .temp " : " .ztemp " ).append( std::to_string( size ) ).append( "\n" );
    if ( mayGo )
    {
      goesUnnamed( name, begin );
    }
  }

  /// A declaration written from `begin` to the end of the text, which goes
  /// where nothing names its byte.
  void goesUnnamed( std::string const& name, std::size_t begin )
  {
    mDeclared.push_back( Declared{ .name = name, .begin = begin, .end = mText.size() } );
  }

  diag::SourceManager const* mSources;
  std::string mSourcePath;

  /// What the Target's processor is, and what the program asked to be
  /// optimised for: `stz` needs both, being the 65SC02's and a trade — see
  /// docs/decisions/0183-a-zero-stored-without-the-accumulator.md.
  Cpu mCpu = Cpu::MOS6502;
  Intent mIntent = Intent::FIT;
  std::string mText;
  std::vector<Declared> mDeclared;
};

/// The text of one function, written on its own: a trial run copies it, and
/// with it nothing of the unit's.
class FunctionWriter : public Output
{
public:
  FunctionWriter(
      diag::SourceManager const& sources, std::string_view sourcePath, Cpu cpu, Intent intent, ir::Unit const& unit )
      : Output( sources, sourcePath, cpu, intent ), mTracker( cpu )
  {
    mTracker.setVolatiles( unit.volatiles );
    for ( ir::Definition const& definition : unit.definitions )
    {
      if ( auto const* object = std::get_if<ir::Global>( &definition ); object != nullptr && object->count.has_value() )
      {
        mArrays.insert( object->name );
      }
    }
  }

  void write( ir::Function const& lowered )
  {
    function( lowered );
  }

  [[nodiscard]] PlaceFacts placeFacts( ir::Function const& lowered )
  {
    function( lowered );
    return PlaceFacts{ .body = std::move( mBody ), .returns = std::move( mReturnHolds ) };
  }

private:
  /// A Proc's scratch byte, in its scope, named as C reserves so that no name
  /// of the program meets it.
  static std::string scratchName( std::uint32_t index )
  {
    return "__t" + std::to_string( index );
  }

  /// A block's label: local to the Proc, and spelled with `@`, which no name
  /// of the program can hold.
  static std::string blockLabel( std::uint32_t index )
  {
    return "@l" + std::to_string( index );
  }

  /// A label the Writer needs inside one instruction's code, `@PREFIXn`.
  std::string localLabel( std::string_view prefix )
  {
    return "@" + std::string{ prefix } + std::to_string( mLocalLabels++ );
  }

  /// A mark where the statement changes, so that the instructions one
  /// statement became stand under one.
  void under( diag::SourceLocation at )
  {
    if ( mMarked != at )
    {
      mark( at, INDENT );
      mMarked = at;
    }
  }

  void line( std::string_view text )
  {
    std::optional<std::string> held;
    if ( std::optional<std::size_t> const on = mResidentOn; on.has_value() )
    {
      held = resident( *on, text );
      if ( !held.has_value() )
      {
        return;
      }
      text = *held;
    }
    emit( text );
  }

  void emit( std::string_view text )
  {
    std::string const written = mTracker.transferred( text );
    if ( !mTracker.note( written ) )
    {
      return;
    }
    mText.append( INDENT ).append( written ).append( "\n" );
  }

  /// The transfer out of a counter's register that waits on the line after
  /// it, written where that line is no comparison.
  void flushResident()
  {
    if ( !mResidentDeferred.empty() )
    {
      emit( std::exchange( mResidentDeferred, std::string{} ) );
    }
  }

  /// A line of a loop whose pair counter `Y` and `X` hold, as it is to be
  /// written. A store of either byte is the transfer into its register, a
  /// load of it into `A` the transfer out, a load into its own register
  /// nothing at all, and a step of it where it lies the step of its register.
  /// Anything else that names the counter, or that writes `X`
  /// or `Y`, refuses the loop, which is then written again without it; so is
  /// a store of it where the flags say something else than `A`, which the
  /// transfer would change. Nothing to write where the line goes.
  [[nodiscard]] std::optional<std::string> resident( std::size_t on, std::string_view text )
  {
    ResidentPair const& pair = mResident[on];
    std::string const low = qualified( pair.counter );
    std::string const high = byteOf( low, 1 );
    std::size_t const space = text.find( ' ' );
    std::string_view const mnemonic = text.substr( 0, space );
    std::string_view const operand = space == std::string_view::npos ? std::string_view{} : text.substr( space + 1 );
    // A byte of the counter compared is compared in its register, which
    // leaves `A` holding what it did: the ways into the loop's body then
    // agree on it.
    if ( !mResidentDeferred.empty() )
    {
      if ( mnemonic == "cmp" && !mentions( operand, low ) )
      {
        std::string const compared = mResidentDeferred == "tya" ? "cpy " : "cpx ";
        mResidentDeferred.clear();
        return compared + std::string{ operand };
      }
      flushResident();
    }
    if ( operand == low || operand == high )
    {
      bool const isLow = operand == low;
      bool& written = mResidentWritten[isLow ? 0 : 1];
      if ( mnemonic == "sta" && mTracker.flagsFromA() )
      {
        // What `A` holds is the byte's now, as the store would have said.
        mTracker.note( text );
        written = true;
        return std::string{ isLow ? "tay" : "tax" };
      }
      if ( written && mnemonic == "lda" )
      {
        if ( !mTracker.holdsInA( operand ) )
        {
          mResidentDeferred = isLow ? "tya" : "txa";
        }
        return std::nullopt;
      }
      if ( written && mnemonic == ( isLow ? "ldy" : "ldx" ) )
      {
        return std::nullopt;
      }
      // A step of the byte where it lies is a step of its register, which
      // leaves the flags as the step would have and `A` as it was: the
      // `inc` / `jne` of a pair counted to its wrap (0288) is `iny` / `jne`.
      if ( written && ( mnemonic == "inc" || mnemonic == "dec" ) )
      {
        std::string_view const step = mnemonic == "inc" ? "in" : "de";
        return std::string{ step } + ( isLow ? "y" : "x" );
      }
      refuseResident( on );
      return std::string{ text };
    }
    static constexpr std::array<std::string_view, 12> WRITES{ "ldx", "ldy", "tax", "tay", "inx", "iny",
                                                              "dex", "dey", "plx", "ply", "tsx", "jsr" };
    if ( mentions( operand, low ) || std::ranges::find( WRITES, mnemonic ) != WRITES.end() )
    {
      refuseResident( on );
    }
    return std::string{ text };
  }

  /// Whether an operand names the object: the name itself, standing alone
  /// as a name and not as part of a longer one.
  [[nodiscard]] static bool mentions( std::string_view operand, std::string_view name )
  {
    auto const partOfName = []( char c )
    { return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_' || c == '.'; };
    for ( std::size_t at = operand.find( name ); at != std::string_view::npos; at = operand.find( name, at + 1 ) )
    {
      std::size_t const end = at + name.size();
      if ( ( at == 0 || !partOfName( operand[at - 1] ) ) && ( end == operand.size() || !partOfName( operand[end] ) ) )
      {
        return true;
      }
    }
    return false;
  }

  void clearPage()
  {
    line( mPageBase.has_value() ? "ldy " + named( *mPageBase ) : std::string{ "ldy #0" } );
    line( "sty " + std::string{ PAGE } );
  }

  void refuseResident( std::size_t on )
  {
    flushResident();
    mResidentRefused.insert( mResident[on].header );
    mResidentOn.reset();
  }

  void label( std::string_view name )
  {
    flushResident();
    mTracker.forget();
    mText.append( name ).append( "\n" );
  }

  /// The instruction of `mBlock` being written, by its place in the block.
  std::size_t mAt = 0;

  /// What `A` held at each `rts` of the function being written, and its body
  /// as written, for a pass that asks where its result and its parameters
  /// could be carried — see docs/decisions/0145-an-argument-in-a-register.md.
  std::vector<std::vector<std::string>> mReturnHolds;
  std::string mBody;

  /// What `A` holds as each block is entered, once the trial run has said what
  /// it holds at the end of every block that leads there: the names every
  /// predecessor agrees on. Empty for the first block, and in the trial itself.
  std::vector<std::vector<std::string>> mBlockEntry;
  std::vector<Carry> mBlockEntryCarry;

  /// What `A` held, and what the carry was, when each block ended, recorded
  /// by the trial run.
  std::vector<std::vector<std::string>> mBlockEnd;
  std::vector<EdgeCarry> mBlockEndCarry;

  /// The same for `X`, carried only into a block entered from one place alone
  /// and outside every loop that counts in `X` — see
  /// docs/decisions/0289-a-pair-only-compared-is-held-in-a-and-x.md.
  std::vector<std::vector<std::string>> mBlockEntryX;
  std::vector<std::vector<std::string>> mBlockEndX;

  /// A run whose text is thrown away: it exists to fill `mBlockEnd`.
  bool mTrial = false;

  /// Whether the last trial entered every block with what it found, so that
  /// the text written after it is that trial's.
  bool mSettled = false;

  /// How many trials a function gets before what is known is taken as final.
  /// Each grows what the one before found, and a loop needs one more than its
  /// nesting; four is more than any program of the suite asks for.
  static constexpr int MAX_TRIALS = 4;

  /// How far back a loop's rotated test may branch, in bytes the text is
  /// estimated at, and still be a Bcc of two: 128, less the two of the
  /// branch. The estimate errs long, a name it does not know being taken for
  /// three bytes; where it errs short the Jcc is five and costs a turn what
  /// the test at the top did, so only bytes are lost — see
  /// docs/decisions/0156-every-loop-tests-where-it-jumps-back.md.
  static constexpr std::size_t ROTATED_REACH = 126;

  /// Whether a block this one leads to is entered with `A` holding the byte,
  /// by what the trials found.
  [[nodiscard]] bool expectedInA( std::uint32_t block, std::string const& name ) const
  {
    ir::Terminator const& leaving = mFunction->blocks[block].terminator;
    std::vector<std::uint32_t> next;
    std::optional<Rotated> const turned = block < mRotatedStep.size() ? mRotatedStep[block] : std::nullopt;
    if ( turned.has_value() )
    {
      // A loop that tests where it jumps back goes into the body or out, and
      // not to the header.
      next.push_back( turned->body );
      next.push_back( turned->exit );
    }
    else if ( leaving.kind == ir::TerminatorKind::JUMP )
    {
      next.push_back( leaving.target );
    }
    else if ( leaving.kind == ir::TerminatorKind::BRANCH )
    {
      next.push_back( leaving.target );
      next.push_back( leaving.otherwise );
    }
    return std::ranges::any_of( next,
                                [this, &name]( std::uint32_t into )
                                {
                                  return into < mBlockEntry.size() &&
                                         std::ranges::find( mBlockEntry[into], name ) != mBlockEntry[into].end();
                                } );
  }

  /// `inc`, `dec`, `asl` or `lsr` on the object, and for a pair the carry or
  /// the borrow taken across by hand: `inc lo` and `inc hi` where the low byte
  /// came round to zero, `dec hi` where the low byte was zero before `dec lo`
  /// — which needs `A` for the test, and is still four instructions against
  /// the six a pair through `A` takes.
  void inPlace( InPlace const& what )
  {
    if ( what.index.has_value() )
    {
      // An element of an array, its index in `X`: `inc t,x` and its kin reach
      // it whole — see
      // docs/decisions/0159-an-operator-in-place-on-an-element.md.
      if ( isANumber( *what.index ) )
      {
        line( std::string{ mnemonicOf( what.kind ) } + " " +
              atAConstantIndex( what.object, {}, *what.index, ir::Type::U8, what.scaled ) );
        return;
      }
      indexIntoX( *what.index, ir::Type::U8, what.scaled );
      line( std::string{ mnemonicOf( what.kind ) } + " " + qualified( what.object ) + addressOffset( *what.index ) +
            ",x" );
      return;
    }
    std::string const low = qualified( what.object );
    std::string const high = byteOf( low, 1 );
    switch ( what.kind )
    {
    case InPlace::Kind::INCREMENT:
      line( "inc " + low );
      if ( what.wide )
      {
        std::string const done = localLabel( "i" );
        line( "bne " + done );
        line( "inc " + high );
        label( done );
      }
      return;
    case InPlace::Kind::DECREMENT:
      if ( what.wide )
      {
        std::string const done = localLabel( "d" );
        line( "lda " + low );
        line( "bne " + done );
        line( "dec " + high );
        label( done );
      }
      line( "dec " + low );
      return;
    case InPlace::Kind::LEFT:
      line( "asl " + low );
      if ( what.wide )
      {
        line( "rol " + high );
      }
      return;
    case InPlace::Kind::RIGHT:
      if ( what.wide )
      {
        line( "lsr " + high );
        line( "ror " + low );
      }
      else
      {
        line( "lsr " + low );
      }
      return;
    }
  }

  /// The instruction a step or a shift of one place is done by.
  [[nodiscard]] static std::string_view mnemonicOf( InPlace::Kind kind )
  {
    switch ( kind )
    {
    case InPlace::Kind::INCREMENT:
      return "inc";
    case InPlace::Kind::DECREMENT:
      return "dec";
    case InPlace::Kind::LEFT:
      return "asl";
    case InPlace::Kind::RIGHT:
      return "lsr";
    }
    return "inc";
  }

  /// The end of a Proc, the Proc chained after it, and the Slot its entry
  /// fills where it fills one.
  void endProc( ir::Function const& lowered )
  {
    mText.append( ".endp" ).append( lowered.then.empty() ? "" : " then " + lowered.then ).append( "\n" );
    implements( lowered.implements, lowered.name );
  }

  /// A function type's Proc: `__ptr`, the parameters and `__ret` as its
  /// Temporaries, and the jump a call through a pointer of the type takes to
  /// reach the member — see docs/decisions/0065-handlers.md.
  void trampoline( ir::Function const& lowered )
  {
    mark( lowered.at, "" );
    mText.append( ".export " ).append( lowered.name ).append( "\n" );
    mText.append( ".proc " ).append( lowered.name ).append( "\n" );
    line( "jmp (" + std::string{ TRAMPOLINE_POINTER } + ")" );
    temporary( std::string{ TRAMPOLINE_POINTER }, ir::Type::POINTER, 0, false );
    for ( ir::Local const& parameter : lowered.parameters )
    {
      line( ".declare arg " + declared( parameter.type, parameter.bytes ) );
      temporary( parameter.name, parameter.type, parameter.bytes, false );
    }
    if ( lowered.result.has_value() )
    {
      line( ".declare ret " + declared( *lowered.result, lowered.resultBytes ) );
      temporary( std::string{ RESULT }, *lowered.result, lowered.resultBytes, false );
    }
    mText.append( ".endp\n" );
  }

  void function( ir::Function const& lowered )
  {
    if ( lowered.isTrampoline )
    {
      trampoline( lowered );
      return;
    }
    // Exported ahead of the Proc, since an `.export` inside it would name
    // `NAME.NAME`; a static function is not exported at all.
    mark( lowered.at, "" );
    if ( !lowered.isStatic )
    {
      mText.append( ".export " ).append( lowered.name ).append( "\n" );
    }
    mText.append( ".proc " )
        .append( lowered.name )
        .append( lowered.pane.empty() ? "" : ", in " + lowered.pane )
        .append( lowered.under.empty() ? "" : ", under " + lowered.under )
        // `as` names the Proc a function type is, and a Slot is none: a Proc
        // that fills a Slot reads the Slot's own Temporaries, and the
        // `.implements` below is what says so — see
        // docs/decisions/0174-a-slot-takes-and-returns.md.
        .append( lowered.memberOf.empty() || lowered.memberOf == lowered.implements ? "" : ", as " + lowered.memberOf )
        .append( "\n" );

    mFunction = &lowered;
    mMarked.reset();
    mTracker.forget();
    mLocalLabels = 0;
    plan( lowered );

    // What `A` holds where a block is entered from more than one place is the
    // meet of what it held where each of them left off — which is not known
    // until each has been written. So the function is written on a copy first,
    // its text thrown away and its block ends kept, and each block is then
    // entered with what every way into it agrees on. Once is not enough where
    // a loop is concerned: the first trial enters the loop's last block with
    // nothing, so the edge back to the header carries nothing, and only the
    // next trial, entering that block with what the trial before found, sees
    // what comes round. Every trial enters each block with a subset of the
    // truth, so every one is sound, and they stop when nothing grows. See
    // docs/decisions/0110-a-value-stays-in-a.md.
    //
    // Which loops are too long to test where they jump back: a Jcc that far
    // is five bytes and a `jmp`, which is what the test at the top cost. Read
    // off the last trial, which rotates every loop it may; where one is too
    // long, the trials are run again without it, so that every trial the
    // text is entered by writes the same blocks.
    mResidentDropped.clear();
    while ( true )
    {
      mFarLatches.clear();
      for ( int sizing = 0; sizing < 2; ++sizing )
      {
        rotate( lowered );
        mBlockEntry.assign( lowered.blocks.size(), {} );
        mBlockEntryCarry.assign( lowered.blocks.size(), Carry::UNKNOWN );
        mBlockEntryX.assign( lowered.blocks.size(), {} );
        std::map<std::uint32_t, std::size_t> reach;
        mSettled = false;
        for ( int round = 0; round < MAX_TRIALS; ++round )
        {
          FunctionWriter trial{ *this };
          trial.mTrial = true;
          trial.mText.clear();
          trial.writeBody( lowered );
          reach = trial.mRotatedReach;
          std::vector<std::vector<std::string>> const grown =
              entriesFrom( lowered, trial.mBlockEnd, trial.mRotatedStep, trial.mFallsInto );
          std::vector<Carry> const carries =
              carriesFrom( lowered, trial.mBlockEndCarry, trial.mRotatedStep, trial.mFallsInto );
          std::vector<std::vector<std::string>> grownX =
              entriesFrom( lowered, trial.mBlockEndX, trial.mRotatedStep, trial.mFallsInto );
          for ( std::uint32_t index = 0; index < grownX.size(); ++index )
          {
            if ( !carriesX( lowered, index ) )
            {
              grownX[index].clear();
            }
          }
          if ( grown == mBlockEntry && carries == mBlockEntryCarry && grownX == mBlockEntryX )
          {
            mSettled = true;
            break;
          }
          mBlockEntry = grown;
          mBlockEntryCarry = carries;
          mBlockEntryX = grownX;
        }
        std::size_t const far = mFarLatches.size();
        for ( auto const& [latch, bytes] : reach )
        {
          if ( bytes > ROTATED_REACH )
          {
            mFarLatches.insert( latch );
          }
        }
        if ( mFarLatches.size() == far )
        {
          break;
        }
      }
      rotate( lowered );
      // A loop whose counter the registers were to hold is written as it will
      // be, and one whose text refused it is planned again without it.
      if ( std::ranges::any_of(
               mResident, [this]( ResidentPair const& pair ) { return !mResidentDropped.contains( pair.header ); } ) )
      {
        FunctionWriter attempt{ *this };
        attempt.writeBody( lowered );
        if ( !attempt.mResidentRefused.empty() )
        {
          mResidentDropped.insert( attempt.mResidentRefused.begin(), attempt.mResidentRefused.end() );
          continue;
        }
      }
      break;
    }
    writeBody( lowered );
  }

  /// What the function's blocks are written by, decided from its IR alone
  /// and once: where each value lives, which loops count in `X`, what is
  /// read where it lies, which block falls into the next. Every trial and the
  /// text after them write by the same plans, and writing changes none.
  void plan( ir::Function const& lowered )
  {
    mScratchSizes.clear();
    mScratchPointers.clear();
    mScratch.assign( lowered.values.size(), std::nullopt );
    mDirect.assign( lowered.values.size(), std::string{} );
    mFused.assign( lowered.blocks.size(), false );
    mLiveIn = liveInOf( lowered );
    mLoops = countedLoopsOf( lowered, mTracker.volatiles(), mLiveIn );
    mResident = residentPairsOf( lowered, mTracker.volatiles(), mLiveIn, mLoops );
    mInPlace.assign( lowered.blocks.size(), {} );
    for ( std::uint32_t index = 0; index < lowered.blocks.size(); ++index )
    {
      ir::Block const& block = lowered.blocks[index];
      mInPlace[index].resize( block.instructions.size() );
      for ( std::size_t at = 0; at < block.instructions.size(); ++at )
      {
        mInPlace[index][at] = inPlaceAt( lowered, block, at );
      }
    }
    mStoredDirectly.assign( lowered.blocks.size(), {} );

    // The bytes of the Proc's own, which a macro body of a `[[with]]` block
    // names qualified, since a body's names resolve at the Module's top level
    // — see docs/decisions/0096-panes-in-c.md.
    mProcBytes.clear();
    for ( ir::Local const& parameter : lowered.parameters )
    {
      mProcBytes.insert( parameter.name );
    }
    mProcBytes.insert( std::string{ RESULT } );
    for ( ir::Local const& local : lowered.locals )
    {
      mProcBytes.insert( local.name );
    }
    std::set<std::string> ownScalars;
    for ( ir::Local const& byte : lowered.parameters )
    {
      if ( byte.type != ir::Type::BLOCK )
      {
        ownScalars.insert( qualified( byte.name ) );
      }
    }
    for ( ir::Local const& byte : lowered.locals )
    {
      if ( byte.type != ir::Type::BLOCK )
      {
        ownScalars.insert( qualified( byte.name ) );
      }
    }
    if ( lowered.result.has_value() && *lowered.result != ir::Type::BLOCK && lowered.memberOf.empty() )
    {
      ownScalars.insert( qualified( std::string{ RESULT } ) );
    }
    mTracker.setOwnScalars( std::move( ownScalars ) );
    for ( ir::Global const& object : lowered.sections )
    {
      mProcBytes.insert( object.name );
      if ( object.count.has_value() )
      {
        mArrays.insert( object.name );
      }
    }
    for ( ir::NamedConstant const& constant : lowered.constants )
    {
      mProcBytes.insert( constant.name );
    }

    // Where each value lives: in `A` when what comes right after the
    // instruction that defines it takes it from there, and otherwise in a
    // scratch byte of its own — see
    // docs/decisions/0076-arithmetic-in-the-subset.md. A value is used once,
    // by its own statement. A comparison a branch reads right after it is
    // read from the flags, and never becomes 0 or 1. A 16-bit value is never in
    // `A`: it is written straight into the object the next instruction stores
    // it in, or else into a pair of scratch bytes — see
    // docs/decisions/0084-widths-data-arrays-pointers-aggregates.md.
    for ( std::uint32_t index = 0; index < lowered.blocks.size(); ++index )
    {
      ir::Block const& block = lowered.blocks[index];
      std::vector<ir::Instruction> const& instructions = block.instructions;
      mStoredDirectly[index].assign( instructions.size(), false );
      std::optional<ir::Value> const last = instructions.empty() ? std::nullopt : ir::resultOf( instructions.back() );
      bool const branchesOnLast = last.has_value() && block.terminator.kind == ir::TerminatorKind::BRANCH &&
                                  isValue( block.terminator.condition, *last );
      mFused[index] = branchesOnLast && std::holds_alternative<ir::Compare>( instructions.back().operation );

      for ( std::size_t at = 0; at < instructions.size(); ++at )
      {
        std::optional<ir::Value> const defined = ir::resultOf( instructions[at] );
        if ( !defined.has_value() )
        {
          continue;
        }
        if ( isWide( lowered, *defined ) )
        {
          // Each byte of the result is written after the bytes of the operands
          // it depends on are read, so the object may be an operand too.
          auto const* const store =
              at + 1 < instructions.size() ? std::get_if<ir::Store>( &instructions[at + 1].operation ) : nullptr;
          if ( store != nullptr && isValue( store->value, *defined ) && ir::sizeOf( store->type ) == 2 )
          {
            mDirect[defined->index] = store->name;
            mStoredDirectly[index][at + 1] = true;
          }
          else if ( std::string const argument = argumentTaking( lowered, instructions, at, *defined );
                    !argument.empty() )
          {
            mDirect[defined->index] = argument;
          }
          else
          {
            mScratch[defined->index] = scratch( 2, lowered.values.at( defined->index ) == ir::Type::POINTER );
          }
          continue;
        }
        // A byte the next instruction takes from `A` is kept there, but only
        // where nothing else reads it: `BASE + ++sp` stores the stepped byte
        // and then reads it again, by then from wherever it lives.
        bool const kept = at + 1 == instructions.size()
                              ? branchesOnLast
                              : takesFromA( instructions[at + 1], *defined ) && ir::readersOf( lowered, *defined ) == 1;
        if ( !kept )
        {
          mScratch[defined->index] = scratch( 1 );
        }
      }
    }

    // Which loops test where they jump back, once every fused compare is
    // known; then the labels, which those loops move from the header to the
    // body: nothing jumps to a rotated loop's header any more, and its step
    // branches into the body.
    mRotatedStep.assign( lowered.blocks.size(), std::nullopt );
    for ( CountedLoop const& loop : mLoops )
    {
      if ( rotates( loop ) )
      {
        mRotatedStep[loop.step] = Rotated{ .body = loop.header + 1, .exit = loop.exit };
      }
    }
    planByteViews( lowered );
    planSharedPairs( lowered );
    planFoldedIndexes( lowered );

    // What the machine can do in memory, decided once the fused compares are
    // known and the Proc's own names can be qualified.
    mAsOperand.assign( lowered.values.size(), std::string{} );
    mAtName.assign( lowered.values.size(), std::string{} );
    mIndexToLoad.assign( lowered.values.size(), std::string{} );
    mDeferred.assign( lowered.blocks.size(), {} );
    mSecondIndex.assign( lowered.blocks.size(), {} );
    mMoved.assign( lowered.blocks.size(), {} );
    mBitTest.assign( lowered.blocks.size(), std::nullopt );
    mShiftedIn.assign( lowered.blocks.size(), false );
    mShiftedBefore.assign( lowered.blocks.size(), false );
    for ( std::uint32_t index = 0; index < lowered.blocks.size(); ++index )
    {
      mDeferred[index].assign( lowered.blocks[index].instructions.size(), false );
      mSecondIndex[index].assign( lowered.blocks[index].instructions.size(), false );
      mMoved[index].assign( lowered.blocks[index].instructions.size(), false );
      planOperandsInMemory( lowered, index );
      planSecondIndex( lowered, index );
      planPairsMoved( lowered, index );
      planIndirectOperands( lowered, index );
      planBitTest( lowered, index );
      planCallResultsInPlace( lowered, index );
    }

    planPagedAddresses( lowered );
    planIndexSteps( lowered );
    planTableReads( lowered );
    planShiftsBySeven( lowered );
    planKnownBounds( lowered );
    planBoundHighs( lowered );
    planLowInX( lowered );
    planMaskedOperands( lowered );

    // Last, since it reads every other plan: which scratch bytes were given
    // for an instruction that turned out not to be written, and then the
    // scratch bytes renumbered without them.
    for ( std::uint32_t index = 0; index < lowered.blocks.size(); ++index )
    {
      planSilentGaps( lowered, index );
    }
    compactScratch();
    mFallsInto.assign( lowered.blocks.size(), std::nullopt );
    for ( std::uint32_t index = 0; index < lowered.blocks.size(); ++index )
    {
      planFallInto( lowered, index );
    }

    // An element stepped where it lies needs its index in `X` and its read
    // written here: not where a second index holds `Y`
    // ([0133](docs/decisions/0133-a-second-index-goes-through-y.md)), and not
    // where another plan already found the read somewhere else to be.
    for ( std::uint32_t index = 0; index < lowered.blocks.size(); ++index )
    {
      ir::Block const& block = lowered.blocks[index];
      for ( std::size_t at = 0; at < block.instructions.size(); ++at )
      {
        std::optional<InPlace>& plan = mInPlace[index][at];
        if ( !plan.has_value() || !plan->index.has_value() )
        {
          continue;
        }
        auto const& load = std::get<ir::Load>( block.instructions[at].operation );
        if ( throughY( index, load.index, load.type, load.scaled ) || mDeferred[index][at] ||
             !mAsOperand[load.result.index].empty() || mFallsInto[index].has_value() )
        {
          plan.reset();
        }
      }
    }
  }

  /// Which loops other than a counted one test where they jump back, and then
  /// the blocks a label is written for: asked again in each round of trials,
  /// since a loop found too long to branch back is not turned in the next.
  void rotate( ir::Function const& lowered )
  {
    for ( std::optional<Rotated>& turned : mRotatedStep )
    {
      if ( turned.has_value() && turned->general )
      {
        turned.reset();
      }
    }
    // Any other loop whose header is its test and nothing else, a compare of
    // objects and constants the branch reads: the test is written again where
    // the loop jumps back, as 0128 writes a counted loop's.
    for ( std::uint32_t index = 0; index < lowered.blocks.size() && lowered.withs.empty(); ++index )
    {
      ir::Terminator const& back = lowered.blocks[index].terminator;
      bool const counted =
          std::ranges::any_of( mLoops, [index]( CountedLoop const& loop ) { return loop.step == index; } );
      if ( mRotatedStep[index].has_value() || counted || back.kind != ir::TerminatorKind::JUMP || back.target > index ||
           mFallsInto[index].has_value() || mFarLatches.contains( index ) )
      {
        continue;
      }
      std::uint32_t const head = back.target;
      ir::Block const& header = lowered.blocks[head];
      ir::Terminator const& test = header.terminator;
      if ( !mFused[head] || header.instructions.size() != 1 || test.kind != ir::TerminatorKind::BRANCH ||
           mBitTest[head].has_value() )
      {
        continue;
      }
      bool const intoTarget = test.target == head + 1;
      std::uint32_t const exit = intoTarget ? test.otherwise : test.target;
      if ( ( !intoTarget && test.otherwise != head + 1 ) || exit == head + 1 || head + 1 > index ||
           nextOf( index ) != exit )
      {
        continue;
      }
      mRotatedStep[index] = Rotated{ .body = head + 1, .exit = exit, .general = true };
    }
    mLabelled.assign( lowered.blocks.size(), false );
    for ( std::uint32_t index = 0; index < lowered.blocks.size(); ++index )
    {
      if ( std::optional<Rotated> const& turned = mRotatedStep[index]; turned.has_value() )
      {
        mLabelled[turned->body] = true;
        continue;
      }
      for ( std::uint32_t const target : jumpsOf( lowered.blocks[index].terminator, nextOf( index ) ) )
      {
        mLabelled[target] = true;
      }
    }
  }

  /// The blocks, by the plans, and what follows them: one trial's run, or the
  /// text itself.
  void writeBody( ir::Function const& lowered )
  {
    mBlockEnd.assign( lowered.blocks.size(), {} );
    mBlockEndCarry.assign( lowered.blocks.size(), {} );
    mBlockEndX.assign( lowered.blocks.size(), {} );
    mBlockTextAt.assign( lowered.blocks.size(), 0 );
    mResidentOn.reset();
    mResidentRefused.clear();
    mResidentDeferred.clear();
    mRotatedReach.clear();
    mInBody = 0;
    mBodies.clear();
    mPendingBodies.clear();
    std::vector<std::string> outside;

    // A parameter that comes in a register is there when the first block is
    // entered, and what reads it from its byte reads it from there — see
    // docs/decisions/0145-an-argument-in-a-register.md.
    mReturnHolds.clear();
    std::size_t const bodyBegin = mText.size();
    for ( ir::Local const& parameter : lowered.parameters )
    {
      if ( parameter.place == "a" )
      {
        mTracker.holdInA( qualified( parameter.name ) );
      }
      else if ( parameter.place == "x" )
      {
        mTracker.holdInX( qualified( parameter.name ) );
      }
    }

    for ( std::uint32_t index = 0; index < lowered.blocks.size(); ++index )
    {
      // A `[[with]]` block's blocks are written apart, as the body of the
      // macro its entry used: the text so far is set aside while they are.
      for ( ir::WithRegion const& region : lowered.withs )
      {
        if ( region.begin == index )
        {
          outside.push_back( std::exchange( mText, std::string{} ) );
          mMarked.reset();
          mTracker.forget();
          ++mInBody;
        }
      }
      ir::Block const& block = lowered.blocks[index];
      mBlock = index;
      mSkipAfterInPlace = 0;
      mBlockTextAt[index] = mText.size();
      // The registers hold both bytes of the counter by the time its loop's
      // header is reached, or the loop is not one of these.
      if ( std::optional<std::size_t> const on = mResidentOn;
           on.has_value() && index == mResident[*on].header && !( mResidentWritten[0] && mResidentWritten[1] ) )
      {
        refuseResident( *on );
      }
      if ( mLabelled[index] )
      {
        label( blockLabel( index ) );
        mTracker.enter( mBlockEntry[index], mBlockEntryCarry[index] );
        if ( index < mBlockEntryX.size() )
        {
          for ( std::string const& name : mBlockEntryX[index] )
          {
            mTracker.holdInX( name );
          }
        }
      }

      // Where a counted loop leaves, its counter goes back into its byte —
      // once, and not at every turn of the test, and not at all where nothing
      // after the loop reads it.
      for ( CountedLoop const& loop : mLoops )
      {
        if ( loop.exit == index && loop.writesBack )
        {
          line( "stx " + qualified( loop.counter ) );
        }
      }

      // The step of such a loop is `inx` or `dex` and nothing else. Written
      // here rather than by skipping the block, so that a `[[with]]` region
      // ending on it is still closed below.
      CountedLoop const* const stepping = loopAt( index );
      bool const isStep = stepping != nullptr && stepping->step == index;
      if ( isStep )
      {
        under( block.instructions.front().at );
        line( stepping->down ? "dex" : "inx" );
      }

      bool pageCleared = false;
      for ( std::size_t at = 0; at < block.instructions.size() && !isStep; ++at )
      {
        for ( std::size_t pair = 0; pair < mResident.size(); ++pair )
        {
          if ( mResident[pair].preheader == index && mResident[pair].from == at &&
               !mResidentDropped.contains( mResident[pair].header ) )
          {
            // `PAGE` is cleared through `Y` before `Y` holds the counter.
            if ( mClearsPage.contains( index ) )
            {
              clearPage();
              pageCleared = true;
            }
            mResidentOn = pair;
            mResidentWritten = {};
          }
        }
        // A comparison the branch reads is written with the branch, a store of
        // a value written straight into its object is that value's, a sum the
        // address takes is nobody's, and a store an operator in place has
        // already made is made.
        // Taken first, on its own: a store an operator in place has made is also
        // the one a wide value would have written straight into its object, and
        // a short-circuit past it would leave the flag set for the instruction
        // after.
        if ( mSkipAfterInPlace > 0 )
        {
          --mSkipAfterInPlace;
          continue;
        }
        bool const madeInPlace = std::exchange( mStoredInPlace, false );
        if ( mFallsInto[index].has_value() && at + 1 == block.instructions.size() )
        {
          // The store the block written next makes for both.
          continue;
        }
        if ( at == 0 && mShiftedBefore[index] )
        {
          // The test before shifted the pair where it lies.
          continue;
        }
        if ( at == 0 && mShiftedIn[index] )
        {
          // The shift the test before wrote: `A` holds what it computes.
          keep( std::get<ir::Binary>( block.instructions.front().operation ).result );
          continue;
        }
        if ( madeInPlace || ( mFused[index] && at + 1 == block.instructions.size() ) || mStoredDirectly[index][at] ||
             isFoldedSum( block.instructions[at] ) || mDeferred[index][at] || mMoved[index][at] ||
             ( index < mStepSilent.size() && at < mStepSilent[index].size() && mStepSilent[index][at] ) )
        {
          continue;
        }
        std::optional<InPlace> const& inPlace = mInPlace[index][at];
        under( block.instructions[at].at );

        // An operator the machine does on the byte itself, where `A` does not
        // hold that byte already and no block this one leads to is entered
        // counting on `A` holding it: with it there, the operator on `A` and
        // one store cost the same and leave `A` holding the result for
        // whatever reads it next — at the head of a loop above all. The trials
        // go in place too, so that what they say a block is entered with is
        // what the operator in place leaves. Where they settled, the real run
        // is the last of them and goes in place as it did; where they did
        // not, it takes the way through `A` where a block it leads to was
        // promised the object there, which gives more than was promised and
        // never less. See docs/decisions/0112-an-operator-in-place.md,
        // docs/decisions/0157-a-wide-comparison-high-byte-first.md for the
        // pair whose high byte the promise names, and
        // docs/decisions/0292-a-settled-run-keeps-its-operators-in-place.md.
        // Of a pair, the high byte is asked about too: the way through `A`
        // the trials take leaves that byte in it, and what a block this one
        // leads to was promised is the high byte, not the pair's name.
        std::string const inPlaceHigh =
            inPlace.has_value() && inPlace->wide ? byteOf( qualified( inPlace->object ), 1 ) : std::string{};
        bool const counted = inPlace.has_value() && !mTrial && !mSettled &&
                             ( expectedInA( index, qualified( inPlace->object ) ) ||
                               ( !inPlaceHigh.empty() && expectedInA( index, inPlaceHigh ) ) );
        if ( inPlace.has_value() && !counted && !mTracker.isVolatile( inPlace->object ) &&
             !mTracker.holdsInA( qualified( inPlace->object ) ) )
        {
          this->inPlace( *inPlace );
          // An element's read, its operator and its store are all written by
          // the one instruction; an object's store alone is.
          mSkipAfterInPlace = inPlace->index.has_value() ? 2 : 0;
          mStoredInPlace = !inPlace->index.has_value();
          continue;
        }
        mAt = at;
        instruction( block.instructions[at] );
      }
      // The loop this block enters reads addresses through `PAGE`, whose low
      // byte nothing else writes while it runs.
      if ( mClearsPage.contains( index ) && !pageCleared )
      {
        clearPage();
      }
      mTracker.forgetLastBranch();
      if ( !mFallsInto[index].has_value() )
      {
        terminator( index );
      }
      if ( mResidentOn.has_value() && index == mResident[*mResidentOn].step )
      {
        flushResident();
        mResidentOn.reset();
      }
      // An element held through an index is not carried into another block,
      // whose ways in may have the register at different elements.
      mBlockEnd[index] = mTracker.inA();
      if ( mEarlyHolds.has_value() )
      {
        std::erase_if( mBlockEnd[index],
                       [this]( std::string const& name )
                       { return std::ranges::find( *mEarlyHolds, name ) == mEarlyHolds->end(); } );
      }
      std::erase_if( mBlockEnd[index], []( std::string const& name ) { return name.contains( ',' ); } );
      mBlockEndX[index] = mTracker.inX();
      if ( mEarlyHoldsX.has_value() )
      {
        std::erase_if( mBlockEndX[index],
                       [this]( std::string const& name )
                       { return std::ranges::find( *mEarlyHoldsX, name ) == mEarlyHoldsX->end(); } );
      }
      std::erase_if( mBlockEndX[index], []( std::string const& name ) { return name.contains( ',' ); } );
      mEarlyHoldsX.reset();
      mBlockEndCarry[index] =
          mTracker.lastBranch().value_or( EdgeCarry{ .taken = mTracker.carry(), .fallen = mTracker.carry() } );
      if ( mEarlyCarry )
      {
        mBlockEndCarry[index].taken = Carry::UNKNOWN;
      }
      mEarlyHolds.reset();
      mEarlyCarry = false;
      for ( ir::WithRegion const& region : lowered.withs )
      {
        if ( region.end == index + 1 )
        {
          // The body is the region's text, named by the use written before it.
          mBodies.emplace_back( mPendingBodies.back(), std::exchange( mText, std::move( outside.back() ) ) );
          mPendingBodies.pop_back();
          outside.pop_back();
          mMarked.reset();
          mTracker.forget();
          --mInBody;
        }
      }
    }

    // The stores to a byte a register carries instead are stores to nothing:
    // the value is where the register leaves it.
    dropStoresToPlaced( lowered, bodyBegin );
    loadBackWhatZeroStored( mText, bodyBegin );
    dropJumpsToWhatFollows( mText, bodyBegin );
    if ( mCpu == Cpu::WDC65SC02 )
    {
      setBitsInPlace( mText, bodyBegin );
    }
    mBody = mText.substr( bodyBegin );

    // The bytes after the last instruction: the parameters and the result,
    // each declared as what it is to the Proc — see
    // docs/decisions/0081-a-procs-signature-is-declared.md — and the Proc's
    // own. The Signature first, but last where a declaration of it keeps its
    // bytes in registers: a `.ztemp` of the Proc's own standing right under
    // such a declaration is refused, since it would look like the
    // declaration's — see docs/decisions/0145-an-argument-in-a-register.md.
    auto const signature = [&]
    {
      for ( ir::Local const& parameter : lowered.parameters )
      {
        if ( !parameter.place.empty() )
        {
          // In a register, and no byte at all — see
          // docs/decisions/0145-an-argument-in-a-register.md.
          line( ".declare arg " + parameter.place );
          continue;
        }
        line( ".declare arg " + declared( parameter.type, parameter.bytes ) );
        if ( parameter.name == lowered.resultByte )
        {
          // The byte the argument is written to is the byte the result is read
          // from, so this one reservation carries both roles — see
          // docs/decisions/0119-one-temporary-carries-two-roles.md.
          line( ".declare ret " + declared( *lowered.result, lowered.resultBytes ) );
        }
        temporary( parameter.name, parameter.type, parameter.bytes, false, lowered.placement );
      }
      // A member of a function type declares no byte of its own: its arguments
      // and its result are the type's — see docs/decisions/0065-handlers.md.
      if ( lowered.result.has_value() && lowered.memberOf.empty() && lowered.resultByte.empty() )
      {
        if ( !lowered.resultElsewhere.empty() )
        {
          // Another Proc's result, and no byte of this one — see
          // docs/decisions/0278-a-result-may-be-another-procs-result.md.
          line( ".declare ret " + lowered.resultElsewhere );
        }
        else if ( lowered.resultPlace == "a" )
        {
          line( ".declare ret a" );
        }
        else if ( lowered.resultPlace == "ma" )
        {
          line( ".declare ret ma" );
          temporary( std::string{ RESULT }, ir::Type::U8, 0, false, lowered.placement );
        }
        else
        {
          line( ".declare ret " + declared( *lowered.result, lowered.resultBytes ) );
          temporary( std::string{ RESULT }, *lowered.result, lowered.resultBytes, false, lowered.placement );
        }
      }
    };
    bool const placed = !lowered.resultPlace.empty() || !lowered.resultElsewhere.empty() ||
                        std::ranges::any_of( lowered.parameters,
                                             []( ir::Local const& parameter ) { return !parameter.place.empty(); } );
    if ( !placed )
    {
      signature();
    }
    for ( ir::Local const& local : lowered.locals )
    {
      temporary( local.name, local.type, local.bytes, true, local.placement );
    }
    if ( !mClearsPage.empty() )
    {
      temporary( std::string{ PAGE }, ir::Type::POINTER, 0, true );
    }
    for ( std::uint32_t index = 0; index < mScratchSizes.size(); ++index )
    {
      std::size_t const begin = mText.size();
      bool const off = lowered.placement == ir::Placement::ABSOLUTE && !mScratchPointers[index];
      mText.append( scratchName( index ) )
          .append( off ? " .temp " : " .ztemp " )
          .append( std::to_string( mScratchSizes[index] ) )
          .append( "\n" );
      goesUnnamed( scratchName( index ), begin );
    }
    for ( ir::NamedConstant const& constant : lowered.constants )
    {
      namedConstant( constant, INDENT );
    }

    // Its objects that are no bytes of the Proc stand in it, beside it and in
    // its scope: a local array a `.temp`, or a `.ztemp` where it was asked
    // onto the zero page, and a `static` local, an array given constants or a
    // local whose address is taken a Section of its own — see
    // docs/decisions/0085-a-section-may-stand-in-a-proc.md.
    for ( ir::Global const& object : lowered.sections )
    {
      if ( object.isTemporary && object.stripes.empty() )
      {
        mText.append( object.name )
            .append( object.placement == ir::Placement::ZEROPAGE ? " .ztemp " : " .temp " )
            .append( std::to_string( object.count.value_or( 1 ) * ir::sizeOf( object.type ) ) )
            .append( "\n" );
        continue;
      }
      global( object, INDENT );
    }
    if ( placed )
    {
      signature();
    }
    endProc( lowered );

    // The bodies of its `[[with]]` blocks, each a macro used once, after the
    // Proc, where a macro may stand — see docs/decisions/0096-panes-in-c.md.
    for ( auto const& [name, body] : mBodies )
    {
      mText.append( "\n.macro " ).append( name ).append( "\n" ).append( body ).append( ".endm\n" );
    }
  }

  [[nodiscard]] std::optional<std::uint32_t> nextOf( std::uint32_t block ) const
  {
    return block + 1 < mFunction->blocks.size() ? std::optional{ block + 1 } : std::nullopt;
  }

  /// A scratch byte, or a pair, taken for the Proc being written; a pair
  /// that holds a pointer, which `(zp),y` may read through.
  std::uint32_t scratch( std::uint32_t size, bool pointer = false )
  {
    mScratchSizes.push_back( size );
    mScratchPointers.push_back( pointer );
    return static_cast<std::uint32_t>( mScratchSizes.size() - 1 );
  }

  [[nodiscard]] bool inA( ir::Operand const& operand ) const
  {
    auto const* value = std::get_if<ir::Value>( &operand );
    return value != nullptr && !mScratch[value->index].has_value() && mDirect[value->index].empty() &&
           mAtName[value->index].empty() && mAsOperand[value->index].empty();
  }

  /// Where a value that is not in `A` lives: its scratch bytes, or the object
  /// it is written straight into.
  [[nodiscard]] std::string location( ir::Value value ) const
  {
    if ( !mAtName[value.index].empty() )
    {
      return mAtName[value.index];
    }
    if ( !mDirect[value.index].empty() )
    {
      return qualified( mDirect[value.index] );
    }
    return qualified( scratchName( mScratch[value.index].value_or( 0 ) ) );
  }

  /// What `A` holds where each block is entered: the names every block that
  /// leads there agrees on, given what each of them ended with. A block only
  /// fallen into from the one before it is entered with the whole of that
  /// block's end; the first block is entered from the caller, with nothing.
  static std::vector<std::vector<std::string>> entriesFrom( ir::Function const& function,
                                                            std::vector<std::vector<std::string>> const& ends,
                                                            std::vector<std::optional<Rotated>> const& rotated,
                                                            std::vector<std::optional<std::uint32_t>> const& falls )
  {
    std::vector<std::optional<std::vector<std::string>>> entries( function.blocks.size() );
    auto const meet = [&entries, &ends]( std::uint32_t from, std::uint32_t into )
    {
      if ( into >= entries.size() )
      {
        return;
      }
      if ( !entries[into].has_value() )
      {
        entries[into] = ends[from];
        return;
      }
      std::erase_if( *entries[into],
                     [&ends, from]( std::string const& name )
                     { return std::ranges::find( ends[from], name ) == ends[from].end(); } );
    };
    for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
    {
      ir::Terminator const& leaving = function.blocks[index].terminator;
      switch ( leaving.kind )
      {
      case ir::TerminatorKind::JUMP:
        if ( std::optional<Rotated> const& turned = rotated[index]; turned.has_value() )
        {
          meet( index, turned.value().body );
          meet( index, turned.value().exit );
          break;
        }
        meet( index, falls[index].value_or( leaving.target ) );
        break;
      case ir::TerminatorKind::BRANCH:
        meet( index, leaving.target );
        meet( index, leaving.otherwise );
        break;
      case ir::TerminatorKind::DISPATCH:
        // The dispatch itself writes `A` on the way through, so a case is
        // entered holding nothing, whatever the block before it held.
        for ( std::uint32_t const into : leaving.targets )
        {
          if ( into < entries.size() )
          {
            entries[into] = std::vector<std::string>{};
          }
        }
        break;
      case ir::TerminatorKind::RETURN:
      case ir::TerminatorKind::TRANSITION:
      case ir::TerminatorKind::FALL:
        break;
      }
    }
    std::vector<std::vector<std::string>> known( function.blocks.size() );
    for ( std::uint32_t index = 1; index < function.blocks.size(); ++index )
    {
      known[index] = entries[index].value_or( std::vector<std::string>{} );
    }
    return known;
  }

  /// Whether `X` may be carried into the block: entered from one block alone,
  /// by an edge the IR shows, and in no loop that counts in `X`, whose ways in
  /// and out the IR does not show.
  [[nodiscard]] bool carriesX( ir::Function const& function, std::uint32_t index ) const
  {
    if ( loopAt( index ) != nullptr || !onlyPredecessorOf( function, index ).has_value() )
    {
      return false;
    }
    return std::ranges::none_of( mLoops,
                                 [index]( CountedLoop const& loop )
                                 { return index == loop.exit || index == loop.preheader || index == loop.header; } ) &&
           std::ranges::none_of(
               mResident, [index]( ResidentPair const& pair ) { return pair.holds( index ) || index == pair.exit; } );
  }

  /// What the carry is where each block is entered: what every way in agrees
  /// on, each way carrying what its own edge says.
  static std::vector<Carry> carriesFrom( ir::Function const& function,
                                         std::vector<EdgeCarry> const& ends,
                                         std::vector<std::optional<Rotated>> const& rotated,
                                         std::vector<std::optional<std::uint32_t>> const& falls )
  {
    std::vector<std::optional<Carry>> entries( function.blocks.size() );
    auto const meet = [&entries]( Carry from, std::uint32_t into )
    {
      if ( into >= entries.size() )
      {
        return;
      }
      if ( !entries[into].has_value() )
      {
        entries[into] = from;
      }
      else if ( *entries[into] != from )
      {
        entries[into] = Carry::UNKNOWN;
      }
    };
    for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
    {
      ir::Terminator const& leaving = function.blocks[index].terminator;
      EdgeCarry const& end = ends[index];
      auto const toward = [&end]( std::uint32_t into ) { return end.jumpsTo == into ? end.taken : end.fallen; };
      switch ( leaving.kind )
      {
      case ir::TerminatorKind::JUMP:
        if ( std::optional<Rotated> const& turned = rotated[index]; turned.has_value() )
        {
          meet( end.taken, turned.value().body );
          meet( end.fallen, turned.value().exit );
          break;
        }
        meet( end.fallen, falls[index].value_or( leaving.target ) );
        break;
      case ir::TerminatorKind::BRANCH:
        meet( toward( leaving.target ), leaving.target );
        meet( toward( leaving.otherwise ), leaving.otherwise );
        break;
      case ir::TerminatorKind::DISPATCH:
        // The shorter form doubles the value with `asl`, which writes the
        // carry, and which form is written is the Target's: nothing is known
        // at a case.
        for ( std::uint32_t const into : leaving.targets )
        {
          meet( Carry::UNKNOWN, into );
        }
        break;
      case ir::TerminatorKind::RETURN:
      case ir::TerminatorKind::TRANSITION:
      case ir::TerminatorKind::FALL:
        break;
      }
    }
    std::vector<Carry> known( function.blocks.size(), Carry::UNKNOWN );
    for ( std::uint32_t index = 1; index < function.blocks.size(); ++index )
    {
      known[index] = entries[index].value_or( Carry::UNKNOWN );
    }
    return known;
  }

  /// The loop whose counter `X` holds while this block runs, or nothing.
  /// Whether a counted loop's test is written a second time where the loop
  /// jumps back, branching into the body while the counter is still below
  /// the bound and falling out otherwise, so that no turn ends in a `jmp` to
  /// a test: the header's test is then taken once, on the way in. Only where
  /// the header is the test and nothing else, since its bound is then a
  /// constant or a byte and reads the same at the loop's end, and where the
  /// loop's exit stands right after the step, which is what the fall-through
  /// lands on. See docs/decisions/0128-a-loop-tests-where-it-jumps-back.md.
  [[nodiscard]] bool rotates( CountedLoop const& loop ) const
  {
    ir::Block const& header = mFunction->blocks[loop.header];
    return mFused[loop.header] && header.instructions.size() == 1 &&
           header.terminator.kind == ir::TerminatorKind::BRANCH && header.terminator.target == loop.header + 1 &&
           header.terminator.otherwise == loop.exit && nextOf( loop.step ) == loop.exit;
  }

  /// Whether a rotated loop's first test is known to pass before it is
  /// written: the counter starts at a constant, the bound is one, and the
  /// start is below it, so the body runs at least once and the header's
  /// `cpx` and branch say nothing the emitter does not know.
  [[nodiscard]] bool entersWithoutTest( CountedLoop const& loop ) const
  {
    ir::Block const& header = mFunction->blocks[loop.header];
    auto const* const compare = std::get_if<ir::Compare>( &header.instructions.back().operation );
    auto const* const counter = compare != nullptr ? std::get_if<ir::Object>( &compare->left ) : nullptr;
    auto const* const bound = compare != nullptr ? std::get_if<ir::Constant>( &compare->right ) : nullptr;
    if ( counter == nullptr || counter->name != loop.counter || bound == nullptr || !bound->name.empty() )
    {
      return false;
    }
    // Held apart from a number it does not start at: the test on the way in
    // leaves no carry the body could count on, so nothing is lost with it.
    if ( compare->op == ir::Comparison::NOT_EQUAL )
    {
      return loop.from != bound->value;
    }
    // Stepped down and held at or above a bound it starts at or above, signed
    // against zero or not — see docs/decisions/0262-a-counter-stepped-down-counts-in-x.md.
    if ( compare->op == ir::Comparison::GREATER_OR_EQUAL && loop.down )
    {
      return loop.from >= bound->value;
    }
    return compare->op == ir::Comparison::LESS && !ir::isSigned( compare->type ) && loop.from < bound->value;
  }

  /// Whether the first thing the loop's body does with the carry is read it:
  /// an add, a subtract or a negation — each a `clc` or `sec` and an `adc` or
  /// `sbc`, and the `clc` or `sec` is what 0113 leaves out where the carry is
  /// known — before anything that sets it.
  ///
  /// A shift is a setter although `rol` and `ror` read the carry: every shift
  /// the emitter writes begins with `asl`, `lsr` or a `cmp`, and the `rol` or
  /// `ror` after it reads the carry that one made, never the one the body was
  /// entered with. A compare and a call are setters too. A load, a store, a
  /// mask or a complement touches no carry, and the search goes on past it.
  ///
  /// This is a question of cost and not of correctness: the carry the body is
  /// entered with is computed from the edges the text has, rotated or not, so
  /// a body that reads it gets its `clc` wherever the carry is not known. What
  /// this saves is that `clc` at every turn, for the price of one test on the
  /// way in.
  [[nodiscard]] bool bodyReadsCarry( CountedLoop const& loop ) const
  {
    for ( ir::Instruction const& instruction : mFunction->blocks[loop.header + 1].instructions )
    {
      if ( auto const* const binary = std::get_if<ir::Binary>( &instruction.operation ) )
      {
        if ( binary->op == ir::BinaryOperator::ADD || binary->op == ir::BinaryOperator::SUBTRACT )
        {
          return true;
        }
        if ( binary->op == ir::BinaryOperator::SHIFT_LEFT || binary->op == ir::BinaryOperator::SHIFT_RIGHT )
        {
          return false;
        }
      }
      else if ( auto const* const unary = std::get_if<ir::Unary>( &instruction.operation );
                unary != nullptr && unary->op == ir::UnaryOperator::NEGATE )
      {
        return true;
      }
      else if ( std::holds_alternative<ir::Compare>( instruction.operation ) ||
                std::holds_alternative<ir::Call>( instruction.operation ) )
      {
        return false;
      }
    }
    return false;
  }

  /// Whether an operand is the number zero, written as a number: a constant
  /// the text names is the assembler's to resolve.
  [[nodiscard]] static bool isZero( ir::Operand const& operand )
  {
    return isNumber( operand, 0 );
  }

  /// Whether an operand is that number, written as a number.
  [[nodiscard]] static bool isNumber( ir::Operand const& operand, std::int64_t value )
  {
    auto const* const constant = std::get_if<ir::Constant>( &operand );
    return constant != nullptr && constant->name.empty() && constant->value == value;
  }

  [[nodiscard]] CountedLoop const* loopAt( std::uint32_t block ) const
  {
    for ( CountedLoop const& loop : mLoops )
    {
      if ( loop.holds( block ) )
      {
        return &loop;
      }
    }
    return nullptr;
  }

  /// Whether `X` holds that local while this block runs.
  [[nodiscard]] bool inX( std::uint32_t block, std::string const& name ) const
  {
    CountedLoop const* const loop = loopAt( block );
    return loop != nullptr && loop->counter == name;
  }

  /// Whether an index is already in `X` because it is the counter of the loop
  /// being written. Every path that would load one asks this, so that what the
  /// analysis allowed and what the emitter writes cannot drift apart.
  /// Whether the operand is the counter of the loop this block runs under,
  /// which `X` holds and its byte does not.
  [[nodiscard]] bool counterInX( ir::Operand const& operand ) const
  {
    auto const* const object = std::get_if<ir::Object>( &operand );
    return object != nullptr && inX( mBlock, object->name );
  }

  /// Whether a byte element of `block` is read or written through `Y`: under
  /// a counted loop, whose counter `X` holds, where its index is not in `X`
  /// already. The block is named rather than read off `mBlock`, since a plan
  /// asks before any block is written.
  [[nodiscard]] bool throughY( std::uint32_t block, ir::Operand const& index, ir::Type type, bool scaled ) const
  {
    return ir::sizeOf( type ) == 1 && !scaled && loopAt( block ) != nullptr && !indexInXAt( block, index, scaled );
  }

  /// Whether an index is the counter of the loop holding the block, or a sum
  /// the address carries, and so in `X` already.
  [[nodiscard]] bool indexInXAt( std::uint32_t block, ir::Operand const& index, bool scaled ) const
  {
    CountedLoop const* const loop = loopAt( block );
    if ( loop == nullptr || scaled )
    {
      return false;
    }
    auto const* const object = std::get_if<ir::Object>( &index );
    return ( object != nullptr && object->name == loop->counter ) || loop->offsetOf( index ).has_value();
  }

  /// An element read once, as the right operand of a byte's operator or
  /// compare, with nothing between that writes memory: the element is that
  /// instruction's memory operand, and the load is not written. Under the `X`
  /// a counted loop holds the index is there already; elsewhere the index is a
  /// byte in memory, loaded into `X` where the element is read — see
  /// docs/decisions/0115-an-index-is-loaded-where-the-element-is-read.md. The
  /// element is read later than it was, so what stands between must be reads
  /// and nothing else.
  /// A wide value a call leaves in the callee's result byte, read once by an
  /// operator or a compare of the same block, is read from that byte: the call
  /// copies it nowhere and the reader names it. The bytes of a callee are a
  /// Temporary whose address nothing may take, so nothing but another call can
  /// write them — and a call standing between is what this refuses, since the
  /// second one would leave its own result where the first's still waits.
  ///
  /// See docs/decisions/0123-a-call-s-result-is-read-where-it-lies.md.
  void planCallResultsInPlace( ir::Function const& function, std::uint32_t index )
  {
    ir::Block const& block = function.blocks[index];
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      auto const* const call = std::get_if<ir::Call>( &block.instructions[at].operation );
      if ( call == nullptr || !call->result.has_value() || !isWide( function, *call->result ) ||
           !mDirect[call->result->index].empty() || ir::readersOf( function, *call->result ) != 1 ||
           !call->resultPlace.empty() )
      {
        continue;
      }

      for ( std::size_t reader = at + 1; reader < block.instructions.size(); ++reader )
      {
        ir::Instruction const& candidate = block.instructions[reader];
        if ( std::holds_alternative<ir::Call>( candidate.operation ) )
        {
          break;
        }
        bool reads = false;
        if ( auto const* const binary = std::get_if<ir::Binary>( &candidate.operation );
             binary != nullptr && ir::sizeOf( binary->type ) == 2 )
        {
          reads = isValue( binary->left, *call->result ) || isValue( binary->right, *call->result );
        }
        else if ( auto const* const compare = std::get_if<ir::Compare>( &candidate.operation );
                  compare != nullptr && ir::sizeOf( compare->type ) == 2 )
        {
          reads = isValue( compare->left, *call->result ) || isValue( compare->right, *call->result );
        }
        if ( !reads )
        {
          continue;
        }
        mAtName[call->result->index] =
            call->returned.empty() ? call->name + "." + std::string{ RESULT } : call->returned;

        // The scratch pair the value was given before it was read where it
        // lies is nobody's now; the renumbering drops its declaration.
        mScratch[call->result->index].reset();
        break;
      }
    }
  }

  /// A 16-bit value computed from one that no other instruction reads takes
  /// that one's scratch pair: every 16-bit operator reads a byte of its
  /// operand before it writes that byte of its answer, as the store straight
  /// into an object already counts on, so the answer may stand where the
  /// operand stood. A conversion that keeps the bits is then no copy at all,
  /// and a shift is done where its operand lies. See
  /// docs/decisions/0138-a-pair-an-operand-leaves-is-the-answers.md.
  void planByteViews( ir::Function const& function )
  {
    mByteView.assign( function.values.size(), std::nullopt );
    std::set<std::string> const own = ownBytesOf( function );
    for ( ir::Block const& block : function.blocks )
    {
      std::vector<ir::Instruction> const& body = block.instructions;
      for ( std::size_t at = 0; at < body.size(); ++at )
      {
        auto const* const shift = std::get_if<ir::Binary>( &body[at].operation );
        auto const* const by = shift != nullptr ? std::get_if<ir::Constant>( &shift->right ) : nullptr;
        if ( shift == nullptr || ir::sizeOf( shift->type ) != 2 || by == nullptr || !by->name.empty() ||
             by->value != 8 || !mScratch[shift->result.index].has_value() ||
             ( shift->op != ir::BinaryOperator::SHIFT_LEFT &&
               ( shift->op != ir::BinaryOperator::SHIFT_RIGHT || ir::isSigned( shift->type ) ||
                 ir::isSigned( ir::typeOf( shift->left, function ) ) ) ) )
        {
          continue;
        }
        bool const left = shift->op == ir::BinaryOperator::SHIFT_LEFT;
        // Of an object, whose name no later plan changes.
        auto const* const object = std::get_if<ir::Object>( &shift->left );
        if ( object == nullptr || mTracker.isVolatile( object->name ) )
        {
          continue;
        }
        std::string const base = ir::baseOf( object->name );

        // Every reader is a 16-bit operator, a compare or a store, reading the
        // bytes where they lie; and nothing before the last of them writes
        // what they lie in. `x << 8`'s high byte is `x`'s low one, which a
        // reader writing `x` must not write before it reads — so such a
        // reader is taken only where its low byte writes `x`'s back unchanged.
        std::uint32_t const readers = ir::readersOf( function, shift->result );
        std::uint32_t seen = 0;
        bool fits = true;
        for ( std::size_t later = at + 1; later < body.size() && fits && seen < readers; ++later )
        {
          ir::Instruction const& instruction = body[later];
          std::uint32_t here = 0;
          ir::eachOperand( instruction,
                           [&here, &shift]( ir::Operand const& operand )
                           { here += isValue( operand, shift->result ) ? 1U : 0U; } );
          if ( here != 0 )
          {
            seen += here;
            if ( auto const* const binary = std::get_if<ir::Binary>( &instruction.operation );
                 binary != nullptr && ir::sizeOf( binary->type ) == 2 && binary->op != ir::BinaryOperator::SHIFT_LEFT &&
                 binary->op != ir::BinaryOperator::SHIFT_RIGHT )
            {
              std::string const& into = mDirect[binary->result.index];
              auto const* const same = std::get_if<ir::Object>( &binary->left );
              fits = !left || into.empty() || ir::baseOf( into ) != base ||
                     ( same != nullptr && same->name == object->name && binary->op != ir::BinaryOperator::AND &&
                       !isValue( binary->left, shift->result ) );
            }
            else if ( auto const* const compare = std::get_if<ir::Compare>( &instruction.operation );
                      compare != nullptr && ir::sizeOf( compare->type ) == 2 )
            {
              fits = true;
            }
            else if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation );
                      store != nullptr && ir::sizeOf( store->type ) == 2 )
            {
              fits = !left || ir::baseOf( store->name ) != base;
            }
            else
            {
              fits = false;
            }
          }
          if ( fits && seen < readers )
          {
            bool const reaches =
                !own.contains( base ) && ( std::holds_alternative<ir::Call>( instruction.operation ) ||
                                           std::holds_alternative<ir::StoreIndirect>( instruction.operation ) ||
                                           std::holds_alternative<ir::Copy>( instruction.operation ) ||
                                           std::holds_alternative<ir::EnterWith>( instruction.operation ) );
            fits = !reaches && !writesObject( instruction, object->name );
          }
        }
        // A reader the instructions do not hold — a branch on it — is counted
        // by `readersOf` and not here, and keeps the shift computed.
        if ( !fits || seen != readers || readers == 0 )
        {
          continue;
        }
        mByteView[shift->result.index] = ByteView{ .source = *object, .left = left };
        mScratch[shift->result.index].reset();
      }
    }
  }

  void planSharedPairs( ir::Function const& function )
  {
    for ( ir::Block const& block : function.blocks )
    {
      for ( ir::Instruction const& instruction : block.instructions )
      {
        ir::Operand const* from = nullptr;
        std::optional<ir::Value> result;
        if ( auto const* const binary = std::get_if<ir::Binary>( &instruction.operation );
             binary != nullptr && ir::sizeOf( binary->type ) == 2 )
        {
          from = &binary->left;
          result = binary->result;
        }
        else if ( auto const* const convert = std::get_if<ir::Convert>( &instruction.operation );
                  convert != nullptr && ir::sizeOf( convert->type ) == 2 &&
                  ir::sizeOf( ir::typeOf( convert->operand, function ) ) == 2 )
        {
          from = &convert->operand;
          result = convert->result;
        }
        auto const* const operand = from != nullptr ? std::get_if<ir::Value>( from ) : nullptr;
        if ( operand == nullptr || !result.has_value() || !mScratch[result->index].has_value() ||
             !mScratch[operand->index].has_value() || !isWide( function, *operand ) ||
             ir::readersOf( function, *operand ) != 1 )
        {
          continue;
        }
        mScratch[result->index] = mScratch[operand->index];
      }
    }

    // The other way: a result written straight into the object it is stored
    // in lends that object to the pair of scratch bytes it is computed from,
    // where the pair is computed by the instruction right before and read by
    // nothing else, and nothing else the result reads is the object — so
    // `middle = (low + high) >> 1` adds into `middle` and shifts it there,
    // with no pair and no copy. See
    // docs/decisions/0155-a-pair-computed-where-its-answer-goes.md.
    for ( ir::Block const& block : function.blocks )
    {
      for ( std::size_t at = 1; at < block.instructions.size(); ++at )
      {
        ir::Operand const* from = nullptr;
        ir::Operand const* other = nullptr;
        std::optional<ir::Value> result;
        if ( auto const* const binary = std::get_if<ir::Binary>( &block.instructions[at].operation );
             binary != nullptr && ir::sizeOf( binary->type ) == 2 )
        {
          from = &binary->left;
          other = &binary->right;
          result = binary->result;
        }
        else if ( auto const* const convert = std::get_if<ir::Convert>( &block.instructions[at].operation );
                  convert != nullptr && ir::sizeOf( convert->type ) == 2 &&
                  ir::sizeOf( ir::typeOf( convert->operand, function ) ) == 2 )
        {
          from = &convert->operand;
          result = convert->result;
        }
        auto const* const operand = from != nullptr ? std::get_if<ir::Value>( from ) : nullptr;
        if ( operand == nullptr || !result.has_value() || mDirect[result->index].empty() ||
             !mScratch[operand->index].has_value() || !isWide( function, *operand ) ||
             ir::readersOf( function, *operand ) != 1 )
        {
          continue;
        }
        // Computed by an operator, which writes its answer where it is told;
        // a call's result is read where it lies, and a load's where it is.
        ir::Instruction const& previous = block.instructions[at - 1];
        bool const computed = std::holds_alternative<ir::Binary>( previous.operation ) ||
                              std::holds_alternative<ir::Unary>( previous.operation ) ||
                              std::holds_alternative<ir::Convert>( previous.operation );
        std::optional<ir::Value> const before = computed ? ir::resultOf( previous ) : std::nullopt;
        std::string const& into = mDirect[result->index];
        auto const* const read = other != nullptr ? std::get_if<ir::Object>( other ) : nullptr;
        if ( !before.has_value() || before->index != operand->index || mTracker.isVolatile( into ) ||
             ( read != nullptr && byteBase( read->name ) == byteBase( into ) ) )
        {
          continue;
        }
        mDirect[operand->index] = into;
        mScratch[operand->index].reset();
      }
    }
  }

  void planFoldedIndexes( ir::Function const& function )
  {
    mFoldedIndex.assign( function.values.size(), std::nullopt );

    // A sum a counted loop's address carries is not written, and needs no
    // scratch byte to wait in either.
    for ( CountedLoop const& loop : mLoops )
    {
      for ( auto const& [value, by] : loop.folded )
      {
        mScratch[value].reset();
      }
    }
    std::vector<Range> const ranges = rangesOf( function );
    std::set<std::string> const own = ownBytesOf( function );
    for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
    {
      std::vector<ir::Instruction> const& body = function.blocks[index].instructions;
      CountedLoop const* const loop = loopAt( index );
      for ( std::size_t at = 0; at < body.size(); ++at )
      {
        auto const* const sum = std::get_if<ir::Binary>( &body[at].operation );
        if ( sum == nullptr || sum->type != ir::Type::U8 ||
             ( sum->op != ir::BinaryOperator::ADD && sum->op != ir::BinaryOperator::SUBTRACT ) ||
             ( loop != nullptr && loop->offsetOf( ir::Operand{ sum->result } ).has_value() ) )
        {
          continue;
        }
        auto const* object = std::get_if<ir::Object>( &sum->left );
        auto const* number = std::get_if<ir::Constant>( &sum->right );
        if ( object == nullptr && sum->op == ir::BinaryOperator::ADD )
        {
          object = std::get_if<ir::Object>( &sum->right );
          number = std::get_if<ir::Constant>( &sum->left );
        }
        // What the ranges promise of a sum that may wrap is the whole of its
        // type, so anything narrower is a sum that did not. A counter `X`
        // holds is no byte to load: its own loop folds what it can.
        if ( object == nullptr || number == nullptr || !number->name.empty() || object->type != ir::Type::U8 ||
             !own.contains( object->name ) || ( loop != nullptr && loop->counter == object->name ) ||
             sum->result.index >= ranges.size() || !ranges[sum->result.index].within( rangeOfType( ir::Type::U8 ) ) ||
             ( ranges[sum->result.index].min == 0 && ranges[sum->result.index].max == 0xFF ) )
        {
          continue;
        }

        // Read only as the index of a byte element, and the byte not written
        // before any of those reads: the byte is loaded where the element is.
        std::uint32_t reads = 0;
        bool indexOnly = true;
        bool written = false;
        for ( std::size_t later = at + 1; later < body.size() && indexOnly; ++later )
        {
          ir::Instruction const& instruction = body[later];
          bool const indexes = std::visit(
              [&sum]( auto const& operation )
              {
                using Operation = std::decay_t<decltype( operation )>;
                if constexpr ( std::is_same_v<Operation, ir::Load> )
                {
                  return isValue( operation.index, sum->result ) && !operation.scaled && operation.high.empty() &&
                         ir::sizeOf( operation.type ) == 1;
                }
                else if constexpr ( std::is_same_v<Operation, ir::StoreElement> )
                {
                  return isValue( operation.index, sum->result ) && !operation.scaled &&
                         ir::sizeOf( operation.type ) == 1 && !isValue( operation.value, sum->result );
                }
                else
                {
                  return false;
                }
              },
              instruction.operation );
          if ( indexes )
          {
            indexOnly = !written;
            ++reads;
            continue;
          }
          std::uint32_t here = 0;
          ir::eachOperand( instruction,
                           [&here, &sum]( ir::Operand const& operand )
                           { here += isValue( operand, sum->result ) ? 1U : 0U; } );
          indexOnly = here == 0;
          written = written || writesObject( instruction, object->name );
        }
        if ( !indexOnly || reads == 0 || reads != ir::readersOf( function, sum->result ) )
        {
          continue;
        }
        std::int64_t const by = sum->op == ir::BinaryOperator::ADD ? number->value : -number->value;
        mFoldedIndex[sum->result.index] = FoldedIndex{ .base = object->name, .by = by };
        mScratch[sum->result.index].reset();
      }
    }
  }

  /// Whether the instruction writes the object, whole or in part.
  [[nodiscard]] static bool writesObject( ir::Instruction const& instruction, std::string const& name )
  {
    if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation ) )
    {
      return ir::baseOf( store->name ) == ir::baseOf( name );
    }
    if ( auto const* const element = std::get_if<ir::StoreElement>( &instruction.operation ) )
    {
      return ir::baseOf( element->name ) == ir::baseOf( name );
    }
    if ( auto const* const copy = std::get_if<ir::Copy>( &instruction.operation ) )
    {
      return ir::baseOf( copy->to.name ) == ir::baseOf( name );
    }
    return false;
  }

  /// What an element's index register is loaded from: the byte a folded sum
  /// adds its number to, or the index itself.
  [[nodiscard]] ir::Operand indexSource( ir::Operand const& index ) const
  {
    auto const* const value = std::get_if<ir::Value>( &index );
    if ( value != nullptr && value->index < mFoldedIndex.size() )
    {
      if ( std::optional<FoldedIndex> const& folded = mFoldedIndex[value->index]; folded.has_value() )
      {
        return ir::Object{ .name = folded.value().base, .type = ir::Type::U8 };
      }
    }
    return index;
  }

  /// The byte an instruction reaches its element by: the name an index
  /// register would be loaded from, and whether the element is written.
  struct ElementIndex
  {
    std::string byte;
    bool written = false;
  };

  /// That byte, where the instruction reaches a byte element by a byte, and
  /// nothing for every other instruction.
  [[nodiscard]] std::optional<ElementIndex> elementIndexOf( ir::Instruction const& instruction ) const
  {
    ir::Operand const* index = nullptr;
    ir::Type type = ir::Type::U8;
    bool written = false;
    if ( auto const* const load = std::get_if<ir::Load>( &instruction.operation ) )
    {
      index = &load->index;
      type = load->type;
    }
    else if ( auto const* const store = std::get_if<ir::StoreElement>( &instruction.operation ) )
    {
      index = &store->index;
      type = store->type;
      written = true;
    }
    if ( index == nullptr || ir::sizeOf( type ) != 1 )
    {
      return std::nullopt;
    }
    ir::Operand const source = indexSource( *index );
    auto const* const byte = std::get_if<ir::Object>( &source );
    if ( byte == nullptr || ir::sizeOf( byte->type ) != 1 )
    {
      return std::nullopt;
    }
    return ElementIndex{ .byte = byte->name, .written = written };
  }

  /// A run of byte elements that turns between two indexes takes a register
  /// each: the index the run only reads by goes into `Y` and the one it
  /// writes by stays in `X`, so that each is loaded once for the whole run
  /// and not before every byte — see
  /// docs/decisions/0201-a-run-between-two-indexes-takes-a-register-each.md.
  /// Not under a counted loop, whose counter holds `X` for the body's whole
  /// length and whose second index already goes through `Y` (0133).
  void planSecondIndex( ir::Function const& function, std::uint32_t index )
  {
    if ( loopAt( index ) != nullptr )
    {
      return;
    }
    ir::Block const& block = function.blocks[index];
    for ( std::size_t at = 0; at < block.instructions.size(); )
    {
      std::size_t end = at;
      std::vector<std::string> names;
      std::set<std::string> written;
      for ( ; end < block.instructions.size() && !mDeferred[index][end]; ++end )
      {
        std::optional<ElementIndex> const reached = elementIndexOf( block.instructions[end] );
        if ( !reached.has_value() )
        {
          break;
        }
        if ( std::ranges::find( names, reached->byte ) == names.end() )
        {
          names.push_back( reached->byte );
        }
        if ( reached->written )
        {
          written.insert( reached->byte );
        }
      }

      // Two indexes and no fewer, since one alone stays in `X` as it is, and
      // no more, since the processor has two registers. Four instructions and
      // no fewer: a single byte read and written loads two registers either
      // way, and it is the second byte on that a register each begins to pay.
      auto const read =
          std::ranges::find_if( names, [&written]( std::string const& name ) { return !written.contains( name ); } );
      if ( names.size() == 2 && end - at >= 4 && read != names.end() )
      {
        for ( std::size_t one = at; one < end; ++one )
        {
          std::optional<ElementIndex> const reached = elementIndexOf( block.instructions[one] );
          mSecondIndex[index][one] = reached.has_value() && reached->byte == *read;
        }
      }
      at = end > at ? end : at + 1;
    }
  }

  /// A pair read from an element and written straight into another is carried
  /// by two bytes and not by a value: the load is written where the store is,
  /// an index register each, and the scratch pair the value would have waited
  /// in is nobody's — see
  /// docs/decisions/0202-a-pair-that-goes-from-one-element-to-another-is-two-bytes.md.
  /// Not under a counted loop, whose counter holds `X`, and not where either
  /// end is volatile, whose reads and writes keep the order they were
  /// written in (0151).
  void planPairsMoved( ir::Function const& function, std::uint32_t index )
  {
    if ( loopAt( index ) != nullptr )
    {
      return;
    }
    std::vector<ir::Instruction> const& instructions = function.blocks[index].instructions;
    for ( std::size_t at = 0; at + 1 < instructions.size(); ++at )
    {
      auto const* const from = std::get_if<ir::Load>( &instructions[at].operation );
      auto const* const to = std::get_if<ir::StoreElement>( &instructions[at + 1].operation );
      if ( from == nullptr || to == nullptr || ir::sizeOf( from->type ) != 2 || ir::sizeOf( to->type ) != 2 ||
           !isValue( to->value, from->result ) || ir::readersOf( function, from->result ) != 1 ||
           mTracker.isVolatile( from->name ) || mTracker.isVolatile( to->name ) )
      {
        continue;
      }
      mMoved[index][at] = true;

      // The pair the value was given before it was carried is nobody's now;
      // the renumbering drops its declaration.
      mScratch[from->result.index].reset();
    }
  }

  /// Whether the element of the instruction being written goes through `Y`
  /// because its run turns between two indexes.
  [[nodiscard]] bool secondIndexHere() const
  {
    return mBlock < mSecondIndex.size() && mAt < mSecondIndex[mBlock].size() && mSecondIndex[mBlock][mAt];
  }

  /// Whether a comparison is a counted loop's test of its counter in `X`,
  /// which is `cpx` and has no indexed form: an element it is compared with
  /// is read into a byte first rather than named as `t,y`.
  [[nodiscard]] bool countsAgainst( std::uint32_t block, ir::Compare const& compare ) const
  {
    auto const* const left = std::get_if<ir::Object>( &compare.left );
    auto const* const right = std::get_if<ir::Object>( &compare.right );
    return ( left != nullptr && inX( block, left->name ) ) || ( right != nullptr && inX( block, right->name ) );
  }

  void planOperandsInMemory( ir::Function const& function, std::uint32_t index )
  {
    ir::Block const& block = function.blocks[index];
    CountedLoop const* const loop = loopAt( index );
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      // Not an element of a volatile array, whose read would move past the
      // reads between.
      auto const* const load = std::get_if<ir::Load>( &block.instructions[at].operation );
      if ( load == nullptr || ir::sizeOf( load->type ) != 1 || load->scaled || !load->high.empty() ||
           ir::readersOf( function, load->result ) != 1 || mTracker.isVolatile( load->name ) )
      {
        continue;
      }
      bool const inXAlready = indexInXAt( index, load->index, load->scaled );
      auto const* const byte = std::get_if<ir::Object>( &load->index );
      if ( !inXAlready && ( byte == nullptr || ir::sizeOf( byte->type ) != 1 ) )
      {
        continue;
      }
      for ( std::size_t reader = at + 1; reader < block.instructions.size(); ++reader )
      {
        ir::Instruction const& candidate = block.instructions[reader];
        ir::Operand const* right = nullptr;
        if ( auto const* binary = std::get_if<ir::Binary>( &candidate.operation );
             binary != nullptr && ir::sizeOf( binary->type ) == 1 && binary->op != ir::BinaryOperator::SHIFT_LEFT &&
             binary->op != ir::BinaryOperator::SHIFT_RIGHT )
        {
          right = &binary->right;
        }
        else if ( auto const* compare = std::get_if<ir::Compare>( &candidate.operation );
                  compare != nullptr && ir::sizeOf( compare->type ) == 1 && !countsAgainst( index, *compare ) )
        {
          right = &compare->right;
        }
        auto const* const between = std::get_if<ir::Load>( &candidate.operation );
        if ( right != nullptr && isValue( *right, load->result ) )
        {
          // Under a counted loop an index that is not its counter goes
          // through `Y`, which the counter leaves free.
          std::optional<std::int64_t> const by = loop != nullptr ? loop->offsetOf( load->index ) : std::nullopt;
          bool const y = !inXAlready && loop != nullptr;
          mAsOperand[load->result.index] =
              qualified( load->name ) + offsetText( by.value_or( 0 ) ) + ( y ? ",y" : ",x" );
          if ( !inXAlready )
          {
            mIndexToLoad[load->result.index] = ( y ? "ldy " : "ldx " ) + qualified( byte->name );
          }
          mDeferred[index][at] = true;

          // The scratch byte the value was given before it became an operand
          // is nobody's now; the renumbering drops its declaration.
          mScratch[load->result.index].reset();
          break;
        }
        // Only another read, or a sum the address takes and the text does
        // not write, may stand between: the element is read later than it
        // was, and nothing may have written it meanwhile. A read that puts
        // its own index in `X` is no matter, since ours is loaded where the
        // element is read.
        auto const* const sum = std::get_if<ir::Binary>( &candidate.operation );
        bool const folded =
            sum != nullptr && loop != nullptr && loop->offsetOf( ir::Operand{ sum->result } ).has_value();
        if ( !folded && between == nullptr )
        {
          break;
        }
      }
    }
  }

  /// A byte read through a pointer on the zero page, read once as the right
  /// operand of an operator or a compare of a byte, or of a 16-bit operator
  /// whose high byte it reads as zero: the instruction reads it from memory
  /// itself, `eor (p),y`, with `Y` loaded where it does, as an element under
  /// `X` is read since 0114. The pointer is a byte of memory, whose name the
  /// plans after this one do not change, and the index a number or a byte.
  /// See docs/decisions/0139-a-byte-through-a-pointer-is-an-operand.md.
  /// An address computed in a loop and read or written once, by the very
  /// next instruction, through a byte: its low byte goes into `Y` as it is
  /// made, its high byte into `PAGE`, and the access is `(PAGE),y` with no
  /// `ldy` — three cycles and three bytes, and no pair of its own. `PAGE`'s
  /// low byte is cleared where the outermost loop around it is entered. Only
  /// where nothing between the sum's low byte and the access may move `Y`: an
  /// operand read through `Y` rules it out. A 65SC02 reads through `(zp)`
  /// with no index, which this would not better. See
  /// docs/decisions/0266-an-address-keeps-its-low-byte-in-y.md.
  void planPagedAddresses( ir::Function const& function )
  {
    mPaged.assign( function.values.size(), false );
    mClearsPage.clear();
    mPageBase.reset();
    bool first = true;
    if ( mCpu == Cpu::WDC65SC02 )
    {
      return;
    }
    std::vector<Loop> const loops = loopsOf( function );

    // Whether every turn of the loop runs the block: with it taken out, no way
    // from the header reaches the block that jumps back.
    auto const everyTurn = [&function]( Loop const& loop, std::uint32_t block )
    {
      if ( block == loop.header )
      {
        return true;
      }
      std::vector<bool> reached( function.blocks.size(), false );
      std::vector<std::uint32_t> pending{ loop.header };
      reached[loop.header] = true;
      while ( !pending.empty() )
      {
        std::uint32_t const from = pending.back();
        pending.pop_back();
        ir::Terminator const& end = function.blocks[from].terminator;
        std::vector<std::uint32_t> next;
        if ( end.kind == ir::TerminatorKind::JUMP )
        {
          next = { end.target };
        }
        else if ( end.kind == ir::TerminatorKind::BRANCH )
        {
          next = { end.target, end.otherwise };
        }
        else if ( end.kind == ir::TerminatorKind::DISPATCH )
        {
          next = end.targets;
        }
        for ( std::uint32_t const to : next )
        {
          if ( to != block && to >= loop.header && to <= loop.last && !reached[to] )
          {
            reached[to] = true;
            pending.push_back( to );
          }
        }
      }
      return !reached[loop.last];
    };
    // Whether the loop leaves only by its header's test: no `return`, no
    // `break` and no way out from any other of its blocks.
    auto const leavesByTest = [&function]( Loop const& loop )
    {
      for ( std::uint32_t block = loop.header; block <= loop.last; ++block )
      {
        ir::Terminator const& end = function.blocks[block].terminator;
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
          return false;
        }
        for ( std::uint32_t const to : next )
        {
          if ( ( to < loop.header || to > loop.last ) && block != loop.header )
          {
            return false;
          }
        }
      }
      return true;
    };
    auto const readsY = [this]( ir::Operand const& operand )
    {
      auto const* const value = std::get_if<ir::Value>( &operand );
      return value != nullptr && ( !mAsOperand[value->index].empty() || !mAtName[value->index].empty() );
    };
    for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
    {
      // The outermost loop around the block clears the low byte once, where
      // it is entered. That pays where every loop on the way in leaves by its
      // test and nothing else, so that a loop entered runs all its turns, and
      // the innermost runs the block at every turn of its own: `puff`'s
      // `decode` is entered once a symbol and mostly returns before it reaches
      // its read, which would pay the clearing for nothing.
      std::vector<Loop const*> chain;
      for ( Loop const& loop : loops )
      {
        if ( index >= loop.header && index <= loop.last )
        {
          chain.push_back( &loop );
        }
      }
      bool pays = !chain.empty() && everyTurn( *chain.back(), index );
      for ( Loop const* const loop : chain )
      {
        pays = pays && leavesByTest( *loop );
      }
      if ( !pays )
      {
        continue;
      }
      Loop const* const around = chain.front();
      std::vector<ir::Instruction> const& instructions = function.blocks[index].instructions;
      for ( std::size_t at = 0; at + 1 < instructions.size(); ++at )
      {
        auto const* const sum = std::get_if<ir::Binary>( &instructions[at].operation );
        if ( sum == nullptr || sum->op != ir::BinaryOperator::ADD || sum->type != ir::Type::POINTER ||
             ir::readersOf( function, sum->result ) != 1 || readsY( sum->left ) || readsY( sum->right ) ||
             !mScratch[sum->result.index].has_value() )
        {
          continue;
        }
        auto const& next = instructions[at + 1].operation;
        bool through = false;
        if ( auto const* const load = std::get_if<ir::LoadIndirect>( &next ) )
        {
          through = isValue( load->pointer, sum->result ) && isZero( load->index ) && ir::sizeOf( load->type ) == 1;
        }
        else if ( auto const* const store = std::get_if<ir::StoreIndirect>( &next ) )
        {
          // What is stored is loaded between the two, and must not move `Y`.
          through = isValue( store->pointer, sum->result ) && isZero( store->index ) &&
                    ir::sizeOf( store->type ) == 1 &&
                    ( std::holds_alternative<ir::Constant>( store->value ) ||
                      std::holds_alternative<ir::Object>( store->value ) );
        }
        if ( !through )
        {
          continue;
        }
        mPaged[sum->result.index] = true;
        mScratch[sum->result.index].reset();
        mClearsPage.insert( around->entry );

        // One array for every such address, indexed by an object: `PAGE`
        // starts at the array.
        auto const* const base = std::get_if<ir::Constant>( &sum->left );
        bool const shared =
            base != nullptr && !base->name.empty() && base->follower.empty() &&
            std::holds_alternative<ir::Object>( sum->right ) &&
            ( first || ( mPageBase.has_value() && mPageBase->name == base->name && mPageBase->value == base->value ) );
        mPageBase = shared ? std::optional<ir::Constant>{ *base } : std::nullopt;
        first = false;
      }
    }
  }

  /// Stores to 16-bit elements one after another, the index of each the same
  /// byte plus a number one or two more than the one before: `place1[c]`,
  /// `place1[c + 1]`, `place1[c + 2]`. `X` holds the first index doubled, and
  /// `X + 2` is the next doubled, wrapping as the sum doubled would, so each
  /// next is `inx / inx` where it was `lda c / clc / adc #1 / asl / tax`. Only
  /// where nothing between them may move `X` or write the byte: the sum
  /// stands right before its store and the store right after the one before,
  /// and what each stores is a number or an object, whose load leaves `X`
  /// alone. See docs/decisions/0269-the-next-element-is-two-on-in-x.md.
  void planIndexSteps( ir::Function const& function )
  {
    mIndexStep.assign( function.values.size(), 0 );
    mStepSilent.assign( function.blocks.size(), {} );
    for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
    {
      std::vector<ir::Instruction> const& instructions = function.blocks[index].instructions;
      mStepSilent[index].assign( instructions.size(), false );

      // The byte and the number the index of a store is, where the store is
      // one this may step to: a 16-bit element, doubled into `X`, of a number
      // or an object.
      auto const indexOf = [&]( std::size_t at ) -> std::optional<std::pair<std::string, std::int64_t>>
      {
        auto const* const store = std::get_if<ir::StoreElement>( &instructions[at].operation );
        if ( store == nullptr || ir::sizeOf( store->type ) != 2 || store->scaled || !store->high.empty() ||
             mMoved[index][at] || mSecondIndex[index][at] || mDeferred[index][at] ||
             std::holds_alternative<ir::Value>( store->value ) || indexInXAt( index, store->index, store->scaled ) )
        {
          return std::nullopt;
        }
        if ( auto const* const object = std::get_if<ir::Object>( &store->index );
             object != nullptr && object->type == ir::Type::U8 )
        {
          return std::pair{ object->name, std::int64_t{ 0 } };
        }
        auto const* const value = std::get_if<ir::Value>( &store->index );
        if ( value == nullptr || at == 0 )
        {
          return std::nullopt;
        }
        auto const* const sum = std::get_if<ir::Binary>( &instructions[at - 1].operation );
        auto const* const byte = sum != nullptr ? std::get_if<ir::Object>( &sum->left ) : nullptr;
        auto const* const by = sum != nullptr ? std::get_if<ir::Constant>( &sum->right ) : nullptr;
        if ( sum == nullptr || sum->op != ir::BinaryOperator::ADD || sum->type != ir::Type::U8 ||
             sum->result.index != value->index || byte == nullptr || byte->type != ir::Type::U8 || by == nullptr ||
             !by->name.empty() || ir::readersOf( function, *value ) != 1 )
        {
          return std::nullopt;
        }
        return std::pair{ byte->name, by->value };
      };

      std::optional<std::pair<std::string, std::int64_t>> before;
      std::size_t beforeAt = 0;
      for ( std::size_t at = 0; at < instructions.size(); ++at )
      {
        std::optional<std::pair<std::string, std::int64_t>> const here = indexOf( at );
        if ( !here.has_value() )
        {
          continue;
        }
        bool const summed = here->second != 0 || std::holds_alternative<ir::Value>(
                                                     std::get<ir::StoreElement>( instructions[at].operation ).index );
        std::size_t const after = summed ? at - 1 : at;
        std::int64_t const step = before.has_value() ? here->second - before->second : 0;
        if ( before.has_value() && before->first == here->first && beforeAt + 1 == after && summed && step >= 1 &&
             step <= 2 )
        {
          auto const& store = std::get<ir::StoreElement>( instructions[at].operation );
          ir::Value const value = std::get<ir::Value>( store.index );
          mIndexStep[value.index] = step;
          mStepSilent[index][at - 1] = true;
          mScratch[value.index].reset();
        }
        before = here;
        beforeAt = at;
      }
    }
  }

  /// A 16-bit element of an array at an index that is a byte, doubled, read or
  /// written by the next instruction: the doubled index's low byte is the
  /// byte shifted left once, which goes into `Y`, and its high byte is the
  /// bit shifted out, which the carry adds to the array's high byte. So the
  /// address is `asl / tay / lda #>t / adc #0 / sta p+1 / lda #<t / sta p`,
  /// with no pair shifted and no pair added, and the access reads `(p),y` with
  /// no `ldy`. `Y` is even, so the `iny` of the element's high byte never
  /// wraps. And a pair read through a pointer that the next instruction reads
  /// once, as an operand of a 16-bit operator, is read by that operator a byte
  /// at a time — see
  /// docs/decisions/0272-a-word-at-a-byte-index-is-read-through-y.md.
  ///
  /// The array may be a pointer as well, its bytes copied with the carry into
  /// the high one; and a pointer of the Proc's own, which lies on the zero
  /// page, read at a byte the ranges hold to 127 carries nothing, so the
  /// address is the pointer itself and only `asl / tay` is written — see
  /// docs/decisions/0276-an-element-through-a-pointer-at-a-small-index.md.
  void planTableReads( ir::Function const& function )
  {
    mTableAddress.assign( function.values.size(), false );
    mTableInPlace.assign( function.values.size(), false );
    mPairThroughY.assign( function.values.size(), false );
    mPairPointer.assign( function.values.size(), std::nullopt );
    if ( mCpu == Cpu::WDC65SC02 )
    {
      return;
    }
    auto const viaY = [this]( ir::Operand const& operand )
    {
      auto const* const value = std::get_if<ir::Value>( &operand );
      return value != nullptr && ( !mAsOperand[value->index].empty() || !mAtName[value->index].empty() );
    };
    std::vector<Range> const ranges = rangesOf( function );
    std::set<std::string> const own = ownBytesOf( function );
    for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
    {
      std::vector<ir::Instruction> const& instructions = function.blocks[index].instructions;
      for ( std::size_t at = 1; at + 1 < instructions.size(); ++at )
      {
        auto const* const sum = std::get_if<ir::Binary>( &instructions[at].operation );
        auto const* const shift = std::get_if<ir::Binary>( &instructions[at - 1].operation );
        if ( sum == nullptr || shift == nullptr || sum->op != ir::BinaryOperator::ADD ||
             sum->type != ir::Type::POINTER || mPaged[sum->result.index] ||
             ir::readersOf( function, sum->result ) != 1 || shift->op != ir::BinaryOperator::SHIFT_LEFT ||
             !isValue( sum->right, shift->result ) || ir::readersOf( function, shift->result ) != 1 ||
             !isNumber( shift->right, 1 ) )
        {
          continue;
        }
        // What is doubled is a byte: one whose high byte is zero, or a byte
        // widened right before the shift, the widening then not written.
        std::optional<std::size_t> widened;
        if ( auto const* const value = std::get_if<ir::Value>( &shift->left ); value != nullptr && at >= 2 )
        {
          auto const* const convert = std::get_if<ir::Convert>( &instructions[at - 2].operation );
          if ( convert != nullptr && convert->result.index == value->index && ir::readersOf( function, *value ) == 1 &&
               ir::typeOf( convert->operand, function ) == ir::Type::U8 )
          {
            widened = at - 2;
          }
        }
        if ( !widened.has_value() && ( named( shift->left, 8 ) != "#0" || viaY( shift->left ) ) )
        {
          continue;
        }
        auto const* const base = std::get_if<ir::Constant>( &sum->left );
        auto const* const pointer = std::get_if<ir::Object>( &sum->left );
        bool const array = base != nullptr && !base->name.empty() && base->value == 0;
        if ( !array &&
             ( pointer == nullptr || pointer->type != ir::Type::POINTER || mTracker.isVolatile( pointer->name ) ) )
        {
          continue;
        }
        auto const& next = instructions[at + 1].operation;
        bool through = false;
        if ( auto const* const load = std::get_if<ir::LoadIndirect>( &next ) )
        {
          through = isValue( load->pointer, sum->result ) && isZero( load->index ) && !load->isVolatile;
        }
        else if ( auto const* const store = std::get_if<ir::StoreIndirect>( &next ) )
        {
          through = isValue( store->pointer, sum->result ) && isZero( store->index ) && !store->isVolatile &&
                    ( std::holds_alternative<ir::Constant>( store->value ) ||
                      std::holds_alternative<ir::Object>( store->value ) );
        }
        if ( !through )
        {
          continue;
        }
        mTableAddress[sum->result.index] = true;
        if ( pointer != nullptr && own.contains( pointer->name ) &&
             rangeOf( shift->left, function, ranges ).max <= 0x7F )
        {
          mTableInPlace[sum->result.index] = true;
          mAtName[sum->result.index] = pointer->name;
          mScratch[sum->result.index].reset();
        }
        mStepSilent[index][at - 1] = true;
        mScratch[shift->result.index].reset();
        if ( widened.has_value() )
        {
          mStepSilent[index][*widened] = true;
          mScratch[std::get<ir::Value>( shift->left ).index].reset();
        }
      }

      // A pair read through a pointer, read once by the next instruction as
      // an operand of a 16-bit operator that reads nothing else through `Y`.
      for ( std::size_t at = 0; at + 1 < instructions.size(); ++at )
      {
        auto const* const load = std::get_if<ir::LoadIndirect>( &instructions[at].operation );
        auto const* const user = std::get_if<ir::Binary>( &instructions[at + 1].operation );
        if ( load == nullptr || user == nullptr || ir::sizeOf( load->type ) != 2 || load->isVolatile ||
             ir::readersOf( function, load->result ) != 1 || ir::sizeOf( user->type ) != 2 ||
             user->op == ir::BinaryOperator::SHIFT_LEFT || user->op == ir::BinaryOperator::SHIFT_RIGHT ||
             user->type == ir::Type::POINTER || mPaged[user->result.index] || mDeferred[index][at] )
        {
          continue;
        }
        auto const* const offset = std::get_if<ir::Constant>( &load->index );
        bool const left = isValue( user->left, load->result );
        ir::Operand const& other = left ? user->right : user->left;
        if ( offset == nullptr || !offset->name.empty() || offset->value < 0 || offset->value > 254 ||
             ( !left && !isValue( user->right, load->result ) ) || viaY( other ) || isValue( other, load->result ) )
        {
          continue;
        }
        bool const preset = isZero( load->index ) && !paged( load->pointer ) &&
                            std::holds_alternative<ir::Value>( load->pointer ) &&
                            mTableAddress[std::get<ir::Value>( load->pointer ).index];
        mPairThroughY[load->result.index] = true;
        mPairPointer[load->result.index] = load->pointer;
        // Read by `named`, which writes the pointer's name once the scratch
        // bytes are numbered for good; non-empty, so that every plan after
        // this takes the value as read through `Y`.
        mAsOperand[load->result.index] = "(pair),y";
        if ( !preset )
        {
          mIndexToLoad[load->result.index] = "ldy #" + std::to_string( offset->value );
        }
        mDeferred[index][at] = true;
        mScratch[load->result.index].reset();
      }
    }
  }

  /// `x ^ (x << 7)`, and `|` and `&` the same: the pair shifted by seven is
  /// not computed on its own but read a byte at a time by the operator, the
  /// high byte first. That byte of the shift is `x+1` shifted out into the
  /// carry and `x` rotated in, `lda x+1 / lsr / lda x / ror`, which leaves
  /// `x`'s bit 0 in the carry; the low byte is that bit alone, `lda #0 /
  /// ror`. Written high byte first, the answer may go into `x` itself, since
  /// the low byte reads nothing of `x` but the carry — see
  /// docs/decisions/0281-a-pair-and-itself-shifted-by-seven-high-byte-first.md.
  void planShiftsBySeven( ir::Function const& function )
  {
    mBySeven.assign( function.values.size(), false );
    for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
    {
      std::vector<ir::Instruction> const& instructions = function.blocks[index].instructions;
      for ( std::size_t at = 1; at < instructions.size(); ++at )
      {
        auto const* const shift = std::get_if<ir::Binary>( &instructions[at - 1].operation );
        auto const* const user = std::get_if<ir::Binary>( &instructions[at].operation );
        if ( shift == nullptr || user == nullptr || shift->op != ir::BinaryOperator::SHIFT_LEFT ||
             ir::sizeOf( shift->type ) != 2 || !isNumber( shift->right, 7 ) ||
             ( user->op != ir::BinaryOperator::XOR && user->op != ir::BinaryOperator::OR &&
               user->op != ir::BinaryOperator::AND ) ||
             ir::sizeOf( user->type ) != 2 || ir::readersOf( function, shift->result ) != 1 ||
             mDeferred[index][at - 1] || mPaged[user->result.index] )
        {
          continue;
        }
        auto const* const shifted = std::get_if<ir::Object>( &shift->left );
        ir::Operand const& other = isValue( user->left, shift->result ) ? user->right : user->left;
        auto const* const same = std::get_if<ir::Object>( &other );
        if ( shifted == nullptr || same == nullptr || same->name != shifted->name || ir::sizeOf( shifted->type ) != 2 ||
             mTracker.isVolatile( shifted->name ) ||
             ( !isValue( user->left, shift->result ) && !isValue( user->right, shift->result ) ) )
        {
          continue;
        }
        mBySeven[user->result.index] = true;
        mDeferred[index][at - 1] = true;
        mScratch[shift->result.index].reset();
      }
    }
  }

  /// A pair masked with a number whose high byte is `$FF`, read once by a
  /// 16-bit sum or difference right after it, is read by that operator a byte
  /// at a time: its low byte `lda x / and #m`, its high byte `x+1` as it is,
  /// with no pair of its own — see
  /// docs/decisions/0285-a-pair-halved-and-doubled-is-its-low-bit-cleared.md.
  void planMaskedOperands( ir::Function const& function )
  {
    mMaskedLow.assign( function.values.size(), std::nullopt );
    for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
    {
      std::vector<ir::Instruction> const& instructions = function.blocks[index].instructions;
      for ( std::size_t at = 0; at + 1 < instructions.size(); ++at )
      {
        auto const* const mask = std::get_if<ir::Binary>( &instructions[at].operation );
        auto const* const user = std::get_if<ir::Binary>( &instructions[at + 1].operation );
        auto const* const number = mask != nullptr ? std::get_if<ir::Constant>( &mask->right ) : nullptr;
        if ( mask == nullptr || user == nullptr || number == nullptr || mask->op != ir::BinaryOperator::AND ||
             ir::sizeOf( mask->type ) != 2 || !number->name.empty() || ( number->value & 0xFF00 ) != 0xFF00 ||
             std::holds_alternative<ir::Constant>( mask->left ) || ir::readersOf( function, mask->result ) != 1 ||
             mDeferred[index][at] || ir::sizeOf( user->type ) != 2 || mPaged[user->result.index] ||
             mTableAddress[user->result.index] || mBySeven[user->result.index] )
        {
          continue;
        }
        bool const left = isValue( user->left, mask->result );
        bool const fits = user->op == ir::BinaryOperator::ADD ? left || isValue( user->right, mask->result )
                                                              : user->op == ir::BinaryOperator::SUBTRACT && left;
        ir::Operand const& other = left ? user->right : user->left;
        if ( !fits || isValue( other, mask->result ) || inA( mask->left ) )
        {
          continue;
        }
        mMaskedLow[mask->result.index] = MaskedLow{ .source = mask->left, .mask = number->value & 0xFF };
        mDeferred[index][at] = true;
        mScratch[mask->result.index].reset();
      }
    }
  }

  /// Whether an operand is the address of a 16-bit element at a byte index,
  /// its low byte in `Y`.
  [[nodiscard]] bool tableAddress( ir::Operand const& operand ) const
  {
    auto const* const value = std::get_if<ir::Value>( &operand );
    return value != nullptr && value->index < mTableAddress.size() && mTableAddress[value->index];
  }

  /// Whether an operand is an address that keeps its low byte in `Y`.
  [[nodiscard]] bool paged( ir::Operand const& operand ) const
  {
    auto const* const value = std::get_if<ir::Value>( &operand );
    return value != nullptr && value->index < mPaged.size() && mPaged[value->index];
  }

  void planIndirectOperands( ir::Function const& function, std::uint32_t index )
  {
    ir::Block const& block = function.blocks[index];
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      auto const* const load = std::get_if<ir::LoadIndirect>( &block.instructions[at].operation );
      if ( load == nullptr || load->isVolatile || ir::sizeOf( load->type ) != 1 ||
           ir::readersOf( function, load->result ) != 1 )
      {
        continue;
      }
      auto const* const pointer = std::get_if<ir::Object>( &load->pointer );
      auto const* const number = std::get_if<ir::Constant>( &load->index );
      auto const* const byte = std::get_if<ir::Object>( &load->index );
      if ( pointer == nullptr || ( number == nullptr && byte == nullptr ) ||
           ( number != nullptr && !number->name.empty() ) || ( byte != nullptr && ir::sizeOf( byte->type ) != 1 ) )
      {
        continue;
      }
      for ( std::size_t reader = at + 1; reader < block.instructions.size(); ++reader )
      {
        ir::Instruction const& candidate = block.instructions[reader];
        ir::Operand const* right = nullptr;
        if ( auto const* binary = std::get_if<ir::Binary>( &candidate.operation );
             binary != nullptr && binary->op != ir::BinaryOperator::SHIFT_LEFT &&
             binary->op != ir::BinaryOperator::SHIFT_RIGHT )
        {
          right = &binary->right;
        }
        else if ( auto const* compare = std::get_if<ir::Compare>( &candidate.operation );
                  compare != nullptr && ir::sizeOf( compare->type ) == 1 )
        {
          right = &compare->right;
        }
        if ( right != nullptr && isValue( *right, load->result ) )
        {
          mAsOperand[load->result.index] = "(" + qualified( pointer->name ) + "),y";
          mIndexToLoad[load->result.index] = "ldy " + named( load->index );
          mDeferred[index][at] = true;
          mScratch[load->result.index].reset();
          break;
        }
        // Only another read may stand between, as for an element under `X`:
        // the byte is read later than it was, and nothing may have written
        // it, the pointer or the index meanwhile.
        if ( !std::holds_alternative<ir::Load>( candidate.operation ) &&
             !std::holds_alternative<ir::LoadIndirect>( candidate.operation ) )
        {
          break;
        }
      }
    }
  }

  /// A block that ends by branching on `(x & 128) != 0`, or `& 64`, or `== 0`:
  /// `bit x` puts the bit in `N` or `V`, the `and` is not written, and `A` is
  /// left as it was — which is what keeps a byte in `A` through a loop that
  /// tests it. See docs/decisions/0114-what-the-machine-does-in-memory.md.
  void planBitTest( ir::Function const& function, std::uint32_t index )
  {
    ir::Block const& block = function.blocks[index];
    if ( !mFused[index] || block.instructions.size() < 2 )
    {
      return;
    }
    auto const* const compare = std::get_if<ir::Compare>( &block.instructions.back().operation );
    auto const* const masked = std::get_if<ir::Binary>( &block.instructions[block.instructions.size() - 2].operation );
    if ( compare == nullptr || masked == nullptr || ir::sizeOf( compare->type ) != 1 ||
         ( compare->op != ir::Comparison::EQUAL && compare->op != ir::Comparison::NOT_EQUAL ) ||
         masked->op != ir::BinaryOperator::AND || ir::sizeOf( masked->type ) != 1 )
    {
      return;
    }
    auto const* const zero = std::get_if<ir::Constant>( &compare->right );
    auto const* const mask = std::get_if<ir::Constant>( &masked->right );
    auto const* const object = std::get_if<ir::Object>( &masked->left );
    if ( zero == nullptr || zero->value != 0 || !isValue( compare->left, masked->result ) || mask == nullptr ||
         !mask->name.empty() || ( mask->value != 128 && mask->value != 64 && mask->value != 1 ) || object == nullptr ||
         ir::readersOf( function, masked->result ) != 1 )
    {
      return;
    }

    // Bit 0, where both ways begin by shifting right what holds it: the shift
    // is the test, `lsr` or `lsr x+1 / ror x`, and the bit is in the carry —
    // see docs/decisions/0280-a-shift-right-both-ways-share-is-the-test-of-bit-0.md.
    if ( mask->value == 1 )
    {
      std::optional<bool> const wide = shiftsBothWays( function, index, object->name, ir::BinaryOperator::SHIFT_RIGHT );
      if ( wide.has_value() )
      {
        mBitTest[index] = BitTest{ .masked = masked->result.index,
                                   .object = object->name,
                                   .high = false,
                                   .shifted = true,
                                   .right = true,
                                   .wide = *wide };
        mDeferred[index][block.instructions.size() - 2] = true;
      }
      return;
    }
    bool const high = mask->value == 128;
    mBitTest[index] =
        BitTest{ .masked = masked->result.index,
                 .object = object->name,
                 .high = high,
                 .shifted = high &&
                            shiftsBothWays( function, index, object->name, ir::BinaryOperator::SHIFT_LEFT ).has_value(),
                 .right = false,
                 .wide = false };
    mDeferred[index][block.instructions.size() - 2] = true;
  }

  /// A block that ends by storing the byte in `A` and jumping on, written
  /// just before a block whose whole text is the same store and the same
  /// jump, falls into it: `eor #7` and on into the other way's `sta`, where it
  /// was `eor #7 / sta x / jmp` over that `sta`. Where each way stores what
  /// `A` holds, the one store is right for both. See
  /// docs/decisions/0132-a-way-falls-into-the-store-it-shares.md.
  void planFallInto( ir::Function const& function, std::uint32_t index )
  {
    std::optional<std::uint32_t> const next = nextOf( index );
    ir::Block const& block = function.blocks[index];
    if ( !next.has_value() || block.instructions.empty() || block.terminator.kind != ir::TerminatorKind::JUMP )
    {
      return;
    }
    ir::Block const& into = function.blocks[*next];
    ir::Terminator const& onward = into.terminator;
    if ( onward.kind != ir::TerminatorKind::JUMP || onward.target != block.terminator.target || onward.target == *next )
    {
      return;
    }

    // The text of the block written next is its store alone: before it only
    // the shift a test of bit 7 has already written into `A`.
    std::size_t const silent = mShiftedIn[*next] ? 1 : 0;
    std::optional<ir::Value> const shifted =
        silent == 1 ? ir::resultOf( into.instructions.front() ) : std::optional<ir::Value>{};
    if ( into.instructions.size() != silent + 1 || ( silent == 1 && ( !shifted.has_value() || !inA( *shifted ) ) ) )
    {
      return;
    }
    auto const stored = [this]( ir::Instruction const& instruction ) -> std::optional<std::string>
    {
      auto const* const store = std::get_if<ir::Store>( &instruction.operation );
      if ( store == nullptr || ir::sizeOf( store->type ) != 1 || !inA( store->value ) )
      {
        return std::nullopt;
      }
      return store->name;
    };
    std::optional<std::string> const ours = stored( block.instructions.back() );
    if ( !ours.has_value() || ours != stored( into.instructions.back() ) ||
         std::ranges::any_of( mInPlace[index], []( std::optional<InPlace> const& made ) { return made.has_value(); } ) )
    {
      return;
    }

    // What writes the text of either block other than its instructions: the
    // loops of `X` and the `[[with]]` regions.
    for ( CountedLoop const& loop : mLoops )
    {
      for ( std::uint32_t const each : { index, *next } )
      {
        if ( each == loop.header || each == loop.step || each == loop.exit || each == loop.preheader )
        {
          return;
        }
      }
    }
    for ( ir::WithRegion const& region : function.withs )
    {
      if ( region.begin == *next || region.end == *next || region.begin == index || region.end == index )
      {
        return;
      }
    }
    mFallsInto[index] = next;
  }

  /// Whether both ways out of a test of bit 7 of the byte begin by shifting
  /// it left by one, and are entered from the test alone: the shift is then
  /// written once, before the branch, and the bit it pushes out is what the
  /// branch reads from the carry — `asl / jcc` — while both ways begin with
  /// the shifted byte already in `A`, which `mShiftedIn` records. See
  /// docs/decisions/0131-a-shift-both-ways-share-is-done-before-the-test.md.
  ///
  /// Or bit 0, where both ways begin by shifting right by one, unsigned: a
  /// byte as above, `lsr` into `A`, or a pair that both ways shift where it
  /// lies, written into the object itself, which is then shifted before the
  /// branch and its ways' shifts not written at all — `mShiftedBefore`. See
  /// docs/decisions/0280-a-shift-right-both-ways-share-is-the-test-of-bit-0.md.
  /// Answers whether the shift is of a pair, or nothing where it is not done.
  std::optional<bool>
  shiftsBothWays( ir::Function const& function, std::uint32_t index, std::string const& tested, ir::BinaryOperator op )
  {
    ir::Terminator const& end = function.blocks[index].terminator;
    // A volatile byte is read once for the test and once for the shift.
    if ( end.target == end.otherwise || end.target == index || end.otherwise == index || mTracker.isVolatile( tested ) )
    {
      return std::nullopt;
    }
    // The loops of `X` enter and leave their blocks by ways the IR does not
    // show — a header passed over, a step that branches into the body — so
    // no block of theirs is taken as entered from here alone.
    for ( CountedLoop const& loop : mLoops )
    {
      for ( std::uint32_t const block : { end.target, end.otherwise } )
      {
        if ( index == loop.header || block == loop.header || block == loop.header + 1 || block == loop.exit )
        {
          return std::nullopt;
        }
      }
    }
    // Per way: nothing, or whether it shifts a pair.
    auto const shifts = [this, &function, &tested, index, op]( std::uint32_t into ) -> std::optional<bool>
    {
      ir::Block const& block = function.blocks[into];
      if ( into == 0 || block.instructions.empty() || onlyPredecessorOf( function, into ) != index )
      {
        return std::nullopt;
      }
      auto const* const shift = std::get_if<ir::Binary>( &block.instructions.front().operation );
      auto const* const object = shift != nullptr ? std::get_if<ir::Object>( &shift->left ) : nullptr;
      auto const* const by = shift != nullptr ? std::get_if<ir::Constant>( &shift->right ) : nullptr;
      if ( shift == nullptr || shift->op != op || object == nullptr || object->name != tested || by == nullptr ||
           !by->name.empty() || by->value != 1 )
      {
        return std::nullopt;
      }
      if ( ir::sizeOf( shift->type ) == 1 && ( op == ir::BinaryOperator::SHIFT_LEFT || shift->type == ir::Type::U8 ) )
      {
        return false;
      }
      if ( op == ir::BinaryOperator::SHIFT_RIGHT && shift->type == ir::Type::U16 &&
           mDirect[shift->result.index] == tested )
      {
        return true;
      }
      return std::nullopt;
    };
    std::optional<bool> const one = shifts( end.target );
    std::optional<bool> const other = shifts( end.otherwise );
    if ( !one.has_value() || one != other )
    {
      return std::nullopt;
    }
    std::vector<bool>& marked = *one ? mShiftedBefore : mShiftedIn;
    marked[end.target] = true;
    marked[end.otherwise] = true;
    return one;
  }

  /// A byte value with a scratch byte of its own, read once by a later
  /// instruction of the same block that takes it from `A` — as the left of an
  /// operator or a compare, as the right of one that commutes, as what a store
  /// writes, as an index, as a conversion's operand, or as a branch's
  /// condition — where everything between is written as nothing, need not
  /// wait at all: it is still in `A` when the reader comes. The scratch byte
  /// was given because the next instruction of the IR did not take the value
  /// from `A`, and that instruction turned out not to be written. See
  /// docs/decisions/0116-a-value-stays-in-a-over-a-silent-gap.md.
  void planSilentGaps( ir::Function const& function, std::uint32_t index )
  {
    ir::Block const& block = function.blocks[index];
    CountedLoop const* const loop = loopAt( index );
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      std::optional<ir::Value> const defined = ir::resultOf( block.instructions[at] );
      if ( !defined.has_value() || !mScratch[defined->index].has_value() || isWide( function, *defined ) ||
           !mAsOperand[defined->index].empty() || mDeferred[index][at] ||
           std::holds_alternative<ir::Call>( block.instructions[at].operation ) ||
           ir::readersOf( function, *defined ) != 1 )
      {
        continue;
      }

      // The one reader, and whether it takes the value from `A`.
      std::optional<std::size_t> reader;
      bool served = false;
      auto const is = [&defined]( ir::Operand const& operand ) { return isValue( operand, *defined ); };
      for ( std::size_t use = at + 1; use < block.instructions.size() && !reader.has_value(); ++use )
      {
        ir::Instruction const& candidate = block.instructions[use];
        std::visit(
            [&]( auto const& operation )
            {
              using Operation = std::decay_t<decltype( operation )>;
              if constexpr ( std::is_same_v<Operation, ir::Store> )
              {
                if ( is( operation.value ) )
                {
                  reader = use;
                  served = ir::sizeOf( operation.type ) == 1 && !operation.name.contains( ',' );
                }
              }
              else if constexpr ( std::is_same_v<Operation, ir::Binary> )
              {
                if ( is( operation.left ) || is( operation.right ) )
                {
                  reader = use;
                  bool const shift =
                      operation.op == ir::BinaryOperator::SHIFT_LEFT || operation.op == ir::BinaryOperator::SHIFT_RIGHT;
                  // The other operand in `A` takes `A`'s way in: one of the
                  // two must then come from memory, and it is ours.
                  bool const commutes =
                      operation.op == ir::BinaryOperator::ADD || operation.op == ir::BinaryOperator::AND ||
                      operation.op == ir::BinaryOperator::OR || operation.op == ir::BinaryOperator::XOR;
                  ir::Operand const& other = is( operation.left ) ? operation.right : operation.left;
                  served = ir::sizeOf( operation.type ) == 1 && !shift && !inA( other ) &&
                           ( is( operation.left ) || commutes );
                }
              }
              else if constexpr ( std::is_same_v<Operation, ir::Compare> )
              {
                if ( is( operation.left ) || is( operation.right ) )
                {
                  reader = use;
                  bool const either =
                      operation.op == ir::Comparison::EQUAL || operation.op == ir::Comparison::NOT_EQUAL;
                  ir::Operand const& other = is( operation.left ) ? operation.right : operation.left;
                  served = ir::sizeOf( operation.type ) == 1 && !inA( other ) && ( is( operation.left ) || either );
                }
              }
              else if constexpr ( std::is_same_v<Operation, ir::Load> )
              {
                if ( is( operation.index ) )
                {
                  reader = use;
                  served = ir::sizeOf( operation.type ) == 1 && !operation.scaled;
                }
              }
              else if constexpr ( std::is_same_v<Operation, ir::Unary> || std::is_same_v<Operation, ir::Convert> )
              {
                if ( is( operation.operand ) )
                {
                  reader = use;
                  served = true;
                }
              }
              else if constexpr ( std::is_same_v<Operation, ir::StoreElement> )
              {
                if ( is( operation.index ) || is( operation.value ) )
                {
                  reader = use;
                }
              }
              else if constexpr ( std::is_same_v<Operation, ir::LoadIndirect> )
              {
                if ( is( operation.pointer ) || is( operation.index ) )
                {
                  reader = use;
                }
              }
              else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> )
              {
                if ( is( operation.pointer ) || is( operation.index ) || is( operation.value ) )
                {
                  reader = use;
                }
              }
              else if constexpr ( std::is_same_v<Operation, ir::Call> )
              {
                if ( std::ranges::any_of( operation.arguments,
                                          [&is]( ir::Argument const& argument ) { return is( argument.value ); } ) )
                {
                  reader = use;
                }
              }
              else if constexpr ( std::is_same_v<Operation, ir::Switch> )
              {
                if ( is( operation.value ) )
                {
                  reader = use;
                }
              }
              else if constexpr ( std::is_same_v<Operation, ir::EnterWith> )
              {
                if ( operation.index.has_value() && is( *operation.index ) )
                {
                  reader = use;
                }
              }
            },
            candidate.operation );
      }
      bool const branchReads = !reader.has_value() && block.terminator.kind == ir::TerminatorKind::BRANCH &&
                               is( block.terminator.condition );
      if ( branchReads )
      {
        reader = block.instructions.size();
        served = true;
      }
      if ( !reader.has_value() || !served )
      {
        continue;
      }

      // Written as nothing: an element deferred to its reader, whose `ldx`
      // leaves `A` alone, or a sum the address of one takes.
      bool silent = true;
      for ( std::size_t here = at + 1; here < *reader && here < block.instructions.size(); ++here )
      {
        auto const* const sum = std::get_if<ir::Binary>( &block.instructions[here].operation );
        bool const folded =
            sum != nullptr && loop != nullptr && loop->offsetOf( ir::Operand{ sum->result } ).has_value();
        silent = silent && ( mDeferred[index][here] || folded );
      }
      if ( silent )
      {
        mScratch[defined->index].reset();
      }
    }
  }

  /// The scratch bytes renumbered without the ones given up above and by the
  /// elements deferred to their readers, so that no `.ztemp` is declared that
  /// nothing reads or writes.
  void compactScratch()
  {
    std::vector<std::optional<std::uint32_t>> renumbered( mScratchSizes.size() );
    std::vector<std::uint32_t> sizes;
    std::vector<bool> pointers;
    for ( std::optional<std::uint32_t> const& used : mScratch )
    {
      if ( used.has_value() && !renumbered[*used].has_value() )
      {
        renumbered[*used] = static_cast<std::uint32_t>( sizes.size() );
        sizes.push_back( mScratchSizes[*used] );
        pointers.push_back( mScratchPointers[*used] );
      }
    }
    for ( std::optional<std::uint32_t>& used : mScratch )
    {
      if ( used.has_value() )
      {
        used = renumbered[*used];
      }
    }
    mScratchSizes = std::move( sizes );
    mScratchPointers = std::move( pointers );
  }

  /// Where an operand is an element whose index is not in `X` yet, the index
  /// goes there now: before the left operand is loaded, which does not touch
  /// `X`, and before the operator reads the element.
  void indexFor( ir::Operand const& operand )
  {
    auto const* const value = std::get_if<ir::Value>( &operand );
    if ( value != nullptr && !mIndexToLoad[value->index].empty() )
    {
      line( mIndexToLoad[value->index] );
    }
  }

  /// The constant an index carries in the address rather than in `X`: `+1` of
  /// `t+1,x`, or nothing.
  [[nodiscard]] std::string addressOffset( ir::Operand const& index ) const
  {
    CountedLoop const* const loop = loopAt( mBlock );
    std::optional<std::int64_t> by = loop != nullptr ? loop->offsetOf( index ) : std::nullopt;
    if ( auto const* const value = std::get_if<ir::Value>( &index ); value != nullptr )
    {
      if ( std::optional<FoldedIndex> const& folded = mFoldedIndex[value->index]; folded.has_value() )
      {
        by = folded.value().by;
      }
    }
    return offsetText( by.value_or( 0 ) );
  }

  /// Whether an index is a number the text may write into the address: a
  /// `const` written by its name is not one, since what the text has for it
  /// is that name.
  [[nodiscard]] static bool isANumber( ir::Operand const& index )
  {
    auto const* const constant = std::get_if<ir::Constant>( &index );
    return constant != nullptr && constant->name.empty();
  }

  /// Where the element at such an index stands, `t+3`, and takes no index
  /// register — see
  /// docs/decisions/0200-an-index-that-is-a-number-is-an-address.md. An array
  /// of 16-bit elements holds two bytes to an index, which the address
  /// carries as the doubled offset; a striped array holds one, as `scaled`
  /// says, and names the stripe its high bytes lie in itself.
  [[nodiscard]] std::string atAConstantIndex( std::string const& name,
                                              std::string const& high,
                                              ir::Operand const& index,
                                              ir::Type type,
                                              bool scaled,
                                              std::uint32_t byte = 0 ) const
  {
    std::int64_t const by = std::get<ir::Constant>( index ).value * ( scaled || ir::sizeOf( type ) == 1 ? 1 : 2 );
    if ( byte == 1 && !high.empty() )
    {
      return qualified( high ) + offsetText( by );
    }
    return qualified( name ) + offsetText( by + static_cast<std::int64_t>( byte ) );
  }

  /// A number an address carries: `+1`, `-4`, or nothing for none.
  [[nodiscard]] static std::string offsetText( std::int64_t by )
  {
    if ( by == 0 )
    {
      return "";
    }
    return by > 0 ? "+" + std::to_string( by ) : std::to_string( by );
  }

  /// Whether the instruction is a sum the address takes, and so is not written.
  [[nodiscard]] bool isFoldedSum( ir::Instruction const& instruction ) const
  {
    CountedLoop const* const loop = loopAt( mBlock );
    auto const* const binary = std::get_if<ir::Binary>( &instruction.operation );
    return binary != nullptr &&
           ( mFoldedIndex[binary->result.index].has_value() || mByteView[binary->result.index].has_value() ||
             ( loop != nullptr && loop->offsetOf( ir::Operand{ binary->result } ).has_value() ) );
  }

  /// The byte `index` places above the low one of what `name` names.
  static std::string byteOf( std::string const& name, std::uint32_t index )
  {
    return index == 0 ? name : name + "+" + std::to_string( index );
  }

  /// An operand as an instruction's operand names it: a constant immediate,
  /// or the byte `shift` places above the low one of an object, of a scratch
  /// byte or pair, or of the object a value is written into. Never a value in
  /// `A`, which has no name.
  [[nodiscard]] std::string named( ir::Operand const& operand, std::uint32_t shift = 0 ) const
  {
    // An operand narrower than what reads it stands for its own value, and the
    // bytes above it are zero — which is what lets the narrowing pass leave a
    // byte where the subset's rules asked for a pair, rather than widening it
    // into one first. Unsigned only: a signed byte's high byte is its sign,
    // and no immediate carries that. See docs/decisions/0108-a-known-zero-high-byte.md.
    if ( shift >= 8 && ir::sizeOf( ir::typeOf( operand, *mFunction ) ) * 8 <= shift &&
         !ir::isSigned( ir::typeOf( operand, *mFunction ) ) )
    {
      return "#0";
    }
    // A pair the operator reads through a pointer a byte at a time.
    if ( auto const* value = std::get_if<ir::Value>( &operand );
         value != nullptr && value->index < mPairPointer.size() )
    {
      if ( std::optional<ir::Operand> const& pointer = mPairPointer[value->index]; pointer.has_value() )
      {
        return "(" + named( *pointer ) + "),y";
      }
    }
    // An element the instruction reads from memory itself, `data+1,x`.
    if ( auto const* value = std::get_if<ir::Value>( &operand ); value != nullptr && !mAsOperand[value->index].empty() )
    {
      return mAsOperand[value->index];
    }
    // A shift by eight, read as the bytes it is made of.
    if ( auto const* value = std::get_if<ir::Value>( &operand ); value != nullptr && value->index < mByteView.size() )
    {
      if ( std::optional<ByteView> const& view = mByteView[value->index]; view.has_value() )
      {
        bool const zero = view.value().left == ( shift < 8 );
        return zero ? std::string{ "#0" } : named( view.value().source, view.value().left ? 0 : 8 );
      }
    }
    if ( auto const* constant = std::get_if<ir::Constant>( &operand ) )
    {
      if ( !constant->name.empty() )
      {
        if ( ir::sizeOf( constant->type ) == 1 )
        {
          return "#" + constant->name;
        }
        // An address with an offset is one operand of `<` or `>`.
        bool const offset = constant->name.find_first_of( "+-" ) != std::string::npos;
        return ( shift == 0 ? "#<" : "#>" ) + ( offset ? "(" + constant->name + ")" : constant->name );
      }
      return "#" + std::to_string( ( static_cast<std::uint64_t>( constant->value ) >> shift ) & 0xFFU );
    }
    if ( auto const* object = std::get_if<ir::Object>( &operand ) )
    {
      return byteOf( qualified( object->name ), shift / 8 );
    }
    return byteOf( location( std::get<ir::Value>( operand ) ), shift / 8 );
  }

  /// A 16-bit operand copied into the two bytes `to` names, unless it is
  /// already there.
  void copy( ir::Operand const& operand, std::string const& to )
  {
    auto const* object = std::get_if<ir::Object>( &operand );
    if ( ( object != nullptr && object->name == to ) || named( operand ) == to )
    {
      return;
    }
    follows( operand );
    line( "lda " + named( operand ) );
    line( "sta " + to );
    follows( operand );
    line( "lda " + named( operand, 8 ) );
    line( "sta " + byteOf( to, 1 ) );
  }

  /// The address of a function is followed by the Proc of its type, and the
  /// line before the taking says so — see docs/decisions/0060-own.md.
  void follows( ir::Operand const& operand )
  {
    auto const* const constant = std::get_if<ir::Constant>( &operand );
    if ( constant != nullptr && !constant->follower.empty() )
    {
      line( ".own " + constant->follower );
    }
  }

  void load( ir::Operand const& operand )
  {
    if ( counterInX( operand ) )
    {
      line( "txa" );
      return;
    }
    if ( !inA( operand ) )
    {
      line( "lda " + named( operand ) );
    }
  }

  /// Where a value just computed in `A` is to live.
  void keep( ir::Value result )
  {
    if ( std::optional<std::uint32_t> const scratch = mScratch[result.index]; scratch.has_value() )
    {
      line( "sta " + qualified( scratchName( *scratch ) ) );
    }
  }

  /// A certain jump to a block that holds nothing and only returns is written
  /// as that return: one byte where the jump is two or three, and three
  /// cycles fewer, the `rts` standing where the jump would have gone. Answers
  /// whether it wrote it. See
  /// docs/decisions/0203-a-jump-to-a-block-that-only-returns-returns.md.
  ///
  /// Here and not in `threadJumps`, which is where 0152 follows a block that
  /// only jumps on, for two reasons the IR cannot see: a jump to the block
  /// written next writes nothing at all, so taking its `rts` would cost the
  /// byte rather than save it wherever something else still reaches that
  /// block; and 0132's fall-into asks that both ways still end in a jump to
  /// the same block, which a `return` in the IR would no longer be.
  [[nodiscard]] bool returned( std::uint32_t from, std::uint32_t target )
  {
    ir::Block const& into = mFunction->blocks[target];
    if ( !into.instructions.empty() || into.terminator.kind != ir::TerminatorKind::RETURN )
    {
      return false;
    }
    if ( tailCalled( mFunction->blocks[from] ) )
    {
      return true;
    }
    under( into.terminator.at );
    mReturnHolds.push_back( mTracker.inA() );
    line( "rts" );
    return true;
  }

  /// A call written last in its block, the function returning right after
  /// it: the `jsr` becomes `jmp` and no `rts` is written, the callee's own
  /// returning for both — a byte, nine cycles and two bytes of stack fewer.
  /// Only where what the function returns is the callee's to leave: nothing,
  /// or the byte the callee returns in, which 0278 names; and not in a
  /// function that answers for a Window. Answers whether it wrote it. See
  /// docs/decisions/0279-a-call-the-function-returns-after-is-a-jump.md.
  [[nodiscard]] bool tailCalled( ir::Block const& block )
  {
    if ( block.instructions.empty() )
    {
      return false;
    }
    auto const* const call = std::get_if<ir::Call>( &block.instructions.back().operation );
    ir::Function const& function = *mFunction;
    if ( call == nullptr || call->result.has_value() || !call->with.empty() || !function.withs.empty() ||
         !function.pane.empty() || !function.under.empty() || function.isTrampoline || !function.memberOf.empty() )
    {
      return false;
    }
    if ( function.result.has_value() )
    {
      std::string const byte = call->returned.empty() ? call->name + "." + std::string{ RESULT } : call->returned;
      if ( function.resultElsewhere.empty() || function.resultElsewhere != byte )
      {
        return false;
      }
    }

    // The `jsr` is the last line written, source marks aside.
    std::string const jsr = std::string{ INDENT } + "jsr " + call->name + "\n";
    std::size_t const at = mText.rfind( jsr );
    if ( at == std::string::npos )
    {
      return false;
    }
    for ( std::size_t rest = at + jsr.size(); rest < mText.size(); )
    {
      std::size_t const end = mText.find( '\n', rest );
      std::string_view const after{ mText.data() + rest, ( end == std::string::npos ? mText.size() : end ) - rest };
      if ( !after.starts_with( std::string{ INDENT } + ".source " ) )
      {
        return false;
      }
      rest = end == std::string::npos ? mText.size() : end + 1;
    }
    // The marks after it were for the `rts`, which is not written.
    mText.resize( at + jsr.size() );
    mText.replace( at + INDENT.size(), 3, "jmp" );
    mMarked.reset();
    mTracker.forget();
    return true;
  }

  void terminator( std::uint32_t index )
  {
    ir::Block const& block = mFunction->blocks[index];
    ir::Terminator const& end = block.terminator;
    std::optional<std::uint32_t> const next = nextOf( index );
    switch ( end.kind )
    {
    case ir::TerminatorKind::DISPATCH:
    {
      // The value is clamped in `A` by the `switch` instruction before this,
      // and `.dispatch` names where each of its values goes: positions of
      // this Proc, so nothing escapes and the Proc keeps its liveness — see
      // docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
      // How it is written is the Target's, and it leaves `A`, `X` and the
      // flags saying nothing.
      under( end.at );
      for ( std::size_t at = 0; at < end.targets.size(); at += TABLE_LINE )
      {
        std::string row;
        for ( std::size_t one = at; one < std::min( end.targets.size(), at + TABLE_LINE ); ++one )
        {
          row.append( one == at ? "" : ", " ).append( blockLabel( end.targets[one] ) );
        }
        line( ".dispatch " + row );
      }
      mTracker.forget();
      return;
    }
    case ir::TerminatorKind::RETURN:
      if ( tailCalled( block ) )
      {
        return;
      }
      under( end.at );
      mReturnHolds.push_back( mTracker.inA() );
      line( "rts" );
      return;
    case ir::TerminatorKind::TRANSITION:
      // The Phase is entered and nothing comes back, so the calls a `return`
      // would unwind are left where they are: the routine takes the stack
      // back to where the Phase entered began — see
      // docs/decisions/0064-phase-in-c.md.
      under( end.at );
      line( ".transition " + end.phase );
      return;
    case ir::TerminatorKind::FALL:
      return;
    case ir::TerminatorKind::JUMP:
    {
      CountedLoop const* const loop = loopAt( index );
      if ( loop != nullptr && loop->step == index && end.target == loop->header && rotates( *loop ) )
      {
        // The header's test, written again here with its branch turned over:
        // into the body where the counter is still below the bound, and out
        // through the fall-through where it is not.
        ir::Block const& header = mFunction->blocks[loop->header];
        under( header.instructions.back().at );
        Jumps const jumps = flags( std::get<ir::Compare>( header.instructions.back().operation ) );
        line( std::string{ jumps.whenTrue } + " " + blockLabel( loop->header + 1 ) );
        return;
      }
      if ( std::optional<Rotated> const& turned = mRotatedStep[index]; turned.has_value() && turned->general )
      {
        rotatedTest( *turned );
        return;
      }
      if ( next != end.target && !returned( index, end.target ) )
      {
        under( end.at );
        std::string_view const taken = mTracker.alwaysTaken();
        line( ( taken.empty() ? std::string{ "jmp" } : std::string{ taken } ) + " " + blockLabel( end.target ) );
      }
      return;
    }
    case ir::TerminatorKind::BRANCH:
    {
      // A rotated loop entered where its first test is known to pass falls
      // straight into the body — see docs/decisions/0128-a-loop-tests-where-it-jumps-back.md.
      CountedLoop const* const loop = loopAt( index );
      if ( loop != nullptr && loop->header == index && rotates( *loop ) && entersWithoutTest( *loop ) )
      {
        // A `<` test passed leaves the carry clear, and the edge back into the
        // body does too, so where the body reads it first a `clc` stands in for
        // the test: 0113 then leaves the body's own `clc` out at every turn —
        // see docs/decisions/0208-a-loop-entered-with-the-carry-cleared.md.
        auto const& compare = std::get<ir::Compare>( block.instructions.back().operation );
        if ( compare.op == ir::Comparison::LESS && bodyReadsCarry( *loop ) )
        {
          under( end.at );
          line( "clc" );
        }
        return;
      }
      under( end.at );
      Jumps jumps{ .whenTrue = "jne", .whenFalse = "jeq" };
      if ( mFused[index] )
      {
        // Where each way goes, for a 16-bit comparison that decides it before
        // the branch: straight there where it is a jump, and past the branch
        // where it is the block written next.
        auto const& compare = std::get<ir::Compare>( block.instructions.back().operation );
        mWays =
            Ways{ .whenTrue = next == end.target ? std::nullopt : std::optional{ blockLabel( end.target ) },
                  .whenFalse = next == end.otherwise ? std::nullopt : std::optional{ blockLabel( end.otherwise ) } };
        jumps = flags( compare );
        mWays.reset();
      }
      else
      {
        // A `bool` in `A` left its flags there; anywhere else, loading it sets
        // them — and it is the flags that are wanted, so the load is written
        // even where `A` already holds the byte.
        mTracker.forget();
        load( end.condition );
      }
      if ( next == end.otherwise )
      {
        line( std::string{ jumps.whenTrue } + " " + blockLabel( end.target ) );
      }
      else if ( next == end.target )
      {
        line( std::string{ jumps.whenFalse } + " " + blockLabel( end.otherwise ) );
      }
      else
      {
        line( std::string{ jumps.whenTrue } + " " + blockLabel( end.target ) );
        if ( !returned( index, end.otherwise ) )
        {
          std::string_view const taken = mTracker.alwaysTaken();
          line( ( taken.empty() ? std::string{ "jmp" } : std::string{ taken } ) + " " + blockLabel( end.otherwise ) );
        }
      }
      if ( mPastBranch.has_value() )
      {
        pastLabel();
      }
      return;
    }
    }
  }

  /// A loop's header test written again where the loop jumps back: into the
  /// body where it holds, falling out to the exit written next where it does
  /// not — see docs/decisions/0156-every-loop-tests-where-it-jumps-back.md.
  /// The header's own plans are read as the header's, with `mBlock` at it.
  void rotatedTest( Rotated const& turned )
  {
    std::uint32_t const head = turned.body - 1;
    ir::Block const& header = mFunction->blocks[head];
    ir::Terminator const& test = header.terminator;
    auto const& compare = std::get<ir::Compare>( header.instructions.back().operation );
    under( header.instructions.back().at );
    mWays = Ways{ .whenTrue = test.target == turned.exit ? std::nullopt : std::optional{ blockLabel( test.target ) },
                  .whenFalse =
                      test.otherwise == turned.exit ? std::nullopt : std::optional{ blockLabel( test.otherwise ) } };
    std::uint32_t const own = std::exchange( mBlock, head );
    Jumps const jumps = flags( compare );
    mBlock = own;
    mWays.reset();
    line( std::string{ test.target == turned.body ? jumps.whenTrue : jumps.whenFalse } + " " +
          blockLabel( turned.body ) );
    mRotatedReach[mBlock] = estimatedBytes( std::string_view{ mText }.substr( mBlockTextAt[turned.body] ) );
    if ( mPastBranch.has_value() )
    {
      pastLabel();
    }
  }

  /// What `A` holds where a test branches past the branch that reads it,
  /// kept for the label it goes to: every such way agrees on it.
  void heldPast()
  {
    if ( !mPastHolds.has_value() )
    {
      mPastHolds = mTracker.inA();
      return;
    }
    std::erase_if( *mPastHolds, [this]( std::string const& name ) { return !mTracker.holdsInA( name ); } );
  }

  /// The label a test went past to, entered from its own branches and from
  /// the one written last. `A` holds there what all of them agree on, where
  /// each branch there said what it held — so a loop whose counter `Y` and
  /// `X` hold enters its body with `A` holding the high byte both ways round,
  /// as docs/decisions/0291-a-pair-counter-lives-in-y-and-x.md has it.
  void pastLabel()
  {
    std::optional<std::vector<std::string>> kept = std::exchange( mPastHolds, std::nullopt );
    if ( kept.has_value() )
    {
      std::erase_if( *kept, [this]( std::string const& name ) { return !mTracker.holdsInA( name ); } );
    }
    if ( std::optional<std::string> const past = std::exchange( mPastBranch, std::nullopt ); past.has_value() )
    {
      label( *past );
    }
    if ( kept.has_value() )
    {
      mTracker.enter( std::move( *kept ), Carry::UNKNOWN );
    }
  }

  /// A comparison left in the flags for the branch after it, and the Jcc that
  /// reads it each way.
  Jumps flags( ir::Compare const& instruction )
  {
    if ( ir::sizeOf( instruction.type ) == 2 )
    {
      return wideFlags( instruction );
    }

    // `(x & 128) != 0`: `bit x` puts bit 7 in `N` and bit 6 in `V`, and reads
    // nothing into `A`.
    if ( std::optional<BitTest> const& test = mBitTest[mBlock];
         test.has_value() && isValue( instruction.left, ir::Value{ test->masked } ) )
    {
      bool const set = instruction.op == ir::Comparison::NOT_EQUAL;
      if ( test->shifted )
      {
        if ( test->wide )
        {
          line( "lsr " + byteOf( qualified( test->object ), 1 ) );
          line( "ror " + qualified( test->object ) );
        }
        else
        {
          load( ir::Object{ .name = test->object, .type = ir::Type::U8 } );
          line( test->right ? "lsr" : "asl" );
        }
        return set ? Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" } : Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" };
      }
      // The carry holds that bit already, where an `asl` pushed it out of `A`
      // since the byte was last written: no `bit`, and nothing read.
      if ( test->high && mTracker.bit7InCarry( qualified( test->object ) ) )
      {
        return set ? Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" } : Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" };
      }
      line( "bit " + qualified( test->object ) );
      if ( test->high )
      {
        return set ? Jumps{ .whenTrue = "jmi", .whenFalse = "jpl" } : Jumps{ .whenTrue = "jpl", .whenFalse = "jmi" };
      }
      return set ? Jumps{ .whenTrue = "jvs", .whenFalse = "jvc" } : Jumps{ .whenTrue = "jvc", .whenFalse = "jvs" };
    }
    ir::Operand const* first = &instruction.left;
    ir::Operand const* second = &instruction.right;
    if ( inA( *second ) )
    {
      std::swap( first, second );
    }

    // A counted loop's test is against `X`, which already holds what the byte
    // would be loaded for — see docs/decisions/0109-an-induction-variable-in-x.md.
    auto const* const counter = std::get_if<ir::Object>( first );
    bool const testsX = counter != nullptr && inX( mBlock, counter->name );
    indexFor( *second );
    if ( !testsX )
    {
      load( *first );
    }
    std::string const against = testsX ? "cpx " : "cmp ";

    switch ( instruction.op )
    {
    case ir::Comparison::EQUAL:
    case ir::Comparison::NOT_EQUAL:
      if ( isNumber( *second, 0xFF ) )
      {
        // Against 255, after `sbc #1`: the carry is clear exactly where it
        // borrowed, which is where `A` is now 255. And a counter stepped down
        // in `X` from a start below 128 holds nothing from 128 to 254, so its
        // sign says it — see docs/decisions/0262-a-counter-stepped-down-counts-in-x.md.
        bool const equal = instruction.op == ir::Comparison::EQUAL;
        if ( !testsX && mTracker.carrySaysNot255() )
        {
          return equal ? Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" }
                       : Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" };
        }
        CountedLoop const* const loop = loopAt( mBlock );
        if ( testsX && mTracker.flagsFromX() && loop->down && loop->from <= 127 )
        {
          return equal ? Jumps{ .whenTrue = "jmi", .whenFalse = "jpl" }
                       : Jumps{ .whenTrue = "jpl", .whenFalse = "jmi" };
        }
      }
      // Against zero, where the flags already say what `A` holds, or what
      // `X` holds for a counter: `Z` is the answer, and `cmp #0` or `cpx #0`
      // would only write it again.
      if ( !isZero( *second ) || !( testsX ? mTracker.flagsFromX() : mTracker.flagsFromA() ) )
      {
        line( against + named( *second ) );
      }
      return instruction.op == ir::Comparison::EQUAL ? Jumps{ .whenTrue = "jeq", .whenFalse = "jne" }
                                                     : Jumps{ .whenTrue = "jne", .whenFalse = "jeq" };
    case ir::Comparison::LESS:
    case ir::Comparison::GREATER_OR_EQUAL:
      break;
    }

    bool const less = instruction.op == ir::Comparison::LESS;
    if ( !ir::isSigned( instruction.type ) )
    {
      // Carry clear where the first is less.
      line( against + named( *second ) );
      return less ? Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" } : Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" };
    }

    // Against zero the subtraction is idle — `A - 0` is `A` — so it cannot
    // overflow, and the sign of the value alone is the answer. `N` is what
    // says it, and the load above left it there; where that load was left out
    // because `A` already held the byte, `cmp #0` says it again. See
    // docs/decisions/0190-the-sign-is-the-answer.md.
    if ( testsX && isZero( *second ) )
    {
      // A signed counter stepped down in `X`: `dex` left its sign in `N`, and
      // `cpx #0` says it again where something else set the flags since —
      // see docs/decisions/0262-a-counter-stepped-down-counts-in-x.md.
      if ( !mTracker.flagsFromX() )
      {
        line( "cpx #0" );
      }
      return less ? Jumps{ .whenTrue = "jmi", .whenFalse = "jpl" } : Jumps{ .whenTrue = "jpl", .whenFalse = "jmi" };
    }
    if ( !testsX && isZero( *second ) )
    {
      if ( !mTracker.flagsFromA() )
      {
        // `ora #0` and not `cmp #0`: both say what `A` holds, and this one
        // leaves the carry alone and counts as setting the flags from `A`,
        // so a second test of the same byte says nothing at all.
        line( "ora #0" );
      }
      return less ? Jumps{ .whenTrue = "jmi", .whenFalse = "jpl" } : Jumps{ .whenTrue = "jpl", .whenFalse = "jmi" };
    }

    // A signed counter is kept in `X` only where it is tested against zero, so
    // this path always has its value in `A`.
    line( "sec" );
    line( "sbc " + named( *second ) );
    signIntoN();
    return less ? Jumps{ .whenTrue = "jmi", .whenFalse = "jpl" } : Jumps{ .whenTrue = "jpl", .whenFalse = "jmi" };
  }

  /// After a subtraction, `N` set where the first was less as signed: the
  /// sign of the difference, turned over where it overflowed. Reads `V`.
  void signIntoN()
  {
    std::string const skip = localLabel( "v" );
    line( "bvc " + skip );
    line( "eor #128" );
    label( skip );
  }

  /// After a subtraction, the carry set where the first was less as signed:
  /// where the sign and the overflow differ. Adding $40 to those two bits of
  /// the status leaves the top bit set exactly then, and shifts it into
  /// carry. Reads `V`, and leaves `A` holding nothing of the difference.
  void signIntoCarry()
  {
    line( "php" );
    line( "pla" );
    line( "and #192" );
    line( "clc" );
    line( "adc #64" );
    line( "asl" );
  }

  /// A 16-bit comparison left in the flags: equality as the low bytes' and,
  /// where they agree, the high bytes'; order as a subtraction whose borrow
  /// runs from the low bytes into the high ones.
  Jumps wideFlags( ir::Compare const& instruction )
  {
    ir::Operand const& left = instruction.left;
    ir::Operand const& right = instruction.right;
    if ( instruction.op == ir::Comparison::EQUAL || instruction.op == ir::Comparison::NOT_EQUAL )
    {
      bool const zero = isZero( right );
      if ( zero && steppedJustBefore( instruction ) )
      {
        return instruction.op == ir::Comparison::EQUAL ? Jumps{ .whenTrue = "jeq", .whenFalse = "jne" }
                                                       : Jumps{ .whenTrue = "jne", .whenFalse = "jeq" };
      }
      if ( pairInXA( instruction ) )
      {
        // The high byte in `A` first, the low one in `X` where they agree —
        // see docs/decisions/0289-a-pair-only-compared-is-held-in-a-and-x.md.
        Ways const ways = mWays.value_or( Ways{} );
        std::optional<std::string> const differTo =
            instruction.op == ir::Comparison::EQUAL ? ways.whenFalse : ways.whenTrue;
        line( "cmp " + boundHigh( instruction ) );
        if ( differTo.has_value() )
        {
          leftEarly( true );
          line( "jne " + *differTo );
        }
        else
        {
          std::string const differ = localLabel( "e" );
          line( "bne " + differ );
          line( "cpx " + named( right ) );
          if ( mWays.has_value() )
          {
            mPastBranch = differ;
            mPastHolds.reset();
          }
          else
          {
            label( differ );
          }
          return instruction.op == ir::Comparison::EQUAL ? Jumps{ .whenTrue = "jeq", .whenFalse = "jne" }
                                                         : Jumps{ .whenTrue = "jne", .whenFalse = "jeq" };
        }
        line( "cpx " + named( right ) );
        return instruction.op == ir::Comparison::EQUAL ? Jumps{ .whenTrue = "jeq", .whenFalse = "jne" }
                                                       : Jumps{ .whenTrue = "jne", .whenFalse = "jeq" };
      }
      line( "lda " + named( left ) );
      if ( !zero || !mTracker.flagsFromA() )
      {
        line( "cmp " + named( right ) );
      }
      // Low bytes that differ decide it: straight to where that goes, where
      // the branch after this is a jump there, and otherwise past the high
      // bytes to that branch — see
      // docs/decisions/0153-a-low-byte-that-differs-branches-where-it-goes.md.
      Ways const ways = mWays.value_or( Ways{} );
      std::optional<std::string> const differTo =
          instruction.op == ir::Comparison::EQUAL ? ways.whenFalse : ways.whenTrue;
      if ( differTo.has_value() )
      {
        // The carry on that way is the low bytes' and not the high bytes',
        // which is what the branch below leaves: no edge may count on it.
        leftEarly( true );
        line( "jne " + *differTo );
        line( "lda " + named( left, 8 ) );
        if ( !zero || !mTracker.flagsFromA() )
        {
          line( "cmp " + boundHigh( instruction ) );
        }
        return instruction.op == ir::Comparison::EQUAL ? Jumps{ .whenTrue = "jeq", .whenFalse = "jne" }
                                                       : Jumps{ .whenTrue = "jne", .whenFalse = "jeq" };
      }
      std::string const differ = localLabel( "e" );
      line( "bne " + differ );
      line( "lda " + named( left, 8 ) );
      if ( !zero || !mTracker.flagsFromA() )
      {
        line( "cmp " + boundHigh( instruction ) );
      }
      if ( mWays.has_value() )
      {
        mPastBranch = differ;
        mPastHolds.reset();
      }
      else
      {
        label( differ );
      }
      return instruction.op == ir::Comparison::EQUAL ? Jumps{ .whenTrue = "jeq", .whenFalse = "jne" }
                                                     : Jumps{ .whenTrue = "jne", .whenFalse = "jeq" };
    }

    bool const less = instruction.op == ir::Comparison::LESS;
    // Against a number whose low byte is zero the high bytes decide it all:
    // where they are equal, the left is at the bound or above — see
    // docs/decisions/0293-a-bound-with-a-zero-low-byte-compares-high-bytes.md.
    if ( auto const* const bound = std::get_if<ir::Constant>( &right );
         bound != nullptr && !ir::isSigned( instruction.type ) && ( bound->name.empty() || bound->valued ) &&
         ( bound->value & 0xFF ) == 0 && bound->value != 0 )
    {
      line( "lda " + named( left, 8 ) );
      line( "cmp " + boundHigh( instruction ) );
      return less ? Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" } : Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" };
    }
    if ( !ir::isSigned( instruction.type ) && mWays.has_value() &&
         ( highFirst( instruction ) || mKnownBound[instruction.result.index] ) )
    {
      // The high bytes first, which decide it wherever they differ: straight
      // to where each way goes, or past the branch to the block written next;
      // the low bytes only where the high ones are equal — see
      // docs/decisions/0157-a-wide-comparison-high-byte-first.md.
      std::optional<std::string> const lessTo = less ? mWays->whenTrue : mWays->whenFalse;
      std::optional<std::string> const notLessTo = less ? mWays->whenFalse : mWays->whenTrue;
      std::optional<std::string> past;
      auto const toward = [&]( std::optional<std::string> const& to, std::string_view jcc, std::string_view bcc )
      {
        // The carry says of this way what the branch below says of it — clear
        // where the high bytes are less, set where they are more — so an edge
        // may count on it as it could on the branch's.
        leftEarly( false );
        if ( to.has_value() )
        {
          line( std::string{ jcc } + " " + *to );
          return;
        }
        past = past.value_or( localLabel( "h" ) );
        heldPast();
        line( std::string{ bcc } + " " + *past );
      };
      mPastHolds.reset();
      line( "lda " + named( left, 8 ) );
      line( "cmp " + boundHigh( instruction ) );
      toward( lessTo, "jcc", "bcc" );
      toward( notLessTo, "jne", "bne" );
      line( "lda " + named( left ) );
      line( "cmp " + named( right ) );
      mPastBranch = std::move( past );
      return less ? Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" } : Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" };
    }
    if ( !ir::isSigned( instruction.type ) && pairInXA( instruction ) )
    {
      // The low bytes by `cpx`, whose carry the high bytes' `sbc` takes on.
      line( "cpx " + named( right ) );
      line( "sbc " + boundHigh( instruction ) );
      return less ? Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" } : Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" };
    }
    subtractWide( left, right, boundHigh( instruction ) );
    if ( !ir::isSigned( instruction.type ) )
    {
      return less ? Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" } : Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" };
    }
    signIntoN();
    return less ? Jumps{ .whenTrue = "jmi", .whenFalse = "jpl" } : Jumps{ .whenTrue = "jpl", .whenFalse = "jmi" };
  }

  /// A pair tested against zero right after the text stepped it by one where
  /// it lies, `inc w / bne @i / inc w+1 / @i`: where the low byte did not wrap
  /// the pair is no zero, so that branch goes where the pair being no zero
  /// goes, and where it did, `inc w+1` has left the answer in `Z` — `inc w /
  /// jne loop / inc w+1 / jne loop`, nothing read again. Answers whether it
  /// wrote it, leaving the branch on `Z` to the caller. See
  /// docs/decisions/0288-a-pair-stepped-to-its-wrap-branches-on-the-step.md.
  [[nodiscard]] bool steppedJustBefore( ir::Compare const& instruction )
  {
    Ways const ways = mWays.value_or( Ways{} );
    std::optional<std::string> const notZeroTo =
        instruction.op == ir::Comparison::EQUAL ? ways.whenFalse : ways.whenTrue;
    if ( !notZeroTo.has_value() || !std::holds_alternative<ir::Object>( instruction.left ) )
    {
      return false;
    }
    std::string const low = named( instruction.left );
    std::string const high = named( instruction.left, 8 );

    // The last lines, source marks aside: the step and its label.
    std::vector<std::pair<std::size_t, std::string>> lines;
    std::size_t end = mText.size();
    while ( end > 0 && lines.size() < 4 )
    {
      std::size_t const begin =
          mText.rfind( '\n', end - 2 ) == std::string::npos ? 0 : mText.rfind( '\n', end - 2 ) + 1;
      std::string const text = mText.substr( begin, end - begin - 1 );
      if ( !text.starts_with( std::string{ INDENT } + ".source " ) )
      {
        lines.emplace( lines.begin(), begin, text );
      }
      end = begin;
    }
    if ( lines.size() != 4 )
    {
      return false;
    }
    std::string const label = lines[3].second;
    std::string const pad{ INDENT };
    // The pair stepped where `Y` and `X` hold it is stepped in them (0291).
    bool const resident = mResidentOn.has_value() && qualified( mResident[*mResidentOn].counter ) == low;
    std::string const stepLow = resident ? pad + "iny" : pad + "inc " + low;
    std::string const stepHigh = resident ? pad + "inx" : pad + "inc " + high;
    if ( lines[0].second != stepLow || lines[1].second != pad + "bne " + label || lines[2].second != stepHigh ||
         label.empty() || label.front() != '@' )
    {
      return false;
    }
    // `bne @i` becomes the branch to where a pair that is no zero goes, and
    // the label it went to is no more.
    mText.erase( lines[3].first );
    mText.replace( lines[1].first, lines[1].second.size(), pad + "jne " + *notZeroTo );
    leftEarly( true );
    return true;
  }

  /// Whether a 16-bit comparison of order is written high bytes first: where
  /// the bound is a constant of more than a byte, or one the assembler names
  /// and the compiler cannot fold, so that the high bytes differ on most of
  /// the range the other operand walks — see
  /// docs/decisions/0157-a-wide-comparison-high-byte-first.md. Against a
  /// bound below 256 they agree on every turn but the last, and asking about
  /// them first costs four cycles a turn. An object the ranges know is
  /// `planKnownBounds`' to answer.
  [[nodiscard]] static bool highFirst( ir::Compare const& instruction )
  {
    auto const* const bound = std::get_if<ir::Constant>( &instruction.right );
    return bound != nullptr && ( ( !bound->name.empty() && !bound->valued ) || bound->value >= 256 );
  }

  /// A 16-bit unsigned `<` or `>=` against a bound the ranges show holds 256
  /// or more where it stands — `limit = 1024 ^ seed` before the loop that
  /// tests it — whose high byte is then known to be no zero, as a constant's
  /// is that 0157 reads high byte first — see
  /// docs/decisions/0287-a-bound-known-past-a-byte-is-compared-high-byte-first.md.
  void planKnownBounds( ir::Function const& function )
  {
    mKnownBound.assign( function.values.size(), false );
    std::vector<std::optional<Range>> const bounds = comparedBoundsOf( function );
    for ( ir::Block const& block : function.blocks )
    {
      for ( ir::Instruction const& instruction : block.instructions )
      {
        auto const* const compare = std::get_if<ir::Compare>( &instruction.operation );
        if ( compare == nullptr || ir::sizeOf( compare->type ) != 2 || ir::isSigned( compare->type ) ||
             ( compare->op != ir::Comparison::LESS && compare->op != ir::Comparison::GREATER_OR_EQUAL ) ||
             std::holds_alternative<ir::Constant>( compare->right ) )
        {
          continue;
        }
        std::optional<Range> const& bound = bounds[compare->result.index];
        mKnownBound[compare->result.index] = bound.has_value() && bound->min >= 256;
      }
    }
  }

  /// Per 16-bit comparison whose right operand is no number of its own, the
  /// high byte the ranges show that operand to hold wherever it stands, where
  /// its least and its most share it: `limit = 4096 ^ seed` is `4096` to
  /// `4351`, so `cmp #16` reads what `cmp __2limit+1` would — see
  /// docs/decisions/0290-a-high-byte-the-ranges-know-is-read-as-a-number.md.
  void planBoundHighs( ir::Function const& function )
  {
    mBoundHigh.assign( function.values.size(), std::nullopt );
    std::vector<std::optional<Range>> const bounds = comparedBoundsOf( function );
    for ( ir::Block const& block : function.blocks )
    {
      for ( ir::Instruction const& instruction : block.instructions )
      {
        auto const* const compare = std::get_if<ir::Compare>( &instruction.operation );
        if ( compare == nullptr || ir::sizeOf( compare->type ) != 2 ||
             !std::holds_alternative<ir::Object>( compare->right ) )
        {
          continue;
        }
        std::optional<Range> const& bound = bounds[compare->result.index];
        if ( bound.has_value() && bound->min >> 8 == bound->max >> 8 )
        {
          mBoundHigh[compare->result.index] = ( bound->min >> 8 ) & 0xFF;
        }
      }
    }
  }

  /// How a 16-bit comparison reads its right operand's high byte: as the
  /// number `planBoundHighs` found it to be, or where it lies.
  [[nodiscard]] std::string boundHigh( ir::Compare const& instruction ) const
  {
    std::optional<std::int64_t> const high = mBoundHigh[instruction.result.index];
    return high.has_value() ? "#" + std::to_string( *high ) : named( instruction.right, 8 );
  }

  /// A pair read through a pointer that a comparison of its block reads keeps
  /// its low byte in `X` as well, `tax` beside its store, and its high byte is
  /// what `A` is left holding: the comparisons then read the two registers,
  /// `cmp k+1 / bne / cpx k` and `cpx k / sbc k+1`, and the block entered from
  /// here alone finds them there too. Not in a loop that counts in `X` — see
  /// docs/decisions/0289-a-pair-only-compared-is-held-in-a-and-x.md.
  void planLowInX( ir::Function const& function )
  {
    mLowInX.assign( function.values.size(), false );
    for ( std::uint32_t index = 0; index < function.blocks.size(); ++index )
    {
      if ( loopAt( index ) != nullptr )
      {
        continue;
      }
      std::vector<ir::Instruction> const& instructions = function.blocks[index].instructions;
      for ( std::size_t at = 0; at < instructions.size(); ++at )
      {
        auto const* const load = std::get_if<ir::LoadIndirect>( &instructions[at].operation );
        if ( load == nullptr || ir::sizeOf( load->type ) != 2 || load->isVolatile ||
             mPairThroughY[load->result.index] || mDeferred[index][at] )
        {
          continue;
        }
        for ( std::size_t later = at + 1; later < instructions.size(); ++later )
        {
          auto const* const compare = std::get_if<ir::Compare>( &instructions[later].operation );
          if ( compare != nullptr && ir::sizeOf( compare->type ) == 2 && isValue( compare->left, load->result ) )
          {
            mLowInX[load->result.index] = true;
          }
        }
      }
    }
  }

  /// Whether the left of a 16-bit comparison is in `X` and `A`, its low byte and
  /// its high byte, and the right is read by an instruction with no index of
  /// its own.
  [[nodiscard]] bool pairInXA( ir::Compare const& instruction ) const
  {
    bool const plain = std::holds_alternative<ir::Constant>( instruction.right ) ||
                       ( std::holds_alternative<ir::Object>( instruction.right ) &&
                         !std::get<ir::Object>( instruction.right ).name.contains( ',' ) );
    return plain && mTracker.holdsInX( named( instruction.left ) ) && mTracker.holdsInA( named( instruction.left, 8 ) );
  }

  /// The high byte of `left - right` in `A`, with the carry, sign and overflow
  /// of the whole subtraction; `high` is how the right's high byte is read.
  void subtractWide( ir::Operand const& left, ir::Operand const& right, std::string const& high )
  {
    line( "lda " + named( left ) );
    line( "cmp " + named( right ) );
    line( "lda " + named( left, 8 ) );
    line( "sbc " + high );
  }

  void instruction( ir::Instruction const& instruction )
  {
    if ( auto const* call = std::get_if<ir::Call>( &instruction.operation ) )
    {
      this->call( *call );
    }
    else if ( auto const* store = std::get_if<ir::Store>( &instruction.operation ) )
    {
      this->store( *store );
    }
    else if ( auto const* unary = std::get_if<ir::Unary>( &instruction.operation ) )
    {
      this->unary( *unary );
    }
    else if ( auto const* binary = std::get_if<ir::Binary>( &instruction.operation ) )
    {
      this->binary( *binary );
    }
    else if ( auto const* choice = std::get_if<ir::Switch>( &instruction.operation ) )
    {
      switchValue( *choice );
    }
    else if ( auto const* convert = std::get_if<ir::Convert>( &instruction.operation ) )
    {
      this->convert( *convert );
    }
    else if ( auto const* load = std::get_if<ir::Load>( &instruction.operation ) )
    {
      this->load( *load );
    }
    else if ( auto const* element = std::get_if<ir::StoreElement>( &instruction.operation ) )
    {
      storeElement( *element );
    }
    else if ( auto const* indirect = std::get_if<ir::LoadIndirect>( &instruction.operation ) )
    {
      loadIndirect( *indirect );
    }
    else if ( auto const* through = std::get_if<ir::StoreIndirect>( &instruction.operation ) )
    {
      storeIndirect( *through );
    }
    else if ( auto const* block = std::get_if<ir::Copy>( &instruction.operation ) )
    {
      copy( *block );
    }
    else if ( auto const* with = std::get_if<ir::EnterWith>( &instruction.operation ) )
    {
      enterWith( *with );
    }
    else
    {
      compare( std::get<ir::Compare>( instruction.operation ) );
    }
  }

  /// The entry of a `[[with]]` block: its index into `X` where it has one,
  /// the `.with`, and the use of the macro the block's statements become —
  /// see docs/decisions/0096-panes-in-c.md.
  void enterWith( ir::EnterWith const& instruction )
  {
    if ( std::optional<ir::Operand> const index = instruction.index; index.has_value() )
    {
      bool const wide = instruction.stateBytes > 1;
      if ( instruction.member )
      {
        // A member's state is the family's first plus the index, in sixteen
        // bits where the Window's states take two bytes: the high one is the
        // family's and the carry out of the low.
        load( *index );
        line( "clc" );
        line( "adc #" + std::string{ wide ? "<" : "" } + instruction.name );
        line( "tax" );
        if ( wide )
        {
          line( "lda #0" );
          line( "adc #>" + instruction.name );
          line( "tay" );
        }
      }
      else
      {
        line( inA( *index ) ? std::string{ "tax" } : "ldx " + named( *index ) );
        if ( wide )
        {
          // The index is a byte, so the state it names is below 256.
          line( "ldy #0" );
        }
      }
    }
    std::string with = ".with " + instruction.name;
    if ( instruction.form == ir::WithForm::AT )
    {
      with += instruction.stateBytes > 1 ? ", xy" : ", x";
    }
    else if ( instruction.form == ir::WithForm::STATE )
    {
      with += " = " + instruction.state;
    }
    line( with );
    mPendingBodies.push_back( instruction.body );
    line( instruction.body );
  }

  /// A name of the Proc's own bytes as a macro body of the Proc writes it:
  /// qualified by the Proc's name, since the body's names resolve at the
  /// Module's top level. A scratch byte is one; an element or a byte of a
  /// local, `NAME+2`, and a member of a striped local, `NAME.x`, are named by
  /// what stands before the operator.
  [[nodiscard]] std::string qualified( std::string const& name ) const
  {
    if ( mInBody == 0 )
    {
      return name;
    }
    std::string const head = name.substr( 0, name.find_first_of( "+." ) );
    bool const scratch = head.starts_with( "__t" ) && head.size() > 3 &&
                         std::ranges::all_of( head.substr( 3 ), []( char c ) { return c >= '0' && c <= '9'; } );
    return scratch || mProcBytes.contains( head ) ? mFunction->name + "." + name : name;
  }

  /// The byte of the callee a 16-bit value is an argument of, where the call
  /// is the next instruction and nothing else reads the value: the value is
  /// computed straight into it, as one stored to an object is, and the call
  /// writes it no more — see
  /// docs/decisions/0277-an-argument-is-computed-where-the-callee-reads-it.md.
  /// Empty where it is not.
  [[nodiscard]] static std::string argumentTaking( ir::Function const& function,
                                                   std::vector<ir::Instruction> const& instructions,
                                                   std::size_t at,
                                                   ir::Value value )
  {
    auto const* const call =
        at + 1 < instructions.size() ? std::get_if<ir::Call>( &instructions[at + 1].operation ) : nullptr;
    if ( call == nullptr || ir::readersOf( function, value ) != 1 )
    {
      return {};
    }
    for ( ir::Argument const& argument : call->arguments )
    {
      if ( isValue( argument.value, value ) && !argument.from.has_value() && argument.place.empty() &&
           ir::sizeOf( argument.type ) == 2 )
      {
        return argument.name;
      }
    }
    return {};
  }

  /// Each argument stored in its byte, the one in `A` first, then the call,
  /// and the result read where it is wanted.
  void call( ir::Call const& instruction )
  {
    // What goes in registers is loaded last, since every byte written to
    // memory goes through `A`: the bytes in memory first, then `X` and `Y`,
    // and `A` right before the `jsr`. A value `A` holds for a register is
    // moved out of the way first where anything is still to be written — see
    // docs/decisions/0145-an-argument-in-a-register.md.
    struct Loaded
    {
      char reg = 'a';
      std::string from;
    };

    std::vector<Loaded> loads;
    bool writesMemory = false;
    bool copiesThroughIndex = false;
    for ( ir::Argument const& argument : instruction.arguments )
    {
      writesMemory = writesMemory || argument.place.empty() || argument.place.contains( 'm' );
      copiesThroughIndex = copiesThroughIndex || ( argument.from.has_value() && argument.bytes > 4 );
    }

    // The one value `A` holds goes first, wherever it goes, before a store of
    // another writes over it.
    std::vector<ir::Argument const*> order;
    for ( ir::Argument const& argument : instruction.arguments )
    {
      if ( inA( argument.value ) )
      {
        order.insert( order.begin(), &argument );
      }
      else
      {
        order.push_back( &argument );
      }
    }
    for ( ir::Argument const* argument : order )
    {
      if ( std::optional<ir::Place> const from = argument->from; from.has_value() )
      {
        copy( ir::Copy{ .from = *from, .to = ir::Place{ .name = argument->name }, .bytes = argument->bytes } );
        continue;
      }
      if ( argument->place.empty() )
      {
        // Computed where it goes, by the instruction before the call.
        if ( auto const* const value = std::get_if<ir::Value>( &argument->value );
             value == nullptr || mDirect[value->index] != argument->name )
        {
          store( ir::Store{ .name = argument->name, .type = argument->type, .value = argument->value } );
        }
        continue;
      }
      std::uint32_t memory = 0;
      for ( std::size_t byte = 0; byte < argument->place.size(); ++byte )
      {
        char const reg = argument->place[byte];
        std::string source = named( argument->value, static_cast<std::uint32_t>( 8 * byte ) );
        if ( byte == 0 && inA( argument->value ) )
        {
          // The value waits in `A`: stored now where its byte is in memory,
          // moved to `X` or `Y` now, left where it is for `A` where nothing is
          // written after it — and otherwise into a byte of its own, for its
          // register to be loaded from last.
          if ( reg == 'm' )
          {
            line( "sta " + byteOf( argument->name, memory++ ) );
            continue;
          }
          if ( ( reg == 'x' || reg == 'y' ) && !copiesThroughIndex )
          {
            line( reg == 'x' ? "tax" : "tay" );
            continue;
          }
          if ( reg != 'a' || writesMemory )
          {
            source = qualified( scratchName( scratch( 1 ) ) );
            line( "sta " + source );
          }
          else
          {
            source.clear();
          }
        }
        if ( reg == 'm' )
        {
          if ( !source.empty() )
          {
            line( "lda " + source );
          }
          line( "sta " + byteOf( argument->name, memory ) );
          ++memory;
          continue;
        }
        loads.push_back( Loaded{ .reg = reg, .from = source } );
      }
    }
    // `X` and `Y` first, which leaves `A` alone, and `A` last.
    std::ranges::stable_sort(
        loads, []( Loaded const& one, Loaded const& other ) { return ( one.reg == 'a' ) < ( other.reg == 'a' ); } );
    for ( Loaded const& load : loads )
    {
      if ( !load.from.empty() )
      {
        line( std::string{ "ld" } + load.reg + " " + load.from );
      }
    }
    if ( !instruction.with.empty() )
    {
      line( ".with " + instruction.with );
    }
    line( "jsr " + instruction.name );
    if ( std::optional<ir::Value> const result = instruction.result; result.has_value() )
    {
      std::string const returned =
          instruction.returned.empty() ? instruction.name + "." + std::string{ RESULT } : instruction.returned;
      if ( !instruction.resultPlace.empty() )
      {
        resultFromPlace( instruction, *result, returned );
        return;
      }
      if ( isWide( *mFunction, *result ) )
      {
        if ( mAtName[result->index].empty() )
        {
          copy( ir::Object{ .name = returned, .type = mFunction->values.at( result->index ) }, location( *result ) );
        }
        return;
      }
      line( "lda " + returned );
      keep( *result );
    }
  }

  /// A result whose bytes are in registers, or some in registers and some in
  /// the byte the callee names: the registers are read first, since reading
  /// a byte of memory goes through `A`.
  void resultFromPlace( ir::Call const& instruction, ir::Value result, std::string const& returned )
  {
    std::string const& place = instruction.resultPlace;
    if ( !isWide( *mFunction, result ) )
    {
      if ( place[0] == 'x' )
      {
        line( "txa" );
      }
      else if ( place[0] == 'y' )
      {
        line( "tya" );
      }
      else if ( place[0] == 'm' )
      {
        line( "lda " + returned );
      }
      keep( result );
      return;
    }
    std::string const to = location( result );
    for ( std::size_t byte = 0; byte < place.size(); ++byte )
    {
      if ( place[byte] != 'm' )
      {
        line( std::string{ "st" } + place[byte] + " " + byteOf( to, static_cast<std::uint32_t>( byte ) ) );
      }
    }
    std::uint32_t memory = 0;
    for ( std::size_t byte = 0; byte < place.size(); ++byte )
    {
      if ( place[byte] == 'm' )
      {
        line( "lda " + byteOf( returned, memory++ ) );
        line( "sta " + byteOf( to, static_cast<std::uint32_t>( byte ) ) );
      }
    }
  }

  /// An element's index into `X`: moved from `A`, or loaded, and doubled for a
  /// 16-bit element, whose bytes are two to an index.
  void indexIntoX( ir::Operand const& index, ir::Type type, bool scaled = false )
  {
    // A counted loop's counter is already there, which is the whole of what
    // keeping it in `X` buys — see docs/decisions/0109-an-induction-variable-in-x.md
    // — and so is a value that waits there.
    if ( indexInXAt( mBlock, index, scaled ) )
    {
      return;
    }
    // The index of the element before it plus a number, which `X` already
    // holds the index of, doubled.
    if ( auto const* const value = std::get_if<ir::Value>( &index );
         value != nullptr && value->index < mIndexStep.size() && mIndexStep[value->index] > 0 )
    {
      for ( std::int64_t step = 0; step < 2 * mIndexStep[value->index]; ++step )
      {
        line( "inx" );
      }
      return;
    }
    if ( ir::sizeOf( type ) == 1 || scaled )
    {
      ir::Operand const source = indexSource( index );
      line( inA( source ) ? std::string{ "tax" } : "ldx " + named( source ) );
      return;
    }
    load( index );
    line( "asl" );
    line( "tax" );
  }

  /// The same into `Y`, for the element a carried pair is read from, whose
  /// `X` the element it is written to has — see
  /// docs/decisions/0202-a-pair-that-goes-from-one-element-to-another-is-two-bytes.md.
  void indexIntoY( ir::Operand const& index, ir::Type type, bool scaled )
  {
    if ( ir::sizeOf( type ) == 1 || scaled )
    {
      ir::Operand const source = indexSource( index );
      line( inA( source ) ? std::string{ "tay" } : "ldy " + named( source ) );
      return;
    }
    load( index );
    line( "asl" );
    line( "tay" );
  }

  /// The operand a byte of an element is read or written by: the address
  /// itself where the index is a number (0200), and otherwise the stripe or
  /// the byte of the element and the register its index sits in.
  [[nodiscard]] std::string elementByte( std::string const& name,
                                         std::string const& high,
                                         ir::Operand const& index,
                                         ir::Type type,
                                         bool scaled,
                                         std::uint32_t byte,
                                         bool throughY ) const
  {
    if ( isANumber( index ) )
    {
      return atAConstantIndex( name, high, index, type, scaled, byte );
    }
    std::string const at = byte == 1 && !high.empty() ? qualified( high ) : byteOf( qualified( name ), byte );
    return at + ( throughY ? ",y" : ",x" );
  }

  /// A pair carried from one element straight into another: two bytes, an
  /// index register each, and no scratch pair between — see
  /// docs/decisions/0202-a-pair-that-goes-from-one-element-to-another-is-two-bytes.md.
  /// The indexes are loaded first, the one that is read into `Y` and the one
  /// that is written into `X`, since doubling either goes through `A`.
  void movePair( ir::Load const& from, ir::StoreElement const& to )
  {
    auto const source = [this, &from] { indexIntoY( from.index, from.type, from.scaled ); };
    auto const target = [this, &to] { indexIntoX( to.index, to.type, to.scaled ); };
    bool const read = !isANumber( from.index );
    bool const written = !isANumber( to.index );

    // Whichever index waits in `A` is taken from it first: doubling the other
    // one goes through `A` and would leave nothing there to take.
    if ( written && inA( to.index ) )
    {
      target();
      if ( read )
      {
        source();
      }
    }
    else
    {
      if ( read )
      {
        source();
      }
      if ( written )
      {
        target();
      }
    }
    for ( std::uint32_t byte = 0; byte < 2; ++byte )
    {
      line( "lda " + elementByte( from.name, from.high, from.index, from.type, from.scaled, byte, true ) );
      line( "sta " + elementByte( to.name, to.high, to.index, to.type, to.scaled, byte, false ) );
    }
  }

  /// An element read through `X`: into `A`, or into the two bytes a 16-bit
  /// value lives in.
  void load( ir::Load const& instruction )
  {
    if ( isANumber( instruction.index ) )
    {
      if ( ir::sizeOf( instruction.type ) == 1 )
      {
        line( "lda " +
              atAConstantIndex( instruction.name, {}, instruction.index, instruction.type, instruction.scaled ) );
        keep( instruction.result );
        return;
      }
      std::string const to = location( instruction.result );
      for ( std::uint32_t byte = 0; byte < 2; ++byte )
      {
        line( "lda " +
              atAConstantIndex(
                  instruction.name, instruction.high, instruction.index, instruction.type, instruction.scaled, byte ) );
        line( "sta " + byteOf( to, byte ) );
      }
      return;
    }
    if ( throughY( mBlock, instruction.index, instruction.type, instruction.scaled ) || secondIndexHere() )
    {
      ir::Operand const source = indexSource( instruction.index );
      line( inA( source ) ? std::string{ "tay" } : "ldy " + named( source ) );
      line( "lda " + qualified( instruction.name ) + addressOffset( instruction.index ) + ",y" );
      keep( instruction.result );
      return;
    }
    indexIntoX( instruction.index, instruction.type, instruction.scaled );
    if ( ir::sizeOf( instruction.type ) == 1 )
    {
      line( "lda " + qualified( instruction.name ) + addressOffset( instruction.index ) + ",x" );
      keep( instruction.result );
      return;
    }
    std::string const to = location( instruction.result );
    for ( std::uint32_t index = 0; index < 2; ++index )
    {
      std::string const from = index == 1 && !instruction.high.empty() ? qualified( instruction.high )
                                                                       : byteOf( qualified( instruction.name ), index );
      line( "lda " + from + ",x" );
      line( "sta " + byteOf( to, index ) );
    }
  }

  /// A value written to an element through `X`, whose index is loaded first so
  /// that an 8-bit value may wait in `A`.
  void storeElement( ir::StoreElement const& instruction )
  {
    if ( mAt > 0 && mBlock < mMoved.size() && mAt - 1 < mMoved[mBlock].size() && mMoved[mBlock][mAt - 1] )
    {
      movePair( std::get<ir::Load>( mFunction->blocks[mBlock].instructions[mAt - 1].operation ), instruction );
      return;
    }
    if ( isANumber( instruction.index ) )
    {
      if ( ir::sizeOf( instruction.type ) == 1 )
      {
        load( instruction.value );
        line( "sta " +
              atAConstantIndex( instruction.name, {}, instruction.index, instruction.type, instruction.scaled ) );
        return;
      }
      for ( std::uint32_t byte = 0; byte < 2; ++byte )
      {
        line( "lda " + named( instruction.value, 8 * byte ) );
        line( "sta " +
              atAConstantIndex(
                  instruction.name, instruction.high, instruction.index, instruction.type, instruction.scaled, byte ) );
      }
      return;
    }
    if ( ir::sizeOf( instruction.type ) == 1 )
    {
      bool const y = throughY( mBlock, instruction.index, instruction.type, instruction.scaled ) || secondIndexHere();
      if ( y || !indexInXAt( mBlock, instruction.index, instruction.scaled ) )
      {
        line( std::string{ y ? "ldy " : "ldx " } + named( indexSource( instruction.index ) ) );
      }
      load( instruction.value );
      line( "sta " + qualified( instruction.name ) + addressOffset( instruction.index ) + ( y ? ",y" : ",x" ) );
      return;
    }
    indexIntoX( instruction.index, instruction.type, instruction.scaled );
    for ( std::uint32_t index = 0; index < 2; ++index )
    {
      std::string const to = index == 1 && !instruction.high.empty() ? qualified( instruction.high )
                                                                     : byteOf( qualified( instruction.name ), index );
      line( "lda " + named( instruction.value, 8 * index ) );
      line( "sta " + to + ",x" );
    }
  }

  /// How a Proc's declaration names a type: a pointer is two bytes, which the
  /// assembler calls a `u16`, and a `struct` or a `union` its bytes.
  static std::string declared( ir::Type type, std::uint32_t bytes = 0 )
  {
    if ( type == ir::Type::BLOCK )
    {
      return "u8[" + std::to_string( bytes ) + "]";
    }
    return std::string{ ir::spellingOf( type == ir::Type::POINTER ? ir::Type::U16 : type ) };
  }

  /// Where a 16-bit `==` or `!=` goes where its bytes differ, while the
  /// branch that reads it is written, where that is a jump.
  /// Where each way of a branch goes while its comparison is written: a
  /// label where it is a jump, nothing where it is the block written next,
  /// which a test deciding early reaches past the branch — see
  /// docs/decisions/0153-a-low-byte-that-differs-branches-where-it-goes.md.
  struct Ways
  {
    std::optional<std::string> whenTrue;
    std::optional<std::string> whenFalse;
  };

  std::optional<Ways> mWays;

  /// The label written after the branch, which such a test went past to.
  std::optional<std::string> mPastBranch;

  /// What `A` holds on every branch to that label, where each of them said.
  std::optional<std::vector<std::string>> mPastHolds;

  /// What `A` held where a comparison branched away before the branch that
  /// reads it — a way out of the block the IR's edges do not know of, since
  /// it goes where one of them goes: the block's end holds no more than
  /// every one of its ways out agrees on. And whether the carry on that way
  /// was another than the branch's, which no edge may then count on.
  std::optional<std::vector<std::string>> mEarlyHolds;
  std::optional<std::vector<std::string>> mEarlyHoldsX;
  bool mEarlyCarry = false;

  /// Notes a way out written before the block's own branch.
  void leftEarly( bool carryDiffers )
  {
    if ( !mEarlyHolds.has_value() )
    {
      mEarlyHolds = mTracker.inA();
    }
    else
    {
      std::erase_if( *mEarlyHolds, [this]( std::string const& name ) { return !mTracker.holdsInA( name ); } );
    }
    if ( !mEarlyHoldsX.has_value() )
    {
      mEarlyHoldsX = mTracker.inX();
    }
    else
    {
      std::erase_if( *mEarlyHoldsX, [this]( std::string const& name ) { return !mTracker.holdsInX( name ); } );
    }
    mEarlyCarry = mEarlyCarry || carryDiffers;
  }

  /// Takes the stores to a byte a register carries out of the body written
  /// from `begin` on: a parameter in a register, the result in `A`, or the
  /// high byte of a result that is `ma`.
  void dropStoresToPlaced( ir::Function const& lowered, std::size_t begin )
  {
    std::vector<std::string> gone;
    for ( ir::Local const& parameter : lowered.parameters )
    {
      if ( !parameter.place.empty() )
      {
        gone.push_back( qualified( parameter.name ) );
      }
    }
    std::string const result = qualified( std::string{ RESULT } );
    if ( lowered.resultPlace == "a" )
    {
      gone.push_back( result );
    }
    else if ( lowered.resultPlace == "ma" )
    {
      gone.push_back( result + "+1" );
    }
    dropStoresTo( mText, begin, gone );
  }

  /// `A` plus or less what the operand names, the carry set up first. One on
  /// a 65SC02 is `inc` or `dec`: one byte and two cycles against three and
  /// four, `A` left holding the same, and the carry left as it was rather
  /// than as the sum sets it. That is the whole difference, with `V`, and it
  /// is why this is written only where an operator's result is computed:
  /// nothing reads either flag out of one, where a comparison's `sbc` is read
  /// for both. See docs/decisions/0184-one-more-in-the-accumulator.md.
  void stepAccumulator( bool add, std::string const& operand )
  {
    if ( mCpu == Cpu::WDC65SC02 && operand == "#1" )
    {
      line( add ? "inc" : "dec" );
      return;
    }
    line( add ? "clc" : "sec" );
    line( std::string{ add ? "adc " : "sbc " } + operand );
  }

  /// Whether a read or a write through a pointer may leave the index out: the
  /// 65SC02 has `(zp)` where the 6502 must write `ldy #0` and index by it.
  /// Two bytes and two cycles, and `Y` keeps what it held, which is the
  /// larger half — see
  /// docs/decisions/0186-a-pointer-read-without-an-index.md.
  [[nodiscard]] bool throughPointerAlone( ir::Operand const& index ) const
  {
    if ( mCpu != Cpu::WDC65SC02 )
    {
      return false;
    }
    auto const* const constant = std::get_if<ir::Constant>( &index );
    return constant != nullptr && constant->name.empty() && ( constant->value & 0xFFFF ) == 0;
  }

  /// One byte of a place into `A`, or from it.
  void placeByte( std::string_view mnemonic, ir::Place const& place, std::uint32_t index )
  {
    if ( std::optional<ir::Operand> const pointer = place.pointer; pointer.has_value() )
    {
      if ( mCpu == Cpu::WDC65SC02 && place.offset + index == 0 )
      {
        line( std::string{ mnemonic } + " (" + named( *pointer ) + ")" );
        return;
      }
      line( "ldy #" + std::to_string( place.offset + index ) );
      line( std::string{ mnemonic } + " (" + named( *pointer ) + "),y" );
      return;
    }
    line( std::string{ mnemonic } + " " + byteOf( qualified( place.name ), index ) );
  }

  /// A `struct` or a `union` copied: a store per byte for four or fewer, and
  /// a loop over `X` between names, or over `Y` where a pointer is on either
  /// side, which then has no offset. The loop counts down to zero with `bpl`
  /// while its first index is below 128, and up to the size past that.
  void copy( ir::Copy const& instruction )
  {
    if ( instruction.bytes <= SMALL_BLOCK )
    {
      for ( std::uint32_t index = 0; index < instruction.bytes; ++index )
      {
        placeByte( "lda", instruction.from, index );
        placeByte( "sta", instruction.to, index );
      }
      return;
    }
    bool const named = !instruction.from.pointer.has_value() && !instruction.to.pointer.has_value();
    std::string const index = named ? "x" : "y";
    auto const at = [&]( ir::Place const& place )
    {
      return place.pointer.has_value() ? "(" + this->named( *place.pointer ) + ")," + index
                                       : qualified( place.name ) + "," + index;
    };
    std::string const again = localLabel( "c" );
    bool const down = instruction.bytes <= 128;
    line( "ld" + index + " #" + std::to_string( down ? instruction.bytes - 1 : 0 ) );
    label( again );
    line( "lda " + at( instruction.from ) );
    line( "sta " + at( instruction.to ) );
    if ( down )
    {
      line( "de" + index );
      line( "bpl " + again );
      return;
    }
    line( "in" + index );
    line( "cp" + index + " #" + std::to_string( instruction.bytes & 0xFFU ) );
    line( "bne " + again );
  }

  /// A value read through a pointer on the zero page, the index in `Y`.
  void loadIndirect( ir::LoadIndirect const& instruction )
  {
    if ( tableAddress( instruction.pointer ) )
    {
      std::string const through = "lda (" + named( instruction.pointer ) + "),y";
      line( through );
      if ( ir::sizeOf( instruction.type ) == 1 )
      {
        keep( instruction.result );
        return;
      }
      std::string const to = location( instruction.result );
      line( "sta " + to );
      lowIntoX( instruction );
      line( "iny" );
      line( through );
      line( "sta " + byteOf( to, 1 ) );
      return;
    }
    if ( paged( instruction.pointer ) )
    {
      line( "lda (" + std::string{ PAGE } + "),y" );
      keep( instruction.result );
      return;
    }
    if ( ir::sizeOf( instruction.type ) == 1 && throughPointerAlone( instruction.index ) )
    {
      line( "lda (" + named( instruction.pointer ) + ")" );
      keep( instruction.result );
      return;
    }
    line( inA( instruction.index ) ? std::string{ "tay" } : "ldy " + named( instruction.index ) );
    std::string const through = "lda (" + named( instruction.pointer ) + "),y";
    line( through );
    if ( ir::sizeOf( instruction.type ) == 1 )
    {
      keep( instruction.result );
      return;
    }
    std::string const to = location( instruction.result );
    line( "sta " + to );
    lowIntoX( instruction );
    line( "iny" );
    line( through );
    line( "sta " + byteOf( to, 1 ) );
  }

  /// The low byte of a pair a comparison reads, kept in `X` beside its store —
  /// see docs/decisions/0289-a-pair-only-compared-is-held-in-a-and-x.md.
  void lowIntoX( ir::LoadIndirect const& instruction )
  {
    if ( instruction.result.index < mLowInX.size() && mLowInX[instruction.result.index] )
    {
      line( "tax" );
    }
  }

  /// A value written through a pointer on the zero page, the index loaded into
  /// `Y` first so that an 8-bit value may wait in `A`.
  void storeIndirect( ir::StoreIndirect const& instruction )
  {
    if ( tableAddress( instruction.pointer ) )
    {
      std::string const through = "sta (" + named( instruction.pointer ) + "),y";
      load( instruction.value );
      line( through );
      if ( ir::sizeOf( instruction.type ) == 2 )
      {
        line( "iny" );
        line( "lda " + named( instruction.value, 8 ) );
        line( through );
      }
      return;
    }
    if ( paged( instruction.pointer ) )
    {
      load( instruction.value );
      line( "sta (" + std::string{ PAGE } + "),y" );
      return;
    }
    if ( ir::sizeOf( instruction.type ) == 1 && throughPointerAlone( instruction.index ) )
    {
      load( instruction.value );
      line( "sta (" + named( instruction.pointer ) + ")" );
      return;
    }
    line( "ldy " + named( instruction.index ) );
    std::string const through = "sta (" + named( instruction.pointer ) + "),y";
    load( instruction.value );
    line( through );
    if ( ir::sizeOf( instruction.type ) == 2 )
    {
      line( "iny" );
      line( "lda " + named( instruction.value, 8 ) );
      line( through );
    }
  }

  /// A value in another type: a byte kept, or two copied, or a narrower value
  /// widened as a store widens it.
  void convert( ir::Convert const& instruction )
  {
    ir::Type const source = ir::typeOf( instruction.operand, *mFunction );
    if ( ir::sizeOf( instruction.type ) == 1 )
    {
      if ( ir::sizeOf( source ) == 2 )
      {
        line( "lda " + named( instruction.operand ) );
      }
      else
      {
        load( instruction.operand );
      }
      keep( instruction.result );
      return;
    }
    std::string const to = location( instruction.result );
    if ( ir::sizeOf( source ) == 2 )
    {
      copy( instruction.operand, to );
      return;
    }
    store( ir::Store{ .name = to, .type = instruction.type, .value = instruction.operand } );
  }

  /// The value, less the smallest label, clamped in `A`: what lies outside
  /// the span the labels cover goes to the entry past it, which holds
  /// `default`'s case or the way past — see
  /// docs/decisions/0166-a-tables-span-is-its-labels.md. A byte spanning
  /// every value it has is inside the span whatever it holds, and is clamped
  /// to nothing. The `.dispatch` that ends the block reads `A` and does its
  /// own doubling, so which form the Target writes is not asked here — see
  /// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
  void switchValue( ir::Switch const& instruction )
  {
    bool const wide = ir::sizeOf( instruction.type ) == 2;
    bool const clamps = wide || instruction.count < MAX_TABLE;
    std::string const span = "#" + std::to_string( instruction.count );
    // The way out is a label only where the high byte's test jumps to it.
    std::string const outside = wide ? localLabel( "w" ) : std::string{};
    std::string const inside = clamps ? localLabel( "w" ) : std::string{};

    if ( instruction.first == 0 )
    {
      // Nothing to take off: a byte indexes the table itself, and of a pair
      // a high byte that holds anything is already outside the span.
      if ( wide )
      {
        line( "lda " + named( instruction.value, 8 ) );
        line( "jne " + outside );
      }
      line( "lda " + named( instruction.value ) );
    }
    else
    {
      // What the subtraction leaves below the span is a label's place in the
      // table; a value below the smallest borrows, and one above the largest
      // is past it, and both are the entry the table ends with. Of a pair the
      // low difference waits in `X` while the high one is taken, there being
      // one accumulator and two subtractions.
      line( "lda " + named( instruction.value ) );
      line( "sec" );
      line( "sbc #" + immediate( instruction.first, 0 ) );
      if ( wide )
      {
        line( "tax" );
        line( "lda " + named( instruction.value, 8 ) );
        line( "sbc #" + immediate( instruction.first, 8 ) );
        line( "jne " + outside );
        line( "txa" );
      }
    }

    if ( clamps )
    {
      line( "cmp " + span );
      line( "jcc " + inside );
      if ( wide )
      {
        label( outside );
      }
      line( "lda " + span );
      label( inside );
    }
    mTracker.forget();
  }

  /// A byte of a number as an immediate operand: its low byte, or its high
  /// where `shift` says.
  [[nodiscard]] static std::string immediate( std::int64_t value, std::uint32_t shift )
  {
    auto const bits = static_cast<std::uint64_t>( value );
    return std::to_string( ( bits >> shift ) & 0xFFU );
  }

  /// Whether the store being written is the block's last of the byte.
  [[nodiscard]] bool setsLoopGoing( std::string const& counter ) const
  {
    std::vector<ir::Instruction> const& body = mFunction->blocks[mBlock].instructions;
    return std::none_of( body.begin() + static_cast<std::ptrdiff_t>( std::min( mAt + 1, body.size() ) ),
                         body.end(),
                         [&counter]( ir::Instruction const& later )
                         {
                           auto const* const store = std::get_if<ir::Store>( &later.operation );
                           return store != nullptr && store->name == counter;
                         } );
  }

  /// Whether a name is a byte this function carries in a register rather than
  /// in memory, whose store the text leaves out.
  [[nodiscard]] bool carriedInRegister( std::string const& name ) const
  {
    if ( mFunction == nullptr )
    {
      return false;
    }
    if ( !mFunction->resultPlace.empty() && name == qualified( std::string{ RESULT } ) )
    {
      return true;
    }
    return std::ranges::any_of( mFunction->parameters,
                                [this, &name]( ir::Local const& parameter )
                                { return !parameter.place.empty() && name == qualified( parameter.name ); } );
  }

  /// The carry an addition or a subtraction needs, and the operation, where
  /// the operand is a literal the carry can be folded into.
  ///
  /// `adc #k` with the carry **set** adds `k + 1`, so where the carry already
  /// stands the wrong way the `clc` is not written and the constant is one
  /// less. The result is the same byte, the carry out is the same — it is set
  /// on exactly the sums `clc / adc #k` sets it on, which is what a wide
  /// addition's high byte reads — and `N` and `Z` come from the same result.
  /// `V` is the one flag that may differ, and nothing reads it from an
  /// operator's result: a signed comparison reads it from its own `sbc`.
  ///
  /// Only for a literal of one or more. Adding zero is the one value whose
  /// carry out would differ, and nothing emits it. See
  /// docs/decisions/0185-a-carry-folded-into-a-constant.md.
  bool foldedCarry( bool add, ir::Operand const& operand )
  {
    auto const* const constant = std::get_if<ir::Constant>( &operand );
    if ( constant == nullptr || !constant->name.empty() )
    {
      return false;
    }
    std::int64_t const value = constant->value & 0xFF;
    if ( value < 1 )
    {
      return false;
    }
    // An addition wants the carry clear and a subtraction wants it set, so
    // each folds the state it would otherwise have to write.
    if ( mTracker.carry() != ( add ? Carry::SET : Carry::CLEAR ) )
    {
      return false;
    }
    line( std::string{ add ? "adc #" : "sbc #" } + std::to_string( value - 1 ) );
    return true;
  }

  /// Stores a zero where the processor can do it without the accumulator, and
  /// says whether it did. The bytes are the `sta`'s, so what this is worth is
  /// the `lda #0` that does not stand and the byte `A` keeps holding — see
  /// docs/decisions/0183-a-zero-stored-without-the-accumulator.md.
  bool storedZero( ir::Operand const& value, std::string const& name )
  {
    // The processor must have it and the program must have asked: measured
    // over the set it is 106 bytes for 14 829 cycles, which is a trade and
    // not a win, and the Intent is where a trade is decided.
    if ( mCpu != Cpu::WDC65SC02 || mIntent != Intent::SIZE )
    {
      return false;
    }
    auto const* const constant = std::get_if<ir::Constant>( &value );
    // A named Constant is written by its name, which `stz` has nowhere to
    // put: the text would stop saying which constant the zero was.
    if ( constant == nullptr || constant->value != 0 || !constant->name.empty() )
    {
      return false;
    }
    // A byte a register carries has its store dropped, and what the register
    // holds is then the value — which `stz` does not put there. So a zero
    // stored into one is loaded and stored as before; see
    // docs/decisions/0145-an-argument-in-a-register.md and 0120.
    if ( carriedInRegister( name ) )
    {
      return false;
    }
    line( "stz " + name );
    return true;
  }

  void store( ir::Store const& instruction )
  {
    ir::Operand const& value = instruction.value;
    std::string const name = qualified( instruction.name );

    // What sets a counted loop going goes into `X` and nowhere else: the byte
    // is written again where the loop leaves. That is the last store of the
    // counter before the loop, and not one the block made of it earlier,
    // which what is between still reads from its byte.
    for ( CountedLoop const& loop : mLoops )
    {
      if ( loop.preheader == mBlock && loop.counter == instruction.name && setsLoopGoing( instruction.name ) )
      {
        line( "ldx #" + std::to_string( loop.from ) );
        return;
      }
    }

    if ( ir::sizeOf( instruction.type ) == 1 )
    {
      if ( !storedZero( value, name ) )
      {
        load( value );
        line( "sta " + name );
      }
      return;
    }

    std::string const high = name + "+1";
    ir::Type const source = ir::typeOf( value, *mFunction );
    if ( ir::sizeOf( source ) == 2 )
    {
      if ( storedZero( value, name ) )
      {
        line( "stz " + high );
        return;
      }
      follows( value );
      line( "lda " + named( value ) );
      line( "sta " + name );
      follows( value );
      line( "lda " + named( value, 8 ) );
      line( "sta " + high );
      return;
    }

    // A narrower value widened: its sign copied into the high byte, or zero.
    if ( storedZero( value, name ) )
    {
      line( "stz " + high );
      return;
    }
    load( value );
    line( "sta " + name );
    signOrZero( ir::isSigned( source ) );
    line( "sta " + high );
  }

  void unary( ir::Unary const& instruction )
  {
    if ( ir::sizeOf( instruction.type ) == 2 )
    {
      std::string const to = location( instruction.result );
      bool const negate = instruction.op == ir::UnaryOperator::NEGATE;
      if ( negate )
      {
        line( "sec" );
      }
      for ( std::uint32_t index = 0; index < 2; ++index )
      {
        if ( negate )
        {
          line( "lda #0" );
          line( "sbc " + named( instruction.operand, 8 * index ) );
        }
        else
        {
          line( "lda " + named( instruction.operand, 8 * index ) );
          line( "eor #255" );
        }
        line( "sta " + byteOf( to, index ) );
      }
      return;
    }
    switch ( instruction.op )
    {
    case ir::UnaryOperator::COMPLEMENT:
      load( instruction.operand );
      line( "eor #255" );
      break;
    case ir::UnaryOperator::LOGICAL_NOT:
      load( instruction.operand );
      line( "eor #1" );
      break;
    case ir::UnaryOperator::NEGATE:
      // Where `A` holds the operand, by value or by name, it is turned over
      // there, and nothing is read — see
      // docs/decisions/0148-a-bit-the-carry-already-holds.md.
      if ( inA( instruction.operand ) || ( std::holds_alternative<ir::Object>( instruction.operand ) &&
                                           mTracker.holdsInA( named( instruction.operand ) ) ) )
      {
        line( "eor #255" );
        stepAccumulator( true, "#1" );
      }
      else
      {
        line( "lda #0" );
        line( "sec" );
        line( "sbc " + named( instruction.operand ) );
      }
      break;
    }
    keep( instruction.result );
  }

  /// The operator's one step on `A`, for a shift by a constant or by a value.
  void shiftOnce( ir::Binary const& instruction )
  {
    if ( instruction.op == ir::BinaryOperator::SHIFT_LEFT )
    {
      line( "asl" );
    }
    else if ( ir::isSigned( instruction.type ) )
    {
      // The sign into carry, and back in at the top.
      line( "cmp #128" );
      line( "ror" );
    }
    else
    {
      line( "lsr" );
    }
  }

  /// Whether the operator is the same with its operands the other way round.
  [[nodiscard]] static bool commutes( ir::BinaryOperator op )
  {
    return op == ir::BinaryOperator::ADD || op == ir::BinaryOperator::AND || op == ir::BinaryOperator::OR ||
           op == ir::BinaryOperator::XOR;
  }

  /// Whether `A` holds the right operand of an operator that commutes, and not
  /// its left: the right is then taken where it is and the left read from
  /// memory, `adc a` rather than `lda a / adc b` — see
  /// docs/decisions/0147-the-operand-a-holds-goes-first.md.
  [[nodiscard]] bool
  heldForCommuting( ir::Binary const& instruction, ir::Operand const& first, ir::Operand const& second ) const
  {
    return commutes( instruction.op ) && std::holds_alternative<ir::Object>( second ) &&
           mTracker.holdsInA( named( second ) ) && !inA( first ) && !mTracker.holdsInA( named( first ) );
  }

  void binary( ir::Binary const& instruction )
  {
    if ( ir::sizeOf( instruction.type ) == 2 )
    {
      wideBinary( instruction );
      return;
    }
    if ( isShiftByValue( instruction ) )
    {
      shiftByValue( instruction );
      return;
    }

    // A byte that is a pair shifted right by eight or more: the high byte,
    // shifted by the rest — see docs/decisions/0140-a-value-only-its-low-byte-is-read-of.md.
    // And one shifted by fewer, every reader of which masks it below what the
    // low byte gives: the low byte, shifted by all of it — see
    // docs/decisions/0273-a-shift-a-mask-keeps-in-a-byte-is-a-byte.md.
    if ( instruction.op == ir::BinaryOperator::SHIFT_RIGHT &&
         ir::sizeOf( ir::typeOf( instruction.left, *mFunction ) ) == 2 )
    {
      std::int64_t const count = std::get<ir::Constant>( instruction.right ).value;
      bool const high = count >= 8;
      line( "lda " + named( instruction.left, high ? 8 : 0 ) );
      for ( std::int64_t done = high ? 8 : 0; done < count; ++done )
      {
        line( "lsr" );
      }
      keep( instruction.result );
      return;
    }

    ir::Operand const* first = &instruction.left;
    ir::Operand const* second = &instruction.right;
    if ( inA( *second ) || counterInX( *second ) || heldForCommuting( instruction, *first, *second ) )
    {
      std::swap( first, second );
    }
    indexFor( *second );
    load( *first );

    switch ( instruction.op )
    {
    case ir::BinaryOperator::ADD:
      if ( !foldedCarry( true, *second ) )
      {
        stepAccumulator( true, named( *second ) );
      }
      break;
    case ir::BinaryOperator::SUBTRACT:
      if ( !foldedCarry( false, *second ) )
      {
        stepAccumulator( false, named( *second ) );
      }
      break;
    case ir::BinaryOperator::AND:
      line( "and " + named( *second ) );
      break;
    case ir::BinaryOperator::OR:
      line( "ora " + named( *second ) );
      break;
    case ir::BinaryOperator::XOR:
      line( "eor " + named( *second ) );
      break;
    case ir::BinaryOperator::SHIFT_LEFT:
    case ir::BinaryOperator::SHIFT_RIGHT:
    {
      std::int64_t const count = std::get<ir::Constant>( *second ).value;
      for ( std::int64_t done = 0; done < count; ++done )
      {
        shiftOnce( instruction );
      }
      break;
    }
    }
    keep( instruction.result );
  }

  /// A 16-bit operator: `A` carries a byte at a time, low first, and the carry
  /// runs between them; a shift is done on the result's own bytes.
  void wideBinary( ir::Binary const& instruction )
  {
    // `x ^ (x << 7)`: the high byte first, from `x` itself — see
    // docs/decisions/0281-a-pair-and-itself-shifted-by-seven-high-byte-first.md.
    if ( mBySeven.size() > instruction.result.index && mBySeven[instruction.result.index] )
    {
      ir::Operand const& other =
          std::holds_alternative<ir::Object>( instruction.left ) ? instruction.left : instruction.right;
      std::string_view mnemonic = "and";
      if ( instruction.op == ir::BinaryOperator::XOR )
      {
        mnemonic = "eor";
      }
      else if ( instruction.op == ir::BinaryOperator::OR )
      {
        mnemonic = "ora";
      }
      std::string const to = location( instruction.result );
      line( "lda " + named( other, 8 ) );
      line( "lsr" );
      line( "lda " + named( other ) );
      line( "ror" );
      line( std::string{ mnemonic } + " " + named( other, 8 ) );
      line( "sta " + byteOf( to, 1 ) );
      line( "lda #0" );
      line( "ror" );
      line( std::string{ mnemonic } + " " + named( other ) );
      line( "sta " + to );
      return;
    }

    // An address that keeps its low byte in `Y`: that byte goes there, and the
    // high byte into `PAGE`.
    bool const page = mPaged.size() > instruction.result.index && mPaged[instruction.result.index];

    // `PAGE` starting at the array: the index's low byte is `Y` as it is, and
    // only the high bytes add — see
    // docs/decisions/0283-a-page-pointer-starts-at-its-array.md.
    if ( std::optional<ir::Constant> const base = mPageBase; page && base.has_value() )
    {
      line( "ldy " + named( instruction.right ) );
      line( "clc" );
      line( "lda " + named( instruction.right, 8 ) );
      line( "adc " + named( *base, 8 ) );
      mTracker.carryClear();
      line( "sta " + byteOf( std::string{ PAGE }, 1 ) );
      return;
    }
    std::string const to = page ? std::string{ PAGE } : location( instruction.result );
    auto const written = [this, page, &to]( std::uint32_t index )
    {
      if ( page && index == 0 )
      {
        line( "tay" );
        return;
      }
      line( "sta " + byteOf( to, index ) );
    };

    // The address of a 16-bit element at a byte index doubled: the byte
    // shifted into `Y`, its top bit carried into the array's high byte.
    if ( mTableAddress.size() > instruction.result.index && mTableAddress[instruction.result.index] )
    {
      std::vector<ir::Instruction> const& here = mFunction->blocks[mBlock].instructions;
      ir::Operand doubled = std::get<ir::Binary>( here[mAt - 1].operation ).left;
      if ( mAt >= 2 && mStepSilent[mBlock][mAt - 2] )
      {
        if ( auto const* const convert = std::get_if<ir::Convert>( &here[mAt - 2].operation );
             convert != nullptr && isValue( doubled, convert->result ) )
        {
          doubled = convert->operand;
        }
      }
      load( doubled );
      line( "asl" );
      line( "tay" );
      if ( mTableInPlace[instruction.result.index] )
      {
        return;
      }
      line( "lda " + named( instruction.left, 8 ) );
      line( "adc #0" );
      if ( elementAddress( instruction ) )
      {
        mTracker.carryClear();
      }
      line( "sta " + byteOf( to, 1 ) );
      line( "lda " + named( instruction.left ) );
      line( "sta " + to );
      return;
    }

    // A pair given back to itself plus a byte, or plus a number below 256:
    // the high byte only ever takes the carry, so it is `inc` past a `bcc`
    // rather than read, added a zero and written. Three cycles where the
    // carry stays clear, and two bytes fewer — see
    // docs/decisions/0270-a-pair-plus-a-byte-takes-the-carry-by-inc.md.
    if ( instruction.op == ir::BinaryOperator::ADD && !page && !mTracker.isVolatile( to ) )
    {
      ir::Operand const* added = nullptr;
      if ( named( instruction.left ) == to && named( instruction.left, 8 ) == byteOf( to, 1 ) )
      {
        added = &instruction.right;
      }
      else if ( named( instruction.right ) == to && named( instruction.right, 8 ) == byteOf( to, 1 ) )
      {
        added = &instruction.left;
      }
      if ( added != nullptr && named( *added, 8 ) == "#0" && named( *added ) != "#0" )
      {
        line( "clc" );
        indexFor( *added );
        line( "lda " + to );
        line( "adc " + named( *added ) );
        line( "sta " + to );
        std::string const done = localLabel( "i" );
        line( "bcc " + done );
        line( "inc " + byteOf( to, 1 ) );
        label( done );
        return;
      }
    }

    std::string_view mnemonic;
    // A sum or a difference whose low bytes add a zero is the other low byte,
    // with no carry out of it: that byte is copied, and the `clc` or `sec`
    // waits for the first byte that is added — `(hi << 8) + lo` is two
    // copies. See docs/decisions/0142-a-shift-by-eight-is-a-view-of-bytes.md.
    bool const lowIsCopy = ( instruction.op == ir::BinaryOperator::ADD &&
                             ( named( instruction.left ) == "#0" || named( instruction.right ) == "#0" ) ) ||
                           ( instruction.op == ir::BinaryOperator::SUBTRACT && named( instruction.right ) == "#0" );
    switch ( instruction.op )
    {
    case ir::BinaryOperator::ADD:
      if ( !lowIsCopy )
      {
        line( "clc" );
      }
      mnemonic = "adc";
      break;
    case ir::BinaryOperator::SUBTRACT:
      if ( !lowIsCopy )
      {
        line( "sec" );
      }
      mnemonic = "sbc";
      break;
    case ir::BinaryOperator::AND:
      mnemonic = "and";
      break;
    case ir::BinaryOperator::OR:
      mnemonic = "ora";
      break;
    case ir::BinaryOperator::XOR:
      mnemonic = "eor";
      break;
    case ir::BinaryOperator::SHIFT_LEFT:
    case ir::BinaryOperator::SHIFT_RIGHT:
      if ( isShiftByValue( instruction ) )
      {
        shiftByValue( instruction );
      }
      else
      {
        wideShift( instruction, to );
      }
      return;
    }
    // A bitwise operator against a high byte of zero leaves the other high
    // byte as it is, or clears it for `&`: no `ora #0`, and nothing at all
    // where the answer is written over that operand — `code | bit` is the low
    // byte alone. See docs/decisions/0138-a-pair-an-operand-leaves-is-the-answers.md.
    bool const bitwise = instruction.op == ir::BinaryOperator::AND || instruction.op == ir::BinaryOperator::OR ||
                         instruction.op == ir::BinaryOperator::XOR;
    ir::Operand const* left = &instruction.left;
    ir::Operand const* right = &instruction.right;
    if ( bitwise && named( *left, 8 ) == "#0" && named( *right, 8 ) != "#0" )
    {
      std::swap( left, right );
    }
    // A byte read from memory by the instruction itself has its index loaded
    // first; its high byte is zero, so only the low byte reads it.
    indexFor( *left );
    indexFor( *right );
    bool carryIn = !lowIsCopy;
    auto const pairThroughY = [this]( ir::Operand const& operand )
    {
      auto const* const value = std::get_if<ir::Value>( &operand );
      return value != nullptr && value->index < mPairThroughY.size() && mPairThroughY[value->index];
    };
    for ( std::uint32_t index = 0; index < 2; ++index )
    {
      // A pair read through a pointer by the operator itself: its high byte
      // stands one on from its low one.
      if ( index == 1 && ( pairThroughY( *left ) || pairThroughY( *right ) ) )
      {
        line( "iny" );
      }
      // A pair masked below its high byte: the low byte masked as it is read,
      // the high byte read as it is.
      MaskedLow const* const leftMasked = maskedLow( *left );
      MaskedLow const* const rightMasked = maskedLow( *right );
      if ( index == 0 && ( leftMasked != nullptr || rightMasked != nullptr ) )
      {
        MaskedLow const& masked = leftMasked != nullptr ? *leftMasked : *rightMasked;
        ir::Operand const& other = leftMasked != nullptr ? *right : *left;
        line( "lda " + named( masked.source ) );
        line( "and #" + std::to_string( masked.mask ) );
        line( std::string{ mnemonic } + " " + named( other ) );
        written( index );
        continue;
      }
      std::string first = leftMasked != nullptr ? named( leftMasked->source, 8 * index ) : named( *left, 8 * index );
      std::string second =
          rightMasked != nullptr ? named( rightMasked->source, 8 * index ) : named( *right, 8 * index );
      // The byte `A` holds already goes first, where the operator commutes.
      if ( commutes( instruction.op ) && mTracker.holdsInA( second ) && !mTracker.holdsInA( first ) )
      {
        std::swap( first, second );
      }
      bool const adds = instruction.op == ir::BinaryOperator::ADD || instruction.op == ir::BinaryOperator::SUBTRACT;
      if ( adds && !carryIn )
      {
        // No carry comes into this byte: a zero on the side that may be one
        // makes it a copy, and otherwise the carry is set up here.
        bool const copies =
            instruction.op == ir::BinaryOperator::ADD ? first == "#0" || second == "#0" : second == "#0";
        if ( copies )
        {
          std::string const kept = second == "#0" ? first : second;
          if ( kept != byteOf( to, index ) )
          {
            line( "lda " + kept );
            written( index );
          }
          continue;
        }
        line( instruction.op == ir::BinaryOperator::ADD ? "clc" : "sec" );
        carryIn = true;
      }
      if ( bitwise && ( first == "#0" || second == "#0" ) )
      {
        // Against a zero byte, either side: `&` is zero, and `|` and `^` are
        // the other byte, nothing at all where that is the answer's own.
        std::string kept = second == "#0" ? first : second;
        if ( instruction.op == ir::BinaryOperator::AND )
        {
          kept = "#0";
        }
        if ( kept != byteOf( to, index ) )
        {
          line( "lda " + kept );
          written( index );
        }
        continue;
      }
      line( "lda " + first );
      line( std::string{ mnemonic } + " " + second );
      written( index );
    }
    if ( elementAddress( instruction ) )
    {
      mTracker.carryClear();
    }
  }

  /// The pair an operand is masked from, where `planMaskedOperands` left it
  /// for its reader.
  [[nodiscard]] MaskedLow const* maskedLow( ir::Operand const& operand ) const
  {
    auto const* const value = std::get_if<ir::Value>( &operand );
    if ( value == nullptr || value->index >= mMaskedLow.size() )
    {
      return nullptr;
    }
    std::optional<MaskedLow> const& found = mMaskedLow[value->index];
    return found.has_value() ? &*found : nullptr;
  }

  /// Whether a sum is the address of an element of an array the program
  /// declares: its high byte carries nothing out, since the element lies in
  /// the array, which lies in memory — see
  /// docs/decisions/0284-an-elements-address-carries-nothing-out.md and the C
  /// subset's rule that an index stays within its array.
  [[nodiscard]] bool elementAddress( ir::Binary const& instruction ) const
  {
    if ( instruction.op != ir::BinaryOperator::ADD || instruction.type != ir::Type::POINTER )
    {
      return false;
    }
    auto const declared = [this]( ir::Operand const& operand )
    {
      auto const* const constant = std::get_if<ir::Constant>( &operand );
      return constant != nullptr && constant->follower.empty() && mArrays.contains( constant->name );
    };
    return declared( instruction.left ) || declared( instruction.right );
  }

  /// A 16-bit shift by a constant: a count of eight or more moves a byte
  /// across first, and what is left is shifted a place at a time.
  /// Seven places is one place short of a byte, so the byte moves and the
  /// value is shifted **the other way** once: `x << 7` is the high byte's
  /// bottom bit into the carry, the low byte rotated right after it — which is
  /// the high byte of the answer — and one more rotate into a zero byte for the
  /// low. Eight instructions against the eighteen a chain of seven takes, and
  /// no byte written twice. See
  /// docs/decisions/0124-a-shift-of-seven-goes-the-other-way.md.
  void wideShiftBySeven( ir::Binary const& instruction, std::string const& to )
  {
    if ( instruction.op == ir::BinaryOperator::SHIFT_LEFT )
    {
      // Both bytes of the operand are read before either byte of the answer is
      // written, so the answer may stand in the operand's own bytes.
      line( "lda " + named( instruction.left, 8 ) );
      line( "lsr" );
      line( "lda " + named( instruction.left ) );
      line( "ror" );
      line( "sta " + byteOf( to, 1 ) );
      line( "lda #0" );
      line( "ror" );
      line( "sta " + to );
      return;
    }
    line( "lda " + named( instruction.left ) );
    line( "asl" );
    line( "lda " + named( instruction.left, 8 ) );
    line( "rol" );
    line( "sta " + to );
    line( "lda #0" );
    line( "rol" );
    line( "sta " + byteOf( to, 1 ) );
  }

  /// Five or six places are a byte across and three or two places back: the
  /// operand stands in three bytes — the far one zero — and they are rotated
  /// the other way, `A` the middle one. `x << 6` is
  ///
  ///     lda x+1 / sta to+1 / lda x
  ///     lsr to+1 / ror / ror to       ; twice
  ///     sta to+1 / lda to / and #192 / sta to
  ///
  /// where the bits the rotates bring into `to` from above are masked off
  /// rather than cleared first, which would want a register `A` is holding.
  /// Where the high byte is known to be zero, its own rotate goes. See
  /// docs/decisions/0264-a-shift-of-five-or-six-goes-the-other-way.md.
  void wideShiftAcross( ir::Binary const& instruction, std::string const& to, std::int64_t count )
  {
    std::int64_t const back = 8 - count;
    if ( instruction.op == ir::BinaryOperator::SHIFT_LEFT )
    {
      std::string const high = named( instruction.left, 8 );
      bool const zeroAbove = high == "#0";
      if ( !zeroAbove )
      {
        line( "lda " + high );
        line( "sta " + byteOf( to, 1 ) );
      }
      line( "lda " + named( instruction.left ) );
      for ( std::int64_t done = 0; done < back; ++done )
      {
        if ( zeroAbove )
        {
          line( "lsr" );
        }
        else
        {
          line( "lsr " + byteOf( to, 1 ) );
          line( "ror" );
        }
        line( "ror " + to );
      }
      line( "sta " + byteOf( to, 1 ) );
      line( "lda " + to );
      line( "and #" + std::to_string( ( 0xFF << count ) & 0xFF ) );
      line( "sta " + to );
      return;
    }
    line( "lda " + named( instruction.left ) );
    line( "sta " + to );
    line( "lda " + named( instruction.left, 8 ) );
    for ( std::int64_t done = 0; done < back; ++done )
    {
      line( "asl " + to );
      line( "rol" );
      line( "rol " + byteOf( to, 1 ) );
    }
    line( "sta " + to );
    line( "lda " + byteOf( to, 1 ) );
    line( "and #" + std::to_string( ( 1 << back ) - 1 ) );
    line( "sta " + byteOf( to, 1 ) );
  }

  void wideShift( ir::Binary const& instruction, std::string const& to )
  {
    std::int64_t count = std::get<ir::Constant>( instruction.right ).value;
    bool const leftwards = instruction.op == ir::BinaryOperator::SHIFT_LEFT;

    // A shift of a signed value to the right brings its sign in at the top,
    // which neither rotating the other way nor shifting in `A` does, so that
    // one keeps to its two bytes a place at a time however near a byte it
    // stands.
    bool const signedRight = !leftwards && ir::isSigned( instruction.type );
    if ( count == 7 && !signedRight )
    {
      wideShiftBySeven( instruction, to );
      return;
    }

    // Five places the other way are a byte longer than five in place on the
    // zero page and six cycles quicker, which is a trade and the Intent's; six
    // places, and five of a byte widened, are shorter and quicker both.
    bool const zeroAbove = leftwards && named( instruction.left, 8 ) == "#0";
    if ( !signedRight && ( count == 6 || ( count == 5 && ( zeroAbove || mIntent == Intent::SPEED ) ) ) )
    {
      wideShiftAcross( instruction, to, count );
      return;
    }

    // Eight places or more move a byte across, and what is left of the count
    // is shifted **in `A`**: the byte vacated is zero, so the pair holds
    // nothing the shifts need. `x >> 9` is `lda x+1 / lsr / sta to / lda #0 /
    // sta to+1`, where it was a store, a pair to shift and two shifts of it.
    if ( count >= 8 && !signedRight )
    {
      line( "lda " + named( instruction.left, leftwards ? 0 : 8 ) );
      for ( std::int64_t done = 8; done < count; ++done )
      {
        line( leftwards ? "asl" : "lsr" );
      }
      line( "sta " + byteOf( to, leftwards ? 1 : 0 ) );
      line( "lda #0" );
      line( "sta " + byteOf( to, leftwards ? 0 : 1 ) );
      return;
    }

    // One place, from bytes that are not `to`'s: through `A`, which the
    // copy loads anyway, rather than copied and then shifted in memory.
    if ( count == 1 && !signedRight && named( instruction.left ) != to )
    {
      follows( instruction.left );
      std::uint32_t const first = leftwards ? 0U : 8U;
      std::uint32_t const second = leftwards ? 8U : 0U;
      line( "lda " + named( instruction.left, first ) );
      line( leftwards ? "asl" : "lsr" );
      line( "sta " + byteOf( to, first / 8 ) );
      line( "lda " + named( instruction.left, second ) );
      line( leftwards ? "rol" : "ror" );
      line( "sta " + byteOf( to, second / 8 ) );
      return;
    }
    if ( count < 8 )
    {
      copy( instruction.left, to );
    }
    else
    {
      line( "lda " + named( instruction.left, 8 ) );
      line( "sta " + to );
      signOrZero( true );
      line( "sta " + byteOf( to, 1 ) );
      count -= 8;
    }
    for ( std::int64_t done = 0; done < count; ++done )
    {
      shiftInPlace( instruction, to );
    }
  }

  /// A 16-bit shift by one place, on the two bytes `to` names.
  void shiftInPlace( ir::Binary const& instruction, std::string const& to )
  {
    if ( instruction.op == ir::BinaryOperator::SHIFT_LEFT )
    {
      line( "asl " + to );
      line( "rol " + byteOf( to, 1 ) );
      return;
    }
    if ( ir::isSigned( instruction.type ) )
    {
      // The sign into carry, and back in at the top.
      line( "lda " + byteOf( to, 1 ) );
      line( "cmp #128" );
      line( "ror " + byteOf( to, 1 ) );
    }
    else
    {
      line( "lsr " + byteOf( to, 1 ) );
    }
    line( "ror " + to );
  }

  /// With a byte in `A`: its sign spread over a byte, $FF or 0, where `isSigned`,
  /// and zero otherwise.
  void signOrZero( bool isSigned )
  {
    if ( isSigned )
    {
      // The sign into carry; zero less the borrow it leaves is 0 or $FF, the
      // opposite of the byte wanted.
      line( "asl" );
      line( "lda #0" );
      line( "sbc #0" );
      line( "eor #255" );
    }
    else
    {
      line( "lda #0" );
    }
  }

  /// A shift by a count held in a `u8`: the count in `Y`, counted down by
  /// `dey`, and the byte the shift moves first in `A` — the high one of a
  /// pair shifted right, the low one of a pair shifted left — so that a round
  /// of a pair is `lsr / ror lo / dey / bne`, twelve cycles, and of a byte
  /// `lsr / dey / bne`, seven. `Y` holds nothing from one instruction to the
  /// next — docs/decisions/0133-a-second-index-goes-through-y.md — and
  /// `X` may be a counted loop's, so the count goes in `Y`. See
  /// docs/decisions/0271-a-shift-by-a-byte-counts-in-y.md.
  void shiftByValue( ir::Binary const& instruction )
  {
    bool const wide = ir::sizeOf( instruction.type ) == 2;
    auto const throughY = [this]( ir::Operand const& operand )
    {
      std::string const low = named( operand );
      return low.contains( ",y" ) || low.contains( '(' );
    };
    if ( throughY( instruction.right ) || throughY( instruction.left ) )
    {
      shiftByValueInMemory( instruction );
      return;
    }
    std::string const again = localLabel( "r" );
    std::string const done = localLabel( "d" );
    std::string const to = wide ? location( instruction.result ) : std::string{};
    // The count is read first, since the result may be written over it.
    line( inA( instruction.right ) ? std::string{ "tay" } : "ldy " + named( instruction.right ) );
    bool const left = instruction.op == ir::BinaryOperator::SHIFT_LEFT;
    if ( wide )
    {
      copy( instruction.left, to );
      line( "lda " + byteOf( to, left ? 0 : 1 ) );
    }
    else
    {
      load( instruction.left );
    }
    line( "cpy #0" );
    line( "beq " + done );
    label( again );
    shiftOnce( instruction );
    if ( wide )
    {
      line( std::string{ left ? "rol " : "ror " } + byteOf( to, left ? 1 : 0 ) );
    }
    line( "dey" );
    line( "bne " + again );
    label( done );
    if ( wide )
    {
      line( "sta " + byteOf( to, left ? 0 : 1 ) );
      return;
    }
    keep( instruction.result );
  }

  /// The same with the count in a scratch byte of its own, where an operand is
  /// read through `Y`: counted down, one shift a round. Neither the count nor
  /// the shifts touch the overflow flag, so a branch on it clear closes the
  /// loop.
  void shiftByValueInMemory( ir::Binary const& instruction )
  {
    std::string const counter = qualified( scratchName( scratch( 1 ) ) );
    std::string const again = localLabel( "r" );
    std::string const done = localLabel( "d" );
    bool const wide = ir::sizeOf( instruction.type ) == 2;
    std::string const to = wide ? location( instruction.result ) : std::string{};
    // The count is read first, since the result may be written over it.
    line( "lda " + named( instruction.right ) );
    line( "sta " + counter );
    if ( wide )
    {
      copy( instruction.left, to );
    }
    else
    {
      line( "lda " + named( instruction.left ) );
    }
    line( "inc " + counter );
    line( "clv" );
    label( again );
    line( "dec " + counter );
    line( "beq " + done );
    if ( wide )
    {
      shiftInPlace( instruction, to );
    }
    else
    {
      shiftOnce( instruction );
    }
    line( "bvc " + again );
    label( done );
    if ( !wide )
    {
      keep( instruction.result );
    }
  }

  /// A comparison's `bool`, 0 or 1, without a branch: the carry a comparison
  /// leaves is rolled into `A`.
  void compare( ir::Compare const& instruction )
  {
    if ( ir::sizeOf( instruction.type ) == 2 )
    {
      wideCompare( instruction );
      return;
    }
    ir::Operand const* first = &instruction.left;
    ir::Operand const* second = &instruction.right;
    if ( inA( *second ) )
    {
      std::swap( first, second );
    }
    indexFor( *second );
    load( *first );

    bool invert = false;
    if ( instruction.op == ir::Comparison::EQUAL || instruction.op == ir::Comparison::NOT_EQUAL )
    {
      // Zero where the two are equal; carry set by any other.
      line( "eor " + named( *second ) );
      line( "cmp #1" );
      invert = instruction.op == ir::Comparison::EQUAL;
    }
    else if ( ir::isSigned( instruction.type ) )
    {
      line( "sec" );
      line( "sbc " + named( *second ) );
      signIntoCarry();
      invert = instruction.op == ir::Comparison::GREATER_OR_EQUAL;
    }
    else
    {
      // Carry set where the first is not less.
      line( "cmp " + named( *second ) );
      invert = instruction.op == ir::Comparison::LESS;
    }
    line( "lda #0" );
    line( "rol" );
    if ( invert )
    {
      line( "eor #1" );
    }
    keep( instruction.result );
  }

  /// A 16-bit comparison's `bool`: the bytes differ where either pair does,
  /// and the order is the carry, or the sign and overflow, of the subtraction.
  void wideCompare( ir::Compare const& instruction )
  {
    bool invert = false;
    if ( instruction.op == ir::Comparison::EQUAL || instruction.op == ir::Comparison::NOT_EQUAL )
    {
      // Zero where both pairs are equal; carry set by any other.
      std::string const differ = localLabel( "e" );
      line( "lda " + named( instruction.left ) );
      line( "eor " + named( instruction.right ) );
      line( "bne " + differ );
      line( "lda " + named( instruction.left, 8 ) );
      line( "eor " + boundHigh( instruction ) );
      label( differ );
      line( "cmp #1" );
      invert = instruction.op == ir::Comparison::EQUAL;
    }
    else
    {
      subtractWide( instruction.left, instruction.right, boundHigh( instruction ) );
      if ( ir::isSigned( instruction.type ) )
      {
        signIntoCarry();
        invert = instruction.op == ir::Comparison::GREATER_OR_EQUAL;
      }
      else
      {
        invert = instruction.op == ir::Comparison::LESS;
      }
    }
    line( "lda #0" );
    line( "rol" );
    if ( invert )
    {
      line( "eor #1" );
    }
    keep( instruction.result );
  }

  Tracker mTracker;

  ir::Function const* mFunction = nullptr;
  std::optional<diag::SourceLocation> mMarked;
  std::uint32_t mLocalLabels = 0;

  /// The bytes of each scratch name of the function being written: one, or a
  /// pair for a 16-bit value.
  std::vector<std::uint32_t> mScratchSizes;

  /// Whether each scratch name holds a pointer, and so lies in the zero page
  /// whatever class the function asked for — see
  /// docs/decisions/0210-placement-is-declared-in-c-too.md.
  std::vector<bool> mScratchPointers;

  /// The scratch name of each value of the function being written, or nothing
  /// for one that lives in `A` or in the object it is stored in.
  std::vector<std::optional<std::uint32_t>> mScratch;

  /// The object each value is written straight into, or empty.
  std::vector<std::string> mDirect;

  /// The byte a wide value already lies in and is read from there — the byte a
  /// call left its result in — so that the call copies nothing and the reader
  /// names that byte. By the value's index; empty for every other value. See
  /// docs/decisions/0123-a-call-s-result-is-read-where-it-lies.md.
  std::vector<std::string> mAtName;

  /// Whether each block's last instruction is a comparison its branch reads
  /// from the flags.
  std::vector<bool> mFused;

  /// Where a rotated loop's step block goes in the text, which is not where
  /// its IR says. By the block's index; nothing for every other block. What
  /// the trackers read in place of the jump to the header — see
  /// docs/decisions/0128-a-loop-tests-where-it-jumps-back.md.
  std::vector<std::optional<Rotated>> mRotatedStep;

  /// Where each block's text begins, the bytes each rotated test reaches back
  /// over, by the block that writes it, and the loops found too long for it.
  std::vector<std::size_t> mBlockTextAt;
  std::map<std::uint32_t, std::size_t> mRotatedReach;
  std::set<std::uint32_t> mFarLatches;

  /// Which blocks a label is written for, and which stores a wide value was
  /// computed straight into, so that the store itself is not written.
  std::vector<bool> mLabelled;
  std::vector<std::vector<bool>> mStoredDirectly;

  /// A byte of the Proc's own plus or minus a number, read only as the index
  /// of byte elements, where the ranges say the sum does not wrap: the byte
  /// goes into the index register and the number into the address, `ldx at /
  /// lda state+1,x`, and the sum is not written. By the sum's value. See
  /// docs/decisions/0134-a-number-added-to-an-index-goes-into-the-address.md.
  struct FoldedIndex
  {
    std::string base;
    std::int64_t by = 0;
  };

  std::vector<std::optional<FoldedIndex>> mFoldedIndex;

  /// The addresses whose low byte goes into `Y` and whose high byte into the
  /// pointer `PAGE`, whose low byte is zero, by the address's value; and the
  /// blocks that clear that low byte before the loop they enter — see
  /// docs/decisions/0266-an-address-keeps-its-low-byte-in-y.md.
  std::vector<bool> mPaged;
  std::set<std::uint32_t> mClearsPage;

  /// The array every address through `PAGE` is an element of, where they
  /// share one: `PAGE`'s low byte is then the array's, `Y` the index's low
  /// byte and the high byte the index's plus the array's — see
  /// docs/decisions/0283-a-page-pointer-starts-at-its-array.md.
  std::optional<ir::Constant> mPageBase;

  /// The arrays the unit and the function declare, by name.
  std::set<std::string> mArrays;

  /// The pairs masked below their high byte, by value — see `MaskedLow`.
  std::vector<std::optional<MaskedLow>> mMaskedLow;

  /// The index of a 16-bit element that is the one before it plus a number,
  /// by the index's value: `X` stepped that many pairs rather than the sum
  /// made and doubled again; and the sums that are therefore not written —
  /// see docs/decisions/0269-the-next-element-is-two-on-in-x.md.
  std::vector<std::int64_t> mIndexStep;
  std::vector<std::vector<bool>> mStepSilent;

  /// The addresses of a 16-bit element at a byte index doubled, whose low
  /// byte goes into `Y` and whose carry goes into the pointer's high byte —
  /// see docs/decisions/0272-a-word-at-a-byte-index-is-read-through-y.md.
  std::vector<bool> mTableAddress;

  /// Those of them whose pointer is the address itself: a pointer of the
  /// Proc's own at an index that carries nothing — see
  /// docs/decisions/0276-an-element-through-a-pointer-at-a-small-index.md.
  std::vector<bool> mTableInPlace;

  /// The 16-bit operators that read a pair shifted left by seven a byte at a
  /// time, high byte first — see
  /// docs/decisions/0281-a-pair-and-itself-shifted-by-seven-high-byte-first.md.
  std::vector<bool> mBySeven;

  /// The 16-bit comparisons against a bound known to hold 256 or more — see
  /// docs/decisions/0287-a-bound-known-past-a-byte-is-compared-high-byte-first.md.
  std::vector<bool> mKnownBound;

  /// The high bytes of 16-bit comparisons' bounds that the ranges know — see
  /// docs/decisions/0290-a-high-byte-the-ranges-know-is-read-as-a-number.md.
  std::vector<std::optional<std::int64_t>> mBoundHigh;

  /// The pairs read through a pointer whose low byte goes into `X` too — see
  /// docs/decisions/0289-a-pair-only-compared-is-held-in-a-and-x.md.
  std::vector<bool> mLowInX;

  /// The pairs read through a pointer that the next instruction reads as an
  /// operand, a byte at a time with `iny` between, rather than through a
  /// scratch pair — and whether `Y` is already where the low byte is.
  std::vector<bool> mPairThroughY;
  std::vector<std::optional<ir::Operand>> mPairPointer;

  /// A 16-bit shift by eight whose answer is bytes that already exist — `x <<
  /// 8` is `#0` below and `x`'s low byte above, `x >> 8` `x`'s high byte below
  /// and `#0` above — read by the names of those bytes and never computed. By
  /// the shift's value: its low byte's name and its high byte's. See
  /// docs/decisions/0142-a-shift-by-eight-is-a-view-of-bytes.md.
  struct ByteView
  {
    ir::Object source;
    bool left = true;
  };

  std::vector<std::optional<ByteView>> mByteView;

  /// Where a block that ends by storing `A` to a byte and jumping on goes in
  /// the text instead: into the block written after it, whose text is that
  /// store and the same jump. Its own store and jump are not written. By the
  /// block's index; nothing for every other block. See
  /// docs/decisions/0132-a-way-falls-into-the-store-it-shares.md.
  std::vector<std::optional<std::uint32_t>> mFallsInto;

  /// The loops whose counter is kept in `X` while they run — see
  /// docs/decisions/0109-an-induction-variable-in-x.md.
  std::vector<CountedLoop> mLoops;

  /// The loops whose pair counter is kept in `Y` and `X` while they run, the
  /// headers of those a run of the text refused, the one being written, and
  /// which of its two bytes the registers hold yet — see
  /// docs/decisions/0291-a-pair-counter-lives-in-y-and-x.md.
  std::vector<ResidentPair> mResident;
  std::set<std::uint32_t> mResidentDropped;
  std::set<std::uint32_t> mResidentRefused;
  std::optional<std::size_t> mResidentOn;
  std::string mResidentDeferred;
  std::array<bool, 2> mResidentWritten{};

  /// The block being written, which is what says whether `X` holds a counter.
  std::uint32_t mBlock = 0;

  /// The loads of each block that the store after them writes, by
  /// instruction — see `planPairsMoved`.
  std::vector<std::vector<bool>> mMoved;

  /// The instructions of each block whose element goes through `Y` because
  /// the run they stand in turns between two indexes and `X` holds the other
  /// — see docs/decisions/0201-a-run-between-two-indexes-takes-a-register-each.md.
  std::vector<std::vector<bool>> mSecondIndex;

  /// The bytes of the Proc's own that are read before written on some way out
  /// of each block — see docs/decisions/0111-a-store-nothing-reads-is-not-written.md.
  std::vector<std::set<std::string>> mLiveIn;

  /// The operators of each block that may be done in place, by instruction —
  /// see docs/decisions/0112-an-operator-in-place.md. Whether one is depends
  /// on what `A` holds when it is reached, which is known only then.
  std::vector<std::vector<std::optional<InPlace>>> mInPlace;

  /// Set where an operator was just done in place, so that the store after it
  /// — which the operator has already written — is not written again.
  bool mStoredInPlace = false;

  /// How many instructions after an operator in place it wrote as well: an
  /// element's operator and its store.
  std::uint32_t mSkipAfterInPlace = 0;

  /// For a value that is an element read under the `X` a counted loop holds,
  /// and read once, as the right operand of a byte's operator or compare: the
  /// operand the text writes in its place, `data+1,x`, so that the element is
  /// the instruction's memory operand and no load stands on its own. By the
  /// value's index; empty for every other value. See
  /// docs/decisions/0114-what-the-machine-does-in-memory.md.
  std::vector<std::string> mAsOperand;

  /// For such a value whose index is not already in `X`: the line that loads
  /// it where the element is read, `ldx i` before `sbc t,x` — or `ldy i`
  /// before `sbc t,y` under a counted loop, whose counter `X` holds. Empty
  /// where `X` holds it, or for every other value. See
  /// docs/decisions/0115-an-index-is-loaded-where-the-element-is-read.md.
  std::vector<std::string> mIndexToLoad;

  /// The loads of each block that `mAsOperand` stands in for, by instruction,
  /// and the `and` of each block that a `bit` stands in for, at its
  /// second-to-last instruction.
  std::vector<std::vector<bool>> mDeferred;

  /// A test of bit 7 or 6 of a byte in memory that ends a block: `bit` reads
  /// the bit into `N` or `V` and touches `A` not at all.
  struct BitTest
  {
    std::uint32_t masked = 0;
    std::string object;
    bool high = true;

    /// Bit 7 read by `asl` into the carry, where both ways out begin with
    /// that shift of the byte.
    bool shifted = false;

    /// Bit 0 read by `lsr` instead, and of a pair shifted where it lies.
    bool right = false;
    bool wide = false;
  };

  std::vector<std::optional<BitTest>> mBitTest;

  /// The blocks entered with `A` holding what their first instruction, a
  /// shift left of a byte, computes: the test before them wrote it.
  std::vector<bool> mShiftedIn;

  /// The blocks whose first instruction, a shift right of a pair into the
  /// pair itself, the test before them has done — see
  /// docs/decisions/0280-a-shift-right-both-ways-share-is-the-test-of-bit-0.md.
  std::vector<bool> mShiftedBefore;

  /// The bytes of the Proc being written, by name, which a macro body of its
  /// `[[with]]` blocks qualifies; how many such bodies the text being written
  /// stands in; the bodies of the Proc so far, each with the name its use
  /// wrote; and the names of the uses written whose bodies are still to come
  /// — see docs/decisions/0096-panes-in-c.md.
  std::set<std::string> mProcBytes;
  std::uint32_t mInBody = 0;
  std::vector<std::pair<std::string, std::string>> mBodies;
  std::vector<std::string> mPendingBodies;
};

/// The fourth pass: the text a file's IR is emitted as, a definition at a
/// time.
class Writer : public Output
{
public:
  Writer( diag::SourceManager const& sources, std::string_view sourcePath, Cpu cpu, Intent intent )
      : Output( sources, sourcePath, cpu, intent ), mPath( sourcePath )
  {
  }

  [[nodiscard]] std::string unit( ir::Unit const& lowered )
  {
    for ( ir::Definition const& definition : lowered.definitions )
    {
      separate();
      if ( auto const* object = std::get_if<ir::Global>( &definition ) )
      {
        global( *object );
        continue;
      }
      if ( auto const* type = std::get_if<ir::Enumeration>( &definition ) )
      {
        enumeration( *type );
        continue;
      }
      if ( auto const* constant = std::get_if<ir::NamedConstant>( &definition ) )
      {
        namedConstant( *constant, "" );
        continue;
      }
      if ( auto const* aggregate = std::get_if<ir::Aggregate>( &definition ) )
      {
        members( *aggregate );
        continue;
      }
      if ( auto const* slot = std::get_if<ir::Slot>( &definition ) )
      {
        // Declared here and exported, since a Slot is a contract between
        // Symbols and every Module that uses it names the same one — see
        // docs/decisions/0031-slots.md.
        mark( slot->at, "" );
        mText.append( ".export " ).append( slot->name ).append( "\n" );
        mText.append( ".slot " ).append( slot->name );
        mText.append( slot->isVector ? ", vector\n" : ", pointer, zeropage\n" );
        // A Slot that takes or returns names its Temporaries beside itself:
        // no Proc holds them, the Cell's own `jmp` being the dispatch, and a
        // Slot is no Label so nothing it stands to the left of is an
        // attribute — see docs/decisions/0174-a-slot-takes-and-returns.md.
        if ( !slot->parameters.empty() || slot->result.has_value() )
        {
          mText.append( ".namespace " ).append( slot->name ).append( "\n" );
          // Exported, as the Slot itself is: an Implementation in another
          // Module reads them, and without the export its `scale.x` would be
          // an attribute of the Slot, which is no Label and has none.
          std::string names;
          for ( ir::Local const& parameter : slot->parameters )
          {
            names.append( names.empty() ? "" : ", " ).append( parameter.name );
          }
          if ( slot->result.has_value() )
          {
            names.append( names.empty() ? "" : ", " ).append( RESULT );
          }
          mText.append( ".export " ).append( names ).append( "\n" );
          for ( ir::Local const& parameter : slot->parameters )
          {
            temporary( parameter.name, parameter.type, parameter.bytes, false );
          }
          if ( slot->result.has_value() )
          {
            temporary( std::string{ RESULT }, *slot->result, slot->resultBytes, false );
          }
          mText.append( ".endns\n" );
        }
        continue;
      }
      auto const& function = std::get<ir::Function>( definition );
      FunctionWriter one{ *mSources, mPath, mCpu, mIntent, lowered };
      one.write( function );
      append( one, function.name );
    }
    dropUnnamedBytes( mText, mDeclared, mRegions, mRegionOwners );
    return std::move( mText );
  }

private:
  /// A function's text after the unit's, and its declarations that may go,
  /// by where they now stand and whose they are.
  void append( Output const& one, std::string const& owner )
  {
    std::size_t const base = mText.size();
    mRegions.push_back( base );
    mRegionOwners.push_back( owner );
    for ( Declared declared : one.declared() )
    {
      declared.begin += base;
      declared.end += base;
      declared.owner = owner;
      declared.region = mRegions.size() - 1;
      mDeclared.push_back( std::move( declared ) );
    }
    mText.append( one.text() );
  }

  /// The source's path as written, for the writer of each function to quote.
  std::string_view mPath;

  /// Where each function's text begins, in the order written, and its name.
  std::vector<std::size_t> mRegions;
  std::vector<std::string> mRegionOwners;
};

} // namespace

std::string writeUnit(
    diag::SourceManager const& sources, std::string_view sourcePath, Cpu cpu, Intent intent, ir::Unit const& lowered )
{
  return Writer{ sources, sourcePath, cpu, intent }.unit( lowered );
}

PlaceFacts placeFacts( diag::SourceManager const& sources,
                       std::string_view sourcePath,
                       Cpu cpu,
                       Intent intent,
                       ir::Function const& lowered,
                       ir::Unit const& unit )
{
  return FunctionWriter{ sources, sourcePath, cpu, intent, unit }.placeFacts( lowered );
}

} // namespace nga::c::detail
