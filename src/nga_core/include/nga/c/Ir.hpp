#pragma once

#include "nga/diag/SourceLocation.hpp"
#include "nga/model/PlacementClass.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace nga::c::ir
{

/// The compiler's own form of a `.ngc` file, between the syntax tree and the
/// text: functions of basic blocks, each a list of instructions ended by one
/// terminator, over SSA values typed by the subset's types. Built from a
/// checked tree, and what the text is emitted from.
///
/// Named by a compiler's usual terms and not by the model's, as the tree is
/// named by the C standard's — see the glossary's preamble. Not a contract:
/// nothing outside the compiler reads it, and `dump` exists for its tests.

/// The byte a function leaves its result in, which its caller reads as
/// `NAME.__ret` — see docs/decisions/0082-a-call-writes-the-callees-bytes.md.
inline constexpr std::string_view RESULT = "__ret";

enum class Type : std::uint8_t
{
  U8,
  I8,
  U16,
  I16,
  BOOL,

  /// An address, two bytes and unsigned: what the compiler knows of what it
  /// points at is the checking pass's.
  POINTER,

  /// A `struct` or a `union`, whose bytes the checking pass counts: never the
  /// type of an operand, which is always one of its members.
  BLOCK,
};

/// The bytes a value of the type takes.
std::uint32_t sizeOf( Type type );

[[nodiscard]] bool isSigned( Type type );

/// `value` as a `type` holds it: its low bits, read with the type's sign.
/// What every fold of the compiler's wraps its answer with, wherever the fold
/// is done — see docs/decisions/0175-a-constant-reaches-its-reader.md.
[[nodiscard]] std::int64_t wrapped( std::int64_t value, Type type );

/// `u8`, `i8`, `u16`, `i16`, `bool` or `ptr`.
std::string_view spellingOf( Type type );

/// A constant an instruction reads. An operand and never an instruction, so
/// that in the text it is an immediate operand and takes no byte of its own —
/// see docs/decisions/0062-a-subset-of-c.md#findings-and-the-order-the-allocator-grows-in.
/// `value` is what the type holds: a `u8` from 0 to 255, an `i8` from -128.
struct Constant
{
  Type type = Type::U8;
  std::int64_t value = 0;

  /// The `const` it is, where it is one read in its own type, by the name it
  /// is written by: the text names it rather than its value — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
  std::string name{};

  /// For the address of a function, the Proc that follows it: the function
  /// type whose `jmp (__ptr)` a call through the pointer goes by, which the
  /// text says with `.own` — see docs/decisions/0060-own.md and
  /// docs/decisions/0065-handlers.md.
  std::string follower{};
};

/// An SSA value: the result of one instruction of its function, numbered in
/// the order the instructions define them.
struct Value
{
  std::uint32_t index = 0;
};

/// An object an instruction reads, of this Module or another, or a byte of its
/// Proc, by its name — or an element of an array at a constant index, by the
/// array's name and the element's offset, `NAME+OFFSET`. An operand as a constant is, and never loaded into a value
/// of its own — see docs/decisions/0076-arithmetic-in-the-subset.md. Read as a
/// type narrower than its own, it is its low bytes, which is what a cast that
/// narrows it keeps.
struct Object
{
  std::string name;
  Type type = Type::U8;
};

using Operand = std::variant<Constant, Value, Object>;

enum class UnaryOperator : std::uint8_t
{
  NEGATE,
  COMPLEMENT,

  /// `!`, over a `bool`.
  LOGICAL_NOT,
};

enum class BinaryOperator : std::uint8_t
{
  ADD,
  SUBTRACT,
  AND,
  OR,
  XOR,
  SHIFT_LEFT,
  SHIFT_RIGHT,
};

/// The four a comparison is lowered to: `a > b` is `b < a`, and `a <= b` is
/// `b >= a`.
enum class Comparison : std::uint8_t
{
  EQUAL,
  NOT_EQUAL,
  LESS,
  GREATER_OR_EQUAL,
};

/// `store NAME, OPERAND`: the operand written to an object of `type`, a
/// narrower operand widened by its own signedness.
struct Store
{
  std::string name;
  Type type = Type::U8;
  Operand value;
};

/// Where the bytes of a `struct` or a `union` are: at a name the text writes,
/// `NAME`, or `offset` bytes on from a pointer on the zero page — see
/// docs/decisions/0086-a-struct-by-value.md.
struct Place
{
  std::string name{};
  std::optional<Operand> pointer{};
  std::uint32_t offset = 0;
};

/// One argument of a call: the operand written to the callee's byte, `f.x`,
/// or for a `struct` or a `union` the bytes copied from `from`.
struct Argument
{
  std::string name;
  Type type = Type::U8;
  Operand value;
  std::optional<Place> from{};
  std::uint32_t bytes = 0;

  /// Where each byte goes, from the low one: `a`, `x` or `y` for a register,
  /// `m` for the next byte of `name`. Empty where every byte is `name`'s —
  /// see docs/decisions/0145-an-argument-in-a-register.md.
  std::string place{};
};

/// `call NAME(ARGUMENTS)`, or `%N = call TYPE NAME(ARGUMENTS)`: every argument
/// written to its byte, the function called by its name — the entry of an
/// assembler's `.proc` included — and, where the call is read as a value, the
/// callee's `__ret` read into one — see
/// docs/decisions/0082-a-call-writes-the-callees-bytes.md.
struct Call
{
  std::string name;
  std::vector<Argument> arguments{};
  std::optional<Value> result{};

  /// The byte the result is read from: the callee's `__ret` where empty, and
  /// for an assembler's `.proc` the one its `.declare ret` names.
  std::string returned{};

  /// The Pane the callee is in and the caller's code is not, which the call
  /// is wrapped in `.with` for; empty for a call wrapped in nothing — see
  /// docs/decisions/0096-panes-in-c.md.
  std::string with{};

  /// Where each byte of the result is, as `Argument::place` says for an
  /// argument's: `returned` holds the `m` bytes alone.
  std::string resultPlace{};
};

/// The four forms of `[[with(...)]]`, as `.with` has them — see
/// docs/decisions/0096-panes-in-c.md.
enum class WithForm : std::uint8_t
{
  /// `with(PANE)`: `.with PANE`, or a family's member `FAMILY + n` where the
  /// index is a constant.
  PANE,

  /// `with(FAMILY, i)` or `with(WINDOW, i)` with `i` a value: loaded into
  /// `X`, and `.with NAME, x`.
  AT,

  /// `with(WINDOW = STATE)`: `.with WINDOW = STATE`.
  STATE,
};

/// `with NAME[, INDEX | = STATE]`: the entry of a `[[with]]` block, whose
/// blocks follow as a region of the function — see `Function::withs`. The
/// index, where one is loaded into `X`, is read here.
struct EnterWith
{
  WithForm form = WithForm::PANE;
  std::string name;
  std::string state{};
  std::optional<Operand> index{};

  /// The name of the macro the block's statements become, used once here.
  std::string body{};

  /// AT of a family: the index counts its members, and `X` takes a state,
  /// so the family's first state is added to it. Of a Window, the index is
  /// the state itself.
  bool member = false;
};

/// `switch OPERAND, FIRST, N`: the value less FIRST, held below N, left where
/// the block's DISPATCH reads it — see
/// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
struct Switch
{
  /// The value the block's DISPATCH goes by, left clamped in `A`: below
  /// `count` where the labels cover it, and `count` itself where they do
  /// not, that entry holding `default` or the way past.
  Operand value;
  std::uint32_t count = 0;

  /// The smallest label's value, taken off the value before it indexes the
  /// table, and the type it is read in: `count` is then the span the labels
  /// cover and not the type's own — see
  /// docs/decisions/0164-a-tables-span-is-its-labels.md.
  std::int64_t first = 0;
  Type type = Type::U8;
};

/// `%N = neg TYPE OPERAND`, or `not`: in the operand's type, wrapping.
struct Unary
{
  Value result;
  UnaryOperator op = UnaryOperator::NEGATE;
  Type type = Type::U8;
  Operand operand;
};

/// `%N = add TYPE LEFT, RIGHT`, and the rest: in the operands' type,
/// wrapping. A shift's count is a constant.
struct Binary
{
  Value result;
  BinaryOperator op = BinaryOperator::ADD;
  Type type = Type::U8;
  Operand left;
  Operand right;
};

/// `%N = lt TYPE LEFT, RIGHT`, and the rest: a `bool`, the operands compared
/// in `type`, signed where it is.
struct Compare
{
  Value result;
  Comparison op = Comparison::EQUAL;
  Type type = Type::U8;
  Operand left;
  Operand right;
};

/// `%N = zext TYPE OPERAND`, `sext`, `trunc` or `bits`: the operand in another
/// type, widened by its own signedness, narrowed to its low bytes, or its bits
/// read with another signedness — what an operator over two widths and a cast
/// do, as docs/decisions/0084-widths-data-arrays-pointers-aggregates.md has
/// them.
struct Convert
{
  Value result;
  Type type = Type::U8;
  Operand operand;
};

/// `%N = load TYPE NAME[INDEX]`: an element of an array at a static address,
/// by a `u8` index `X` holds — twice the index for a 16-bit element — see
/// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#arrays--task-3c.
/// An element at a constant index is an object, `NAME+OFFSET`, and no load.
struct Load
{
  Value result;
  Type type = Type::U8;
  std::string name;
  Operand index;

  /// An index already counted in bytes, an element of a `struct` being more
  /// than one or two.
  bool scaled = false;

  /// For a 16-bit value in a striped array, the name its high bytes lie at.
  std::string high{};
};

/// `store NAME[INDEX], VALUE`: a value of the element's type written to an
/// element, as `Load` reaches one.
struct StoreElement
{
  std::string name;
  Type type = Type::U8;
  Operand index;
  Operand value;
  bool scaled = false;
  std::string high{};
};

/// `%N = load TYPE (POINTER),INDEX`: what a pointer on the zero page points
/// at, `INDEX` bytes on, read through `(zp),y` with the index in `Y` — see
/// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#pointers--task-3d.
/// A 16-bit value's high byte is one further on.
struct LoadIndirect
{
  Value result;
  Type type = Type::U8;
  Operand pointer;
  Operand index;

  /// Through a pointer to `volatile`: kept, and read where it stands, however
  /// its value is used — see docs/decisions/0151-volatile.md.
  bool isVolatile = false;
};

/// `store (POINTER),INDEX, VALUE`: a value written where `LoadIndirect` reads.
struct StoreIndirect
{
  Type type = Type::U8;
  Operand pointer;
  Operand index;
  Operand value;

  /// Through a pointer to `volatile`.
  bool isVolatile = false;
};

/// `copy N, FROM -> TO`: the bytes of a `struct` or a `union` copied, a store
/// per byte for four or fewer and a loop over an index for more.
struct Copy
{
  Place from;
  Place to;
  std::uint32_t bytes = 0;
};

struct Instruction
{
  std::variant<Store,
               Call,
               Unary,
               Binary,
               Compare,
               Switch,
               Convert,
               Load,
               StoreElement,
               LoadIndirect,
               StoreIndirect,
               Copy,
               EnterWith>
      operation;

  /// The statement of the C it came from, whose line the text's `.source`
  /// names. The instructions of one statement share it.
  diag::SourceLocation at;
};

enum class TerminatorKind : std::uint8_t
{
  /// `ret`: back to the caller.
  RETURN,

  /// `enter PHASE`: the Phase named is entered and nothing returns — see
  /// docs/decisions/0064-phase-in-c.md.
  TRANSITION,

  /// `jump blockN`.
  JUMP,

  /// `branch CONDITION, blockN, blockM`: to the first where the `bool` holds,
  /// and to the second where it does not.
  BRANCH,

  /// `fall`: on into the Proc chained after this one by `then`.
  FALL,

  /// `dispatch VALUE, blockN, blockM, ...`: to the block the value picks,
  /// which the code before it has held below their number. Written as the
  /// assembler's `.dispatch`, whose targets are positions of the Proc itself
  /// — see docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
  DISPATCH,
};

/// What ends a block, and the only way out of it.
struct Terminator
{
  TerminatorKind kind = TerminatorKind::RETURN;

  /// The statement whose flow it is: the `return`, the closing brace of a body
  /// that falls off its end, or the `if` or loop it goes around.
  diag::SourceLocation at;

  /// Where a JUMP goes, and where a BRANCH goes when its condition holds, by
  /// the block's index.
  std::uint32_t target = 0;

  /// Where a BRANCH goes when its condition does not hold.
  std::uint32_t otherwise = 0;

  /// A BRANCH's `bool`.
  Operand condition{};

  /// A TRANSITION's Phase, as it was written: the text carries the name and
  /// the assembler resolves it.
  std::string phase{};

  /// Where a DISPATCH goes, by block index, one entry per value from zero;
  /// a block may stand more than once, which is how the holes of the table
  /// and `default` are written. Its value is `condition`.
  std::vector<std::uint32_t> targets{};
};

struct Block
{
  std::vector<Instruction> instructions;
  Terminator terminator;
};

/// The blocks control may reach from this terminator, in the order it names
/// them. Nothing for `ret`, `enter` and `fall`: what the Proc holds does not
/// outlive the call, and a Proc chained by `then` is another Function.
std::vector<std::uint32_t> successorsOf( Terminator const& terminator );

/// A byte of a Proc's own: a local of the C, or one the compiler needed —
/// see docs/decisions/0080-a-local-is-a-byte-of-its-proc.md.
struct Local
{
  std::string name;
  Type type = Type::U8;

  /// The bytes of a `block`: a `struct` or a `union`.
  std::uint32_t bytes = 0;

  /// For a parameter, the register it comes in, `a` or `x`, where the body
  /// reads it there first and nowhere else, so that it has no byte at all;
  /// empty for one in its byte — see
  /// docs/decisions/0145-an-argument-in-a-register.md.
  std::string place{};

  /// Where the byte lies: the innermost `[[placement]]` over its declaration,
  /// and the zero page where nothing said — see
  /// docs/decisions/0210-placement-is-declared-in-c-too.md. A block of more
  /// than a few bytes is written off the zero page whatever this says, since
  /// an instruction reaches none of it in two bytes.
  model::PlacementClass placement = model::PlacementClass::ZEROPAGE;
};

/// A `const` given a constant: a Constant of the assembler, taking no byte.
struct NamedConstant
{
  std::string name;
  Constant value;
  bool isStatic = false;
  diag::SourceLocation at;
};

/// An object at file scope, or one of a function's own in a Namespace of its
/// name: `.res`, or the value it is given, or the elements of an array.
struct Global
{
  std::string name;
  Type type = Type::U8;
  bool isStatic = false;
  diag::SourceLocation at;
  std::optional<Constant> value{};

  /// An array's number of elements of `type`, and what it is given, as many
  /// as it has, or none.
  std::optional<std::uint32_t> count{};
  std::vector<Constant> elements{};

  /// A local array, whose bytes are its function's only while it runs.
  bool isTemporary = false;

  /// What the source declared `const`, with the value kept in the Section
  /// rather than written into it at run time: the emitter says `readonly` over
  /// it, so that the solver may put it in a `rom` Region even where its address
  /// is taken — see
  /// docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
  bool isConst = false;

  /// For an object of a function, the name the program wrote, where `name` is
  /// the one the compiler made of it. A finding says this one: nothing outside
  /// this file has ever seen the other, and a diagnostic that un-mangled a name
  /// would be a second place that knows how one is spelled.
  std::string written{};

  /// Where the object lies: `absolute` unless `[[placement(zeropage)]]` asked
  /// otherwise, and the zero page whatever is written for a pointer, which
  /// `(zp),y` reads through — see
  /// docs/decisions/0210-placement-is-declared-in-c-too.md.
  model::PlacementClass placement = model::PlacementClass::ABSOLUTE;

  /// A striped array's stripes, a byte of each element to a stripe: the names
  /// its Label lies under in the array's Namespace, and what it is given —
  /// see docs/decisions/0089-a-stripe-is-a-section.md.
  std::vector<std::pair<std::vector<std::string>, std::vector<Constant>>> stripes{};

  /// The Pane its Section is `in`, or empty — see
  /// docs/decisions/0096-panes-in-c.md.
  std::string pane{};

  /// The Slot whose Cell holds this object's address wherever the Module is
  /// present, or empty — see docs/decisions/0031-slots.md.
  std::string implements{};
};

/// A Slot this unit declares: a Symbol with at most one live definition per
/// Phase, reached through a Cell the Transition routine rewrites — see
/// docs/decisions/0031-slots.md and
/// docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md. `isVector` is
/// the Binding: a `jmp` and an address for a function, an address alone for
/// data, which then lies on the zero page because `(),y` needs it there.
struct Slot
{
  std::string name;
  bool isVector = false;
  diag::SourceLocation at;

  /// What a Slot that takes or returns names in a Namespace of its own name:
  /// the Temporaries every Implementation reads and every caller writes,
  /// there being no Proc to hold them, the Cell's own `jmp` being the
  /// dispatch — see docs/decisions/0174-a-slot-takes-and-returns.md.
  std::vector<Local> parameters{};
  std::optional<Type> result{};
  std::uint32_t resultBytes = 0;
};

/// The blocks of one `[[with]]` block, `[begin, end)` among the function's,
/// written as a macro used once under `.with` where the `EnterWith` that
/// ends block `begin - 1` stands — see docs/decisions/0096-panes-in-c.md.
struct WithRegion
{
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
};

struct Function
{
  std::string name;
  bool isStatic = false;
  diag::SourceLocation at;

  /// Written `inline`: every call of it the program's C writes is wrapped
  /// into its caller, and a shape that cannot be is a finding of its own —
  /// see docs/decisions/0172-a-call-with-one-site-is-wrapped.md.
  bool isInline = false;
  diag::SourceSpan inlineSpan{};

  /// Its parameters in the order C lists them, each a byte of the Proc named
  /// by its C name, and the type of its result where it returns one.
  std::vector<Local> parameters{};
  std::optional<Type> result{};
  std::uint32_t resultBytes = 0;

  /// The parameter whose byte is also the result's, or empty where the result
  /// has `__ret` of its own: the Proc then declares that one reservation both
  /// an argument and the result, and every caller reads the result from it —
  /// see docs/decisions/0120-a-function-returns-through-a-parameter.md.
  std::string resultByte{};

  /// Where the result's bytes are left, as a `.declare ret` spells it: `a`,
  /// or `ma` for a pair whose high byte is in `A` at the `rts` — or empty for
  /// `__ret` — see docs/decisions/0145-an-argument-in-a-register.md.
  std::string resultPlace{};

  /// Where the bytes the compiler takes for itself lie: the function's own
  /// `[[placement]]`, since scratch has no name for anything finer to reach —
  /// see docs/decisions/0210-placement-is-declared-in-c-too.md.
  model::PlacementClass placement = model::PlacementClass::ZEROPAGE;

  /// The bytes it holds, written as `.ztemp` after its last instruction.
  std::vector<Local> locals{};

  /// Its `const` locals given a constant, written as Constants of the Proc.
  std::vector<NamedConstant> constants{};

  /// Its objects that are no bytes of the Proc — `static` locals and local
  /// arrays — written in a Namespace of its name after it.
  std::vector<Global> sections{};

  /// The type of every value the instructions define, by the value's index.
  std::vector<Type> values;

  /// The first is where a call enters.
  std::vector<Block> blocks;

  /// The Proc chained after this one by `then`, or empty.
  std::string then{};

  /// The Pane its Proc is `in`, or empty: the function's own, which the
  /// Procs of its `switch`es share — see docs/decisions/0096-panes-in-c.md.
  std::string pane{};

  /// The Pane its Proc runs `under`, or empty: a function written
  /// `[[under]]`, or the Proc a `[[with(...), trampoline]]` block became — see
  /// docs/decisions/0098-a-proc-declares-what-is-shown.md.
  std::string under{};

  /// The Slot whose Cell holds this Proc's entry wherever the Module is
  /// present, or empty — see docs/decisions/0031-slots.md.
  std::string implements{};

  /// The function type whose Temporaries this Proc's arguments and result
  /// are, or empty: a member declares none of its own, whoever calls it — see
  /// docs/decisions/0065-handlers.md.
  std::string memberOf{};

  /// A function type's own Proc: `__ptr` and the parameters as Temporaries,
  /// and one `jmp (__ptr)`, which is where a call through a pointer of the
  /// type goes and how it reaches the member — see
  /// docs/decisions/0065-handlers.md. It has no body of its own.
  bool isTrampoline = false;

  /// Its `[[with]]` blocks, in the order their regions begin.
  std::vector<WithRegion> withs{};
};

/// An `enum struct`: its name, and its enumerators numbered by their place.
struct Enumeration
{
  std::string name;
  std::vector<std::string> enumerators;
  diag::SourceLocation at;

  /// Written `enum struct`: its enumerators are Constants of a Namespace of
  /// its name. An `enum` writes them at the file's own scope instead — see
  /// docs/decisions/0162-an-enum-without-struct.md.
  bool isScoped = true;
};

/// A `struct` or a `union`: its members' offsets, as Constants of a Namespace
/// of its name that the assembler reads.
struct Aggregate
{
  std::string name;
  std::vector<std::pair<std::string, std::uint32_t>> members;
  diag::SourceLocation at;
};

using Definition = std::variant<Global, Function, Enumeration, NamedConstant, Aggregate, Slot>;

/// One `.ngc` file: what it defines, in the order written.
struct Unit
{
  std::vector<Definition> definitions;

  /// The names whose bytes change or act behind the program's back — a
  /// constant pointer aimed at a register of the Target, or a name declared
  /// `volatile` — every read and write of which is one read or write of the
  /// text, in the order the source has them: nothing holds them in a
  /// register, and no pass moves, merges or drops an access to them. See
  /// docs/decisions/0151-volatile.md.
  std::set<std::string> volatiles{};
};

/// Every operand of an instruction, so that a pass rewriting operands touches
/// each exactly once. What ends a block has one operand of its own, a BRANCH's
/// condition, which a pass visits beside these.
template <typename Visitor>
void eachOperand( Instruction& instruction, Visitor visit )
{
  std::visit(
      [&visit]( auto& operation )
      {
        using Operation = std::decay_t<decltype( operation )>;
        if constexpr ( std::is_same_v<Operation, Store> || std::is_same_v<Operation, Switch> )
        {
          visit( operation.value );
        }
        else if constexpr ( std::is_same_v<Operation, Unary> || std::is_same_v<Operation, Convert> )
        {
          visit( operation.operand );
        }
        else if constexpr ( std::is_same_v<Operation, Binary> || std::is_same_v<Operation, Compare> )
        {
          visit( operation.left );
          visit( operation.right );
        }
        else if constexpr ( std::is_same_v<Operation, Load> )
        {
          visit( operation.index );
        }
        else if constexpr ( std::is_same_v<Operation, StoreElement> )
        {
          visit( operation.index );
          visit( operation.value );
        }
        else if constexpr ( std::is_same_v<Operation, LoadIndirect> )
        {
          visit( operation.pointer );
          visit( operation.index );
        }
        else if constexpr ( std::is_same_v<Operation, StoreIndirect> )
        {
          visit( operation.pointer );
          visit( operation.index );
          visit( operation.value );
        }
        else if constexpr ( std::is_same_v<Operation, Call> )
        {
          for ( Argument& argument : operation.arguments )
          {
            visit( argument.value );
          }
        }
        else if constexpr ( std::is_same_v<Operation, EnterWith> )
        {
          if ( operation.index.has_value() )
          {
            visit( *operation.index );
          }
        }
      },
      instruction.operation );
}

/// Every name of an object an instruction holds, each once, as the string the
/// instruction keeps it in: a store's, an operand read as an object, an
/// array's, either end of a copy. The callee of a call is not one of them, and
/// neither is the byte its result is read from: those are a Proc's names and
/// not this function's. What a pass renaming objects walks, as `eachOperand`
/// is what one rewriting operands walks.
template <typename Visit>
void eachName( Operand& operand, Visit const& visit )
{
  if ( auto* const object = std::get_if<Object>( &operand ) )
  {
    visit( object->name );
  }
}

template <typename Visit>
void eachName( Place& place, Visit const& visit )
{
  if ( !place.name.empty() )
  {
    visit( place.name );
  }
  if ( place.pointer.has_value() )
  {
    eachName( *place.pointer, visit );
  }
}

template <typename Visit>
void eachName( Instruction& instruction, Visit const& visit )
{
  std::visit(
      [&visit]( auto& operation )
      {
        using Operation = std::decay_t<decltype( operation )>;
        if constexpr ( std::is_same_v<Operation, Store> )
        {
          visit( operation.name );
          eachName( operation.value, visit );
        }
        else if constexpr ( std::is_same_v<Operation, Unary> || std::is_same_v<Operation, Convert> )
        {
          eachName( operation.operand, visit );
        }
        else if constexpr ( std::is_same_v<Operation, Binary> || std::is_same_v<Operation, Compare> )
        {
          eachName( operation.left, visit );
          eachName( operation.right, visit );
        }
        else if constexpr ( std::is_same_v<Operation, Load> )
        {
          visit( operation.name );
          eachName( operation.index, visit );
        }
        else if constexpr ( std::is_same_v<Operation, StoreElement> )
        {
          visit( operation.name );
          eachName( operation.index, visit );
          eachName( operation.value, visit );
        }
        else if constexpr ( std::is_same_v<Operation, LoadIndirect> )
        {
          eachName( operation.pointer, visit );
          eachName( operation.index, visit );
        }
        else if constexpr ( std::is_same_v<Operation, StoreIndirect> )
        {
          eachName( operation.pointer, visit );
          eachName( operation.index, visit );
          eachName( operation.value, visit );
        }
        else if constexpr ( std::is_same_v<Operation, Copy> )
        {
          eachName( operation.from, visit );
          eachName( operation.to, visit );
        }
        else if constexpr ( std::is_same_v<Operation, Call> )
        {
          for ( Argument& argument : operation.arguments )
          {
            eachName( argument.value, visit );
            if ( argument.from.has_value() )
            {
              eachName( *argument.from, visit );
            }
          }
        }
        else if constexpr ( std::is_same_v<Operation, Switch> )
        {
          eachName( operation.value, visit );
        }
        else if constexpr ( std::is_same_v<Operation, EnterWith> )
        {
          if ( operation.index.has_value() )
          {
            eachName( *operation.index, visit );
          }
        }
      },
      instruction.operation );
}

/// The type an operand has in `function`.
[[nodiscard]] Type typeOf( Operand const& operand, Function const& function );

/// The value an instruction defines, or nothing for one that defines none.
[[nodiscard]] std::optional<Value> resultOf( Instruction const& instruction );

/// The unit as text for a reader, an instruction a line — what the compiler's
/// tests compare. It carries no source position: the `.source` marks of the
/// text the unit is emitted as are held to those.
std::string dump( Unit const& unit );

} // namespace nga::c::ir
