#include "nga/c/Compile.hpp"

#include "nga/c/Assigned.hpp"

#include "nga/c/Fold.hpp"
#include "nga/c/Hoist.hpp"
#include "nga/c/Inline.hpp"
#include "nga/c/Liveness.hpp"
#include "nga/c/Loops.hpp"
#include "nga/c/Narrow.hpp"
#include "nga/c/Ranges.hpp"
#include "nga/c/Result.hpp"
#include "nga/c/Reuse.hpp"
#include "nga/c/Select.hpp"
#include "nga/c/Walk.hpp"

#include "nga/c/Lexer.hpp"
#include "nga/c/Parser.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nga::c
{

namespace
{

/// Where an instruction stands: indented, since a name at column one defines
/// a label.
constexpr std::string_view INDENT = "        ";

/// A function's result byte, which no name of C can be — see
/// docs/decisions/0082-a-call-writes-the-callees-bytes.md.
using ir::RESULT;

/// The Temporary of a function type that holds where a call through a pointer
/// of it goes — see docs/decisions/0065-handlers.md.
constexpr std::string_view TRAMPOLINE_POINTER = "__ptr";

/// The enumerators an `enum struct` holds at most, which is what a byte
/// numbers.
constexpr std::size_t MAX_ENUMERATORS = 256;

/// The enumerators up to which a dispatcher tests the value in a chain, and
/// past which it jumps through a table — see
/// docs/decisions/0078-switch-over-an-enum-struct.md#chain-or-table.
constexpr std::size_t MAX_CHAIN = 6;

/// How many labels a `switch` may hold and still be written where it stands,
/// a compare and a branch each, rather than as Procs of its own — see
/// docs/decisions/0164-a-small-switch-stands-where-it-is.md.
constexpr std::size_t MAX_IN_PLACE = MAX_CHAIN;

/// A table reaches 256 entries, an index being a byte.
constexpr std::int64_t MAX_TABLE = 256;

/// What shape a `switch` takes: written where it stands, or a dispatcher
/// whose table spans its labels — see
/// docs/decisions/0164-a-tables-span-is-its-labels.md.
struct SwitchForm
{
  bool inPlace = true;

  /// The smallest label's value, and how many values its labels span.
  std::int64_t first = 0;
  std::int64_t span = 0;
};

/// What shape a `switch` of these label values takes: written where it
/// stands where the labels are few, or where they lie too far apart for a
/// table of 256; a dispatcher whose table spans them otherwise.
[[nodiscard]] SwitchForm formOf( std::vector<std::int64_t> const& values )
{
  SwitchForm form;
  if ( values.size() <= MAX_IN_PLACE )
  {
    return form;
  }
  auto const [low, high] = std::ranges::minmax( values );
  form.first = low;
  form.span = high - low + 1;
  form.inPlace = form.span > MAX_TABLE;
  return form;
}

/// A table's entries to a line of the text.
constexpr std::size_t TABLE_LINE = 8;

/// What a name stands for, as far as a statement asks.
enum class NameKind : std::uint8_t
{
  FUNCTION,
  OBJECT,
  TYPE,
  ASSEMBLER_PROC,
  ASSEMBLER_OTHER,
};

/// What a pointer points at: a type, the `enum struct` it is where it is
/// one, and whether it is `const` or `volatile` — see
/// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#pointers--task-3d.
struct Pointee
{
  ir::Type type = ir::Type::U8;
  std::optional<std::uint32_t> enumeration{};
  bool isConst = false;
  std::optional<std::uint32_t> aggregate{};

  /// The function type pointed at, among the program's: a pointer to one is
  /// the only pointer to a function the subset has — see
  /// docs/decisions/0065-handlers.md and
  /// docs/decisions/0103-what-building-handlers-needs.md.
  std::optional<std::uint32_t> function{};

  /// Every read and store through the pointer is one in the text — see
  /// docs/decisions/0151-volatile.md.
  bool isVolatile = false;
};

/// Whether two pointers point at one type, whatever either says of `const`
/// or `volatile`.
bool samePointee( Pointee const& left, Pointee const& right )
{
  return left.type == right.type && left.enumeration == right.enumeration && left.aggregate == right.aggregate &&
         left.function == right.function;
}

/// A type as a signature spells it: a keyword's, or an `enum struct`'s,
/// whose name is resolved once every file has been gathered; or a pointer to
/// one, whose type is then `ptr`.
struct NamedType
{
  ir::Type type = ir::Type::U8;
  std::optional<std::uint32_t> enumeration{};
  std::optional<Token> typeName{};
  std::optional<Pointee> pointee{};

  /// The function type a value would be of, which nothing may be: it is a
  /// type only as a pointer — see docs/decisions/0103-what-building-handlers-needs.md.
  std::optional<std::uint32_t> function{};

  /// The `struct` or `union` a value is of, whose type is then `block`.
  std::optional<std::uint32_t> aggregate{};
};

struct ParameterType
{
  Token name;
  NamedType type;

  /// Whether a name was written, which a declaration after `extern` may leave
  /// out.
  bool isNamed = true;
};

struct Definition
{
  NameKind kind = NameKind::FUNCTION;
  bool isStatic = false;
  Token name;

  /// An object's type; a function and a type hold `u8` here.
  ir::Type type = ir::Type::U8;

  /// The `enum struct` a TYPE is, and the one an OBJECT is of.
  std::optional<std::uint32_t> enumeration;

  /// An OBJECT's type where it is a name, until it is resolved.
  std::optional<Token> typeName;

  /// A FUNCTION's result, or nothing for `void`, and its parameters in the
  /// order written.
  std::optional<NamedType> result{};
  std::vector<ParameterType> parameters{};

  /// An OBJECT written `const`, and the value an OBJECT is given where it is
  /// declared.
  bool isConst = false;
  Expression const* value = nullptr;

  /// An OBJECT that is an array: its size as written, and its number of
  /// elements where the list it is given decides it.
  bool isArray = false;
  Expression const* size = nullptr;
  std::optional<std::uint32_t> count{};

  /// What an OBJECT that is a pointer points at; its `type` is then `ptr`.
  std::optional<Pointee> pointee{};

  /// The `struct` or `union` a TYPE is, and the one an OBJECT is of.
  std::optional<std::uint32_t> aggregate{};

  /// The function type a TYPE names, whose parameters and result are this
  /// Definition's; a type only as a pointer — see
  /// docs/decisions/0065-handlers.md.
  std::optional<std::uint32_t> function{};

  /// An array written `[[striped]]`, laid out a member per table — see
  /// docs/decisions/0089-a-stripe-is-a-section.md.
  bool isStriped = false;

  /// An OBJECT written `extern`: how C reads a name the assembler exports —
  /// see docs/decisions/0093-extern-declares-how-c-reads-the-assembler.md.
  bool isExtern = false;

  /// The Pane a FUNCTION or an OBJECT at file scope is written `[[in]]`, or
  /// empty — see docs/decisions/0096-panes-in-c.md. What the attribute names
  /// is checked where the declaration is; here it is the text.
  std::string pane{};

  /// The Pane a FUNCTION is written `[[under]]` and runs with shown, or
  /// empty — see docs/decisions/0098-a-proc-declares-what-is-shown.md.
  std::string under{};

  /// An `extern` written `[[slot]]`: a Slot this program declares, whose Cell
  /// the Transition routine rewrites on every edge — see
  /// docs/decisions/0064-phase-in-c.md and
  /// docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md.
  bool isSlot = false;

  /// The Slot a definition is written `[[implements(NAME)]]` for, or empty:
  /// its Label is what the Cell holds wherever this Module is present.
  std::string implements{};

  /// An OBJECT written `volatile`, itself and not what it points at — see
  /// docs/decisions/0151-volatile.md.
  bool isVolatile = false;

  /// An enumerator of an `enum` written without `struct`, by its place in
  /// the type, which is its value: the name is the file's own, and reads as
  /// a `const` of the type — see
  /// docs/decisions/0162-an-enum-without-struct.md.
  std::optional<std::uint32_t> enumerator{};
};

/// An `enum struct` of the program, numbered where it was gathered.
struct EnumerationType
{
  std::string name;
  std::vector<std::string> enumerators;
};

/// A member of a `struct` or a `union`: its type, an array of it where its
/// declarator is one, and its offset once its type is laid out.
struct MemberType
{
  Token name;
  NamedType type;
  bool isArray = false;
  Expression const* size = nullptr;
  std::uint32_t count = 1;
  std::uint32_t offset = 0;
};

/// A `struct` or a `union` of the program — see
/// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
/// Its members lie one after another with no padding, the machine having no
/// alignment, and a union's all at its start.
struct AggregateType
{
  std::string name;
  bool isUnion = false;
  std::vector<MemberType> members;
  std::uint32_t size = 0;
};

/// A function type the program names: the Proc a pointer to it goes through,
/// whose Temporaries hold the arguments of every call — see
/// docs/decisions/0065-handlers.md.
struct FunctionType
{
  std::string name;
  std::optional<NamedType> result{};
  std::vector<ParameterType> parameters{};
};

/// The types the program declares: its `enum struct`s, `struct`s and `union`s,
/// and the function types it names.
struct Types
{
  std::vector<EnumerationType> enumerations;
  std::vector<AggregateType> aggregates;
  std::vector<FunctionType> functions;
};

/// Which function type each function taken as one belongs to, gathered over
/// the whole program while the bodies are checked and read when they are
/// emitted: a member reads the type's Temporaries and not its own, and a
/// direct call to it writes them — see docs/decisions/0065-handlers.md. Keyed
/// by the Definition for a function of C and by the name for a Proc of the
/// assembler, which declares its membership with `as`.
struct Membership
{
  std::map<Definition const*, std::uint32_t> ofFunction;
  std::map<std::string, std::uint32_t, std::less<>> ofProc;

  /// Where each was taken first, for the finding about a second type.
  std::map<Definition const*, Token> takenAt;

  /// A call through a pointer of a function type, by the function it stands
  /// in: a member calling its own type would write the Temporaries it is
  /// still reading, which waits for the copy of 0065 — see
  /// docs/decisions/0103-what-building-handlers-needs.md.
  struct CallOfType
  {
    Definition const* caller = nullptr;
    std::uint32_t type = 0;
    diag::SourceSpan span;
    diag::DiagnosticSink* sink = nullptr;
  };

  std::vector<CallOfType> callsOfType;
};

/// One file on its way through the passes.
struct Unit
{
  SourceFile const* source = nullptr;
  TranslationUnit tree;

  /// The file's own findings, so that whether it holds an error is a question
  /// about it alone.
  diag::DiagnosticSink* sink = nullptr;

  /// What the file defines, by spelling. An ordered map, so that nothing that
  /// walks it depends on hashing.
  std::map<std::string, Definition, std::less<>> definitions;
};

/// What a name means where a statement uses it.
struct Meaning
{
  NameKind kind = NameKind::FUNCTION;
  ir::Type type = ir::Type::U8;
  std::optional<std::uint32_t> enumeration;

  /// A local's byte, and the Proc that holds it; empty for everything named
  /// by its own spelling — see
  /// docs/decisions/0080-a-local-is-a-byte-of-its-proc.md.
  std::string byte;
  std::string owner;

  /// A FUNCTION's definition, whose signature a call is held to and which
  /// stands for it in the call graph.
  Definition const* definition = nullptr;

  /// An OBJECT written `const`, and, where it was given a constant, its value:
  /// what it is read as, since it takes no byte.
  bool isConst = false;
  std::optional<std::int64_t> value{};

  /// An OBJECT that is an array of `type`, and its number of elements where it
  /// is settled.
  bool isArray = false;
  std::optional<std::uint32_t> count{};

  /// What an OBJECT that is a pointer points at.
  std::optional<Pointee> pointee{};

  /// A parameter of the function being read, and where a local was declared,
  /// which names it among those whose address is taken.
  bool isParameter = false;
  std::optional<diag::SourceLocation> declaredAt{};

  /// The `struct` or `union` a TYPE is, and the one an OBJECT is of.
  std::optional<std::uint32_t> aggregate{};

  /// The function type a TYPE names, which is a type only as a pointer — see
  /// docs/decisions/0065-handlers.md.
  std::optional<std::uint32_t> function{};

  /// An array written `[[striped]]`, laid out a member per table — see
  /// docs/decisions/0089-a-stripe-is-a-section.md.
  bool isStriped = false;

  /// What an `.asm` Module exports, for a name of the assembler's — see
  /// docs/decisions/0092-what-the-assembler-exports-is-typed-by-its-shape.md.
  ExternalName const* external = nullptr;

  /// An OBJECT written `volatile`, itself and not what it points at.
  bool isVolatile = false;
};

/// Whether a name is a Constant of the assembler that folds to no number C
/// knows: a `u16` written by its name.
bool isOpaqueConstant( Meaning const& meaning )
{
  return meaning.external != nullptr && meaning.external->kind == ExternalKind::CONSTANT &&
         !meaning.external->value.has_value();
}

/// Whether a name is a Constant of the assembler that folds, which is a
/// constant of C as a literal is.
bool isFoldedConstant( Meaning const& meaning )
{
  return meaning.definition == nullptr && meaning.external != nullptr &&
         meaning.external->kind == ExternalKind::CONSTANT && meaning.external->value.has_value();
}

/// Whether a name is a Constant of the assembler, typed by its shape or
/// declared: a value, and never a place.
bool isAssemblerConstant( Meaning const& meaning )
{
  return meaning.external != nullptr && meaning.external->kind == ExternalKind::CONSTANT;
}

/// Whether an object lies at an address the assembler knows: one at file
/// scope, of the program or of the assembler.
bool atFixedAddress( Meaning const& meaning )
{
  return meaning.definition != nullptr || meaning.external != nullptr;
}

ir::Type typeNamed( Keyword keyword )
{
  switch ( keyword )
  {
  case Keyword::I8:
    return ir::Type::I8;
  case Keyword::U16:
    return ir::Type::U16;
  case Keyword::I16:
    return ir::Type::I16;
  case Keyword::BOOL:
    return ir::Type::BOOL;
  default:
    return ir::Type::U8;
  }
}

NamedType namedType( Token const& token )
{
  if ( token.kind == TokenKind::IDENTIFIER )
  {
    return NamedType{ .type = ir::Type::U8, .enumeration = std::nullopt, .typeName = token };
  }
  return NamedType{ .type = typeNamed( token.keyword ), .enumeration = std::nullopt, .typeName = std::nullopt };
}

/// A type as a signature spells it, or a pointer to it where `pointer` says.
NamedType namedType( Token const& token, bool pointer, bool pointeeConst, bool pointeeVolatile )
{
  NamedType named = namedType( token );
  if ( pointer )
  {
    named.pointee = Pointee{ .type = named.type, .enumeration = std::nullopt, .isConst = pointeeConst };
    named.pointee->isVolatile = pointeeVolatile;
    named.type = ir::Type::POINTER;
  }
  return named;
}

/// The definition gathered for a function of `unit`, which is the one of its
/// name unless the name was defined twice, which has been reported.
Definition const*
definitionOf( diag::SourceManager const& sources, Unit const& unit, FunctionDefinition const& function )
{
  auto const found = unit.definitions.find( sources.textOf( function.name.span() ) );
  if ( found == unit.definitions.end() || found->second.name.location != function.name.location )
  {
    return nullptr;
  }
  return &found->second;
}

std::int64_t lowestOf( ir::Type type )
{
  if ( !ir::isSigned( type ) )
  {
    return 0;
  }
  return -( std::int64_t{ 1 } << ( ( 8 * ir::sizeOf( type ) ) - 1 ) );
}

std::int64_t highestOf( ir::Type type )
{
  if ( type == ir::Type::BOOL )
  {
    return 1;
  }
  std::int64_t const span = std::int64_t{ 1 } << ( 8 * ir::sizeOf( type ) );
  return ir::isSigned( type ) ? ( span / 2 ) - 1 : span - 1;
}

using ir::wrapped;

bool isShift( TokenKind kind )
{
  return kind == TokenKind::LESS_LESS || kind == TokenKind::GREATER_GREATER;
}

bool isComparison( TokenKind kind )
{
  return kind == TokenKind::LESS || kind == TokenKind::GREATER || kind == TokenKind::LESS_EQUAL ||
         kind == TokenKind::GREATER_EQUAL || kind == TokenKind::EQUAL_EQUAL || kind == TokenKind::BANG_EQUAL;
}

bool isLogical( TokenKind kind )
{
  return kind == TokenKind::AMPERSAND_AMPERSAND || kind == TokenKind::PIPE_PIPE;
}

/// What a name among constants stands for: the value of a `const`, or the
/// number of an enumerator.
using NameValue = std::function<std::int64_t( Expression const& )>;

/// The value of an expression of constants, folded exactly. Nothing here
/// wraps: what wraps is what the machine computes, and a constant is computed
/// by a compiler, which has no width — the type has only to hold the answer,
/// which is checked where the constant is used — see
/// docs/decisions/0206-a-constant-is-folded-exactly.md, which narrows
/// docs/decisions/0076-arithmetic-in-the-subset.md. `type` is what `~` is
/// taken in, it being the one operator with no answer without a width; where
/// it is nothing, `~` flips 64 bits. A name is `valueOf`'s.
std::int64_t folded( Expression const& node, std::optional<ir::Type> type, NameValue const& valueOf )
{
  if ( node.kind == ExpressionKind::INTEGER_CONSTANT || node.kind == ExpressionKind::CHARACTER_CONSTANT ||
       node.kind == ExpressionKind::PREDEFINED_CONSTANT )
  {
    return node.value;
  }
  if ( node.kind == ExpressionKind::IDENTIFIER || node.kind == ExpressionKind::QUALIFIED_NAME ||
       node.kind == ExpressionKind::SIZEOF )
  {
    return valueOf( node );
  }
  if ( node.kind == ExpressionKind::CAST )
  {
    // A cast is the one place a constant changes width: what is cast is folded
    // exactly and then held in the type cast to, as C converts a constant.
    ir::Type const cast = node.pointer ? ir::Type::POINTER : typeNamed( node.token.keyword );
    return wrapped( folded( *node.left, std::nullopt, valueOf ), cast );
  }
  if ( node.kind == ExpressionKind::UNARY )
  {
    auto const operand = static_cast<std::uint64_t>( folded( *node.left, type, valueOf ) );
    if ( node.token.kind == TokenKind::BANG )
    {
      return operand == 0 ? 1 : 0;
    }
    if ( node.token.kind == TokenKind::MINUS )
    {
      return static_cast<std::int64_t>( 0 - operand );
    }
    // `~1` is 254 in a `u8` and 65534 in a `u16`: the bits flipped are the
    // type's, which is why `~` reads the type where every other operator does
    // not.
    auto const flipped = static_cast<std::int64_t>( ~operand );
    return type.has_value() ? wrapped( flipped, *type ) : flipped;
  }
  if ( node.kind != ExpressionKind::BINARY )
  {
    return 0;
  }

  std::int64_t const left = folded( *node.left, type, valueOf );
  auto const l = static_cast<std::uint64_t>( left );
  if ( isShift( node.token.kind ) )
  {
    // A count past the fold's own width shifts everything out, which is what a
    // count this large means; the typed case is refused before it is folded.
    std::int64_t const count = folded( *node.right, std::nullopt, valueOf );
    bool const past = count < 0 || count > 62;
    if ( node.token.kind == TokenKind::LESS_LESS )
    {
      return past ? 0 : static_cast<std::int64_t>( l << count );
    }
    if ( past )
    {
      return left < 0 ? -1 : 0;
    }
    return left >> count;
  }

  std::int64_t const right = folded( *node.right, type, valueOf );
  auto const r = static_cast<std::uint64_t>( right );
  switch ( node.token.kind )
  {
  case TokenKind::STAR:
    return static_cast<std::int64_t>( l * r );
  case TokenKind::SLASH:
    // Truncated towards zero, as C divides; by zero is refused before it is
    // folded.
    return right == 0 ? 0 : static_cast<std::int64_t>( left / right );
  case TokenKind::PERCENT:
    return right == 0 ? 0 : static_cast<std::int64_t>( left % right );
  case TokenKind::PLUS:
    return static_cast<std::int64_t>( l + r );
  case TokenKind::MINUS:
    return static_cast<std::int64_t>( l - r );
  case TokenKind::AMPERSAND:
    return static_cast<std::int64_t>( l & r );
  case TokenKind::PIPE:
    return static_cast<std::int64_t>( l | r );
  case TokenKind::CARET:
    return static_cast<std::int64_t>( l ^ r );
  default:
    return 0;
  }
}

/// What a name means where `sizeof` reads it, or nothing.
using MeaningOf = std::function<std::optional<Meaning>( Expression const& )>;

/// The bytes `sizeof` counts: of a type written as a keyword, of an
/// `enum struct` named, of an object or an array, or of one element of an
/// array — see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#arrays--task-3c.
/// Nothing for any other operand.
/// A `struct` or a `union` of the program, by its number.
using AggregateOf = std::function<AggregateType const&( std::uint32_t )>;

/// What an expression is a place of, as `sizeof` and a list read it: a type,
/// and an array of it where it is one.
struct Shape
{
  ir::Type type = ir::Type::U8;
  std::optional<std::uint32_t> enumeration{};
  std::optional<Pointee> pointee{};
  std::optional<std::uint32_t> aggregate{};
  bool isArray = false;
  std::uint32_t count = 1;
};

/// The bytes one value of a type takes.
std::uint32_t bytesOf( ir::Type type, std::optional<std::uint32_t> aggregate, AggregateOf const& aggregateOf )
{
  if ( type == ir::Type::BLOCK && aggregate.has_value() )
  {
    return aggregateOf( *aggregate ).size;
  }
  return ir::sizeOf( type );
}

/// The bytes a place takes: its type's, times its elements.
std::uint32_t bytesOf( Shape const& shape, AggregateOf const& aggregateOf )
{
  return bytesOf( shape.type, shape.aggregate, aggregateOf ) * ( shape.isArray ? shape.count : 1 );
}

/// The shape of a member.
Shape shapeOf( MemberType const& member )
{
  return Shape{ .type = member.type.type,
                .enumeration = member.type.enumeration,
                .pointee = member.type.pointee,
                .aggregate = member.type.aggregate,
                .isArray = member.isArray,
                .count = member.count };
}

/// The member of a `struct` or a `union` that `name` names, or nothing.
MemberType const*
memberNamed( AggregateType const& aggregate, std::string_view name, diag::SourceManager const& sources )
{
  auto const found = std::ranges::find_if(
      aggregate.members, [&]( MemberType const& member ) { return sources.textOf( member.name.span() ) == name; } );
  return found == aggregate.members.end() ? nullptr : &*found;
}

/// The shape of what a name, an element, a pointee or a member is, where
/// the names in it name what they must.
std::optional<Shape> shapeOf( Expression const& node,
                              MeaningOf const& meaningOf,
                              AggregateOf const& aggregateOf,
                              diag::SourceManager const& sources )
{
  switch ( node.kind )
  {
  case ExpressionKind::IDENTIFIER:
  {
    std::optional<Meaning> const meaning = meaningOf( node );
    if ( !meaning.has_value() || meaning->kind != NameKind::OBJECT )
    {
      return std::nullopt;
    }
    return Shape{ .type = meaning->type,
                  .enumeration = meaning->enumeration,
                  .pointee = meaning->pointee,
                  .aggregate = meaning->aggregate,
                  .isArray = meaning->isArray,
                  .count = meaning->count.value_or( 0 ) };
  }
  case ExpressionKind::INDEX:
  case ExpressionKind::DEREFERENCE:
  {
    std::optional<Shape> const outer = shapeOf( *node.left, meaningOf, aggregateOf, sources );
    if ( !outer.has_value() )
    {
      return std::nullopt;
    }
    if ( outer->isArray )
    {
      Shape element = *outer;
      element.isArray = false;
      element.count = 1;
      return element;
    }
    if ( !outer->pointee.has_value() )
    {
      return std::nullopt;
    }
    return Shape{ .type = outer->pointee->type,
                  .enumeration = outer->pointee->enumeration,
                  .pointee = std::nullopt,
                  .aggregate = outer->pointee->aggregate,
                  .isArray = false,
                  .count = 1 };
  }
  case ExpressionKind::MEMBER:
  {
    std::optional<Shape> const outer = shapeOf( *node.left, meaningOf, aggregateOf, sources );
    if ( !outer.has_value() )
    {
      return std::nullopt;
    }
    std::optional<std::uint32_t> aggregate = outer->aggregate;
    if ( node.token.kind == TokenKind::ARROW )
    {
      aggregate = outer->pointee.has_value() ? outer->pointee->aggregate : std::nullopt;
    }
    else if ( outer->isArray )
    {
      aggregate.reset();
    }
    if ( !aggregate.has_value() )
    {
      return std::nullopt;
    }
    MemberType const* const member =
        memberNamed( aggregateOf( *aggregate ), sources.textOf( node.right->token.span() ), sources );
    return member == nullptr ? std::nullopt : std::optional{ shapeOf( *member ) };
  }
  default:
    return std::nullopt;
  }
}

/// The bytes `sizeof` counts: of a type written as a keyword or a pointer to
/// one, of a type named, or of what an expression is a place of — see
/// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#arrays--task-3c.
/// Nothing for any other operand.
std::optional<std::int64_t> sizeCounted( Expression const& node,
                                         MeaningOf const& meaningOf,
                                         AggregateOf const& aggregateOf,
                                         diag::SourceManager const& sources )
{
  if ( node.left == nullptr )
  {
    return ir::sizeOf( node.pointer ? ir::Type::POINTER : typeNamed( node.token.keyword ) );
  }
  Expression const& operand = *node.left;
  if ( operand.kind == ExpressionKind::STRING_LITERAL )
  {
    return static_cast<std::int64_t>( operand.arguments.size() );
  }
  if ( operand.kind == ExpressionKind::IDENTIFIER )
  {
    if ( std::optional<Meaning> const meaning = meaningOf( operand );
         meaning.has_value() && meaning->kind == NameKind::TYPE )
    {
      return meaning->aggregate.has_value() ? aggregateOf( *meaning->aggregate ).size : 1;
    }
  }
  std::optional<Shape> const shape = shapeOf( operand, meaningOf, aggregateOf, sources );
  if ( !shape.has_value() )
  {
    return std::nullopt;
  }
  return bytesOf( *shape, aggregateOf );
}

/// What an expression is, as far as the operators around it ask.
struct Typed
{
  /// False where the expression was refused, which has been reported.
  bool valid = false;

  /// Its type, or nothing for an integer constant, which takes the type of
  /// where it is used.
  std::optional<ir::Type> type;

  /// A comparison its constant decides: the outcome it always has.
  std::optional<bool> decided;

  /// The `enum struct` a value is of, whose type is then `u8`.
  std::optional<std::uint32_t> enumeration;

  /// A typed expression of constants — a cast of one, or computed from one —
  /// whose value is folded where it is compiled.
  bool known = false;

  /// For a cast that widens a value, the type the value had, whose range is
  /// still all it holds.
  std::optional<ir::Type> widenedFrom{};

  /// What a pointer points at, its type then `ptr`; and `nullptr`, which is
  /// every pointer's.
  std::optional<Pointee> pointee{};
  bool isNull = false;

  /// An address the assembler knows, of an object at file scope or of an
  /// element of one at a constant index, which a value given where it is
  /// declared may be.
  bool address = false;

  /// An element or a pointee that is `const`, reached through a pointer where
  /// `throughPointer` says.
  bool readOnly = false;
  bool throughPointer = false;

  /// The `struct` or `union` a value is of, whose type is then `block`.
  std::optional<std::uint32_t> aggregate{};

  /// `&f`: the function whose address this is, in C or in the assembler.
  /// Which named type the pointer is of is decided where it goes, and the
  /// taking is what makes `f` a member of it — see
  /// docs/decisions/0065-handlers.md.
  Definition const* taken = nullptr;
  ExternalName const* takenProc = nullptr;
  std::optional<Token> takenAt{};

  /// An element or a pointee that is `volatile`, as `readOnly` says of
  /// `const`.
  bool isVolatile = false;

  /// A `? :` whose ways are both constants: it takes the type of where it is
  /// used, as a constant does, and is no constant, a condition deciding it
  /// where the program runs — see
  /// docs/decisions/0169-a-conditional-operator.md.
  bool atRun = false;
};

Typed refused()
{
  return Typed{
    .valid = false, .type = std::nullopt, .decided = std::nullopt, .enumeration = std::nullopt, .known = false
  };
}

Typed constant()
{
  return Typed{
    .valid = true, .type = std::nullopt, .decided = std::nullopt, .enumeration = std::nullopt, .known = false
  };
}

Typed typed( ir::Type type )
{
  return Typed{ .valid = true, .type = type, .decided = std::nullopt, .enumeration = std::nullopt, .known = false };
}

Typed enumerated( std::uint32_t enumeration )
{
  return Typed{
    .valid = true, .type = ir::Type::U8, .decided = std::nullopt, .enumeration = enumeration, .known = false
  };
}

/// A value of a `struct` or a `union`.
Typed blockOf( std::uint32_t aggregate )
{
  Typed result = typed( ir::Type::BLOCK );
  result.aggregate = aggregate;
  return result;
}

/// A pointer to `pointee`.
Typed pointing( Pointee const& pointee )
{
  Typed result = typed( ir::Type::POINTER );
  result.pointee = pointee;
  return result;
}

/// What a pointer to `pointee` reaches, `const` where the pointee is.
Typed reached( Pointee const& pointee )
{
  Typed result = pointee.enumeration.has_value() ? enumerated( *pointee.enumeration ) : typed( pointee.type );
  if ( pointee.aggregate.has_value() )
  {
    result = blockOf( *pointee.aggregate );
  }
  result.readOnly = pointee.isConst;
  result.isVolatile = pointee.isVolatile;
  result.throughPointer = true;
  return result;
}

Typed typedAs( NamedType const& type )
{
  if ( type.pointee.has_value() )
  {
    return pointing( *type.pointee );
  }
  if ( type.aggregate.has_value() )
  {
    return blockOf( *type.aggregate );
  }
  return type.enumeration.has_value() ? enumerated( *type.enumeration ) : typed( type.type );
}

/// Whether an expression's value is known where it is compiled: a constant,
/// or a typed expression of constants.
bool isKnown( Typed const& kind )
{
  return ( !kind.type.has_value() || kind.known ) && !kind.atRun;
}

/// The wider of two types, the first where they are as wide.
ir::Type widerOf( ir::Type left, ir::Type right )
{
  return ir::sizeOf( right ) > ir::sizeOf( left ) ? right : left;
}

/// The one name an attribute is written with — `[[in(NAME)]]`,
/// `[[under(NAME)]]`, `[[transition(NAME)]]` — where it has exactly one;
/// empty otherwise, which the check reports.
std::string
nameAttribute( std::vector<Attribute> const& attributes, diag::SourceManager const& sources, std::string_view which )
{
  for ( Attribute const& attribute : attributes )
  {
    if ( !attribute.prefix.has_value() && attribute.arguments.size() == 1 &&
         attribute.arguments.front().kind == TokenKind::IDENTIFIER && sources.textOf( attribute.name.span() ) == which )
    {
      return std::string{ sources.textOf( attribute.arguments.front().span() ) };
    }
  }
  return {};
}

/// The class `[[placement(zeropage)]]` or `[[placement(absolute)]]` asks for,
/// and nothing where the attribute does not stand — see
/// docs/decisions/0210-placement-is-declared-in-c-too.md.
std::optional<model::PlacementClass> placementAttribute( std::vector<Attribute> const& attributes,
                                                         diag::SourceManager const& sources )
{
  std::string const said = nameAttribute( attributes, sources, "placement" );
  if ( said == "zeropage" )
  {
    return model::PlacementClass::ZEROPAGE;
  }
  if ( said == "absolute" )
  {
    return model::PlacementClass::ABSOLUTE;
  }
  return std::nullopt;
}

/// Whether a block says nothing but where its declarations lie, in which case
/// it is an ordinary block and not one of the `[[with]]` family — see
/// docs/decisions/0210-placement-is-declared-in-c-too.md.
bool placementOnly( std::vector<Attribute> const& attributes, diag::SourceManager const& sources )
{
  return !attributes.empty() && std::ranges::all_of( attributes,
                                                     [&]( Attribute const& one )
                                                     { return sources.textOf( one.name.span() ) == "placement"; } );
}

/// A class in force for as long as the block that asked for it is being
/// lowered. Nothing is pushed where the block asked for nothing, so the
/// innermost entry is always the one that answers.
class Placed
{
public:
  Placed( std::vector<model::PlacementClass>& stack, std::optional<model::PlacementClass> asked )
      : mStack{ asked.has_value() ? &stack : nullptr }
  {
    if ( asked.has_value() )
    {
      stack.push_back( *asked );
    }
  }

  Placed( Placed const& ) = delete;
  Placed( Placed&& ) = delete;
  Placed& operator=( Placed const& ) = delete;
  Placed& operator=( Placed&& ) = delete;

  ~Placed()
  {
    if ( mStack != nullptr )
    {
      mStack->pop_back();
    }
  }

private:
  std::vector<model::PlacementClass>* mStack;
};

/// Whether a block is written `[[with(...), trampoline]]` — see
/// docs/decisions/0097-a-trampoline-is-declared.md and
/// docs/decisions/0098-a-proc-declares-what-is-shown.md.
bool isTrampoline( std::vector<Attribute> const& attributes, diag::SourceManager const& sources )
{
  return std::ranges::any_of( attributes,
                              [&sources]( Attribute const& attribute )
                              {
                                return !attribute.prefix.has_value() && !attribute.hasArguments &&
                                       sources.textOf( attribute.name.span() ) == "trampoline";
                              } );
}

/// What `[[with(...)]]` is written with, by the shape of its tokens: one of the
/// four forms of docs/decisions/0096-panes-in-c.md, or nothing for a shape
/// that is none of them. The index of `with(NAME, i)` is one token, a name or
/// a constant, since it is computed before the block.
struct WithSpec
{
  ir::WithForm form = ir::WithForm::PANE;
  Token name;
  std::optional<Token> index{};
  std::optional<Token> state{};
};

std::optional<WithSpec> withAttribute( Attribute const& attribute )
{
  std::vector<Token> const& tokens = attribute.arguments;
  if ( tokens.empty() || tokens.front().kind != TokenKind::IDENTIFIER )
  {
    return std::nullopt;
  }
  if ( tokens.size() == 1 )
  {
    return WithSpec{ .form = ir::WithForm::PANE, .name = tokens[0] };
  }
  if ( tokens.size() != 3 )
  {
    return std::nullopt;
  }
  if ( tokens[1].kind == TokenKind::COMMA &&
       ( tokens[2].kind == TokenKind::IDENTIFIER || tokens[2].kind == TokenKind::INTEGER_CONSTANT ) )
  {
    return WithSpec{ .form = ir::WithForm::AT, .name = tokens[0], .index = tokens[2] };
  }
  if ( tokens[1].kind == TokenKind::EQUAL && tokens[2].kind == TokenKind::IDENTIFIER )
  {
    return WithSpec{ .form = ir::WithForm::STATE, .name = tokens[0], .state = tokens[2] };
  }
  return std::nullopt;
}

/// The `[[with]]` of a block's attributes, where one is written.
std::optional<Attribute> withOf( std::vector<Attribute> const& attributes, diag::SourceManager const& sources )
{
  for ( Attribute const& attribute : attributes )
  {
    if ( !attribute.prefix.has_value() && sources.textOf( attribute.name.span() ) == "with" )
    {
      return attribute;
    }
  }
  return std::nullopt;
}

/// What a `[[with]]` block shows, as the check and the lowering both keep it
/// for the statements inside: the Window, and the Pane or family, or the
/// named state, shown of it — nothing of the Window under `with(WINDOW, i)`.
struct Shown
{
  std::string window;
  std::string pane{};
  std::string state{};
};

/// Whether a Pane is shown by a block: named, or a member of the family
/// named, or pinned to the named state shown.
bool shows( Shown const& shown, ExternalName const& pane )
{
  return shown.pane == pane.name ||
         ( !shown.state.empty() && shown.window == pane.window && pane.paneState == shown.state );
}

/// What a name is, for a finding that expected a Pane, a family or a Window
/// of it: the assembler's own word for what it exports, or the kind of C.
std::string kindOfName( Meaning const& meaning )
{
  if ( meaning.external != nullptr && meaning.definition == nullptr && !meaning.external->reason.empty() )
  {
    return meaning.external->reason;
  }
  switch ( meaning.kind )
  {
  case NameKind::FUNCTION:
    return "a function";
  case NameKind::TYPE:
    return "a type";
  case NameKind::OBJECT:
    return meaning.isArray ? "an array" : "an object";
  case NameKind::ASSEMBLER_PROC:
    return "a `.proc`";
  case NameKind::ASSEMBLER_OTHER:
    return "a name of the assembler's";
  }
  return "a name";
}

/// The Pane of the callee a call reaches, or empty.
std::string paneOfCallee( Definition const* callee, ExternalName const* external )
{
  if ( callee != nullptr )
  {
    return callee->pane;
  }
  return external != nullptr ? external->pane : std::string{};
}

/// The Pane the callee runs under, or empty.
std::string underOfCallee( Definition const* callee, ExternalName const* external )
{
  if ( callee != nullptr )
  {
    return callee->under;
  }
  return external != nullptr ? external->under : std::string{};
}

/// Whether an attribute of this name is written, whatever it is given.
bool hasAttribute( std::vector<Attribute> const& attributes,
                   diag::SourceManager const& sources,
                   std::string_view which )
{
  return std::ranges::any_of(
      attributes,
      [&]( Attribute const& attribute )
      { return !attribute.prefix.has_value() && sources.textOf( attribute.name.span() ) == which; } );
}

/// Whether a declaration is written `[[striped]]`, as the subset spells it.
bool isStriped( std::vector<Attribute> const& attributes, diag::SourceManager const& sources )
{
  return std::ranges::any_of( attributes,
                              [&]( Attribute const& attribute )
                              {
                                return !attribute.prefix.has_value() && !attribute.hasArguments &&
                                       sources.textOf( attribute.name.span() ) == "striped";
                              } );
}

/// A call of a function of the program, where the checking pass saw it.
struct CallSite
{
  Definition const* callee = nullptr;
  diag::SourceSpan span;

  /// The Proc of the assembler called, where the callee is one.
  ExternalName const* external = nullptr;
};

/// The first pass: every name a file defines, a name defined twice in it, and
/// each `enum struct`, numbered among the program's.
void gatherSignatures( diag::SourceManager const& sources, Unit& unit, Types& types )
{
  std::vector<EnumerationType>& enumerations = types.enumerations;
  auto const redefined = [&]( Token const& name, Token const& previous )
  {
    // C would take two `u8 x;` as one object; the subset refuses them as it
    // refuses two functions of one name — see
    // docs/decisions/0071-the-subsets-spelling.md.
    std::string const spelling{ sources.textOf( name.span() ) };
    unit.sink->add( diagnostic( diag::DiagnosticId::C_REDEFINITION )
                        .at( name.location, name.length )
                        .arg( "name", spelling )
                        .note( diagnostic( diag::DiagnosticId::C_PREVIOUS_DEFINITION )
                                   .at( previous.location, previous.length )
                                   .arg( "name", spelling ) ) );
  };
  auto const define = [&]( Definition definition )
  {
    std::string const spelling{ sources.textOf( definition.name.span() ) };
    auto const [existing, added] = unit.definitions.try_emplace( spelling, definition );
    if ( !added )
    {
      redefined( definition.name, existing->second.name );
    }
  };

  for ( ExternalDeclaration const& declaration : unit.tree.declarations )
  {
    if ( auto const* function = std::get_if<FunctionDefinition>( &declaration ) )
    {
      std::vector<ParameterType> parameters;
      parameters.reserve( function->parameters.size() );
      for ( Parameter const& parameter : function->parameters )
      {
        parameters.push_back( ParameterType{ .name = parameter.name,
                                             .type = namedType( parameter.type,
                                                                parameter.isPointer,
                                                                parameter.pointeeConst,
                                                                parameter.isPointer && parameter.pointeeVolatile ),
                                             .isNamed = parameter.isNamed } );
      }
      std::optional<NamedType> const result =
          function->result.has_value()
              ? std::optional{ namedType( *function->result,
                                          function->resultPointer,
                                          function->resultPointeeConst,
                                          function->resultPointer && function->resultPointeeVolatile ) }
              : std::nullopt;
      if ( function->isTypedef )
      {
        // A function type: the name is a type, and the Proc a pointer to it
        // goes through — see docs/decisions/0065-handlers.md. Its parameters
        // and its result are the Definition's, as a function's are.
        auto const index = static_cast<std::uint32_t>( types.functions.size() );
        types.functions.push_back( FunctionType{ .name = std::string{ sources.textOf( function->name.span() ) },
                                                 .result = result,
                                                 .parameters = parameters } );
        Definition type{ .kind = NameKind::TYPE,
                         .isStatic = false,
                         .name = function->name,
                         .type = ir::Type::U8,
                         .enumeration = std::nullopt,
                         .typeName = std::nullopt,
                         .result = result,
                         .parameters = std::move( parameters ) };
        type.function = index;
        define( std::move( type ) );
        continue;
      }
      bool const isSlot = function->isExtern && hasAttribute( function->attributes, sources, "slot" );

      // A Slot that takes or returns **is** the function type its
      // Implementations are members of: they read its Temporaries and declare
      // none, exactly as a member of a `typedef`'s type does, and a call
      // writes them and `jsr`s the Cell — see
      // docs/decisions/0174-a-slot-takes-and-returns.md.
      std::optional<std::uint32_t> slotType;
      if ( isSlot && ( !parameters.empty() || result.has_value() ) )
      {
        slotType = static_cast<std::uint32_t>( types.functions.size() );
        types.functions.push_back( FunctionType{ .name = std::string{ sources.textOf( function->name.span() ) },
                                                 .result = result,
                                                 .parameters = parameters } );
      }

      Definition declared{
        .kind = NameKind::FUNCTION,
        .isStatic = function->isStatic,
        .name = function->name,
        .type = ir::Type::U8,
        .enumeration = std::nullopt,
        .typeName = std::nullopt,
        .result = result,
        .parameters = std::move( parameters ),
        .isExtern = function->isExtern,
        .pane = function->isExtern ? std::string{} : nameAttribute( function->attributes, sources, "in" ),
        .under = function->isExtern ? std::string{} : nameAttribute( function->attributes, sources, "under" ),
        .isSlot = isSlot,
        .implements = nameAttribute( function->attributes, sources, "implements" )
      };
      declared.function = slotType;
      define( std::move( declared ) );
      continue;
    }
    if ( auto const* aggregate = std::get_if<StructSpecifier>( &declaration ) )
    {
      AggregateType type{ .name = std::string{ sources.textOf( aggregate->name.span() ) },
                          .isUnion = aggregate->isUnion,
                          .members = {} };
      for ( Declaration const& member : aggregate->members )
      {
        for ( std::size_t index = 0; index < member.declarators.size(); ++index )
        {
          DeclaratorShape const& shape = member.shapes[index];
          type.members.push_back( MemberType{ .name = member.declarators[index],
                                              .type = namedType( member.type,
                                                                 shape.isPointer,
                                                                 member.isConst && shape.isPointer,
                                                                 member.isVolatile && shape.isPointer ),
                                              .isArray = shape.isArray,
                                              .size = shape.size.get(),
                                              .count = 1,
                                              .offset = 0 } );
        }
      }
      auto const index = static_cast<std::uint32_t>( types.aggregates.size() );
      types.aggregates.push_back( std::move( type ) );
      Definition definition{ .kind = NameKind::TYPE,
                             .isStatic = false,
                             .name = aggregate->name,
                             .type = ir::Type::BLOCK,
                             .enumeration = std::nullopt,
                             .typeName = std::nullopt,
                             .result = std::nullopt,
                             .parameters = {} };
      definition.aggregate = index;
      define( std::move( definition ) );
      continue;
    }
    if ( auto const* specifier = std::get_if<EnumSpecifier>( &declaration ) )
    {
      std::string const name{ sources.textOf( specifier->name.span() ) };
      if ( specifier->enumerators.size() > MAX_ENUMERATORS )
      {
        unit.sink->add( diagnostic( diag::DiagnosticId::C_TOO_MANY_ENUMERATORS )
                            .at( specifier->name.location, specifier->name.length )
                            .arg( "name", name )
                            .arg( "count", static_cast<std::int64_t>( specifier->enumerators.size() ) ) );
      }
      EnumerationType type{ .name = name, .enumerators = {} };
      std::map<std::string, Token, std::less<>> seen;
      for ( Token const& enumerator : specifier->enumerators )
      {
        std::string spelling{ sources.textOf( enumerator.span() ) };
        if ( auto const [existing, added] = seen.try_emplace( spelling, enumerator ); !added )
        {
          redefined( enumerator, existing->second );
        }
        type.enumerators.push_back( std::move( spelling ) );
      }
      auto const index = static_cast<std::uint32_t>( enumerations.size() );
      enumerations.push_back( std::move( type ) );
      define( Definition{ .kind = NameKind::TYPE,
                          .isStatic = false,
                          .name = specifier->name,
                          .type = ir::Type::U8,
                          .enumeration = index,
                          .typeName = std::nullopt,
                          .result = std::nullopt,
                          .parameters = {} } );

      // An `enum` written without `struct` gives the file its enumerators
      // too, each a `const` of the type — see
      // docs/decisions/0162-an-enum-without-struct.md.
      if ( !specifier->isScoped )
      {
        for ( std::uint32_t at = 0; at < specifier->enumerators.size(); ++at )
        {
          Definition enumerator{ .kind = NameKind::OBJECT,
                                 .isStatic = false,
                                 .name = specifier->enumerators[at],
                                 .type = ir::Type::U8,
                                 .enumeration = index,
                                 .typeName = std::nullopt,
                                 .result = std::nullopt,
                                 .parameters = {},
                                 .isConst = true };
          enumerator.enumerator = at;
          define( std::move( enumerator ) );
        }
      }
      continue;
    }
    auto const& object = std::get<Declaration>( declaration );
    bool const named = object.type.kind == TokenKind::IDENTIFIER;
    for ( std::size_t index = 0; index < object.declarators.size(); ++index )
    {
      DeclaratorShape const& shape = object.shapes[index];
      std::optional<Pointee> const pointee = shape.isPointer
                                                 ? std::optional{ Pointee{ .type = typeNamed( object.type.keyword ),
                                                                           .enumeration = std::nullopt,
                                                                           .isConst = object.isConst,
                                                                           .aggregate = std::nullopt,
                                                                           .function = std::nullopt,
                                                                           .isVolatile = object.isVolatile } }
                                                 : std::nullopt;
      define( Definition{ .kind = NameKind::OBJECT,
                          .isStatic = object.isStatic,
                          .name = object.declarators[index],
                          .type = shape.isPointer ? ir::Type::POINTER : typeNamed( object.type.keyword ),
                          .enumeration = std::nullopt,
                          .typeName = named ? std::optional{ object.type } : std::nullopt,
                          .result = std::nullopt,
                          .parameters = {},
                          .isConst = shape.isPointer ? shape.isConstPointer : object.isConst,
                          .value = object.initialisers[index].get(),
                          .isArray = object.shapes[index].isArray,
                          .size = object.shapes[index].size.get(),
                          .count = object.shapes[index].size == nullptr && object.shapes[index].hasList
                                       ? std::optional{ static_cast<std::uint32_t>( object.shapes[index].list.size() ) }
                                       : std::nullopt,
                          .pointee = pointee,
                          .aggregate = std::nullopt,
                          .isStriped = isStriped( object.attributes, sources ),
                          .isExtern = object.isExtern,
                          .pane = object.isExtern ? std::string{} : nameAttribute( object.attributes, sources, "in" ),
                          .under = {},
                          .isSlot = object.isExtern && hasAttribute( object.attributes, sources, "slot" ),
                          .implements = nameAttribute( object.attributes, sources, "implements" ),
                          .isVolatile = shape.isPointer ? shape.isVolatilePointer : object.isVolatile } );
    }
  }
}

/// Every type written as a name — an object's, a function's result and its
/// parameters' — given the `enum struct` it names: the file's own definition
/// first, and then any file's, as every name is found.
void resolveTypeNames( diag::SourceManager const& sources, std::deque<Unit>& units, Types& registry )
{
  std::map<std::string, Definition const*, std::less<>> types;
  for ( Unit const& unit : units )
  {
    for ( auto const& [spelling, definition] : unit.definitions )
    {
      if ( definition.kind == NameKind::TYPE )
      {
        types.try_emplace( spelling, &definition );
      }
    }
  }

  for ( Unit& unit : units )
  {
    auto const resolved = [&]( Token const& typeName ) -> Definition const*
    {
      std::string_view const name = sources.textOf( typeName.span() );
      Definition const* found = nullptr;
      if ( auto const own = unit.definitions.find( name ); own != unit.definitions.end() )
      {
        found = own->second.kind == NameKind::TYPE ? &own->second : nullptr;
      }
      else if ( auto const other = types.find( name ); other != types.end() )
      {
        found = other->second;
      }
      if ( found == nullptr )
      {
        unit.sink->add( diagnostic( diag::DiagnosticId::C_NOT_A_TYPE )
                            .at( typeName.location, typeName.length )
                            .arg( "name", std::string{ name } ) );
      }
      return found;
    };
    // A named type is an `enum struct`, a value of one byte, or a `struct` or
    // a `union`, a block of the bytes its members take.
    auto const resolve = [&resolved]( NamedType& type )
    {
      std::optional<Token> const typeName = type.typeName;
      if ( !typeName.has_value() )
      {
        return;
      }
      Definition const* const found = resolved( *typeName );
      if ( found == nullptr )
      {
        return;
      }
      if ( type.pointee.has_value() )
      {
        type.pointee->enumeration = found->enumeration;
        type.pointee->aggregate = found->aggregate;
        type.pointee->function = found->function;
        type.pointee->type = found->aggregate.has_value() ? ir::Type::BLOCK : ir::Type::U8;
        return;
      }
      type.enumeration = found->enumeration;
      type.aggregate = found->aggregate;
      type.function = found->function;
      type.type = found->aggregate.has_value() ? ir::Type::BLOCK : ir::Type::U8;
    };

    for ( auto& [spelling, definition] : unit.definitions )
    {
      if ( definition.kind == NameKind::FUNCTION )
      {
        if ( definition.result.has_value() )
        {
          resolve( *definition.result );
        }
        for ( ParameterType& parameter : definition.parameters )
        {
          resolve( parameter.type );
        }
        continue;
      }
      if ( definition.kind == NameKind::TYPE && definition.aggregate.has_value() )
      {
        for ( MemberType& member : registry.aggregates.at( *definition.aggregate ).members )
        {
          resolve( member.type );
        }
        continue;
      }
      if ( definition.kind != NameKind::OBJECT || !definition.typeName.has_value() )
      {
        continue;
      }
      NamedType type{ .type = definition.type,
                      .enumeration = std::nullopt,
                      .typeName = definition.typeName,
                      .pointee = definition.pointee };
      resolve( type );
      definition.type = type.type;
      definition.enumeration = type.enumeration;
      definition.pointee = type.pointee;
      definition.aggregate = type.aggregate;
      definition.function = type.function;
    }
  }
}

/// Every name of the program, and what one means in a file.
class Names
{
public:
  Names( diag::SourceManager const& sources,
         std::deque<Unit> const& units,
         std::span<ExternalName const> externals,
         Types types )
      : mSources( &sources ), mEnumerations( std::move( types.enumerations ) ),
        mAggregates( std::move( types.aggregates ) ), mFunctionTypes( std::move( types.functions ) )
  {
    // A name two files export is Merge's to refuse, on the text both compile
    // to; here the first one stands for it, so that neither file is told the
    // name is undeclared.
    for ( Unit const& unit : units )
    {
      for ( auto const& [spelling, definition] : unit.definitions )
      {
        mUnits.emplace( &definition, &unit );
        if ( !definition.isStatic )
        {
          mExported.try_emplace( spelling, meaningOf( definition ) );
        }
      }
    }
    for ( Unit const& unit : units )
    {
      for ( auto const& [spelling, definition] : unit.definitions )
      {
        if ( definition.isExtern )
        {
          mExterns.try_emplace( spelling, &definition );
        }
        else if ( !definition.isStatic )
        {
          mDefined.try_emplace( spelling, &definition );
        }
      }
    }
    for ( ExternalName const& external : externals )
    {
      mExternals.try_emplace( external.name, &external );
      Meaning meaning{
        .kind = NameKind::ASSEMBLER_OTHER, .type = external.type, .enumeration = std::nullopt, .byte = {}, .owner = {}
      };
      meaning.external = &external;
      switch ( external.kind )
      {
      case ExternalKind::PROC:
        meaning.kind = NameKind::ASSEMBLER_PROC;
        break;
      case ExternalKind::MEMORY:
        meaning.kind = NameKind::OBJECT;
        meaning.isArray = external.isArray;
        meaning.count = external.count;
        break;
      case ExternalKind::CONSTANT:
        meaning.kind = NameKind::OBJECT;
        meaning.type = ir::Type::U16;
        meaning.isConst = true;
        meaning.value = external.value;
        break;
      case ExternalKind::SHAPELESS:
      case ExternalKind::RESERVED:
      case ExternalKind::UNREAD:
      case ExternalKind::CHARSET:
      case ExternalKind::PANE:
      case ExternalKind::WINDOW:
        break;
      }
      mExported.try_emplace( external.name, meaning );
    }
  }

  /// What `spelling` means in `unit`: its own definition first, a static one
  /// included, and then what another file or an `.asm` Module exports. A
  /// `const` given a value carries it once its value is settled.
  [[nodiscard]] std::optional<Meaning> resolve( Unit const& unit, std::string_view spelling ) const
  {
    std::optional<Meaning> meaning;
    if ( auto const own = unit.definitions.find( spelling ); own != unit.definitions.end() )
    {
      meaning = meaningOf( own->second );
    }
    else if ( auto const exported = mExported.find( spelling ); exported != mExported.end() )
    {
      meaning = exported->second;
    }
    if ( meaning.has_value() && meaning->definition != nullptr && meaning->definition->isExtern &&
         !meaning->definition->isSlot )
    {
      // What the declaration reads, and a Constant's value where it folds. A
      // Slot is the `extern` that names nothing of the assembler's: the tool
      // gives it a Cell, and C reads it as the function or the pointer its
      // type says — see docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md.
      meaning->external = externalNamed( spelling );
      if ( meaning->external != nullptr && meaning->external->kind == ExternalKind::CONSTANT )
      {
        meaning->value = meaning->external->value;
      }
      if ( meaning->kind == NameKind::FUNCTION )
      {
        meaning->kind = NameKind::ASSEMBLER_PROC;
      }
    }
    if ( meaning.has_value() && meaning->definition != nullptr )
    {
      if ( auto const settled = mValues.find( meaning->definition ); settled != mValues.end() )
      {
        meaning->value = settled->second;
      }
      if ( auto const counted = mCounts.find( meaning->definition ); counted != mCounts.end() )
      {
        meaning->count = counted->second;
      }
    }
    return meaning;
  }

  [[nodiscard]] EnumerationType const& enumeration( std::uint32_t index ) const
  {
    return mEnumerations.at( index );
  }

  [[nodiscard]] AggregateType const& aggregate( std::uint32_t index ) const
  {
    return mAggregates.at( index );
  }

  [[nodiscard]] FunctionType const& functionType( std::uint32_t index ) const
  {
    return mFunctionTypes.at( index );
  }

  [[nodiscard]] std::span<FunctionType const> functionTypes() const
  {
    return mFunctionTypes;
  }

  /// Lays out a `struct` or a `union` whose members' types are laid out.
  void settleLayout( std::uint32_t index, AggregateType layout )
  {
    mAggregates.at( index ) = std::move( layout );
  }

  /// The file a definition stands in.
  [[nodiscard]] Unit const& unitOf( Definition const& definition ) const
  {
    return *mUnits.at( &definition );
  }

  /// What an `.asm` Module exports by that name, or nothing.
  [[nodiscard]] ExternalName const* externalNamed( std::string_view spelling ) const
  {
    auto const found = mExternals.find( spelling );
    return found == mExternals.end() ? nullptr : found->second;
  }

  /// The first `extern` of a name, in the order the files are given.
  [[nodiscard]] Definition const* firstExtern( std::string_view spelling ) const
  {
    auto const found = mExterns.find( spelling );
    return found == mExterns.end() ? nullptr : found->second;
  }

  /// What a file of C defines by that name and exports, or nothing.
  [[nodiscard]] Definition const* definedInC( std::string_view spelling ) const
  {
    auto const found = mDefined.find( spelling );
    return found == mDefined.end() ? nullptr : found->second;
  }

  [[nodiscard]] AggregateOf aggregateOf() const
  {
    return [this]( std::uint32_t index ) -> AggregateType const& { return aggregate( index ); };
  }

  /// The place of the enumerator `NAME::NAME` names in its type, as `unit`
  /// reads it, where it names one.
  [[nodiscard]] std::optional<std::uint32_t> enumeratorIndex( Unit const& unit, Expression const& node ) const
  {
    // An enumerator of an `enum` written without `struct` is named by itself
    // — see docs/decisions/0162-an-enum-without-struct.md.
    if ( node.kind == ExpressionKind::IDENTIFIER )
    {
      std::optional<Meaning> const named = resolve( unit, mSources->textOf( node.token.span() ) );
      return named.has_value() && named->definition != nullptr ? named->definition->enumerator : std::nullopt;
    }
    if ( node.kind != ExpressionKind::QUALIFIED_NAME )
    {
      return std::nullopt;
    }
    std::optional<Meaning> const meaning = resolve( unit, mSources->textOf( node.left->token.span() ) );
    if ( !meaning.has_value() || !meaning->enumeration.has_value() || meaning->kind != NameKind::TYPE )
    {
      return std::nullopt;
    }
    std::vector<std::string> const& enumerators = mEnumerations.at( *meaning->enumeration ).enumerators;
    auto const found = std::ranges::find( enumerators, mSources->textOf( node.right->token.span() ) );
    if ( found == enumerators.end() )
    {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>( found - enumerators.begin() );
  }

  /// Settles the value of a `const` at file scope, every `const` it names
  /// having been settled first.
  void settle( Definition const& definition, std::int64_t value )
  {
    mValues.insert_or_assign( &definition, value );
  }

  /// Settles the number of elements of an array at file scope whose size is
  /// written, what the size names having been settled first.
  void settleCount( Definition const& definition, std::uint32_t count )
  {
    mCounts.insert_or_assign( &definition, count );
  }

private:
  static Meaning meaningOf( Definition const& definition )
  {
    return Meaning{ .kind = definition.kind,
                    .type = definition.type,
                    .enumeration = definition.enumeration,
                    .byte = {},
                    .owner = {},
                    .definition = &definition,
                    .isConst = definition.isConst,
                    .value = definition.enumerator.has_value()
                                 ? std::optional{ static_cast<std::int64_t>( *definition.enumerator ) }
                                 : std::nullopt,
                    .isArray = definition.isArray,
                    .count = definition.count,
                    .pointee = definition.pointee,
                    .isParameter = false,
                    .declaredAt = std::nullopt,
                    .aggregate = definition.aggregate,
                    .function = definition.function,
                    .isStriped = definition.isStriped,
                    .external = nullptr,
                    .isVolatile = definition.isVolatile };
  }

  diag::SourceManager const* mSources;
  std::vector<EnumerationType> mEnumerations;
  std::vector<AggregateType> mAggregates;
  std::vector<FunctionType> mFunctionTypes;
  std::map<std::string, Meaning, std::less<>> mExported;
  std::map<Definition const*, Unit const*> mUnits;
  std::map<Definition const*, std::int64_t> mValues;
  std::map<Definition const*, std::uint32_t> mCounts;
  std::map<std::string, ExternalName const*, std::less<>> mExternals;
  std::map<std::string, Definition const*, std::less<>> mExterns;
  std::map<std::string, Definition const*, std::less<>> mDefined;
};

/// The most bytes a run holds: the address space.
constexpr std::uint64_t MAX_RUN_BYTES = 65536;

/// The bytes a run of the assembler's holds before what stops it.
std::uint32_t runBytes( ExternalName const& external )
{
  std::uint64_t bytes = 0;
  for ( ExternalCell const& cell : external.cells )
  {
    bytes += cell.bytes;
  }
  return static_cast<std::uint32_t>( std::min<std::uint64_t>( bytes, MAX_RUN_BYTES ) );
}

/// The count of an `extern` array written `NAME[]`: the elements its run holds
/// to the end of its Section, which the check holds to leave nothing over.
void settleExterns( std::deque<Unit> const& units, Names& names )
{
  for ( Unit const& unit : units )
  {
    for ( auto const& [spelling, definition] : unit.definitions )
    {
      ExternalName const* const external = names.externalNamed( spelling );
      if ( !definition.isExtern || !definition.isArray || definition.size != nullptr || external == nullptr )
      {
        continue;
      }
      std::uint32_t const element =
          std::max<std::uint32_t>( 1, bytesOf( definition.type, definition.aggregate, names.aggregateOf() ) );
      names.settleCount( definition, runBytes( *external ) / element );
    }
  }
}

/// Whether a definition is a `const` given a value, which is a constant of the
/// assembler where the check finds the value is one. Not a `volatile` one,
/// which is read wherever the source reads it — see
/// docs/decisions/0151-volatile.md.
bool isConstant( Definition const& definition )
{
  return definition.kind == NameKind::OBJECT && definition.isConst && !definition.isVolatile &&
         definition.value != nullptr && !definition.isArray && definition.type != ir::Type::BLOCK;
}

/// Whether a definition is settled before the check: a constant, an array
/// whose size is written, and a `struct` or a `union`, since a constant may
/// count the bytes of any of them.
bool isSettled( Definition const& definition )
{
  return isConstant( definition ) || ( definition.isArray && definition.size != nullptr ) ||
         ( definition.aggregate.has_value() && definition.kind == NameKind::TYPE );
}

/// The most elements an array holds: as many bytes as the address space has.
constexpr std::int64_t MAX_ARRAY_BYTES = 65536;

/// The most bytes a copy of a `struct` or a `union` takes, one byte of index
/// counting them — see docs/decisions/0086-a-struct-by-value.md.
constexpr std::uint32_t MAX_COPY_BYTES = 256;

/// The elements a striped array holds at most: an index in `X` reaches them.
constexpr std::uint32_t MAX_STRIPED_ELEMENTS = 256;

/// The bytes up to which a `struct` or a `union` is copied a store per byte,
/// and lies on the zero page as a parameter, a result or a local.
constexpr std::uint32_t SMALL_BLOCK = 4;

/// Whether an expression takes an address with `&`.
bool containsAddress( Expression const& node )
{
  if ( node.kind == ExpressionKind::ADDRESS )
  {
    return true;
  }
  std::array<Expression const*, 2> const children{ node.left.get(), node.right.get() };
  return std::ranges::any_of( children,
                              []( Expression const* child ) { return child != nullptr && containsAddress( *child ); } );
}

/// Every name an expression reads, in the order written.
void namesIn( Expression const& node, std::vector<Expression const*>& into )
{
  if ( node.kind == ExpressionKind::IDENTIFIER )
  {
    into.push_back( &node );
    return;
  }
  if ( node.kind == ExpressionKind::QUALIFIED_NAME )
  {
    return;
  }
  // The name after `.` or `->` is a member's, which no scope defines.
  if ( node.kind == ExpressionKind::MEMBER )
  {
    namesIn( *node.left, into );
    return;
  }
  for ( Expression const* child : { node.left.get(), node.right.get() } )
  {
    if ( child != nullptr )
    {
      namesIn( *child, into );
    }
  }
  for ( ExpressionPtr const& argument : node.arguments )
  {
    namesIn( *argument, into );
  }
}

/// The value of every `const` at file scope, and the number of elements of
/// every array there whose size is written, each settled after the ones it
/// names, a cycle among them refused once, where the name closing it is read
/// — see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
/// Walked on a stack of its own, so that a long chain of constants costs no
/// depth of the tool's.
void settleConstants( diag::SourceManager const& sources, std::deque<Unit> const& units, Names& names )
{
  enum class State : std::uint8_t
  {
    ACTIVE,
    DONE,
  };
  std::map<Definition const*, State> state;
  std::set<Definition const*> cyclic;

  auto const referenced = [&]( Unit const& unit, Token const& name ) -> Definition const*
  {
    std::optional<Meaning> const meaning = names.resolve( unit, sources.textOf( name.span() ) );
    if ( !meaning.has_value() || meaning->definition == nullptr || !isSettled( *meaning->definition ) )
    {
      return nullptr;
    }
    return meaning->definition;
  };

  auto const settle = [&]( Definition const& definition )
  {
    Unit const& unit = names.unitOf( definition );
    MeaningOf const meaningOf = [&]( Expression const& name )
    { return names.resolve( unit, sources.textOf( name.token.span() ) ); };
    NameValue const valueOf = [&]( Expression const& node ) -> std::int64_t
    {
      if ( node.kind == ExpressionKind::QUALIFIED_NAME )
      {
        return names.enumeratorIndex( unit, node ).value_or( 0 );
      }
      if ( node.kind == ExpressionKind::SIZEOF )
      {
        return sizeCounted(
                   node,
                   meaningOf,
                   [&names]( std::uint32_t index ) -> AggregateType const& { return names.aggregate( index ); },
                   sources )
            .value_or( 0 );
      }
      std::optional<Meaning> const meaning = meaningOf( node );
      return meaning.has_value() ? meaning->value.value_or( 0 ) : 0;
    };
    // A `const` pointer given an address holds it in bytes: the assembler knows
    // the address, and the compiler does not.
    std::vector<Expression const*> reads;
    if ( !definition.isArray && definition.kind == NameKind::OBJECT )
    {
      namesIn( *definition.value, reads );
    }
    bool const address = !definition.isArray && definition.pointee.has_value() &&
                         ( containsAddress( *definition.value ) ||
                           std::ranges::any_of( reads,
                                                [&]( Expression const* read )
                                                {
                                                  std::optional<Meaning> const meaning = meaningOf( *read );
                                                  return meaning.has_value() && meaning->isArray;
                                                } ) );
    if ( address )
    {
      return;
    }
    if ( definition.kind == NameKind::TYPE )
    {
      // Each member's bytes: its type's, times its elements.
      AggregateType layout = names.aggregate( *definition.aggregate );
      std::uint32_t offset = 0;
      std::uint32_t size = 0;
      for ( MemberType& member : layout.members )
      {
        std::uint32_t bytes = ir::sizeOf( member.type.type );
        if ( member.type.aggregate.has_value() && !member.type.pointee.has_value() )
        {
          bytes = cyclic.contains( &definition ) ? 0 : names.aggregate( *member.type.aggregate ).size;
        }
        if ( member.isArray && member.size != nullptr && !cyclic.contains( &definition ) )
        {
          member.count = static_cast<std::uint32_t>(
              std::clamp<std::int64_t>( folded( *member.size, std::nullopt, valueOf ), 0, MAX_ARRAY_BYTES ) );
        }
        member.offset = layout.isUnion ? 0 : offset;
        std::uint32_t const extent = static_cast<std::uint32_t>(
            std::min<std::uint64_t>( std::uint64_t{ bytes } * member.count, MAX_ARRAY_BYTES ) );
        offset =
            static_cast<std::uint32_t>( std::min<std::uint64_t>( std::uint64_t{ offset } + extent, MAX_ARRAY_BYTES ) );
        size = layout.isUnion ? std::max( size, extent ) : offset;
      }
      layout.size = size;
      names.settleLayout( *definition.aggregate, std::move( layout ) );
      return;
    }
    if ( definition.isArray )
    {
      std::int64_t const count = cyclic.contains( &definition ) ? 0 : folded( *definition.size, std::nullopt, valueOf );
      names.settleCount( definition,
                         static_cast<std::uint32_t>( std::clamp<std::int64_t>( count, 0, MAX_ARRAY_BYTES ) ) );
      return;
    }
    std::int64_t value = 0;
    if ( !cyclic.contains( &definition ) )
    {
      value = wrapped( folded( *definition.value, definition.type, valueOf ), definition.type );
    }
    names.settle( definition, value );
  };

  struct Frame
  {
    Definition const* definition = nullptr;
    std::vector<Token> reads;
    std::size_t next = 0;
  };

  // What a definition's value is settled from reads: the names in a
  // constant's value or an array's size, and a type's members' sizes and the
  // types of those that hold one by value.
  auto const readsOf = [&]( Definition const& definition )
  {
    std::vector<Expression const*> expressions;
    std::vector<Token> tokens;
    if ( definition.kind == NameKind::TYPE )
    {
      for ( MemberType const& member : names.aggregate( *definition.aggregate ).members )
      {
        if ( member.size != nullptr )
        {
          namesIn( *member.size, expressions );
        }
        if ( member.type.typeName.has_value() && !member.type.pointee.has_value() )
        {
          tokens.push_back( *member.type.typeName );
        }
      }
    }
    else
    {
      namesIn( definition.isArray ? *definition.size : *definition.value, expressions );
    }
    for ( Expression const* read : expressions )
    {
      tokens.push_back( read->token );
    }
    return tokens;
  };

  for ( Unit const& unit : units )
  {
    for ( auto const& [spelling, root] : unit.definitions )
    {
      if ( !isSettled( root ) || state.contains( &root ) )
      {
        continue;
      }
      std::vector<Frame> frames;
      auto const enter = [&]( Definition const& definition )
      {
        state[&definition] = State::ACTIVE;
        frames.push_back( Frame{ .definition = &definition, .reads = readsOf( definition ), .next = 0 } );
      };
      enter( root );
      while ( !frames.empty() )
      {
        Frame& top = frames.back();
        if ( top.next == top.reads.size() )
        {
          settle( *top.definition );
          state[top.definition] = State::DONE;
          frames.pop_back();
          continue;
        }
        Token const read = top.reads[top.next++];
        Definition const* const target = referenced( names.unitOf( *top.definition ), read );
        if ( target == nullptr )
        {
          continue;
        }
        auto const found = state.find( target );
        if ( found == state.end() )
        {
          enter( *target );
          continue;
        }
        if ( found->second == State::DONE )
        {
          continue;
        }
        if ( target == top.definition && target->kind != NameKind::TYPE )
        {
          // A `const` naming itself is read in its own value, which the check
          // reports as that.
          cyclic.insert( target );
          continue;
        }

        // The frames from the one it names to this one are the cycle.
        std::string cycle;
        bool inCycle = false;
        for ( Frame const& frame : frames )
        {
          inCycle = inCycle || frame.definition == target;
          if ( inCycle )
          {
            cyclic.insert( frame.definition );
            cycle += "`" + std::string{ sources.textOf( frame.definition->name.span() ) } + "` -> ";
          }
        }
        cycle += "`" + std::string{ sources.textOf( target->name.span() ) } + "`";
        // A type holding itself by value has no size; anything else in the
        // cycle is a constant defined through itself.
        names.unitOf( *top.definition )
            .sink->add( diagnostic( target->kind == NameKind::TYPE ? diag::DiagnosticId::C_AGGREGATE_HOLDS_ITSELF
                                                                   : diag::DiagnosticId::C_CONSTANT_CYCLE )
                            .at( read.location, read.length )
                            .arg( "cycle", cycle ) );
      }
    }
  }
}

/// The types of a file's expressions and the rules of
/// docs/decisions/0076-arithmetic-in-the-subset.md, reporting into a sink
/// where it is given one: the checking pass reports, and the lowering pass,
/// which runs only on a file the check passed, asks the same questions again
/// without one.
/// The whole expression the type check was given, which a compound assignment
/// used as a value is held against: set by the outermost call, whichever entry
/// it came in by, and cleared when that call returns — see
/// docs/decisions/0198-a-compound-assignment-is-a-value.md.
class Rooted
{
public:
  Rooted( Expression const*& root, Expression const& node ) : mRoot( &root ), mSet( root == nullptr )
  {
    if ( mSet )
    {
      root = &node;
    }
  }

  Rooted( Rooted const& ) = delete;
  Rooted( Rooted&& ) = delete;
  Rooted& operator=( Rooted const& ) = delete;
  Rooted& operator=( Rooted&& ) = delete;

  ~Rooted()
  {
    if ( mSet )
    {
      *mRoot = nullptr;
    }
  }

private:
  Expression const** mRoot;
  bool mSet;
};

class Typing
{
public:
  Typing( diag::SourceManager const& sources,
          Names const& names,
          Unit const& unit,
          diag::DiagnosticSink* sink,
          Membership* members = nullptr )
      : mSources( &sources ), mNames( &names ), mUnit( &unit ), mSink( sink ), mMembers( members )
  {
  }

  [[nodiscard]] std::string spellingOf( Token const& token ) const
  {
    return std::string{ mSources->textOf( token.span() ) };
  }

  [[nodiscard]] std::string textOf( diag::SourceSpan span ) const
  {
    return std::string{ mSources->textOf( span ) };
  }

  [[nodiscard]] EnumerationType const& enumeration( std::uint32_t index ) const
  {
    return mNames->enumeration( index );
  }

  [[nodiscard]] FunctionType const& functionType( std::uint32_t index ) const
  {
    return mNames->functionType( index );
  }

  /// Says whose body is being read, so that what is recorded names it.
  void within( Definition const* function )
  {
    mWithin = function;
  }

  [[nodiscard]] AggregateType const& aggregate( std::uint32_t index ) const
  {
    return mNames->aggregate( index );
  }

  [[nodiscard]] AggregateOf aggregateOf() const
  {
    return [names = mNames]( std::uint32_t index ) -> AggregateType const& { return names->aggregate( index ); };
  }

  [[nodiscard]] Names const& names() const
  {
    return *mNames;
  }

  /// The bytes one value of a type takes.
  [[nodiscard]] std::uint32_t bytes( ir::Type type, std::optional<std::uint32_t> aggregateIndex ) const
  {
    return bytesOf( type, aggregateIndex, aggregateOf() );
  }

  /// The shape of what an expression is a place of, as the lowering reads it.
  [[nodiscard]] std::optional<Shape> shape( Expression const& node ) const
  {
    return shapeOf( node, [this]( Expression const& read ) { return lookup( read ); }, aggregateOf(), *mSources );
  }

  void add( diag::Diagnostic value )
  {
    if ( mSink != nullptr )
    {
      mSink->add( std::move( value ) );
    }
  }

  /// What a name means, reporting it where it names nothing, or names what
  /// of the assembler's C does not read; a call reports that itself.
  std::optional<Meaning> resolve( Expression const& identifier, bool called = false )
  {
    std::string_view const spelling = mSources->textOf( identifier.token.span() );
    if ( std::optional<Meaning> const local = declared( spelling ); local.has_value() )
    {
      return local;
    }
    std::optional<Meaning> meaning = mNames->resolve( *mUnit, spelling );
    if ( !meaning.has_value() )
    {
      name( diag::DiagnosticId::C_NOT_DECLARED, identifier );
    }
    if ( meaning.has_value() && meaning->kind == NameKind::ASSEMBLER_OTHER && !called )
    {
      unread( identifier, *meaning->external );
      return std::nullopt;
    }
    return meaning;
  }

  /// A local of a block being read, the innermost of that name.
  [[nodiscard]] std::optional<Meaning> declared( std::string_view spelling ) const
  {
    for ( auto const& scope : std::views::reverse( mScopes ) )
    {
      if ( auto const found = scope.find( spelling ); found != scope.end() )
      {
        return found->second;
      }
    }
    return std::nullopt;
  }

  void openScope()
  {
    mScopes.emplace_back();
  }

  void closeScope()
  {
    mScopes.pop_back();
  }

  /// Declares a local in the innermost scope. False where that scope declares
  /// the name already, which is the caller's to report; a name of an outer
  /// scope, or of the program, is hidden as C hides it.
  bool declareLocal( Token const& name, Meaning const& meaning )
  {
    if ( mScopes.empty() )
    {
      openScope();
    }
    return mScopes.back().try_emplace( spellingOf( name ), meaning ).second;
  }

  /// The type a declaration names: a keyword, or an `enum struct`.
  /// What each label of a `switch` stands for, in the order written: an
  /// enumerator's place in its type, or the constant the label is. A label
  /// that is neither counts as zero, and is refused where it is checked.
  [[nodiscard]] std::vector<std::int64_t> labelValues( Statement const& node, ir::Type type )
  {
    std::vector<std::int64_t> values;
    for ( Statement const& clause : node.items )
    {
      if ( clause.expression == nullptr )
      {
        continue;
      }
      if ( std::optional<std::uint32_t> const enumerator = enumeratorIndex( *clause.expression ) )
      {
        values.push_back( *enumerator );
        continue;
      }
      values.push_back( isKnown( expression( *clause.expression ) ) ? wrapped( fold( *clause.expression, type ), type )
                                                                    : 0 );
    }
    return values;
  }

  /// The type a local written `auto` takes: what the value it is given is,
  /// which every pass reads the same way. Nothing where the value has no type
  /// of its own — an integer constant, `nullptr` — which is reported at the
  /// declarator, or where the value was refused, which has been reported. See
  /// docs/decisions/0161-auto-takes-the-type-of-the-value.md.
  [[nodiscard]] std::optional<Meaning> deducedType( Token const& declarator, Expression const& value )
  {
    Typed const kind = expression( value );
    if ( !kind.valid )
    {
      return std::nullopt;
    }
    if ( !kind.type.has_value() || kind.isNull || kind.taken != nullptr || kind.takenProc != nullptr )
    {
      add( diagnostic( diag::DiagnosticId::C_AUTO_WITHOUT_TYPE )
               .at( declarator.location, declarator.length )
               .arg( "name", spellingOf( declarator ) )
               .arg( "what", describe( kind ) ) );
      return std::nullopt;
    }
    Meaning object{
      .kind = NameKind::OBJECT, .type = *kind.type, .enumeration = kind.enumeration, .byte = {}, .owner = {}
    };
    object.pointee = kind.pointee;
    object.aggregate = kind.aggregate;
    return object;
  }

  std::optional<Meaning> declaredType( Token const& token )
  {
    if ( token.kind != TokenKind::IDENTIFIER )
    {
      return Meaning{ .kind = NameKind::OBJECT,
                      .type = typeNamed( token.keyword ),
                      .enumeration = std::nullopt,
                      .byte = {},
                      .owner = {} };
    }
    std::optional<Meaning> const named = mNames->resolve( *mUnit, mSources->textOf( token.span() ) );
    if ( !named.has_value() || named->kind != NameKind::TYPE )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_A_TYPE )
               .at( token.location, token.length )
               .arg( "name", spellingOf( token ) ) );
      return std::nullopt;
    }

    Meaning object{
      .kind = NameKind::OBJECT, .type = named->type, .enumeration = named->enumeration, .byte = {}, .owner = {}
    };
    object.aggregate = named->aggregate;
    object.function = named->function;
    return object;
  }

  /// The place of the enumerator `NAME::NAME` names in its type, where it
  /// names one.
  [[nodiscard]] std::optional<std::uint32_t> enumeratorIndex( Expression const& node ) const
  {
    return mNames->enumeratorIndex( *mUnit, node );
  }

  /// What a name of the program means in this file, without a word where it
  /// names nothing.
  [[nodiscard]] std::optional<Meaning> lookupName( std::string_view spelling ) const
  {
    return mNames->resolve( *mUnit, spelling );
  }

  /// A name as it is read here, by its spelling: a local first, then the
  /// program's.
  [[nodiscard]] std::optional<Meaning> lookupSpelling( std::string_view spelling ) const
  {
    if ( std::optional<Meaning> const local = declared( spelling ); local.has_value() )
    {
      return local;
    }
    return mNames->resolve( *mUnit, spelling );
  }

  /// A name as it is read here, without a word where it names nothing.
  [[nodiscard]] std::optional<Meaning> lookup( Expression const& identifier ) const
  {
    std::string_view const spelling = mSources->textOf( identifier.token.span() );
    if ( std::optional<Meaning> const local = declared( spelling ); local.has_value() )
    {
      return local;
    }
    return mNames->resolve( *mUnit, spelling );
  }

  /// The value of an expression of constants, as `folded` has it, its names
  /// read here.
  [[nodiscard]] std::int64_t fold( Expression const& node, std::optional<ir::Type> type ) const
  {
    NameValue const valueOf = [this]( Expression const& name ) -> std::int64_t
    {
      if ( name.kind == ExpressionKind::QUALIFIED_NAME )
      {
        return enumeratorIndex( name ).value_or( 0 );
      }
      if ( name.kind == ExpressionKind::SIZEOF )
      {
        return sizeCounted(
                   name, [this]( Expression const& read ) { return lookup( read ); }, aggregateOf(), *mSources )
            .value_or( 0 );
      }
      std::optional<Meaning> const meaning = lookup( name );
      return meaning.has_value() ? meaning->value.value_or( 0 ) : 0;
    };
    return folded( node, type, valueOf );
  }

  /// Gives the innermost local of that name what it was found to be once its
  /// declaration was read.
  void settleLocal( Token const& name, Meaning const& meaning )
  {
    if ( auto const found = mScopes.back().find( spellingOf( name ) ); found != mScopes.back().end() )
    {
      found->second = meaning;
    }
  }

  /// A kind of value as a finding names it.
  [[nodiscard]] std::string describe( Typed const& kind ) const
  {
    if ( kind.isNull )
    {
      return "`nullptr`";
    }
    if ( kind.aggregate.has_value() )
    {
      return "a `" + mNames->aggregate( *kind.aggregate ).name + "`";
    }
    if ( std::optional<Pointee> const pointee = kind.pointee; pointee.has_value() )
    {
      std::string target = pointee->enumeration.has_value() ? mNames->enumeration( *pointee->enumeration ).name
                                                            : std::string{ ir::spellingOf( pointee->type ) };
      if ( pointee->aggregate.has_value() )
      {
        target = mNames->aggregate( *pointee->aggregate ).name;
      }
      return "a `" + std::string{ pointee->isConst ? "const " : "" } +
             std::string{ pointee->isVolatile ? "volatile " : "" } + target + "*`";
    }
    if ( kind.enumeration.has_value() )
    {
      return "a `" + mNames->enumeration( *kind.enumeration ).name + "`";
    }
    if ( kind.type.has_value() )
    {
      return "a `" + std::string{ ir::spellingOf( *kind.type ) } + "`";
    }
    return "an integer constant";
  }

  /// Where the calls the expressions typed from now on make are written
  /// down, or nowhere.
  void recordCalls( std::vector<CallSite>* into )
  {
    mCallSites = into;
  }

  /// Where the locals whose address is taken are written down, by where they
  /// are declared, or nowhere.
  void recordAddresses( std::set<std::uint32_t>* into )
  {
    mAddressed = into;
  }

  /// A call through a pointer of a function type, held to the type's
  /// signature: every member reads the arguments from the same Temporaries,
  /// so the type is the whole of what a call may know — see
  /// docs/decisions/0065-handlers.md.
  Typed throughPointer( Expression const& node, FunctionType const& type, std::uint32_t which, bool read )
  {
    Expression const& target = *node.left;
    if ( mMembers != nullptr && mWithin != nullptr )
    {
      mMembers->callsOfType.push_back(
          Membership::CallOfType{ .caller = mWithin, .type = which, .span = node.span, .sink = mSink } );
    }
    if ( type.parameters.size() != node.arguments.size() )
    {
      add( diagnostic( diag::DiagnosticId::C_ARGUMENT_COUNT )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) )
               .arg( "given", static_cast<std::int64_t>( node.arguments.size() ) )
               .arg( "count", static_cast<std::int64_t>( type.parameters.size() ) ) );
    }
    bool const paired = type.parameters.size() == node.arguments.size();
    for ( std::size_t index = 0; index < node.arguments.size(); ++index )
    {
      Expression const& argument = *node.arguments[index];
      Typed const kind = expression( argument );
      if ( paired && kind.valid )
      {
        NamedType const& parameter = type.parameters[index].type;
        assignable( argument, kind, parameter.type, parameter.enumeration, parameter.pointee, parameter.aggregate );
      }
    }
    if ( !read )
    {
      return typed( ir::Type::U8 );
    }
    if ( !type.result.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOTHING_RETURNED_READ )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) ) );
      return refused();
    }
    return typedAs( *type.result );
  }

  /// A call: of a name, and one that can be called, with an argument for each
  /// parameter, each assignable to it. Read as a value where `read` says, and
  /// then of the callee's result, which it must have — see
  /// docs/decisions/0082-a-call-writes-the-callees-bytes.md.
  Typed call( Expression const& node, bool read )
  {
    Expression const& target = *node.left;
    std::optional<Meaning> meaning;
    if ( target.kind != ExpressionKind::IDENTIFIER )
    {
      refuse( node.span, "a call through an expression" );
    }
    else
    {
      meaning = resolve( target, true );
    }

    if ( meaning.has_value() && meaning->kind == NameKind::ASSEMBLER_PROC )
    {
      return assemblerCall( node, *meaning, read );
    }
    // A call through a pointer of a function type: the arguments go to the
    // type's Temporaries and the result comes from its own, so what a call is
    // held to is the type's signature — see docs/decisions/0065-handlers.md.
    if ( meaning.has_value() && meaning->kind == NameKind::OBJECT && meaning->pointee.has_value() &&
         meaning->pointee->function.has_value() )
    {
      return throughPointer(
          node, mNames->functionType( *meaning->pointee->function ), *meaning->pointee->function, read );
    }
    Definition const* const callee =
        meaning.has_value() && meaning->kind == NameKind::FUNCTION ? meaning->definition : nullptr;
    if ( meaning.has_value() && ( meaning->kind == NameKind::OBJECT || meaning->kind == NameKind::TYPE ||
                                  meaning->kind == NameKind::ASSEMBLER_OTHER ) )
    {
      name( diag::DiagnosticId::C_NOT_A_FUNCTION, target );
    }
    if ( callee != nullptr && callee->parameters.size() != node.arguments.size() )
    {
      add( diagnostic( diag::DiagnosticId::C_ARGUMENT_COUNT )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) )
               .arg( "given", static_cast<std::int64_t>( node.arguments.size() ) )
               .arg( "count", static_cast<std::int64_t>( callee->parameters.size() ) ) );
    }
    bool const paired = callee != nullptr && callee->parameters.size() == node.arguments.size();
    for ( std::size_t index = 0; index < node.arguments.size(); ++index )
    {
      Expression const& argument = *node.arguments[index];
      Typed const kind = expression( argument );
      if ( kind.valid && kind.aggregate.has_value() && throughStripes( argument ) )
      {
        refuse( argument.span, "a struct in a striped array given whole as an argument" );
      }
      if ( paired && kind.valid )
      {
        NamedType const& parameter = callee->parameters[index].type;
        assignable( argument, kind, parameter.type, parameter.enumeration, parameter.pointee, parameter.aggregate );
      }
    }

    if ( callee == nullptr )
    {
      return refused();
    }
    if ( mCallSites != nullptr )
    {
      mCallSites->push_back( CallSite{ .callee = callee, .span = node.span, .external = nullptr } );
    }
    if ( !read )
    {
      return typed( ir::Type::U8 );
    }
    if ( !callee->result.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOTHING_RETURNED_READ )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) ) );
      return refused();
    }
    return paired ? typedAs( *callee->result ) : refused();
  }

  /// Whether a call or a step stands anywhere in the expression: a compound
  /// assignment reaches its target twice in the tree and once in the text, so
  /// either there would be taken twice where the source wrote it once — see
  /// docs/decisions/0158-a-compound-assignment-and-a-step.md and
  /// docs/decisions/0198-a-compound-assignment-is-a-value.md.
  [[nodiscard]] static bool holdsCallOrStep( Expression const& node )
  {
    if ( node.kind == ExpressionKind::CALL || ( node.kind == ExpressionKind::ASSIGNMENT && node.compound ) )
    {
      return true;
    }
    if ( ( node.left != nullptr && holdsCallOrStep( *node.left ) ) ||
         ( node.right != nullptr && holdsCallOrStep( *node.right ) ) )
    {
      return true;
    }
    return std::ranges::any_of( node.arguments,
                                []( ExpressionPtr const& argument )
                                { return argument != nullptr && holdsCallOrStep( *argument ); } );
  }

  /// An assignment held to its rules — the target assignable, the value of the
  /// target's type — wherever it is written, as a statement or as a value:
  /// what it assigns to, or nothing where either side was refused.
  std::optional<Meaning> assignment( Expression const& node )
  {
    Rooted const rooted( mRoot, node );
    Expression const& target = *node.left;
    if ( node.compound && holdsCallOrStep( target ) )
    {
      refuse( target.span, "a call or a step in what `+=`, `++` or their kin assign to" );
      return std::nullopt;
    }
    std::optional<Meaning> object;
    if ( target.kind == ExpressionKind::INDEX || target.kind == ExpressionKind::DEREFERENCE ||
         target.kind == ExpressionKind::MEMBER )
    {
      // An element, or what a pointer points at, is assigned as an object of
      // its type, unless it is `const`; a member that is an array is not.
      Typed const reached = expression( target );
      if ( std::optional<Shape> const place = shape( target );
           reached.valid && place.has_value() && place->isArray && target.kind == ExpressionKind::MEMBER )
      {
        add( diagnostic( diag::DiagnosticId::C_ARRAY_ASSIGNED )
                 .at( target.span.begin, target.span.length )
                 .arg( "name", textOf( target.span ) ) );
      }
      else if ( reached.valid && reached.readOnly )
      {
        if ( reached.throughPointer )
        {
          add( diagnostic( diag::DiagnosticId::C_WRITE_THROUGH_CONST ).at( target.span.begin, target.span.length ) );
        }
        else
        {
          Expression const* root = target.left.get();
          while ( root->left != nullptr )
          {
            root = root->left.get();
          }
          name( diag::DiagnosticId::C_CONST_ASSIGNED, *root );
        }
      }
      else if ( reached.valid )
      {
        object = Meaning{ .kind = NameKind::OBJECT,
                          .type = reached.type.value_or( ir::Type::U8 ),
                          .enumeration = reached.enumeration,
                          .byte = {},
                          .owner = {} };
        object->pointee = reached.pointee;
        object->aggregate = reached.aggregate;
      }
    }
    else if ( target.kind != ExpressionKind::IDENTIFIER )
    {
      refuse( target.span, "an assignment to an expression" );
    }
    else if ( std::optional<Meaning> const meaning = resolve( target ); meaning.has_value() )
    {
      if ( meaning->kind == NameKind::OBJECT && meaning->isArray )
      {
        name( diag::DiagnosticId::C_ARRAY_ASSIGNED, target );
      }
      else if ( meaning->kind == NameKind::OBJECT && meaning->isConst )
      {
        name( diag::DiagnosticId::C_CONST_ASSIGNED, target );
      }
      else if ( meaning->kind == NameKind::FUNCTION || meaning->kind == NameKind::ASSEMBLER_PROC )
      {
        name( diag::DiagnosticId::C_NOT_ASSIGNABLE, target );
      }
      else if ( meaning->kind == NameKind::TYPE )
      {
        name( diag::DiagnosticId::C_NOT_DECLARED, target );
      }
      else
      {
        object = meaning;
      }
    }

    Typed const value = expression( *node.right );
    if ( !object.has_value() || !value.valid )
    {
      return std::nullopt;
    }
    assignable( *node.right, value, object->type, object->enumeration, object->pointee, object->aggregate );
    return object;
  }

  /// The name at the root of what an assignment assigns to: the object, the
  /// array an element is of, or the pointer a pointee is reached through.
  [[nodiscard]] static Expression const& rootOf( Expression const& target )
  {
    Expression const* root = &target;
    while ( root->left != nullptr && root->kind != ExpressionKind::IDENTIFIER )
    {
      root = root->left.get();
    }
    return *root;
  }

  /// How often `spelling` is reached in an expression as the source wrote it,
  /// `stepped` — the step being held to the rule — and the copy of its target
  /// every compound assignment's tree holds not counting, the source having
  /// written neither — see
  /// docs/decisions/0198-a-compound-assignment-is-a-value.md.
  [[nodiscard]] std::uint32_t
  reaches( Expression const& node, std::string_view spelling, Expression const& stepped ) const
  {
    if ( &node == &stepped )
    {
      return 0;
    }
    if ( node.kind == ExpressionKind::IDENTIFIER )
    {
      return mSources->textOf( node.token.span() ) == spelling ? 1 : 0;
    }
    std::uint32_t count = 0;
    // A member's name is the member's, not an object's, and an enumerator's
    // is its `enum struct`'s.
    bool const named = node.kind == ExpressionKind::MEMBER || node.kind == ExpressionKind::QUALIFIED_NAME;
    if ( node.left != nullptr )
    {
      count += reaches( *node.left, spelling, stepped );
    }
    if ( node.right != nullptr && !named )
    {
      // `a += b` is `a = a + b`: the target under the operator is the copy the
      // parser wrote, and only what stands beside it is the source's.
      Expression const& value = *node.right;
      bool const desugared = node.kind == ExpressionKind::ASSIGNMENT && node.compound &&
                             value.kind == ExpressionKind::BINARY && value.right != nullptr;
      count += desugared ? reaches( *value.right, spelling, stepped ) : reaches( value, spelling, stepped );
    }
    for ( ExpressionPtr const& argument : node.arguments )
    {
      if ( argument != nullptr )
      {
        count += reaches( *argument, spelling, stepped );
      }
    }
    return count;
  }

  /// An assignment read for its value: `a = b` is none of them, and `a += b`,
  /// `++a` and `a++` are — what the target holds after the step, or before it
  /// where the operator was written after the target — see
  /// docs/decisions/0198-a-compound-assignment-is-a-value.md.
  Typed assigned( Expression const& node )
  {
    Rooted const rooted( mRoot, node );
    if ( !node.compound )
    {
      refuse( node.span, "an assignment used as a value" );
      return refused();
    }
    std::optional<Meaning> const object = assignment( node );
    if ( !object.has_value() )
    {
      return refused();
    }
    if ( Expression const& root = rootOf( *node.left );
         mRoot != nullptr && root.kind == ExpressionKind::IDENTIFIER &&
         reaches( *mRoot, mSources->textOf( root.token.span() ), node ) > 0 )
    {
      add( diagnostic( diag::DiagnosticId::C_STEP_REACHED_AGAIN )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( root.token ) )
               .arg( "operator", spellingOf( node.token ) ) );
      return refused();
    }
    Typed result = typed( object->type );
    result.enumeration = object->enumeration;
    result.pointee = object->pointee;
    result.aggregate = object->aggregate;
    return result;
  }

  /// An expression read for its value.
  Typed expression( Expression const& node )
  {
    Rooted const rooted( mRoot, node );
    switch ( node.kind )
    {
    case ExpressionKind::INTEGER_CONSTANT:
      return constant();
    case ExpressionKind::STRING_LITERAL:
    {
      // A `const u8*` to a Section of its bytes, whose address the assembler
      // knows.
      charsetOf( node.token );
      Typed result = pointing( Pointee{ .type = ir::Type::U8, .enumeration = std::nullopt, .isConst = true } );
      result.address = true;
      return result;
    }
    case ExpressionKind::CHARACTER_CONSTANT:
    {
      charsetOf( node.token );
      if ( !isTranslated( node ) )
      {
        return constant();
      }
      // A byte only the assembler knows, once the Charset is resolved.
      Typed result = typed( ir::Type::U8 );
      result.address = true;
      return result;
    }
    case ExpressionKind::PREDEFINED_CONSTANT:
    {
      Typed result = typed( ir::Type::BOOL );
      result.known = true;
      return result;
    }
    case ExpressionKind::IDENTIFIER:
      return identifier( node );
    case ExpressionKind::QUALIFIED_NAME:
      return qualified( node );
    case ExpressionKind::CALL:
      return call( node, true );
    case ExpressionKind::ASSIGNMENT:
      return assigned( node );
    case ExpressionKind::UNARY:
      return unary( node );
    case ExpressionKind::CAST:
      return cast( node );
    case ExpressionKind::INDEX:
      return element( node );
    case ExpressionKind::SIZEOF:
      return sizeOf( node );
    case ExpressionKind::DEREFERENCE:
      return dereference( node );
    case ExpressionKind::ADDRESS:
      return addressOf( node );
    case ExpressionKind::NULL_POINTER:
    {
      Typed result = typed( ir::Type::POINTER );
      result.isNull = true;
      return result;
    }
    case ExpressionKind::MEMBER:
      return member( node );
    case ExpressionKind::LIST:
      add( diagnostic( diag::DiagnosticId::C_LIST_SHAPE )
               .at( node.span.begin, node.span.length )
               .arg( "what", std::string{ "a value" } )
               .arg( "given", std::string{ "a list" } ) );
      return refused();
    case ExpressionKind::BINARY:
      return binary( node );
    case ExpressionKind::CONDITIONAL:
      return conditional( node );
    }
    return refused();
  }

  /// `c ? a : b`: a `bool` decides, and the ways are of one type, which the
  /// answer is. A constant takes the other way's type, and two constants
  /// take the type of where the answer is used, as one constant does — see
  /// docs/decisions/0169-a-conditional-operator.md.
  Typed conditional( Expression const& node )
  {
    bool const decided = condition( *node.left );
    Typed const whenTrue = expression( *node.arguments.front() );
    Typed const whenFalse = expression( *node.arguments.back() );
    if ( !decided || !whenTrue.valid || !whenFalse.valid )
    {
      return refused();
    }
    if ( whenTrue.aggregate.has_value() || whenFalse.aggregate.has_value() )
    {
      refuse( node.span, "a `?:` of a `struct` or a `union`" );
      return refused();
    }
    if ( !whenTrue.type.has_value() && !whenFalse.type.has_value() )
    {
      // Two constants: the answer takes its type from where it is used, as
      // one constant does, but is no constant itself — a condition decides
      // it where the program runs.
      Typed result = constant();
      result.atRun = true;
      return result;
    }
    if ( !whenTrue.type.has_value() || !whenFalse.type.has_value() )
    {
      // One constant beside a value: the constant is held to the value's
      // type, as it would be assigned to an object of it.
      bool const constantFirst = !whenTrue.type.has_value();
      Typed const& typed = constantFirst ? whenFalse : whenTrue;
      Expression const& given = constantFirst ? *node.arguments.front() : *node.arguments.back();
      assignable( given,
                  constantFirst ? whenTrue : whenFalse,
                  typed.type.value_or( ir::Type::U8 ),
                  typed.enumeration,
                  typed.pointee );
      Typed result = typed;
      result.known = false;
      result.address = false;
      return result;
    }
    if ( whenTrue.type != whenFalse.type || whenTrue.enumeration != whenFalse.enumeration ||
         ( whenTrue.pointee.has_value() != whenFalse.pointee.has_value() ) ||
         ( whenTrue.pointee.has_value() && !samePointee( *whenTrue.pointee, *whenFalse.pointee ) ) )
    {
      add( diagnostic( diag::DiagnosticId::C_TYPES_DIFFER )
               .at( node.span.begin, node.span.length )
               .arg( "left", describe( whenTrue ) )
               .arg( "right", describe( whenFalse ) ) );
      return refused();
    }
    Typed result = whenTrue;
    result.known = false;
    result.address = false;
    // What either way points at is `const` or `volatile` where either is.
    if ( result.pointee.has_value() && whenFalse.pointee.has_value() )
    {
      result.pointee->isConst = whenTrue.pointee->isConst || whenFalse.pointee->isConst;
      result.pointee->isVolatile = whenTrue.pointee->isVolatile || whenFalse.pointee->isVolatile;
    }
    result.readOnly = whenTrue.readOnly || whenFalse.readOnly;
    return result;
  }

  /// A call of an assembler's `.proc`, held to its Signature as a call of C is
  /// to its parameters: scalars only — see
  /// docs/decisions/0092-what-the-assembler-exports-is-typed-by-its-shape.md#calls--task-4b.
  Typed assemblerCall( Expression const& node, Meaning const& meaning, bool read )
  {
    Expression const& target = *node.left;
    if ( mCallSites != nullptr )
    {
      mCallSites->push_back( CallSite{ .callee = nullptr, .span = node.span, .external = meaning.external } );
    }
    if ( meaning.definition != nullptr )
    {
      return declaredCall( node, *meaning.definition, meaning.external, read );
    }
    ExternalName const& proc = *meaning.external;
    if ( proc.arguments.size() != node.arguments.size() )
    {
      add( diagnostic( diag::DiagnosticId::C_ARGUMENT_COUNT )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) )
               .arg( "given", static_cast<std::int64_t>( node.arguments.size() ) )
               .arg( "count", static_cast<std::int64_t>( proc.arguments.size() ) ) );
    }
    bool const handsBytes =
        std::ranges::any_of( proc.arguments, []( ExternalByte const& byte ) { return byte.isBytes; } ) ||
        ( proc.result.has_value() && proc.result->isBytes );
    if ( handsBytes )
    {
      add( diagnostic( diag::DiagnosticId::C_BYTES_HANDED_OVER )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) ) );
    }
    bool const paired = proc.arguments.size() == node.arguments.size();
    for ( std::size_t index = 0; index < node.arguments.size(); ++index )
    {
      Expression const& argument = *node.arguments[index];
      Typed const kind = expression( argument );
      if ( paired && kind.valid && !proc.arguments[index].isBytes )
      {
        assignable( argument, kind, proc.arguments[index].type, std::nullopt, std::nullopt, std::nullopt );
      }
    }
    if ( !read )
    {
      return handsBytes ? refused() : typed( ir::Type::U8 );
    }
    if ( !proc.result.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOTHING_RETURNED_READ )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) ) );
      return refused();
    }
    return paired && !handsBytes ? typed( proc.result->type ) : refused();
  }

  /// A call of a Proc of the assembler an `extern` declares, held to the
  /// declaration as a call of C is to its function's; the declaration is held
  /// to the Proc where it stands — see
  /// docs/decisions/0094-extern-declares-how-c-calls-a-proc.md.
  Typed declaredCall( Expression const& node, Definition const& declared, ExternalName const* proc, bool read )
  {
    Expression const& target = *node.left;
    if ( declared.parameters.size() != node.arguments.size() )
    {
      add( diagnostic( diag::DiagnosticId::C_ARGUMENT_COUNT )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) )
               .arg( "given", static_cast<std::int64_t>( node.arguments.size() ) )
               .arg( "count", static_cast<std::int64_t>( declared.parameters.size() ) ) );
    }
    bool const paired = declared.parameters.size() == node.arguments.size();
    for ( std::size_t index = 0; index < node.arguments.size(); ++index )
    {
      Expression const& argument = *node.arguments[index];
      Typed const kind = expression( argument );
      if ( kind.valid && kind.aggregate.has_value() && throughStripes( argument ) )
      {
        refuse( argument.span, "a struct in a striped array given whole as an argument" );
      }
      if ( paired && kind.valid )
      {
        NamedType const& parameter = declared.parameters[index].type;
        assignable( argument, kind, parameter.type, parameter.enumeration, parameter.pointee, parameter.aggregate );
      }
    }
    bool const proper = proc != nullptr && proc->kind == ExternalKind::PROC;
    if ( !read )
    {
      return proper ? typed( ir::Type::U8 ) : refused();
    }
    if ( !declared.result.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOTHING_RETURNED_READ )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) ) );
      return refused();
    }
    return paired && proper ? typedAs( *declared.result ) : refused();
  }

  /// A condition: a `bool`, or `!`, `&&` and `||` over conditions — see
  /// docs/decisions/0079-a-condition-is-a-bool.md. True where it is one.
  bool condition( Expression const& node )
  {
    if ( node.kind == ExpressionKind::BINARY && isLogical( node.token.kind ) )
    {
      bool const left = condition( *node.left );
      bool const right = condition( *node.right );
      return left && right;
    }
    if ( node.kind == ExpressionKind::UNARY && node.token.kind == TokenKind::BANG )
    {
      return condition( *node.left );
    }
    Typed const kind = expression( node );
    if ( !kind.valid )
    {
      return false;
    }
    if ( kind.type == ir::Type::BOOL )
    {
      return true;
    }
    add( diagnostic( diag::DiagnosticId::C_CONDITION_NOT_BOOL )
             .at( node.span.begin, node.span.length )
             .arg( "what", describe( kind ) ) );
    return false;
  }

  /// Whether a value of `kind` may be assigned to an object of `target`, and
  /// of the `enum struct` `targetEnumeration` where it is of one, reporting
  /// where it may not.
  void assignable( Expression const& value,
                   Typed const& kind,
                   ir::Type target,
                   std::optional<std::uint32_t> targetEnumeration,
                   std::optional<Pointee> const& targetPointee = std::nullopt,
                   std::optional<std::uint32_t> targetAggregate = std::nullopt )
  {
    // A `struct` or a `union` takes a value of its own type, copied — see
    // docs/decisions/0086-a-struct-by-value.md.
    if ( targetAggregate.has_value() || kind.aggregate.has_value() )
    {
      if ( kind.aggregate != targetAggregate )
      {
        Typed const object = targetAggregate.has_value() ? blockOf( *targetAggregate ) : typed( target );
        differ( value, object, kind );
        return;
      }
      if ( mNames->aggregate( *targetAggregate ).size > MAX_COPY_BYTES )
      {
        refuse( value.span, "a copy of more than 256 bytes" );
      }
      return;
    }
    // The address of a function goes to a pointer of a function type, whose
    // signature it must have; the taking is what makes it a member — see
    // docs/decisions/0065-handlers.md.
    if ( kind.taken != nullptr || kind.takenProc != nullptr )
    {
      takesFunction( value, targetPointee, kind );
      return;
    }
    // A pointer takes `nullptr`, or a pointer to its own type that adds `const`
    // or `volatile` or keeps it.
    if ( targetPointee.has_value() || kind.pointee.has_value() || kind.isNull )
    {
      bool const fits = targetPointee.has_value() &&
                        ( kind.isNull || ( kind.pointee.has_value() && samePointee( *kind.pointee, *targetPointee ) &&
                                           ( !kind.pointee->isConst || targetPointee->isConst ) &&
                                           ( !kind.pointee->isVolatile || targetPointee->isVolatile ) ) );
      if ( !fits )
      {
        Typed object = targetEnumeration.has_value() ? enumerated( *targetEnumeration ) : typed( target );
        object.pointee = targetPointee;
        pointersDiffer( value, object, kind );
      }
      return;
    }
    if ( kind.enumeration.has_value() || targetEnumeration.has_value() )
    {
      if ( kind.enumeration != targetEnumeration )
      {
        Typed const object = targetEnumeration.has_value() ? enumerated( *targetEnumeration ) : typed( target );
        differ( value, object, kind );
      }
      return;
    }
    if ( !kind.type.has_value() )
    {
      if ( target == ir::Type::BOOL )
      {
        mixedBool( value );
        return;
      }
      constantFits( value, target );
      return;
    }

    ir::Type const source = *kind.type;
    if ( ( source == ir::Type::BOOL ) != ( target == ir::Type::BOOL ) )
    {
      mixedBool( value );
    }
    else if ( target == ir::Type::BOOL )
    {
      return;
    }
    else if ( ir::isSigned( source ) != ir::isSigned( target ) )
    {
      add( diagnostic( diag::DiagnosticId::C_MIXED_SIGNEDNESS )
               .at( value.span.begin, value.span.length )
               .arg( "left", std::string{ ir::spellingOf( target ) } )
               .arg( "right", std::string{ ir::spellingOf( source ) } ) );
    }
    else if ( ir::sizeOf( source ) > ir::sizeOf( target ) )
    {
      add( diagnostic( diag::DiagnosticId::C_NARROWING_ASSIGNMENT )
               .at( value.span.begin, value.span.length )
               .arg( "from", std::string{ ir::spellingOf( source ) } )
               .arg( "to", std::string{ ir::spellingOf( target ) } ) );
    }
  }

  /// Whether an expression of constants folds into a value `type` holds,
  /// reported where it is written. What is held is the answer and not the
  /// numbers on the way to it — `256 - 1` is 255 and stands where a `u8` is
  /// wanted, `200 + 100` is 300 and does not — see
  /// docs/decisions/0206-a-constant-is-folded-exactly.md. A `?:` is two
  /// constants and neither of them is the answer, so each way is held on its
  /// own; a `-` written directly before a constant is its sign, as the fold
  /// reads it.
  bool constantFits( Expression const& node, ir::Type type )
  {
    if ( node.kind == ExpressionKind::CONDITIONAL )
    {
      bool const whenTrue = constantFits( *node.arguments.front(), type );
      bool const whenFalse = constantFits( *node.arguments.back(), type );
      return whenTrue && whenFalse;
    }
    if ( !foldsHere( node ) )
    {
      return true;
    }
    std::int64_t const value = fold( node, type );
    if ( value >= lowestOf( type ) && value <= highestOf( type ) )
    {
      return true;
    }
    add( diagnostic( diag::DiagnosticId::C_CONSTANT_OUT_OF_RANGE )
             .at( node.span.begin, node.span.length )
             .arg( "value", value )
             .arg( "type", std::string{ ir::spellingOf( type ) } ) );
    return false;
  }

  /// Whether every name of an expression of constants has a value here, so
  /// that the fold is the expression's own answer: a Constant the assembler
  /// keeps as a name has none, and a character of a Charset is the
  /// assembler's to translate, so neither is judged against a type.
  [[nodiscard]] bool foldsHere( Expression const& node ) const
  {
    switch ( node.kind )
    {
    case ExpressionKind::INTEGER_CONSTANT:
    case ExpressionKind::SIZEOF:
    case ExpressionKind::QUALIFIED_NAME:
      return true;
    case ExpressionKind::IDENTIFIER:
    {
      std::optional<Meaning> const meaning = lookup( node );
      return meaning.has_value() && isFoldedConstant( *meaning );
    }
    case ExpressionKind::CAST:
    case ExpressionKind::UNARY:
      return foldsHere( *node.left );
    case ExpressionKind::BINARY:
      return foldsHere( *node.left ) && foldsHere( *node.right );
    default:
      return false;
    }
  }

  /// Whether an element of a type lies in stripes: a `u16` or an `i16`, its
  /// low bytes and its high ones, or a `struct` of members that do, a member
  /// of one byte a stripe of its own; no union, whose members overlap, and no
  /// member array, which a second index would reach.
  [[nodiscard]] bool stripable( ir::Type type, std::optional<std::uint32_t> aggregateIndex ) const
  {
    if ( !aggregateIndex.has_value() )
    {
      return type == ir::Type::U16 || type == ir::Type::I16;
    }
    AggregateType const& type2 = mNames->aggregate( *aggregateIndex );
    if ( type2.isUnion )
    {
      return false;
    }
    return std::ranges::all_of( type2.members,
                                [this]( MemberType const& member )
                                {
                                  return !member.isArray &&
                                         ( !member.type.aggregate.has_value() || member.type.pointee.has_value() ||
                                           stripable( member.type.type, member.type.aggregate ) );
                                } );
  }

  /// Whether a place lies in a striped array's stripes: an element of one, or a
  /// member of an element by `.`.
  [[nodiscard]] bool throughStripes( Expression const& node ) const
  {
    Expression const* at = &node;
    while ( ( at->kind == ExpressionKind::MEMBER && at->token.kind == TokenKind::DOT ) ||
            at->kind == ExpressionKind::INDEX )
    {
      if ( at->kind == ExpressionKind::INDEX && at->left->kind == ExpressionKind::IDENTIFIER )
      {
        std::optional<Meaning> const array = lookup( *at->left );
        return array.has_value() && array->isStriped;
      }
      at = at->left.get();
    }
    return false;
  }

  /// Whether a character constant is translated by a Charset, and so no
  /// constant where it is compiled.
  [[nodiscard]] bool isTranslated( Expression const& node ) const
  {
    std::string_view const text = node.spelling.empty() ? mSources->textOf( node.token.span() ) : node.spelling;
    return !text.empty() && text.front() != '\'' && text.front() != '"';
  }

  /// A literal's prefix, held to be a Charset the assembler exports, reported
  /// once for the literal however many characters it holds.
  void charsetOf( Token const& token )
  {
    std::string_view const text = mSources->textOf( token.span() );
    std::string_view const prefix = text.substr( 0, text.find_first_of( "\"'" ) );
    if ( prefix.empty() || !mCheckedPrefixes.insert( token.location.rawOffset() ).second )
    {
      return;
    }
    ExternalName const* const external = mNames->externalNamed( prefix );
    if ( external == nullptr )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_DECLARED )
               .at( token.location, static_cast<std::uint32_t>( prefix.size() ) )
               .arg( "name", std::string{ prefix } ) );
      return;
    }
    if ( external->kind != ExternalKind::CHARSET )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_A_CHARSET )
               .at( token.location, static_cast<std::uint32_t>( prefix.size() ) )
               .arg( "name", std::string{ prefix } ) );
    }
  }

  void refuse( diag::SourceSpan span, std::string_view construct )
  {
    add( diagnostic( diag::DiagnosticId::C_NOT_COMPILED_YET )
             .at( span.begin, span.length )
             .arg( "construct", std::string{ construct } ) );
  }

  void name( diag::DiagnosticId id, Expression const& identifier )
  {
    add( diagnostic( id )
             .at( identifier.token.location, identifier.token.length )
             .arg( "name", spellingOf( identifier.token ) ) );
  }

  /// A name of the assembler's that C reads nothing of: a Label with no
  /// shape, a `reserved` Region, or a Symbol of a kind C does not read — see
  /// docs/decisions/0092-what-the-assembler-exports-is-typed-by-its-shape.md.
  void unread( Expression const& identifier, ExternalName const& external )
  {
    unread( identifier.token, external );
  }

  void unread( Token const& token, ExternalName const& external )
  {
    diag::DiagnosticId id = diag::DiagnosticId::C_NOT_READ;
    if ( external.kind == ExternalKind::SHAPELESS )
    {
      id = diag::DiagnosticId::C_NO_SHAPE;
    }
    else if ( external.kind == ExternalKind::RESERVED )
    {
      id = diag::DiagnosticId::C_RESERVED_REGION;
    }
    diag::Diagnostic finding = diagnostic( id )
                                   .at( token.location, token.length )
                                   .arg( "name", spellingOf( token ) )
                                   .arg( "reason", external.reason );
    if ( external.definition.has_value() )
    {
      finding = std::move( finding ).note( diagnostic( diag::DiagnosticId::C_DEFINED_HERE )
                                               .at( external.definition->begin, external.definition->length )
                                               .arg( "name", external.name ) );
    }
    add( std::move( finding ) );
  }

  /// Whether a function's signature is the type's, and what differs where it
  /// is not: what a taking asks of `&f`, and what an `[[implements]]` asks of
  /// what fills a Slot that takes or returns.
  [[nodiscard]] std::optional<std::string> signatureDiffers( FunctionType const& type,
                                                             Definition const& function ) const
  {
    if ( function.parameters.size() != type.parameters.size() )
    {
      return "it takes " + std::to_string( function.parameters.size() ) + " and the type " +
             std::to_string( type.parameters.size() );
    }
    for ( std::size_t index = 0; index < type.parameters.size(); ++index )
    {
      if ( !sameType( function.parameters[index].type, type.parameters[index].type ) )
      {
        return "its parameter " + std::to_string( index + 1 ) + " is " +
               describe( typedAs( function.parameters[index].type ) ) + " and the type's " +
               describe( typedAs( type.parameters[index].type ) );
      }
    }
    if ( function.result.has_value() != type.result.has_value() ||
         ( function.result.has_value() && !sameType( *function.result, *type.result ) ) )
    {
      return "its result is not the type's";
    }
    return std::nullopt;
  }

  /// A function written `[[implements(SLOT)]]` of a Slot that takes or
  /// returns joins the Slot's own function type, so that it reads the Slot's
  /// Temporaries and declares none of its own — see
  /// docs/decisions/0174-a-slot-takes-and-returns.md.
  void joinType( Definition const* function, std::uint32_t type )
  {
    if ( mMembers != nullptr && function != nullptr )
    {
      mMembers->ofFunction.try_emplace( function, type );
    }
  }

private:
  void aggregateOperand( Expression const& node )
  {
    add( diagnostic( diag::DiagnosticId::C_AGGREGATE_OPERATOR )
             .at( node.span.begin, node.span.length )
             .arg( "what", std::string{ "this" } ) );
  }

  /// `s.m` and `p->m`: a member of a `struct` or a `union`, or of one a
  /// pointer points at, `const` where what holds it is. An array member is a
  /// pointer to its first element, as an array's name is.
  Typed member( Expression const& node )
  {
    Typed const outer = expression( *node.left );
    if ( !outer.valid )
    {
      return refused();
    }
    bool const arrow = node.token.kind == TokenKind::ARROW;
    std::optional<Pointee> const pointee = outer.pointee;
    std::optional<std::uint32_t> aggregateIndex = outer.aggregate;
    if ( arrow )
    {
      aggregateIndex = pointee.has_value() ? pointee->aggregate : std::nullopt;
    }
    if ( !aggregateIndex.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_AN_AGGREGATE )
               .at( node.left->span.begin, node.left->span.length )
               .arg( "what", arrow ? "what " + describe( outer ) + " points at" : describe( outer ) ) );
      return refused();
    }
    AggregateType const& type = mNames->aggregate( *aggregateIndex );
    std::string const name = spellingOf( node.right->token );
    MemberType const* const found = memberNamed( type, name, *mSources );
    if ( found == nullptr )
    {
      add( diagnostic( diag::DiagnosticId::C_NO_SUCH_MEMBER )
               .at( node.right->token.location, node.right->token.length )
               .arg( "type", type.name )
               .arg( "name", name ) );
      return refused();
    }
    bool const readOnly = arrow ? pointee.value_or( Pointee{} ).isConst : outer.readOnly;
    bool const isVolatile = arrow ? pointee.value_or( Pointee{} ).isVolatile : outer.isVolatile;
    Typed result = typedAs( found->type );
    if ( found->isArray )
    {
      result = pointing( Pointee{ .type = found->type.type,
                                  .enumeration = found->type.enumeration,
                                  .isConst = readOnly,
                                  .aggregate = found->type.aggregate,
                                  .function = std::nullopt,
                                  .isVolatile = isVolatile } );
      result.address = fixedPlace( node );
    }
    result.readOnly = readOnly;
    result.isVolatile = isVolatile;
    result.throughPointer = arrow || outer.throughPointer;
    return result;
  }

  void pointersDiffer( Expression const& node, Typed const& left, Typed const& right )
  {
    add( diagnostic( diag::DiagnosticId::C_POINTERS_DIFFER )
             .at( node.span.begin, node.span.length )
             .arg( "left", describe( left ) )
             .arg( "right", describe( right ) ) );
  }

  /// Whether a place is at an address the assembler knows: an object at file
  /// scope, and a member of one by `.` or an element of an array by a constant
  /// index, however deep — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
  bool fixedPlace( Expression const& node )
  {
    switch ( node.kind )
    {
    case ExpressionKind::IDENTIFIER:
    {
      std::optional<Meaning> const meaning = lookup( node );
      return meaning.has_value() && meaning->kind == NameKind::OBJECT && !meaning->pointee.has_value() &&
             atFixedAddress( *meaning ) && !isAssemblerConstant( *meaning );
    }
    case ExpressionKind::MEMBER:
      return node.token.kind == TokenKind::DOT && fixedPlace( *node.left );
    case ExpressionKind::INDEX:
    {
      std::optional<Shape> const outer = shape( *node.left );
      return outer.has_value() && outer->isArray && fixedPlace( *node.left ) && isKnown( expression( *node.right ) );
    }
    default:
      return false;
    }
  }

  /// The object at the root of `s.a.b` or `t[i].a`, whose address `&` takes
  /// with the member's: a local among them is written down as addressed.
  void markRoot( Expression const& node )
  {
    Expression const* root = &node;
    while ( ( root->kind == ExpressionKind::MEMBER && root->token.kind == TokenKind::DOT ) ||
            root->kind == ExpressionKind::INDEX )
    {
      root = root->left.get();
    }
    if ( root->kind != ExpressionKind::IDENTIFIER )
    {
      return;
    }
    if ( std::optional<Meaning> const meaning = lookup( *root ); meaning.has_value() && !meaning->pointee.has_value() )
    {
      addressed( *meaning );
    }
  }

  /// A local whose address is taken, written down so that the lowering gives
  /// it a Section rather than a Temporary.
  void addressed( Meaning const& meaning )
  {
    if ( mAddressed != nullptr && meaning.declaredAt.has_value() )
    {
      mAddressed->insert( meaning.declaredAt->rawOffset() );
    }
  }

  /// A name's value as an operand types it: a pointer, a value of an
  /// `enum struct`, or of its type.
  static Typed valueOf( Meaning const& meaning )
  {
    if ( meaning.pointee.has_value() )
    {
      return pointing( *meaning.pointee );
    }
    if ( meaning.aggregate.has_value() )
    {
      return blockOf( *meaning.aggregate );
    }
    return meaning.enumeration.has_value() ? enumerated( *meaning.enumeration ) : typed( meaning.type );
  }

  /// `*p`: what a pointer points at.
  Typed dereference( Expression const& node )
  {
    Typed const pointer = expression( *node.left );
    if ( !pointer.valid )
    {
      return refused();
    }
    if ( !pointer.pointee.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_A_POINTER )
               .at( node.left->span.begin, node.left->span.length )
               .arg( "what", describe( pointer ) ) );
      return refused();
    }
    return reached( *pointer.pointee );
  }

  /// `&x`: of an object, of an element, or of what a pointer points at. A
  /// parameter has no address to give, being its call's `.ztemp`.
  Typed addressOf( Expression const& node )
  {
    Expression const& operand = *node.left;
    if ( operand.kind == ExpressionKind::DEREFERENCE )
    {
      Typed const pointer = expression( *operand.left );
      if ( pointer.valid && !pointer.pointee.has_value() )
      {
        add( diagnostic( diag::DiagnosticId::C_NOT_A_POINTER )
                 .at( operand.left->span.begin, operand.left->span.length )
                 .arg( "what", describe( pointer ) ) );
        return refused();
      }
      return pointer.valid ? pointing( *pointer.pointee ) : refused();
    }
    if ( operand.kind == ExpressionKind::INDEX && operand.left->kind != ExpressionKind::IDENTIFIER )
    {
      // An element of a member array, or of what a pointer an expression gives
      // points at.
      Typed const element = expression( operand );
      if ( !element.valid )
      {
        return refused();
      }
      markRoot( operand );
      Typed result = pointing( Pointee{ .type = element.type.value_or( ir::Type::U8 ),
                                        .enumeration = element.enumeration,
                                        .isConst = element.readOnly,
                                        .aggregate = element.aggregate,
                                        .function = std::nullopt,
                                        .isVolatile = element.isVolatile } );
      result.address = fixedPlace( operand );
      return result;
    }
    if ( operand.kind == ExpressionKind::INDEX )
    {
      Typed const element = expression( operand );
      std::optional<Meaning> const target = lookup( *operand.left );
      if ( !element.valid || !target.has_value() )
      {
        return refused();
      }
      if ( target->isStriped )
      {
        stripedElement( node, spellingOf( operand.left->token ) );
        return refused();
      }
      if ( target->pointee.has_value() )
      {
        return pointing( *target->pointee );
      }
      addressed( *target );
      Typed result = pointing( Pointee{ .type = target->type,
                                        .enumeration = target->enumeration,
                                        .isConst = target->isConst,
                                        .aggregate = target->aggregate,
                                        .function = std::nullopt,
                                        .isVolatile = target->isVolatile } );
      result.address = atFixedAddress( *target ) && isKnown( expression( *operand.right ) );
      return result;
    }
    if ( operand.kind == ExpressionKind::MEMBER )
    {
      Typed const reachedMember = expression( operand );
      if ( !reachedMember.valid )
      {
        return refused();
      }
      // In stripes a byte has an address, and a value of two bytes none.
      if ( throughStripes( operand ) &&
           ( reachedMember.aggregate.has_value() || ir::sizeOf( reachedMember.type.value_or( ir::Type::U8 ) ) != 1 ) )
      {
        stripedElement( node, textOf( operand.span ) );
        return refused();
      }
      markRoot( operand );
      Typed result = pointing( Pointee{ .type = reachedMember.type.value_or( ir::Type::U8 ),
                                        .enumeration = reachedMember.enumeration,
                                        .isConst = reachedMember.readOnly,
                                        .aggregate = reachedMember.aggregate,
                                        .function = std::nullopt,
                                        .isVolatile = reachedMember.isVolatile } );
      result.address = fixedPlace( operand );
      return result;
    }
    if ( operand.kind != ExpressionKind::IDENTIFIER )
    {
      refuse( node.span, "`&` of anything but an object, an element or what a pointer points at" );
      return refused();
    }
    std::optional<Meaning> const meaning = resolve( operand );
    if ( !meaning.has_value() )
    {
      return refused();
    }
    if ( meaning->kind == NameKind::FUNCTION || meaning->kind == NameKind::ASSEMBLER_PROC )
    {
      // The address of a function is a pointer to a named function type, and
      // where it goes decides which — see docs/decisions/0065-handlers.md.
      // The taking is carried here; the assignment holds it to the type's
      // signature and records the member.
      Typed result = typed( ir::Type::POINTER );
      result.address = true;
      result.taken = meaning->definition;
      result.takenProc = meaning->external;
      result.takenAt = operand.token;
      return result;
    }
    if ( meaning->kind != NameKind::OBJECT )
    {
      refuse( node.span, "`&` of anything but an object, an element or what a pointer points at" );
      return refused();
    }
    if ( meaning->isParameter )
    {
      name( diag::DiagnosticId::C_ADDRESS_OF_PARAMETER, operand );
      return refused();
    }
    if ( meaning->isArray )
    {
      refuse( node.span, "`&` of a whole array" );
      return refused();
    }
    if ( meaning->pointee.has_value() )
    {
      refuse( node.span, "a pointer to a pointer" );
      return refused();
    }
    if ( meaning->value.has_value() || isOpaqueConstant( *meaning ) )
    {
      refuse( node.span, "`&` of a `const` that takes no byte" );
      return refused();
    }
    addressed( *meaning );
    Typed result = pointing( Pointee{ .type = meaning->type,
                                      .enumeration = meaning->enumeration,
                                      .isConst = meaning->isConst,
                                      .aggregate = meaning->aggregate,
                                      .function = std::nullopt,
                                      .isVolatile = meaning->isVolatile } );
    result.address = atFixedAddress( *meaning );
    return result;
  }

  /// An operator with a pointer: `p + n`, `n + p` and `p - n` over any
  /// integer, `p - q` of one pointee counting elements, and `==` and `!=` of
  /// one pointee or with `nullptr` — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#pointers--task-3d.
  Typed pointerOperator( Expression const& node, Typed const& left, Typed const& right )
  {
    TokenKind const op = node.token.kind;
    auto const offset = [this]( Expression const& expression, Typed const& kind )
    {
      if ( kind.pointee.has_value() || kind.isNull || kind.enumeration.has_value() || kind.type == ir::Type::BOOL ||
           kind.aggregate.has_value() )
      {
        return false;
      }
      if ( kind.type.has_value() )
      {
        return true;
      }
      std::int64_t const value = fold( expression, std::nullopt );
      if ( value >= lowestOf( ir::Type::I16 ) && value <= highestOf( ir::Type::U16 ) )
      {
        return true;
      }
      add( diagnostic( diag::DiagnosticId::C_CONSTANT_OUT_OF_RANGE )
               .at( expression.span.begin, expression.span.length )
               .arg( "value", value )
               .arg( "type", std::string{ "ptr" } ) );
      return false;
    };
    bool const leftPointer = left.pointee.has_value();
    bool const rightPointer = right.pointee.has_value();
    if ( op == TokenKind::PLUS && ( leftPointer || rightPointer ) && !( leftPointer && rightPointer ) )
    {
      bool const fits = leftPointer ? offset( *node.right, right ) : offset( *node.left, left );
      return fits ? pointing( leftPointer ? *left.pointee : *right.pointee ) : refused();
    }
    if ( op == TokenKind::MINUS && leftPointer && !rightPointer && !right.isNull )
    {
      return offset( *node.right, right ) ? pointing( *left.pointee ) : refused();
    }
    if ( op == TokenKind::MINUS && leftPointer && rightPointer )
    {
      if ( samePointee( *left.pointee, *right.pointee ) )
      {
        return typed( ir::Type::I16 );
      }
      pointersDiffer( node, left, right );
      return refused();
    }
    if ( op == TokenKind::EQUAL_EQUAL || op == TokenKind::BANG_EQUAL )
    {
      if ( left.isNull && right.isNull )
      {
        refuse( node.span, "a comparison of two constants" );
        return refused();
      }
      if ( ( leftPointer && right.isNull ) || ( left.isNull && rightPointer ) ||
           ( leftPointer && rightPointer && samePointee( *left.pointee, *right.pointee ) ) )
      {
        return typed( ir::Type::BOOL );
      }
      pointersDiffer( node, left, right );
      return refused();
    }
    if ( ( op == TokenKind::PLUS || op == TokenKind::MINUS ) && ( left.isNull || right.isNull ) )
    {
      pointersDiffer( node, left, right );
      return refused();
    }
    refuse( node.span, "this operator of a pointer" );
    return refused();
  }

  void mixedBool( Expression const& node )
  {
    add( diagnostic( diag::DiagnosticId::C_BOOL_MIXED ).at( node.span.begin, node.span.length ) );
  }

  void differ( Expression const& node, Typed const& left, Typed const& right )
  {
    add( diagnostic( diag::DiagnosticId::C_TYPES_DIFFER )
             .at( node.span.begin, node.span.length )
             .arg( "left", describe( left ) )
             .arg( "right", describe( right ) ) );
  }

  void computed( Expression const& node, std::uint32_t enumeration )
  {
    add( diagnostic( diag::DiagnosticId::C_ENUMERATION_COMPUTED )
             .at( node.span.begin, node.span.length )
             .arg( "type", mNames->enumeration( enumeration ).name ) );
  }

  Typed identifier( Expression const& node )
  {
    std::optional<Meaning> const meaning = resolve( node );
    if ( !meaning.has_value() )
    {
      return refused();
    }
    switch ( meaning->kind )
    {
    case NameKind::OBJECT:
    {
      if ( meaning->isArray && meaning->isStriped )
      {
        stripedElement( node, spellingOf( node.token ) );
        return refused();
      }
      if ( meaning->isArray )
      {
        // An array's name alone is a pointer to its first element, as C has it.
        addressed( *meaning );
        Typed result = pointing( Pointee{ .type = meaning->type,
                                          .enumeration = meaning->enumeration,
                                          .isConst = meaning->isConst,
                                          .aggregate = meaning->aggregate,
                                          .function = std::nullopt,
                                          .isVolatile = meaning->isVolatile } );
        result.address = atFixedAddress( *meaning );
        return result;
      }
      if ( isFoldedConstant( *meaning ) )
      {
        return constant();
      }
      if ( isOpaqueConstant( *meaning ) )
      {
        // Known to the assembler where the text is assembled, and so a value
        // given where an object is declared, as an address is: a `u16`, or
        // what an `extern` declares.
        Typed result = meaning->definition != nullptr ? valueOf( *meaning ) : typed( ir::Type::U16 );
        result.address = true;
        return result;
      }
      Typed result = valueOf( *meaning );
      result.readOnly = meaning->aggregate.has_value() && meaning->isConst;
      result.isVolatile = meaning->aggregate.has_value() && meaning->isVolatile;
      result.known = meaning->value.has_value();
      return result;
    }
    case NameKind::TYPE:
      name( diag::DiagnosticId::C_NOT_DECLARED, node );
      return refused();
    case NameKind::FUNCTION:
    case NameKind::ASSEMBLER_PROC:
    {
      // A function's name alone is its address, as C has it and as an array's
      // name above is its first element's. Which named type the pointer is of
      // is decided where it goes, and the taking is what makes the function a
      // member of it, exactly as for `&f` — see
      // docs/decisions/0065-handlers.md.
      Typed result = typed( ir::Type::POINTER );
      result.address = true;
      result.taken = meaning->definition;
      result.takenProc = meaning->external;
      result.takenAt = node.token;
      return result;
    }
    case NameKind::ASSEMBLER_OTHER:
      return refused();
    }
    return refused();
  }

  Typed qualified( Expression const& node )
  {
    std::optional<Meaning> const meaning = resolve( *node.left );
    if ( !meaning.has_value() )
    {
      return refused();
    }
    if ( meaning->kind != NameKind::TYPE || !meaning->enumeration.has_value() )
    {
      name( diag::DiagnosticId::C_NOT_AN_ENUMERATION, *node.left );
      return refused();
    }
    if ( !enumeratorIndex( node ).has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NO_SUCH_ENUMERATOR )
               .at( node.right->token.location, node.right->token.length )
               .arg( "type", mNames->enumeration( *meaning->enumeration ).name )
               .arg( "name", spellingOf( node.right->token ) ) );
      return refused();
    }
    Typed result = enumerated( *meaning->enumeration );
    result.known = true;
    return result;
  }

  /// Whether a type may stand under an arithmetic operator, reporting where
  /// it may not.
  bool arithmetic( Expression const& node, ir::Type type )
  {
    if ( type == ir::Type::BLOCK )
    {
      aggregateOperand( node );
      return false;
    }
    if ( type == ir::Type::BOOL )
    {
      mixedBool( node );
      return false;
    }
    return true;
  }

  void stripedElement( Expression const& node, std::string const& name )
  {
    add( diagnostic( diag::DiagnosticId::C_STRIPED_ELEMENT )
             .at( node.span.begin, node.span.length )
             .arg( "name", name ) );
  }

  /// The array or the pointer `t[i]` names, reporting where it names neither.
  std::optional<Meaning> arrayOf( Expression const& node )
  {
    Expression const& target = *node.left;
    if ( target.kind != ExpressionKind::IDENTIFIER )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_AN_ARRAY )
               .at( target.span.begin, target.span.length )
               .arg( "name", textOf( target.span ) ) );
      return std::nullopt;
    }
    std::optional<Meaning> meaning = resolve( target );
    if ( !meaning.has_value() )
    {
      return std::nullopt;
    }
    if ( meaning->kind != NameKind::OBJECT || ( !meaning->isArray && !meaning->pointee.has_value() ) )
    {
      name( diag::DiagnosticId::C_NOT_AN_ARRAY, target );
      return std::nullopt;
    }
    return meaning;
  }

  /// A constant index inside the array, reporting one outside.
  bool indexInside( Expression const& index, Meaning const& array, Token const& name )
  {
    std::int64_t const value = fold( index, std::nullopt );
    std::int64_t const count = array.count.value_or( 0 );
    // An array of the assembler's whose count a string leaves unknown checks
    // no constant index.
    if ( ( value >= 0 && value < count ) || ( array.isArray && !array.count.has_value() ) )
    {
      return true;
    }
    add( diagnostic( diag::DiagnosticId::C_INDEX_OUTSIDE )
             .at( index.span.begin, index.span.length )
             .arg( "index", value )
             .arg( "name", spellingOf( name ) )
             .arg( "count", count ) );
    return false;
  }

  /// `t[i]`: an element of an array at a static address, indexed by a
  /// constant inside it or by an unsigned integer — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#arrays--task-3c;
  /// or what a pointer points at, `i` elements on, by any integer.
  Typed element( Expression const& node )
  {
    // An expression that is no name is indexed as the pointer it is: a member
    // array, or a pointer a call returns.
    if ( node.left->kind != ExpressionKind::IDENTIFIER )
    {
      Typed const target = expression( *node.left );
      Typed const index = expression( *node.right );
      if ( !target.valid || !index.valid )
      {
        return refused();
      }
      if ( !target.pointee.has_value() )
      {
        add( diagnostic( diag::DiagnosticId::C_NOT_AN_ARRAY )
                 .at( node.left->span.begin, node.left->span.length )
                 .arg( "name", textOf( node.left->span ) ) );
        return refused();
      }
      if ( !isKnown( index ) || index.pointee.has_value() || index.isNull )
      {
        ir::Type const indexType = index.type.value_or( ir::Type::U8 );
        if ( index.enumeration.has_value() || index.pointee.has_value() || index.isNull ||
             index.aggregate.has_value() || indexType == ir::Type::BOOL )
        {
          add( diagnostic( diag::DiagnosticId::C_INDEX_NOT_U8 )
                   .at( node.right->span.begin, node.right->span.length )
                   .arg( "allowed", std::string{ "an integer" } )
                   .arg( "what", describe( index ) ) );
          return refused();
        }
      }
      return reached( *target.pointee );
    }
    std::optional<Meaning> const array = arrayOf( node );
    Typed const index = expression( *node.right );
    if ( !array.has_value() || !index.valid )
    {
      return refused();
    }
    bool const pointer = array->pointee.has_value();
    Typed result = reached( array->pointee.value_or( Pointee{
        .type = array->type, .enumeration = array->enumeration, .isConst = false, .aggregate = array->aggregate } ) );
    result.readOnly = pointer ? result.readOnly : array->isConst;
    result.isVolatile = pointer ? result.isVolatile : array->isVolatile;
    result.throughPointer = pointer;
    if ( isKnown( index ) && !index.pointee.has_value() && !index.isNull )
    {
      return pointer || indexInside( *node.right, *array, node.left->token ) ? result : refused();
    }
    ir::Type const indexType = index.type.value_or( ir::Type::U8 );
    bool const integer = !index.enumeration.has_value() && !index.pointee.has_value() && !index.isNull &&
                         !index.aggregate.has_value() && indexType != ir::Type::BOOL;
    // A striped array's stripes share no address, so its index is `X`'s.
    if ( array->isStriped && ( !integer || indexType != ir::Type::U8 ) )
    {
      add( diagnostic( diag::DiagnosticId::C_INDEX_NOT_U8 )
               .at( node.right->span.begin, node.right->span.length )
               .arg( "allowed", std::string{ "a `u8` or a constant inside the array, which is striped" } )
               .arg( "what", describe( index ) ) );
      return refused();
    }
    if ( !integer || ( !pointer && ir::isSigned( indexType ) ) )
    {
      add( diagnostic( diag::DiagnosticId::C_INDEX_NOT_U8 )
               .at( node.right->span.begin, node.right->span.length )
               .arg( "allowed",
                     std::string{ pointer ? "an integer" : "a `u8`, a `u16` or a constant inside the array" } )
               .arg( "what", describe( index ) ) );
      return refused();
    }
    return result;
  }

  /// `sizeof`: of a type, an object, an array or an element, never computed;
  /// an integer constant, as a literal is.
  Typed sizeOf( Expression const& node )
  {
    if ( node.left == nullptr )
    {
      return constant();
    }
    Expression const& operand = *node.left;
    if ( operand.kind == ExpressionKind::MEMBER || operand.kind == ExpressionKind::STRING_LITERAL )
    {
      return expression( operand ).valid ? constant() : refused();
    }
    bool const reaches = operand.kind == ExpressionKind::INDEX || operand.kind == ExpressionKind::DEREFERENCE;
    Expression const* const named = reaches ? operand.left.get() : &operand;
    if ( named->kind != ExpressionKind::IDENTIFIER )
    {
      refuse( node.span, "`sizeof` of anything but a type, an object, an element or what a pointer points at" );
      return refused();
    }
    std::optional<Meaning> const meaning = resolve( *named );
    if ( !meaning.has_value() )
    {
      return refused();
    }
    if ( reaches && ( meaning->kind != NameKind::OBJECT || ( !meaning->isArray && !meaning->pointee.has_value() ) ) )
    {
      name( diag::DiagnosticId::C_NOT_AN_ARRAY, *named );
      return refused();
    }
    if ( operand.kind == ExpressionKind::DEREFERENCE || meaning->pointee.has_value() )
    {
      return constant();
    }
    if ( operand.kind == ExpressionKind::INDEX )
    {
      if ( !isKnown( expression( *operand.right ) ) )
      {
        return constant();
      }
      return indexInside( *operand.right, *meaning, named->token ) ? constant() : refused();
    }
    if ( ( meaning->kind != NameKind::OBJECT && meaning->kind != NameKind::TYPE ) ||
         ( isAssemblerConstant( *meaning ) && meaning->definition == nullptr ) )
    {
      refuse( node.span, "`sizeof` of anything but a type, an object or an element" );
      return refused();
    }
    if ( meaning->isArray && !meaning->count.has_value() )
    {
      name( diag::DiagnosticId::C_COUNT_UNKNOWN, *named );
      return refused();
    }
    return constant();
  }

  /// `(type)x`: an integer to any integer, and an `enum struct` or a `bool` to
  /// an integer; nothing to a `bool` but a `bool` — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md.
  Typed cast( Expression const& node )
  {
    Typed const operand = expression( *node.left );
    if ( !operand.valid )
    {
      return refused();
    }
    if ( operand.aggregate.has_value() )
    {
      aggregateOperand( node );
      return refused();
    }
    auto const castRefused = [&]( std::string const& type )
    {
      add( diagnostic( diag::DiagnosticId::C_CAST_REFUSED )
               .at( node.span.begin, node.span.length )
               .arg( "what", describe( operand ) )
               .arg( "type", type ) );
      return refused();
    };
    // A pointer casts to a pointer or a `u16`, and a `u16` or a constant to a
    // pointer: an address is what a register of the machine has.
    if ( node.pointer )
    {
      Pointee const pointee{ .type = typeNamed( node.token.keyword ),
                             .enumeration = std::nullopt,
                             .isConst = node.pointeeConst,
                             .aggregate = std::nullopt,
                             .function = std::nullopt,
                             .isVolatile = node.pointeeVolatile };
      bool const fromAddress =
          operand.pointee.has_value() || operand.isNull ||
          ( !operand.enumeration.has_value() && ( !operand.type.has_value() || operand.type == ir::Type::U16 ) );
      if ( !fromAddress )
      {
        return castRefused( std::string{ node.pointeeConst ? "const " : "" } +
                            std::string{ node.pointeeVolatile ? "volatile " : "" } +
                            std::string{ ir::spellingOf( pointee.type ) } + "*" );
      }
      if ( !operand.type.has_value() && !constantFits( *node.left, ir::Type::U16 ) )
      {
        return refused();
      }
      Typed result = pointing( pointee );
      result.known = isKnown( operand ) && !operand.isNull;
      return result;
    }
    if ( operand.pointee.has_value() || operand.isNull )
    {
      if ( typeNamed( node.token.keyword ) != ir::Type::U16 )
      {
        return castRefused( std::string{ ir::spellingOf( typeNamed( node.token.keyword ) ) } );
      }
      Typed result = typed( ir::Type::U16 );
      result.known = operand.known;
      return result;
    }
    ir::Type const target = typeNamed( node.token.keyword );
    bool const fromBool = !operand.enumeration.has_value() && operand.type == ir::Type::BOOL;
    if ( target == ir::Type::BOOL && !fromBool )
    {
      add( diagnostic( diag::DiagnosticId::C_CAST_REFUSED )
               .at( node.span.begin, node.span.length )
               .arg( "what", describe( operand ) )
               .arg( "type", std::string{ ir::spellingOf( target ) } ) );
      return refused();
    }
    Typed result = typed( target );
    result.known = isKnown( operand ) && !operand.enumeration.has_value() && !fromBool;
    std::optional<ir::Type> const source = operand.type;
    if ( !result.known && !operand.enumeration.has_value() && source.has_value() &&
         ir::sizeOf( *source ) < ir::sizeOf( target ) && ( ir::isSigned( target ) || !ir::isSigned( *source ) ) )
    {
      // Where a signed value is widened to an unsigned type, what it holds is
      // no longer one range.
      result.widenedFrom = source;
    }
    return result;
  }

  Typed unary( Expression const& node )
  {
    Typed const operand = expression( *node.left );
    if ( operand.valid && operand.aggregate.has_value() )
    {
      aggregateOperand( node );
      return refused();
    }
    if ( operand.valid && ( operand.pointee.has_value() || operand.isNull ) )
    {
      refuse( node.span, "this operator of a pointer" );
      return refused();
    }
    if ( operand.valid && operand.enumeration.has_value() )
    {
      computed( node, *operand.enumeration );
      return refused();
    }
    if ( node.token.kind == TokenKind::BANG )
    {
      if ( !operand.valid )
      {
        return refused();
      }
      if ( operand.type != ir::Type::BOOL )
      {
        mixedBool( *node.left );
        return refused();
      }
      Typed result = typed( ir::Type::BOOL );
      if ( operand.decided.has_value() )
      {
        result.decided = !*operand.decided;
      }
      return result;
    }
    if ( !operand.valid || !operand.type.has_value() )
    {
      return operand;
    }
    if ( !arithmetic( node, *operand.type ) )
    {
      return refused();
    }
    Typed result = typed( *operand.type );
    result.known = operand.known;
    return result;
  }

  /// Two typed operands of one operator, of one signedness: the operator works
  /// in the wider of their types.
  Typed agreeing( Expression const& node, ir::Type left, ir::Type right )
  {
    if ( !arithmetic( node, left ) || !arithmetic( node, right ) )
    {
      return refused();
    }
    if ( ir::isSigned( left ) != ir::isSigned( right ) )
    {
      add( diagnostic( diag::DiagnosticId::C_MIXED_SIGNEDNESS )
               .at( node.token.location, node.token.length )
               .arg( "left", std::string{ ir::spellingOf( left ) } )
               .arg( "right", std::string{ ir::spellingOf( right ) } ) );
      return refused();
    }
    return typed( widerOf( left, right ) );
  }

  Typed binary( Expression const& node )
  {
    TokenKind const op = node.token.kind;
    if ( isLogical( op ) )
    {
      // Their operands are conditions, and their answer is a `bool` a byte
      // takes from either path — see
      // docs/decisions/0079-a-condition-is-a-bool.md.
      bool const left = condition( *node.left );
      bool const right = condition( *node.right );
      return left && right ? typed( ir::Type::BOOL ) : refused();
    }

    Typed const left = expression( *node.left );
    Typed const right = expression( *node.right );
    if ( !left.valid || !right.valid )
    {
      return refused();
    }
    if ( left.aggregate.has_value() || right.aggregate.has_value() )
    {
      aggregateOperand( node );
      return refused();
    }
    if ( left.pointee.has_value() || left.isNull || right.pointee.has_value() || right.isNull )
    {
      return pointerOperator( node, left, right );
    }
    if ( op == TokenKind::STAR || op == TokenKind::SLASH || op == TokenKind::PERCENT )
    {
      // Folded where both sides are constants, so that `sizeof t / sizeof t[0]`
      // counts, and computed otherwise — see
      // docs/decisions/0095-literals-and-the-runtime.md. A constant divisor of
      // zero is refused; one a run computes is the runtime's.
      std::optional<ir::Type> const type = left.type.has_value() ? left.type : right.type;
      if ( op != TokenKind::STAR && isKnown( right ) && !right.enumeration.has_value() &&
           fold( *node.right, type ) == 0 )
      {
        add( diagnostic( diag::DiagnosticId::C_DIVISION_BY_ZERO )
                 .at( node.right->span.begin, node.right->span.length ) );
        return refused();
      }
    }

    // Values of an `enum struct` are compared with values of their own type,
    // and never computed — see
    // docs/decisions/0078-switch-over-an-enum-struct.md#the-type.
    if ( left.enumeration.has_value() || right.enumeration.has_value() )
    {
      if ( !isComparison( op ) )
      {
        computed( node, left.enumeration.value_or( right.enumeration.value_or( 0 ) ) );
        return refused();
      }
      if ( left.enumeration != right.enumeration )
      {
        differ( node, left, right );
        return refused();
      }
      return typed( ir::Type::BOOL );
    }

    if ( isShift( op ) )
    {
      return shift( node, left, right );
    }
    if ( isComparison( op ) )
    {
      return comparison( node, left, right );
    }

    if ( !left.type.has_value() && !right.type.has_value() )
    {
      return constant();
    }
    Typed result = refused();
    if ( left.type.has_value() && right.type.has_value() )
    {
      result = agreeing( node, *left.type, *right.type );
    }
    else
    {
      ir::Type const type = left.type.has_value() ? *left.type : *right.type;
      Expression const& constantSide = left.type.has_value() ? *node.right : *node.left;
      if ( arithmetic( node, type ) && constantFits( constantSide, type ) )
      {
        result = typed( type );
      }
    }
    result.known = result.valid && isKnown( left ) && isKnown( right );
    return result;
  }

  Typed shift( Expression const& node, Typed const& left, Typed const& right )
  {
    if ( right.type.has_value() && !right.known )
    {
      // A count held in a `u8` is a loop, whatever it holds.
      if ( !left.type.has_value() )
      {
        refuse( node.span, "a constant shifted by a value" );
        return refused();
      }
      if ( *right.type != ir::Type::U8 )
      {
        refuse( node.right->span, "a shift by a value that is not a `u8`" );
        return refused();
      }
      return arithmetic( node, *left.type ) ? typed( *left.type ) : refused();
    }
    if ( !left.type.has_value() )
    {
      return constant();
    }
    ir::Type const type = *left.type;
    if ( !arithmetic( node, type ) )
    {
      return refused();
    }
    std::int64_t const count = fold( *node.right, std::nullopt );
    std::int64_t const largest = ( 8 * static_cast<std::int64_t>( ir::sizeOf( type ) ) ) - 1;
    if ( count < 0 || count > largest )
    {
      add( diagnostic( diag::DiagnosticId::C_SHIFT_OUT_OF_RANGE )
               .at( node.right->span.begin, node.right->span.length )
               .arg( "type", std::string{ ir::spellingOf( type ) } )
               .arg( "count", count )
               .arg( "largest", largest ) );
      return refused();
    }
    Typed result = typed( type );
    result.known = left.known;
    return result;
  }

  Typed comparison( Expression const& node, Typed const& left, Typed const& right )
  {
    // A translated character is a constant only the assembler knows, and
    // two constants are not compared whoever knows them.
    auto const translated = [this]( Expression const& side )
    { return side.kind == ExpressionKind::CHARACTER_CONSTANT && isTranslated( side ); };
    bool const leftConstant = isKnown( left ) || translated( *node.left );
    bool const rightConstant = isKnown( right ) || translated( *node.right );
    if ( leftConstant && rightConstant )
    {
      refuse( node.span, "a comparison of two constants" );
      return refused();
    }
    if ( left.type.has_value() && right.type.has_value() )
    {
      return agreeing( node, *left.type, *right.type ).valid ? typed( ir::Type::BOOL ) : refused();
    }

    ir::Type const type = left.type.has_value() ? *left.type : right.type.value_or( ir::Type::U8 );
    if ( !arithmetic( node, type ) )
    {
      return refused();
    }
    Expression const& constantSide = left.type.has_value() ? *node.right : *node.left;
    std::int64_t const value = fold( constantSide, std::nullopt );

    // A value a cast widened holds only what its own type did, and a constant
    // outside that decides the comparison as one outside the wider type does.
    ir::Type const range = ( left.type.has_value() ? left : right ).widenedFrom.value_or( type );
    if ( value >= lowestOf( range ) && value <= highestOf( range ) )
    {
      return typed( ir::Type::BOOL );
    }

    // A constant the type cannot hold decides the comparison: every value of
    // the type stands on one side of it.
    bool const above = value > highestOf( range );
    TokenKind op = node.token.kind;
    if ( !left.type.has_value() )
    {
      // `c < x` is `x > c`.
      switch ( op )
      {
      case TokenKind::LESS:
        op = TokenKind::GREATER;
        break;
      case TokenKind::GREATER:
        op = TokenKind::LESS;
        break;
      case TokenKind::LESS_EQUAL:
        op = TokenKind::GREATER_EQUAL;
        break;
      case TokenKind::GREATER_EQUAL:
        op = TokenKind::LESS_EQUAL;
        break;
      default:
        break;
      }
    }
    bool outcome = false;
    switch ( op )
    {
    case TokenKind::LESS:
    case TokenKind::LESS_EQUAL:
      outcome = above;
      break;
    case TokenKind::GREATER:
    case TokenKind::GREATER_EQUAL:
      outcome = !above;
      break;
    case TokenKind::BANG_EQUAL:
      outcome = true;
      break;
    default:
      break;
    }
    add( diagnostic( diag::DiagnosticId::C_COMPARISON_ALWAYS )
             .at( constantSide.span.begin, constantSide.span.length )
             .arg( "value", value )
             .arg( "type", std::string{ ir::spellingOf( range ) } )
             .arg( "outcome", std::string{ outcome ? "true" : "false" } ) );
    return Typed{
      .valid = true, .type = ir::Type::BOOL, .decided = outcome, .enumeration = std::nullopt, .known = false
    };
  }

  /// `&f` where a pointer of a function type is wanted: the signature must be
  /// the type's, and the function is then a member of it. A member reads the
  /// type's Temporaries, so one function belongs to one type (`NGA7298`).
  void takesFunction( Expression const& value, std::optional<Pointee> const& target, Typed const& kind )
  {
    std::string const name = kind.takenAt.has_value() ? spellingOf( *kind.takenAt ) : std::string{ "a function" };
    if ( !target.has_value() || !target->function.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_TAKEN_SIGNATURE )
               .at( value.span.begin, value.span.length )
               .arg( "name", name )
               .arg( "type", std::string{ "pointer the value goes to" } )
               .arg( "what", "the address of a function goes to a pointer of a function type alone" ) );
      return;
    }
    FunctionType const& type = mNames->functionType( *target->function );
    std::optional<std::string> const wrong = signatureDiffers( type, kind );
    if ( wrong.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_TAKEN_SIGNATURE )
               .at( value.span.begin, value.span.length )
               .arg( "name", name )
               .arg( "type", type.name )
               .arg( "what", *wrong ) );
      return;
    }
    if ( mMembers == nullptr )
    {
      return;
    }
    if ( kind.taken != nullptr )
    {
      auto const [held, added] = mMembers->ofFunction.try_emplace( kind.taken, *target->function );
      if ( !added && held->second != *target->function )
      {
        add( diagnostic( diag::DiagnosticId::C_TAKEN_TWICE )
                 .at( value.span.begin, value.span.length )
                 .arg( "name", name )
                 .arg( "type", type.name )
                 .arg( "other", mNames->functionType( held->second ).name ) );
        return;
      }
      if ( added && kind.takenAt.has_value() )
      {
        mMembers->takenAt.try_emplace( kind.taken, *kind.takenAt );
      }
      return;
    }
    auto const [held, added] = mMembers->ofProc.try_emplace( kind.takenProc->name, *target->function );
    if ( !added && held->second != *target->function )
    {
      add( diagnostic( diag::DiagnosticId::C_TAKEN_TWICE )
               .at( value.span.begin, value.span.length )
               .arg( "name", name )
               .arg( "type", type.name )
               .arg( "other", mNames->functionType( held->second ).name ) );
    }
  }

  /// How a function differs from a type, or nothing where it does not: the
  /// count of the parameters, their types in order, and the result.
  [[nodiscard]] std::optional<std::string> signatureDiffers( FunctionType const& type, Typed const& kind ) const
  {
    if ( kind.takenProc != nullptr && kind.taken == nullptr )
    {
      // A Proc of the assembler declares its membership with `as`, and its
      // arguments are the type's; nothing else about it is read here.
      return kind.takenProc->as == type.name
                 ? std::nullopt
                 : std::optional{ kind.takenProc->as.empty()
                                      ? std::string{ "a proc of the assembler is a member where it says `as`" }
                                      : "the proc says `as " + kind.takenProc->as + "`" };
    }
    return signatureDiffers( type, *kind.taken );
  }

  /// Whether two declared types are one.
  [[nodiscard]] static bool sameType( NamedType const& left, NamedType const& right )
  {
    if ( left.pointee.has_value() != right.pointee.has_value() )
    {
      return false;
    }
    if ( left.pointee.has_value() && !samePointee( *left.pointee, *right.pointee ) )
    {
      return false;
    }
    return left.type == right.type && left.enumeration == right.enumeration && left.aggregate == right.aggregate;
  }

  diag::SourceManager const* mSources;
  Names const* mNames;
  Unit const* mUnit;
  diag::DiagnosticSink* mSink;

  /// The expression a compound assignment used as a value is held against.
  Expression const* mRoot = nullptr;

  /// Where a taking of a function's address is recorded, over the whole
  /// program; nothing where the walk is not the Checker's.
  Membership* mMembers = nullptr;

  /// The function whose body is being read, for what is recorded of it.
  Definition const* mWithin = nullptr;

  /// The locals of the blocks being walked, innermost last.
  std::vector<std::map<std::string, Meaning, std::less<>>> mScopes;

  std::vector<CallSite>* mCallSites = nullptr;
  std::set<std::uint32_t>* mAddressed = nullptr;

  /// The literals whose prefix has been checked, by where they stand.
  std::set<std::uint32_t> mCheckedPrefixes;
};

/// A call of one function of the program by another, and where it stands:
/// an edge of the call graph recursion is refused on.
struct CallEdge
{
  Definition const* caller = nullptr;
  Definition const* callee = nullptr;

  /// The file it stands in, by its place among the program's.
  std::size_t unit = 0;
  diag::SourceSpan span;
  diag::DiagnosticSink* sink = nullptr;
};

/// The second pass: what every statement of a file names, and whether the
/// compiler compiles it.
class Checker
{
public:
  Checker( diag::SourceManager const& sources,
           Names const& names,
           Unit const& unit,
           std::size_t index,
           std::vector<CallEdge>& edges,
           std::set<std::uint32_t>& addressed,
           Membership& members )
      : mSources( &sources ), mUnit( &unit ), mIndex( index ), mEdges( &edges ),
        mTyping( sources, names, unit, unit.sink, &members )
  {
    mTyping.recordAddresses( &addressed );
  }

  void check()
  {
    for ( ExternalDeclaration const& declaration : mUnit->tree.declarations )
    {
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration );
           function != nullptr && function->isTypedef )
      {
        // A function type declares no function: what it names is the Proc a
        // pointer to it goes through — see docs/decisions/0065-handlers.md.
        attributesKnown( function->attributes, AttributeSite::FUNCTION );
        continue;
      }
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration );
           function != nullptr && function->isExtern )
      {
        externProc( *function );
        continue;
      }
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration ) )
      {
        functionDefinition( *function );
        continue;
      }
      if ( auto const* object = std::get_if<Declaration>( &declaration ) )
      {
        fileScope( *object );
      }
      if ( auto const* aggregate = std::get_if<StructSpecifier>( &declaration ) )
      {
        structure( *aggregate );
      }
    }
  }

private:
  /// A `struct` or a `union`: members, each named once, none `const` nor
  /// given a value, and at least one — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
  void structure( StructSpecifier const& aggregate )
  {
    std::string const typeName = mTyping.spellingOf( aggregate.name );
    std::size_t members = 0;
    std::map<std::string, Token, std::less<>> seen;
    for ( Declaration const& member : aggregate.members )
    {
      for ( std::size_t index = 0; index < member.declarators.size(); ++index )
      {
        ++members;
        Token const& name = member.declarators[index];
        DeclaratorShape const& shape = member.shapes[index];
        std::string const spelling = mTyping.spellingOf( name );
        if ( auto const [existing, added] = seen.try_emplace( spelling, name ); !added )
        {
          mTyping.add( diagnostic( diag::DiagnosticId::C_MEMBER_TWICE )
                           .at( name.location, name.length )
                           .arg( "name", spelling )
                           .arg( "type", typeName ) );
        }
        if ( ( member.isConst && !shape.isPointer ) || shape.isConstPointer || member.initialisers[index] != nullptr ||
             shape.hasList )
        {
          mTyping.add( diagnostic( diag::DiagnosticId::C_MEMBER_REFUSED )
                           .at( name.location, name.length )
                           .arg( "name", spelling ) );
        }
        if ( ( member.isVolatile && !shape.isPointer ) || shape.isVolatilePointer )
        {
          mTyping.refuse( name.span(), "a `volatile` member" );
        }
        if ( shape.isArray && shape.isPointer )
        {
          mTyping.refuse( member.span, "an array of pointers" );
        }
        else if ( shape.isArray )
        {
          std::optional<Meaning> const type = mTyping.declaredType( member.type );
          arraySize( name,
                     shape,
                     type.has_value() ? type->type : ir::Type::U8,
                     type.has_value() ? type->aggregate : std::nullopt );
        }
      }
    }
    if ( members == 0 )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EMPTY_AGGREGATE )
                       .at( aggregate.name.location, aggregate.name.length )
                       .arg( "name", typeName ) );
    }
  }

  /// Objects at file scope: a `const` given a value, and any object given one
  /// given a constant, since the loader writes it — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
  void fileScope( Declaration const& object )
  {
    if ( object.type.keyword == Keyword::AUTO )
    {
      // An object at file scope is given a constant, which has no type of its
      // own to deduce from — see
      // docs/decisions/0161-auto-takes-the-type-of-the-value.md.
      mTyping.refuse( object.type.span(), "`auto` at file scope" );
      return;
    }
    attributesKnown( object.attributes, object.isExtern ? AttributeSite::EXTERN : AttributeSite::GLOBAL );
    if ( !object.isExtern )
    {
      placementAsked(
          object, placementAttribute( object.attributes, *mSources ), isStriped( object.attributes, *mSources ), true );
    }
    for ( std::size_t index = 0; index < object.declarators.size(); ++index )
    {
      Token const& declarator = object.declarators[index];
      auto const found = mUnit->definitions.find( mSources->textOf( declarator.span() ) );
      if ( found == mUnit->definitions.end() || found->second.name.location != declarator.location )
      {
        continue;
      }
      Definition const& definition = found->second;
      DeclaratorShape const& array = object.shapes[index];
      if ( !shaped( object, index ) )
      {
        continue;
      }
      if ( definition.function.has_value() && !array.isPointer && definition.typeName.has_value() )
      {
        // A function type has no bytes, so nothing is of it: it is a type as
        // a pointer alone — see
        // docs/decisions/0103-what-building-handlers-needs.md. The name is
        // there by construction: a function type is reached by one.
        mTyping.add( diagnostic( diag::DiagnosticId::C_FUNCTION_TYPE_VALUE )
                         .at( declarator.location, declarator.length )
                         .arg( "name", mTyping.spellingOf( *definition.typeName ) ) );
        continue;
      }
      if ( definition.isExtern )
      {
        externDeclaration( object, index, definition );
        continue;
      }
      if ( !definition.pane.empty() && definition.type == ir::Type::POINTER && !definition.isArray )
      {
        // A pointer lies on the zero page, which no Window covers.
        mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_MISPLACED )
                         .at( declarator.location, declarator.length )
                         .arg( "attribute", std::string{ "in" } )
                         .arg( "applies", std::string{ "a function or an object that is no pointer" } )
                         .arg( "name", mTyping.spellingOf( declarator ) ) );
      }
      implementsSlot( declarator, definition );
      Meaning type{ .kind = NameKind::OBJECT,
                    .type = definition.type,
                    .enumeration = definition.enumeration,
                    .byte = {},
                    .owner = {} };
      type.pointee = definition.pointee;
      type.aggregate = definition.aggregate;
      if ( array.isArray )
      {
        type.isArray = true;
        type.count = arraySize( declarator, array, type.type, type.aggregate );
      }
      if ( definition.isStriped )
      {
        striped( declarator, array, type );
      }
      given( declarator, object.initialisers[index].get(), array, definition.isConst, true, type );
    }
  }

  /// `extern TYPE NAME;`: a name the assembler exports, read as the type says
  /// and held to the bytes — see
  /// docs/decisions/0093-extern-declares-how-c-reads-the-assembler.md.
  void externDeclaration( Declaration const& object, std::size_t index, Definition const& definition )
  {
    Token const& declarator = object.declarators[index];
    std::string const spelling = mTyping.spellingOf( declarator );
    Names const& names = mTyping.names();
    auto const refuse = [&]( std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_REFUSED )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "what", std::move( what ) ) );
    };
    if ( object.initialisers[index] != nullptr || object.shapes[index].hasList )
    {
      refuse( "is given a value, and its bytes are the assembler's" );
      return;
    }
    if ( definition.isSlot )
    {
      slotDeclaration( object, index, definition );
      return;
    }
    if ( !object.attributes.empty() && !definition.isStriped )
    {
      refuse( "carries attributes, and where it stands is the assembler's" );
      return;
    }
    if ( definition.isStriped )
    {
      refuse( "is `[[striped]]`, and its layout is the assembler's" );
      return;
    }
    if ( Definition const* const first = names.firstExtern( spelling ); first != &definition && first != nullptr )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_TWICE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .note( diagnostic( diag::DiagnosticId::C_PREVIOUS_DEFINITION )
                                  .at( first->name.location, first->name.length )
                                  .arg( "name", spelling ) ) );
      return;
    }
    if ( names.definedInC( spelling ) != nullptr )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_OF_C )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling ) );
      return;
    }
    ExternalName const* const external = names.externalNamed( spelling );
    if ( external == nullptr )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_OF_NOTHING )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling ) );
      return;
    }
    switch ( external->kind )
    {
    case ExternalKind::PROC:
      refuse( "names a `.proc`, which is declared as a function" );
      return;
    case ExternalKind::UNREAD:
    case ExternalKind::RESERVED:
    case ExternalKind::CHARSET:
    case ExternalKind::PANE:
    case ExternalKind::WINDOW:
      mTyping.unread( declarator, *external );
      return;
    case ExternalKind::CONSTANT:
      externConstant( declarator, definition, *external );
      return;
    case ExternalKind::MEMORY:
    case ExternalKind::SHAPELESS:
      externData( declarator, definition, *external );
      return;
    }
  }

  /// `extern RESULT NAME(PARAMETERS);`: a Proc of the assembler, its count of
  /// parameters and its result held to its `.declare`s, and each parameter's
  /// name and type to its byte — see
  /// docs/decisions/0094-extern-declares-how-c-calls-a-proc.md.
  /// A Slot to data: `[[slot]] extern T* const NAME;`. The Binding is the
  /// type's, so the type must be one a Binding comes off — a pointer, and a
  /// `const` one, since the Cell is the Transition routine's to write. See
  /// docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md.
  void slotDeclaration( Declaration const& object, std::size_t index, Definition const& definition )
  {
    Token const& declarator = object.declarators[index];
    std::string const spelling = mTyping.spellingOf( declarator );
    auto const refuse = [&]( std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_SLOT_TYPE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "what", std::move( what ) ) );
    };
    for ( Attribute const& attribute : object.attributes )
    {
      if ( mTyping.spellingOf( attribute.name ) != "slot" )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_MISPLACED )
                         .at( attribute.span.begin, attribute.span.length )
                         .arg( "attribute", mTyping.spellingOf( attribute.name ) )
                         .arg( "applies", std::string{ "a declaration the assembler does not own" } )
                         .arg( "name", std::string{ "a slot" } ) );
        return;
      }
    }
    if ( definition.isArray || object.shapes[index].isArray )
    {
      refuse( "this is an array" );
      return;
    }
    if ( definition.type != ir::Type::POINTER )
    {
      refuse( "this is a `" + std::string{ ir::spellingOf( definition.type ) } + "`" );
      return;
    }
    if ( !definition.isConst )
    {
      refuse( "this pointer is not `const`, and the cell is the transition routine's to write" );
      return;
    }
    Names const& names = mTyping.names();
    if ( Definition const* const first = names.firstExtern( spelling ); first != &definition && first != nullptr )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_TWICE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .note( diagnostic( diag::DiagnosticId::C_PREVIOUS_DEFINITION )
                                  .at( first->name.location, first->name.length )
                                  .arg( "name", spelling ) ) );
    }
  }

  /// What a definition written `[[implements(NAME)]]` fills: the Cell holds
  /// its address, and only C knows what the Cell is read as, since
  /// `.implements` hands the assembler an address and not a type — see
  /// docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md. A name that
  /// is no Slot of this program passes unread, and Merge answers it.
  void implementsSlot( Token const& where, Definition const& definition )
  {
    if ( definition.implements.empty() )
    {
      return;
    }
    std::optional<Meaning> const slot = mTyping.lookupName( definition.implements );
    if ( !slot.has_value() || slot->definition == nullptr || !slot->definition->isSlot )
    {
      return;
    }
    Definition const& declared = *slot->definition;
    auto const refuse = [&]( std::string wanted, std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_IMPLEMENTS_TYPE )
                       .at( where.location, where.length )
                       .arg( "name", mTyping.spellingOf( where ) )
                       .arg( "slot", definition.implements )
                       .arg( "wanted", std::move( wanted ) )
                       .arg( "what", std::move( what ) ) );
    };
    if ( declared.kind == NameKind::FUNCTION )
    {
      std::string const wanted =
          declared.function.has_value() ? "a function of the slot's own signature" : "a `void(void)`";
      if ( definition.kind != NameKind::FUNCTION )
      {
        refuse( wanted, "this is an object" );
        return;
      }
      if ( !declared.function.has_value() )
      {
        if ( !definition.parameters.empty() || definition.result.has_value() )
        {
          refuse( wanted, "this one takes or returns something" );
        }
        return;
      }
      // The Slot is the function type, so an Implementation is held to it as
      // a taking holds `&f`, and joins it — see
      // docs/decisions/0174-a-slot-takes-and-returns.md.
      FunctionType const& type = mTyping.functionType( *declared.function );
      if ( std::optional<std::string> const wrong = mTyping.signatureDiffers( type, definition ); wrong.has_value() )
      {
        refuse( wanted, *wrong );
        return;
      }
      mTyping.joinType( &definition, *declared.function );
      return;
    }
    if ( definition.kind != NameKind::OBJECT )
    {
      refuse( "what the slot points at", "this is a function" );
      return;
    }
    if ( !declared.pointee.has_value() )
    {
      return;
    }
    Pointee const& wanted = *declared.pointee;
    Pointee const held{ .type = definition.type,
                        .enumeration = definition.enumeration,
                        .isConst = definition.isConst,
                        .aggregate = definition.aggregate };
    if ( definition.type == ir::Type::POINTER )
    {
      if ( !definition.pointee.has_value() || !samePointee( wanted, *definition.pointee ) )
      {
        refuse( describePointee( wanted ), "this points at something else" );
      }
      return;
    }
    if ( !samePointee( wanted, held ) )
    {
      refuse( describePointee( wanted ), "this is " + describePointee( held ) );
    }
  }

  /// A pointee as a finding spells it.
  [[nodiscard]] std::string describePointee( Pointee const& pointee ) const
  {
    NamedType const named{ .type = pointee.type,
                           .enumeration = pointee.enumeration,
                           .typeName = std::nullopt,
                           .pointee = std::nullopt,
                           .aggregate = pointee.aggregate };
    return mTyping.describe( typedAs( named ) );
  }

  void externProc( FunctionDefinition const& function )
  {
    Token const& declarator = function.name;
    std::string const spelling = mTyping.spellingOf( declarator );
    Definition const* const definition = definitionOf( *mSources, *mUnit, function );
    if ( definition == nullptr )
    {
      return;
    }
    Names const& names = mTyping.names();
    if ( definition->isSlot )
    {
      // A Slot to a function binds a `vector`, and what the Cell holds is
      // one Implementation per Phase: their bytes are their own, and what
      // would name parameters across them is the handler type of 0065.
      for ( Attribute const& attribute : function.attributes )
      {
        if ( mTyping.spellingOf( attribute.name ) != "slot" )
        {
          mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_MISPLACED )
                           .at( attribute.span.begin, attribute.span.length )
                           .arg( "attribute", mTyping.spellingOf( attribute.name ) )
                           .arg( "applies", std::string{ "a declaration the assembler does not own" } )
                           .arg( "name", std::string{ "a slot" } ) );
          return;
        }
      }
      return;
    }
    if ( !function.attributes.empty() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_REFUSED )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "what", std::string{ "carries attributes, and the Proc's are the assembler's" } ) );
      return;
    }
    if ( Definition const* const first = names.firstExtern( spelling ); first != definition && first != nullptr )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_TWICE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .note( diagnostic( diag::DiagnosticId::C_PREVIOUS_DEFINITION )
                                  .at( first->name.location, first->name.length )
                                  .arg( "name", spelling ) ) );
      return;
    }
    if ( names.definedInC( spelling ) != nullptr )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_OF_C )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling ) );
      return;
    }
    ExternalName const* const proc = names.externalNamed( spelling );
    if ( proc == nullptr )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_OF_NOTHING )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling ) );
      return;
    }
    auto const mismatch = [&]( Token const& at, std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_SIGNATURE )
                       .at( at.location, at.length )
                       .arg( "name", spelling )
                       .arg( "what", std::move( what ) ) );
    };
    if ( proc->kind != ExternalKind::PROC )
    {
      mismatch( declarator, "names no `.proc`" );
      return;
    }
    if ( definition->parameters.size() != proc->arguments.size() )
    {
      mismatch( declarator,
                "has " + std::to_string( definition->parameters.size() ) + " parameters, and the `.proc` declares " +
                    std::to_string( proc->arguments.size() ) + " arguments" );
      return;
    }
    if ( definition->result.has_value() != proc->result.has_value() )
    {
      mismatch( declarator,
                definition->result.has_value() ? "returns a value, and the `.proc` declares no result"
                                               : "returns nothing, and the `.proc` declares a result" );
      return;
    }
    for ( std::size_t index = 0; index < definition->parameters.size(); ++index )
    {
      ParameterType const& parameter = definition->parameters[index];
      ExternalByte const& byte = proc->arguments[index];
      // A byte a register carries has no name to be held to; one in part
      // in memory has its reservation's — see
      // docs/decisions/0145-an-argument-in-a-register.md.
      std::string_view const byteName = byte.name.empty()
                                            ? std::string_view{ byte.place }
                                            : std::string_view{ byte.name }.substr( byte.name.rfind( '.' ) + 1 );
      if ( parameter.isNamed && !byte.name.empty() && mTyping.spellingOf( parameter.name ) != byteName )
      {
        mismatch( parameter.name,
                  "names its parameter `" + mTyping.spellingOf( parameter.name ) + "` where the `.proc` declares `" +
                      std::string{ byteName } + "`" );
        return;
      }
      if ( !fitsByte( parameter.type, byte ) )
      {
        mismatch( parameter.name,
                  "reads `" + std::string{ byteName } + "`, " + describeByte( byte ) + ", as " +
                      describeNamed( parameter.type ) );
        return;
      }
    }
    if ( proc->result.has_value() && !fitsByte( *definition->result, *proc->result ) )
    {
      mismatch( declarator,
                "returns " + describeNamed( *definition->result ) + ", and the `.proc` returns " +
                    describeByte( *proc->result ) );
    }
  }

  /// Whether a type of C may be read over a declared byte, by 0094's table.
  [[nodiscard]] bool fitsByte( NamedType const& type, ExternalByte const& byte ) const
  {
    std::uint32_t const bytes = type.pointee.has_value() ? 2U : mTyping.bytes( type.type, type.aggregate );
    if ( bytes != byte.bytes )
    {
      return false;
    }
    bool const aggregate = type.aggregate.has_value() && !type.pointee.has_value();
    // A `struct` is copied byte by byte into memory, which a register is not.
    if ( aggregate && !byte.place.empty() )
    {
      return false;
    }
    if ( !byte.isWritten )
    {
      // By its shape: any type of its size, and bytes only as a `struct`.
      return byte.bytes <= 2 || aggregate;
    }
    if ( byte.isBytes )
    {
      return aggregate;
    }
    if ( aggregate )
    {
      return false;
    }
    if ( type.pointee.has_value() )
    {
      return byte.type == ir::Type::U16;
    }
    if ( type.enumeration.has_value() )
    {
      return byte.type == ir::Type::U8;
    }
    return type.type == byte.type;
  }

  [[nodiscard]] static std::string describeByte( ExternalByte const& byte )
  {
    if ( !byte.place.empty() )
    {
      return std::to_string( byte.bytes ) + ( byte.bytes == 1 ? " byte" : " bytes" ) + " in `" + byte.place + "`";
    }
    if ( !byte.isWritten )
    {
      return std::to_string( byte.bytes ) + ( byte.bytes == 1 ? " byte" : " bytes" ) + " of no type written";
    }
    if ( byte.isBytes )
    {
      return "`u8[" + std::to_string( byte.bytes ) + "]`";
    }
    return "`" + std::string{ ir::spellingOf( byte.type ) } + "`";
  }

  [[nodiscard]] std::string describeNamed( NamedType const& type ) const
  {
    if ( type.pointee.has_value() )
    {
      return "a pointer";
    }
    if ( type.aggregate.has_value() )
    {
      return "`" + mTyping.aggregate( *type.aggregate ).name + "`";
    }
    if ( type.enumeration.has_value() )
    {
      return "`" + mTyping.enumeration( *type.enumeration ).name + "`";
    }
    return "`" + std::string{ ir::spellingOf( type.type ) } + "`";
  }

  /// A Constant declared: `const`, a scalar, an `enum struct` or a pointer,
  /// and a value it holds where it folds.
  void externConstant( Token const& declarator, Definition const& definition, ExternalName const& external )
  {
    std::string const spelling = mTyping.spellingOf( declarator );
    if ( !definition.isConst || definition.isArray || definition.aggregate.has_value() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_REFUSED )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "what",
                             std::string{ "names a Constant, which is declared `const`, as a scalar, an `enum struct` "
                                          "or a pointer" } ) );
      return;
    }
    if ( !external.value.has_value() )
    {
      return;
    }
    std::int64_t const value = *external.value;
    if ( definition.enumeration.has_value() || definition.type == ir::Type::BOOL )
    {
      std::size_t const count =
          definition.enumeration.has_value() ? mTyping.enumeration( *definition.enumeration ).enumerators.size() : 2;
      if ( value < 0 || std::cmp_greater_equal( value, count ) )
      {
        notOfType( declarator, "`" + spelling + "`", value, definition );
      }
      return;
    }
    if ( value < lowestOf( definition.type ) || value > highestOf( definition.type ) )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_CONSTANT_OUT_OF_RANGE )
                       .at( declarator.location, declarator.length )
                       .arg( "value", value )
                       .arg( "type", std::string{ ir::spellingOf( definition.type ) } ) );
    }
  }

  void notOfType( Token const& declarator, std::string what, std::int64_t value, Definition const& definition )
  {
    std::string const type = definition.enumeration.has_value() ? mTyping.enumeration( *definition.enumeration ).name
                                                                : std::string{ ir::spellingOf( definition.type ) };
    mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_VALUE )
                     .at( declarator.location, declarator.length )
                     .arg( "what", std::move( what ) )
                     .arg( "value", value )
                     .arg( "type", type ) );
  }

  /// One value of C a declaration reads: where it lies and what it is.
  struct Leaf
  {
    std::uint32_t offset = 0;
    std::uint32_t bytes = 1;
    ir::Type type = ir::Type::U8;
    std::optional<std::uint32_t> enumeration{};
    std::string path{};
  };

  /// The values of C a type holds, member by member and element by element.
  void leavesOf( ir::Type type,
                 std::optional<std::uint32_t> enumeration,
                 std::optional<std::uint32_t> aggregate,
                 bool pointer,
                 std::uint32_t offset,
                 std::string const& path,
                 std::vector<Leaf>& into ) const
  {
    if ( aggregate.has_value() && !pointer )
    {
      for ( MemberType const& member : mTyping.aggregate( *aggregate ).members )
      {
        std::uint32_t const bytes =
            mTyping.bytes( member.type.pointee.has_value() ? ir::Type::POINTER : member.type.type,
                           member.type.pointee.has_value() ? std::nullopt : member.type.aggregate );
        std::uint32_t const count = member.isArray ? member.count : 1U;
        std::string const named = path + "." + mTyping.spellingOf( member.name );
        for ( std::uint32_t element = 0; element < count; ++element )
        {
          leavesOf( member.type.type,
                    member.type.enumeration,
                    member.type.aggregate,
                    member.type.pointee.has_value(),
                    member.offset + offset + ( element * bytes ),
                    member.isArray ? named + "[" + std::to_string( element ) + "]" : named,
                    into );
        }
      }
      return;
    }
    ir::Type const leaf = pointer ? ir::Type::POINTER : type;
    into.push_back( Leaf{ .offset = offset,
                          .bytes = ir::sizeOf( leaf ),
                          .type = leaf,
                          .enumeration = pointer ? std::nullopt : enumeration,
                          .path = path } );
  }

  /// A Label or a Region declared: as many bytes as the type, laid over the
  /// items with no value of C splitting one, and the values that fold held
  /// to a `bool` and an `enum struct`.
  void externData( Token const& declarator, Definition const& definition, ExternalName const& external )
  {
    std::string const spelling = mTyping.spellingOf( declarator );
    Meaning const meaning = mTyping.lookupName( spelling ).value_or( Meaning{} );
    bool const pointer = definition.pointee.has_value();
    std::uint32_t const element =
        pointer ? 2U : mTyping.bytes( definition.type, pointer ? std::nullopt : definition.aggregate );
    std::uint32_t const count = definition.isArray ? meaning.count.value_or( 0 ) : 1U;
    std::uint32_t const run = runBytes( external );
    auto const size = [&]( std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_SIZE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "what", std::move( what ) ) );
    };
    auto const notData = [&]
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_NOT_DATA )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "reason", external.stop ) );
    };

    std::uint32_t const total = element * count;
    if ( definition.isArray && definition.size == nullptr )
    {
      if ( !external.stop.empty() )
      {
        notData();
        return;
      }
      if ( run == 0 || run % element != 0 )
      {
        size( "reads elements of " + std::to_string( element ) + " bytes, and the " + std::to_string( run ) +
              " bytes to the end of its Section are no whole number of them" );
        return;
      }
    }
    else if ( external.isRegion && run != total )
    {
      size( "reads " + std::to_string( total ) + " bytes, and the Region is " + std::to_string( run ) + " long" );
      return;
    }
    else if ( run < total )
    {
      if ( !external.stop.empty() )
      {
        notData();
        return;
      }
      size( "reads " + std::to_string( total ) + " bytes, and the assembler wrote " + std::to_string( run ) +
            " to the end of its Section" );
      return;
    }

    // Where a value of C may begin and end: between two items, and anywhere
    // inside a reservation.
    std::vector<bool> between( total + 1U, false );
    std::map<std::uint32_t, ExternalCell const*> items;
    std::uint32_t at = 0;
    for ( ExternalCell const& cell : external.cells )
    {
      if ( at > total )
      {
        break;
      }
      between[at] = true;
      if ( !cell.isReserved )
      {
        items.emplace( at, &cell );
      }
      for ( std::uint32_t inside = 1; cell.isReserved && inside < cell.bytes && at + inside <= total; ++inside )
      {
        between[at + inside] = true;
      }
      at += cell.bytes;
      if ( at <= total )
      {
        between[at] = true;
      }
    }

    std::vector<Leaf> leaves;
    for ( std::uint32_t index = 0; index < count; ++index )
    {
      leavesOf( definition.type,
                definition.enumeration,
                definition.aggregate,
                pointer,
                index * element,
                definition.isArray ? spelling + "[" + std::to_string( index ) + "]" : spelling,
                leaves );
    }
    for ( Leaf const& leaf : leaves )
    {
      if ( !between[leaf.offset] || !between[leaf.offset + leaf.bytes] )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_SPLITS )
                         .at( declarator.location, declarator.length )
                         .arg( "path", leaf.path )
                         .arg( "offset", static_cast<std::int64_t>( leaf.offset ) ) );
        return;
      }
    }
    for ( Leaf const& leaf : leaves )
    {
      if ( !leaf.enumeration.has_value() && leaf.type != ir::Type::BOOL )
      {
        continue;
      }
      auto const item = items.find( leaf.offset );
      std::optional<std::int64_t> const itemValue = item == items.end() ? std::nullopt : item->second->value;
      if ( !itemValue.has_value() )
      {
        continue;
      }
      std::int64_t const value = *itemValue;
      std::size_t const values =
          leaf.enumeration.has_value() ? mTyping.enumeration( *leaf.enumeration ).enumerators.size() : 2;
      if ( value < 0 || std::cmp_greater_equal( value, values ) )
      {
        std::string const type = leaf.enumeration.has_value() ? mTyping.enumeration( *leaf.enumeration ).name
                                                              : std::string{ ir::spellingOf( leaf.type ) };
        mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_VALUE )
                         .at( declarator.location, declarator.length )
                         .arg( "what", "`" + leaf.path + "`" )
                         .arg( "value", value )
                         .arg( "type", type ) );
        return;
      }
    }
  }

  /// Where attributes stand: on a function, on a declaration at file scope, on
  /// an `extern`, on a declaration of locals, or before a block.
  enum class AttributeSite : std::uint8_t
  {
    FUNCTION,
    GLOBAL,
    EXTERN,
    LOCAL,
    BLOCK,
    RETURN,
  };

  /// What an attribute stands on, as a finding says it.
  [[nodiscard]] static std::string_view whatStands( AttributeSite site )
  {
    switch ( site )
    {
    case AttributeSite::FUNCTION:
      return "a function";
    case AttributeSite::BLOCK:
      return "a block";
    case AttributeSite::LOCAL:
      return "a local";
    case AttributeSite::RETURN:
      return "a `return`";
    case AttributeSite::GLOBAL:
    case AttributeSite::EXTERN:
      break;
    }
    return "a declaration";
  }

  /// The attributes of a declaration, a function or a block: the subset's,
  /// written without a prefix — see
  /// docs/decisions/0088-an-attribute-has-no-prefix.md; `[[striped]]` on an
  /// array alone, `[[in(PANE)]]` on a function or an object at file scope,
  /// `[[with(...)]]` before a block — see docs/decisions/0096-panes-in-c.md.
  /// `[[transition(PHASE)]]` before a `return` — see
  /// docs/decisions/0064-phase-in-c.md. What `with` is given is read where
  /// the block is checked, and what `transition` is where the `return` is.
  /// What `[[placement]]` is refused over, and where it says nothing — see
  /// docs/decisions/0210-placement-is-declared-in-c-too.md. A pointer is read
  /// through by `(zp),y` and lies in the zero page whatever is asked; a
  /// striped array's place is settled by its layout; and asking for the class
  /// a declaration has anyway is a `.off` that silenced nothing.
  void placementAsked( Declaration const& declared,
                       std::optional<model::PlacementClass> asked,
                       bool stripes,
                       bool atFileScope )
  {
    if ( !asked.has_value() )
    {
      return;
    }
    auto const at = [&]( diag::DiagnosticId id, Token const& name )
    { return diagnostic( id ).at( name.location, name.length ).arg( "name", mTyping.spellingOf( name ) ); };
    for ( std::size_t index = 0; index < declared.declarators.size(); ++index )
    {
      Token const& declarator = declared.declarators[index];
      if ( stripes )
      {
        mTyping.add( at( diag::DiagnosticId::C_PLACEMENT_AND_STRIPED, declarator ) );
        continue;
      }
      bool const pointer =
          index < declared.shapes.size() && declared.shapes[index].isPointer && !declared.shapes[index].isArray;
      if ( pointer && *asked == model::PlacementClass::ABSOLUTE )
      {
        mTyping.add( at( diag::DiagnosticId::C_PLACEMENT_ON_POINTER, declarator ) );
        continue;
      }
      bool const already = atFileScope ? ( *asked == model::PlacementClass::ABSOLUTE && !pointer )
                                       : ( *asked == model::PlacementClass::ZEROPAGE && mPlacement.back() == *asked );
      if ( already )
      {
        mTyping.add(
            at( diag::DiagnosticId::C_PLACEMENT_SAYS_NOTHING, declarator )
                .arg( "class", std::string{ *asked == model::PlacementClass::ZEROPAGE ? "zeropage" : "absolute" } ) );
      }
    }
  }

  /// The classes in force while a body is checked, the function's at the
  /// bottom, so that an attribute asking for what is already in force can say
  /// so — see docs/decisions/0210-placement-is-declared-in-c-too.md.
  std::vector<model::PlacementClass> mPlacement{ model::PlacementClass::ZEROPAGE };

  void attributesKnown( std::vector<Attribute> const& attributes, AttributeSite site )
  {
    auto const misplaced = [&]( Attribute const& attribute, std::string_view applies, std::string_view what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_MISPLACED )
                       .at( attribute.span.begin, attribute.span.length )
                       .arg( "attribute", mTyping.spellingOf( attribute.name ) )
                       .arg( "applies", std::string{ applies } )
                       .arg( "name", std::string{ what } ) );
    };
    for ( Attribute const& attribute : attributes )
    {
      std::string const name = mTyping.spellingOf( attribute.name );
      bool const known = name == "striped" || name == "in" || name == "with" || name == "under" ||
                         name == "trampoline" || name == "transition" || name == "slot" || name == "implements" ||
                         name == "placement";
      bool const bare = name == "striped" || name == "trampoline" || name == "slot";
      if ( attribute.prefix.has_value() || !known || ( bare && attribute.hasArguments ) )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_UNKNOWN_ATTRIBUTE )
                         .at( attribute.span.begin, attribute.span.length )
                         .arg( "name", mTyping.textOf( attribute.span ) ) );
        continue;
      }
      if ( name == "slot" )
      {
        // A Slot is the one thing an `extern` declares rather than describes,
        // and its own check reads the type — see
        // docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md.
        if ( site != AttributeSite::EXTERN )
        {
          misplaced( attribute, "an `extern`", whatStands( site ) );
        }
        continue;
      }
      if ( name == "implements" )
      {
        if ( site != AttributeSite::FUNCTION && site != AttributeSite::GLOBAL )
        {
          misplaced( attribute, "a function or an object at file scope", whatStands( site ) );
        }
        continue;
      }
      if ( site == AttributeSite::EXTERN )
      {
        // The extern's own check refuses every attribute on it.
        continue;
      }
      if ( name == "placement" )
      {
        // The one attribute whose argument is a word of the model rather than
        // the name of something declared — see
        // docs/decisions/0210-placement-is-declared-in-c-too.md.
        if ( site == AttributeSite::RETURN )
        {
          misplaced( attribute, "a declaration, a block or a function", whatStands( site ) );
          continue;
        }
        std::string said;
        if ( attribute.arguments.size() == 1 && attribute.arguments.front().kind == TokenKind::IDENTIFIER )
        {
          said = mTyping.spellingOf( attribute.arguments.front() );
        }
        if ( said != "zeropage" && said != "absolute" )
        {
          mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_ARGUMENTS )
                           .at( attribute.span.begin, attribute.span.length )
                           .arg( "attribute", name )
                           .arg( "takes", std::string{ "`zeropage` or `absolute`" } ) );
        }
        continue;
      }
      if ( name == "transition" )
      {
        if ( site != AttributeSite::RETURN )
        {
          misplaced( attribute, "a `return`", whatStands( site ) );
        }
        continue;
      }
      if ( site == AttributeSite::RETURN )
      {
        misplaced( attribute, name == "striped" ? "an array" : "a declaration or a block", whatStands( site ) );
        continue;
      }
      if ( name == "striped" )
      {
        if ( site == AttributeSite::FUNCTION || site == AttributeSite::BLOCK )
        {
          misplaced( attribute, "an array", site == AttributeSite::FUNCTION ? "a function" : "a block" );
        }
        continue;
      }
      if ( name == "with" || name == "trampoline" )
      {
        if ( site != AttributeSite::BLOCK )
        {
          misplaced( attribute, "a block", site == AttributeSite::FUNCTION ? "a function" : "a declaration" );
        }
        continue;
      }
      bool const under = name == "under";
      if ( site == AttributeSite::LOCAL || site == AttributeSite::BLOCK ||
           ( under && site != AttributeSite::FUNCTION ) )
      {
        std::string_view what = "an object";
        if ( site == AttributeSite::LOCAL )
        {
          what = "a local";
        }
        else if ( site == AttributeSite::BLOCK )
        {
          what = "a block";
        }
        misplaced( attribute, under ? "a function" : "a function or an object at file scope", what );
        continue;
      }
      if ( attribute.arguments.size() != 1 || attribute.arguments.front().kind != TokenKind::IDENTIFIER )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_ARGUMENTS )
                         .at( attribute.span.begin, attribute.span.length )
                         .arg( "attribute", name )
                         .arg( "takes", std::string{ under ? "one name, a pane" : "one name, a pane or a family" } ) );
        continue;
      }
      std::string const pane = mTyping.spellingOf( attribute.arguments.front() );
      ExternalName const* const named = paneNamed( pane );
      if ( named == nullptr || ( under && named->paneCount > 1 ) )
      {
        std::optional<Meaning> const meaning = mTyping.lookupName( pane );
        std::string what = meaning.has_value() ? kindOfName( *meaning ) : "a name the program does not know";
        mTyping.add( diagnostic( diag::DiagnosticId::C_PANE_NAME )
                         .at( attribute.arguments.front().location, attribute.arguments.front().length )
                         .arg( "name", pane )
                         .arg( "what", std::move( what ) )
                         .arg( "form", std::string{ under ? "under(NAME)" : "in(NAME)" } )
                         .arg( "takes", std::string{ under ? "a pane" : "a pane or a family" } ) );
      }
    }
  }

  /// The Pane the function being checked runs with shown: the one it is
  /// `in`, or the one it declares it runs `under`; empty in `fixed`.
  [[nodiscard]] std::string const& ownPane() const
  {
    return mFunction->pane.empty() ? mFunction->under : mFunction->pane;
  }

  /// The innermost block around the statement being checked that shows one
  /// Pane, which is the Pane the Procs of a `switch` there run under, or
  /// nothing — see docs/decisions/0098-a-proc-declares-what-is-shown.md.
  [[nodiscard]] ExternalName const* paneShown() const
  {
    for ( std::size_t depth = mShown.size(); depth > 0; --depth )
    {
      ExternalName const* const named = paneNamed( mShown[depth - 1].pane );
      if ( named != nullptr && named->paneCount == 1 )
      {
        return named;
      }
    }
    return nullptr;
  }

  /// A striped array: an array, of elements that lie in stripes, 256 at most —
  /// see docs/decisions/0089-a-stripe-is-a-section.md.
  void striped( Token const& declarator, DeclaratorShape const& shape, Meaning const& type )
  {
    std::string const name = mTyping.spellingOf( declarator );
    if ( !shape.isArray || shape.isPointer )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_MISPLACED )
                       .at( declarator.location, declarator.length )
                       .arg( "attribute", std::string{ "striped" } )
                       .arg( "applies", std::string{ "an array" } )
                       .arg( "name", name ) );
      return;
    }
    if ( !mTyping.stripable( type.type, type.aggregate ) )
    {
      Typed kind = type.aggregate.has_value() ? blockOf( *type.aggregate ) : typed( type.type );
      if ( type.enumeration.has_value() )
      {
        kind = enumerated( *type.enumeration );
      }
      mTyping.add( diagnostic( diag::DiagnosticId::C_NOT_STRIPABLE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", name )
                       .arg( "what", mTyping.describe( kind ) ) );
      return;
    }
    if ( type.count.value_or( 0 ) > MAX_STRIPED_ELEMENTS )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_STRIPED_TOO_LONG )
                       .at( declarator.location, declarator.length )
                       .arg( "name", name )
                       .arg( "count", static_cast<std::int64_t>( type.count.value_or( 0 ) ) ) );
    }
  }

  /// Whether a declarator's shape is one the subset has: in a declaration of
  /// pointers or of none, and no array of pointers.
  bool shaped( Declaration const& declaration, std::size_t index )
  {
    DeclaratorShape const& shape = declaration.shapes[index];
    Token const& declarator = declaration.declarators[index];
    if ( shape.isPointer != declaration.shapes.front().isPointer )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_MIXED_DECLARATORS )
                       .at( declarator.location, declarator.length )
                       .arg( "name", mTyping.spellingOf( declarator ) )
                       .arg( "what", std::string{ shape.isPointer ? "a pointer" : "no pointer" } ) );
      return false;
    }
    if ( shape.isPointer && shape.isArray )
    {
      mTyping.refuse( declaration.span, "an array of pointers" );
      return false;
    }
    return true;
  }

  /// Whether an expression reads the name being declared, which is in scope
  /// there, as C has it, and holds nothing yet — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
  /// The first read is reported.
  bool readsItself( Token const& declarator, Expression const& value )
  {
    std::vector<Expression const*> reads;
    namesIn( value, reads );
    std::string const spelling = mTyping.spellingOf( declarator );
    auto const self = std::ranges::find_if(
        reads, [&]( Expression const* read ) { return mTyping.spellingOf( read->token ) == spelling; } );
    if ( self == reads.end() )
    {
      return false;
    }
    mTyping.add( diagnostic( diag::DiagnosticId::C_READ_IN_OWN_INITIALISER )
                     .at( ( *self )->token.location, ( *self )->token.length )
                     .arg( "name", spelling ) );
    return true;
  }

  /// The number of elements of an array: its size, a constant of at least one
  /// whose elements fit the address space, or else the length of its list.
  std::optional<std::uint32_t> arraySize( Token const& declarator,
                                          DeclaratorShape const& array,
                                          ir::Type type,
                                          std::optional<std::uint32_t> aggregate = std::nullopt )
  {
    std::int64_t const largest = MAX_ARRAY_BYTES / std::max<std::uint32_t>( 1, mTyping.bytes( type, aggregate ) );
    auto const refuse = [&]
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ARRAY_SIZE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", mTyping.spellingOf( declarator ) )
                       .arg( "largest", largest ) );
      return std::nullopt;
    };
    if ( array.size == nullptr )
    {
      if ( !array.hasList || array.list.empty() )
      {
        return refuse();
      }
      return static_cast<std::uint32_t>( array.list.size() );
    }
    if ( readsItself( declarator, *array.size ) )
    {
      return std::nullopt;
    }
    Typed const kind = mTyping.expression( *array.size );
    if ( !kind.valid )
    {
      return std::nullopt;
    }
    std::int64_t const count = isKnown( kind ) ? mTyping.fold( *array.size, std::nullopt ) : 0;
    if ( count < 1 || count > largest )
    {
      return refuse();
    }
    return static_cast<std::uint32_t>( count );
  }

  /// What a declarator is given: a `const` is given a value, an array a list
  /// no longer than it, and a value an object keeps from the start is a
  /// constant. The value of a scalar constant, where it is one.
  std::optional<std::int64_t> given( Token const& declarator,
                                     Expression const* value,
                                     DeclaratorShape const& array,
                                     bool isConst,
                                     bool constantRequired,
                                     Meaning const& type )
  {
    bool const aggregate = type.aggregate.has_value() && !array.isArray;
    if ( ( array.isArray && value != nullptr ) || ( !array.isArray && !aggregate && array.hasList ) )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_LIST_MISMATCH )
                       .at( declarator.location, declarator.length )
                       .arg( "name", mTyping.spellingOf( declarator ) )
                       .arg( "what", std::string{ array.isArray ? "an array" : "no array" } )
                       .arg( "given", std::string{ array.isArray ? "a single value" : "a list" } ) );
      return std::nullopt;
    }
    if ( value == nullptr && !array.hasList )
    {
      if ( isConst )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_CONST_WITHOUT_VALUE )
                         .at( declarator.location, declarator.length )
                         .arg( "name", mTyping.spellingOf( declarator ) ) );
      }
      return std::nullopt;
    }
    if ( !array.hasList )
    {
      return single( declarator, *value, constantRequired, type );
    }
    if ( aggregate )
    {
      aggregateList( declarator, array.list, *type.aggregate, array.listSpan, constantRequired );
      return std::nullopt;
    }
    // A literal's end is dropped where the array holds its characters exactly.
    std::size_t const given = array.fromLiteral && type.count.has_value() && array.list.size() == *type.count + 1
                                  ? array.list.size() - 1
                                  : array.list.size();
    if ( type.count.has_value() && given > *type.count )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_LIST_TOO_LONG )
                       .at( array.listSpan.begin, array.listSpan.length )
                       .arg( "name", mTyping.spellingOf( declarator ) )
                       .arg( "count", static_cast<std::int64_t>( *type.count ) )
                       .arg( "given", static_cast<std::int64_t>( array.list.size() ) ) );
    }
    Meaning element = type;
    element.isArray = false;
    for ( ExpressionPtr const& listed : array.list )
    {
      listElement( declarator, *listed, element, constantRequired );
    }
    return std::nullopt;
  }

  /// One value a list holds for a place of `slot`'s type: a list of its own
  /// for a `struct` or a `union`, or a value — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
  void listElement( Token const& declarator, Expression const& value, Meaning const& slot, bool constantRequired )
  {
    if ( value.kind != ExpressionKind::LIST )
    {
      single( declarator, value, constantRequired, slot );
      return;
    }
    if ( !slot.aggregate.has_value() )
    {
      listShape( value.span, "a value of `" + mTyping.spellingOf( declarator ) + "`", "a list" );
      return;
    }
    std::vector<ExpressionPtr> const& elements = value.arguments;
    aggregateList( declarator, elements, *slot.aggregate, value.span, constantRequired );
  }

  /// The list a `struct` or a `union` is given: a value per member in order, a
  /// union's first alone, an array member's in a list of its own.
  void aggregateList( Token const& declarator,
                      std::vector<ExpressionPtr> const& elements,
                      std::uint32_t aggregateIndex,
                      diag::SourceSpan span,
                      bool constantRequired )
  {
    AggregateType const& type = mTyping.aggregate( aggregateIndex );
    std::size_t const holds = type.isUnion ? 1 : type.members.size();
    if ( elements.size() > holds )
    {
      listShape( span,
                 "`" + type.name + "`",
                 std::to_string( elements.size() ) + " values for " + std::to_string( holds ) +
                     ( type.isUnion ? " member, its first" : " members" ) );
      return;
    }
    for ( std::size_t index = 0; index < elements.size(); ++index )
    {
      MemberType const& member = type.members[index];
      Meaning slot{ .kind = NameKind::OBJECT,
                    .type = member.type.type,
                    .enumeration = member.type.enumeration,
                    .byte = {},
                    .owner = {} };
      slot.pointee = member.type.pointee;
      slot.aggregate = member.type.aggregate;
      Expression const& value = *elements[index];
      if ( !member.isArray )
      {
        listElement( declarator, value, slot, constantRequired );
        continue;
      }
      std::string const memberName = "`" + type.name + "::" + mTyping.spellingOf( member.name ) + "`";
      if ( value.kind != ExpressionKind::LIST )
      {
        listShape( value.span, memberName + ", an array,", "no list" );
        continue;
      }
      if ( value.arguments.size() > member.count )
      {
        listShape( value.span,
                   memberName + ", of " + std::to_string( member.count ) + " elements,",
                   std::to_string( value.arguments.size() ) + " values" );
        continue;
      }
      for ( ExpressionPtr const& element : value.arguments )
      {
        listElement( declarator, *element, slot, constantRequired );
      }
    }
  }

  void listShape( diag::SourceSpan span, std::string const& what, std::string const& given )
  {
    mTyping.add( diagnostic( diag::DiagnosticId::C_LIST_SHAPE )
                     .at( span.begin, span.length )
                     .arg( "what", what )
                     .arg( "given", given ) );
  }

  /// One value given, of the object's type, or of an element of the array's.
  std::optional<std::int64_t>
  single( Token const& declarator, Expression const& value, bool constantRequired, Meaning const& type )
  {
    if ( readsItself( declarator, value ) )
    {
      return std::nullopt;
    }
    Typed const kind = mTyping.expression( value );
    if ( !kind.valid )
    {
      return std::nullopt;
    }
    mTyping.assignable( value, kind, type.type, type.enumeration, type.pointee, type.aggregate );
    if ( isKnown( kind ) )
    {
      return wrapped( mTyping.fold( value, type.type ), type.type );
    }
    // An address the assembler knows, and `nullptr`, are what a pointer at
    // file scope may be given.
    if ( constantRequired && !kind.address && !kind.isNull )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_NOT_CONSTANT )
                       .at( value.span.begin, value.span.length )
                       .arg( "what", "`" + mTyping.spellingOf( declarator ) + "`" ) );
    }
    return std::nullopt;
  }

  /// A function's parameters are locals of the scope its body's statements
  /// stand in, so that the body declares none of their names again.
  void functionDefinition( FunctionDefinition const& function )
  {
    attributesKnown( function.attributes, AttributeSite::FUNCTION );
    // The class its body is checked under, which a block may narrow and a
    // declaration may narrow again — see
    // docs/decisions/0210-placement-is-declared-in-c-too.md.
    mPlacement.assign(
        1, placementAttribute( function.attributes, *mSources ).value_or( model::PlacementClass::ZEROPAGE ) );
    Definition const* const self = definitionOf( *mSources, *mUnit, function );
    if ( self == nullptr )
    {
      return;
    }
    mFunction = self;
    mTyping.within( self );
    implementsSlot( function.name, *self );
    if ( !self->pane.empty() && !self->under.empty() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_UNDER_WITH_IN )
                       .at( function.name.location, function.name.length )
                       .arg( "name", mTyping.spellingOf( function.name ) ) );
    }
    std::vector<CallSite> calls;
    mTyping.recordCalls( &calls );
    mCalls = &calls;
    mChecked = 0;
    mShown.clear();
    mBlockLoops.clear();
    mBlockSwitches.clear();
    mDeclaredIn.clear();
    mCarrying.clear();

    mTyping.openScope();
    std::string const owner = mTyping.spellingOf( function.name );
    for ( std::size_t index = 0; index < function.parameters.size(); ++index )
    {
      Parameter const& written = function.parameters[index];
      NamedType const& type = self->parameters[index].type;
      Meaning parameter{ .kind = NameKind::OBJECT,
                         .type = type.type,
                         .enumeration = type.enumeration,
                         .byte = mTyping.spellingOf( written.name ),
                         .owner = owner,
                         .definition = nullptr };
      parameter.pointee = type.pointee;
      parameter.aggregate = type.aggregate;
      parameter.isParameter = true;
      // A parameter is a byte of its Proc, which only the Proc reaches.
      if ( written.pointeeVolatile && !written.isPointer )
      {
        mTyping.refuse( written.name.span(), "a `volatile` parameter" );
      }
      if ( std::optional<diag::SourceSpan> const size = written.size; size.has_value() )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_ARRAY_PARAMETER_SIZE )
                         .at( size->begin, size->length )
                         .arg( "name", mTyping.spellingOf( written.name ) ) );
      }
      if ( !mTyping.declareLocal( written.name, parameter ) )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_REDEFINITION )
                         .at( written.name.location, written.name.length )
                         .arg( "name", mTyping.spellingOf( written.name ) ) );
      }
    }
    for ( Statement const& item : function.body.items )
    {
      statement( item );
    }
    mTyping.closeScope();

    mTyping.recordCalls( nullptr );
    mCalls = nullptr;
    for ( CallSite const& call : calls )
    {
      if ( call.callee != nullptr )
      {
        mEdges->push_back(
            CallEdge{ .caller = self, .callee = call.callee, .unit = mIndex, .span = call.span, .sink = mUnit->sink } );
      }
    }
  }

  /// The calls the statement just checked made, against the Panes: one into a
  /// family is made in a block that shows the member; one into another Pane
  /// of a Window a block shows is refused, since the exit of its `.with`
  /// would put back what the function's own code shows and not the block's;
  /// and a callee in the caller's own Pane, or in what a block shows, needs no
  /// switch — see docs/decisions/0096-panes-in-c.md.
  void checkCalls()
  {
    if ( mCalls == nullptr )
    {
      return;
    }
    for ( ; mChecked < mCalls->size(); ++mChecked )
    {
      CallSite const& call = ( *mCalls )[mChecked];
      std::string const name = call.callee != nullptr ? mTyping.spellingOf( call.callee->name ) : call.external->name;
      // A Proc that runs under a Pane is called where the Pane is shown —
      // see docs/decisions/0098-a-proc-declares-what-is-shown.md.
      std::string const under = underOfCallee( call.callee, call.external );
      ExternalName const* const runsWith = paneNamed( under );
      if ( runsWith != nullptr && under != ownPane() &&
           !std::ranges::any_of( mShown, [&]( Shown const& block ) { return shows( block, *runsWith ); } ) )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_UNDER_CALLED_ELSEWHERE )
                         .at( call.span.begin, call.span.length )
                         .arg( "name", name )
                         .arg( "pane", under ) );
        continue;
      }
      std::string const pane = paneOfCallee( call.callee, call.external );
      ExternalName const* const shownPane = paneNamed( pane );
      if ( shownPane == nullptr || pane == ownPane() )
      {
        continue;
      }
      if ( std::ranges::any_of( mShown, [&]( Shown const& block ) { return shows( block, *shownPane ); } ) )
      {
        continue;
      }
      if ( shownPane->paneCount > 1 )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_CALL_INTO_FAMILY )
                         .at( call.span.begin, call.span.length )
                         .arg( "name", name )
                         .arg( "family", pane ) );
        continue;
      }
      // Code in a Pane cannot switch its own Window: the call goes through a
      // Trampoline, which the programmer writes.
      ExternalName const* const own = paneNamed( mFunction->pane );
      if ( own != nullptr && own->window == shownPane->window )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_CALL_ACROSS_OWN_WINDOW )
                         .at( call.span.begin, call.span.length )
                         .arg( "name", name )
                         .arg( "pane", pane )
                         .arg( "window", shownPane->window )
                         .arg( "own", mFunction->pane ) );
        continue;
      }
      if ( std::ranges::any_of( mShown, [&]( Shown const& block ) { return block.window == shownPane->window; } ) )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_CALL_ACROSS_BLOCK_WINDOW )
                         .at( call.span.begin, call.span.length )
                         .arg( "name", name )
                         .arg( "pane", pane )
                         .arg( "window", shownPane->window ) );
      }
    }
  }

  /// The Pane or family a name is, or null.
  [[nodiscard]] ExternalName const* paneNamed( std::string_view name ) const
  {
    if ( name.empty() )
    {
      return nullptr;
    }
    std::optional<Meaning> const meaning = mTyping.lookupName( name );
    return meaning.has_value() && meaning->external != nullptr && meaning->external->kind == ExternalKind::PANE
               ? meaning->external
               : nullptr;
  }

  /// A block written `[[with(...)]]`: what it shows, on a Window no block
  /// around it shows; inside it `return`, and `break` or `continue` of a loop
  /// around it, are refused, and so is a `switch` — see
  /// docs/decisions/0096-panes-in-c.md.
  void withBlock( Statement const& node )
  {
    attributesKnown( node.attributes, AttributeSite::BLOCK );
    std::optional<Attribute> const attribute = withOf( node.attributes, *mSources );
    std::optional<Shown> shown;
    if ( attribute.has_value() )
    {
      shown = withShown( *attribute );
    }
    else if ( isTrampoline( node.attributes, *mSources ) )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_ARGUMENTS )
                       .at( node.attributes.front().span.begin, node.attributes.front().span.length )
                       .arg( "attribute", std::string{ "trampoline" } )
                       .arg( "takes", std::string{ "a `with` beside it, whose block it makes a trampoline" } ) );
    }
    if ( attribute.has_value() && shown.has_value() &&
         std::ranges::any_of( mShown, [&]( Shown const& outer ) { return outer.window == shown->window; } ) )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_WITH_NESTED_WINDOW )
                       .at( attribute->span.begin, attribute->span.length )
                       .arg( "window", shown->window ) );
      shown.reset();
    }
    if ( attribute.has_value() && shown.has_value() )
    {
      trampolineForm( node, *attribute, *shown );
    }
    // A block that could not be resolved is checked as a plain one, so that
    // what it holds is still read.
    if ( !shown.has_value() )
    {
      mTyping.openScope();
      for ( Statement const& item : node.items )
      {
        statement( item );
      }
      mTyping.closeScope();
      return;
    }
    mShown.push_back( *shown );
    mBlockLoops.push_back( mLoops );
    mBlockSwitches.push_back( mSwitches );
    mLoops = 0;
    mTyping.openScope();
    for ( Statement const& item : node.items )
    {
      statement( item );
    }
    mTyping.closeScope();
    mLoops = mBlockLoops.back();
    mBlockLoops.pop_back();
    mBlockSwitches.pop_back();
    mShown.pop_back();
  }

  /// The form of a block held to its place — see
  /// docs/decisions/0097-a-trampoline-is-declared.md. Code in a Pane cannot
  /// switch its own Window, so a block there on another state of the Window
  /// is a Trampoline and says so with `trampoline`; a block on the Pane the
  /// code is in, or on the family, shows what is shown. Anywhere else the
  /// block is a macro under `.with`, and `trampoline` is refused.
  void trampolineForm( Statement const& node, Attribute const& attribute, Shown const& shown )
  {
    bool const written = isTrampoline( node.attributes, *mSources );
    ExternalName const* const own = paneNamed( mFunction->pane );
    if ( own != nullptr && own->window == shown.window && ( shown.pane == mFunction->pane || own->paneCount > 1 ) )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_WITH_OWN_PANE )
                       .at( attribute.span.begin, attribute.span.length )
                       .arg( "pane", mFunction->pane ) );
      return;
    }
    bool const needed = own != nullptr && own->window == shown.window;
    if ( needed && !written )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_TRAMPOLINE_NEEDED )
                       .at( attribute.span.begin, attribute.span.length )
                       .arg( "window", shown.window )
                       .arg( "pane", mFunction->pane ) );
    }
    else if ( !needed && written )
    {
      std::string where = "the code is in fixed";
      if ( !mFunction->under.empty() )
      {
        where = "the code runs under `" + mFunction->under + "`, which it shows again itself";
      }
      else if ( own != nullptr )
      {
        where = "the block is on another window than the code's pane";
      }
      mTyping.add( diagnostic( diag::DiagnosticId::C_TRAMPOLINE_NOT_NEEDED )
                       .at( attribute.span.begin, attribute.span.length )
                       .arg( "where", std::move( where ) ) );
    }
  }

  /// What `[[with(...)]]` names, held to its form: a Pane for `with(PANE)`, a
  /// family or a Window with an index for `with(NAME, i)`, a Window for
  /// `with(WINDOW = STATE)`; the index a `u8` or a constant.
  std::optional<Shown> withShown( Attribute const& attribute )
  {
    std::optional<WithSpec> const spec = withAttribute( attribute );
    if ( !spec.has_value() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_ARGUMENTS )
                       .at( attribute.span.begin, attribute.span.length )
                       .arg( "attribute", std::string{ "with" } )
                       .arg( "takes",
                             std::string{ "`with(PANE)`, `with(FAMILY, i)`, `with(WINDOW, i)` or "
                                          "`with(WINDOW = STATE)`, with `i` a name or a constant" } ) );
      return std::nullopt;
    }
    std::string const name = mTyping.spellingOf( spec->name );
    std::optional<Meaning> const meaning = mTyping.lookupName( name );
    ExternalName const* const external =
        meaning.has_value() && meaning->definition == nullptr ? meaning->external : nullptr;
    bool const isPane = external != nullptr && external->kind == ExternalKind::PANE && external->paneCount == 1;
    bool const isFamily = external != nullptr && external->kind == ExternalKind::PANE && external->paneCount > 1;
    bool const isWindow = external != nullptr && external->kind == ExternalKind::WINDOW;
    auto const refuse = [&]( std::string form, std::string takes )
    {
      std::string what = meaning.has_value() ? kindOfName( *meaning ) : "a name the program does not know";
      mTyping.add( diagnostic( diag::DiagnosticId::C_PANE_NAME )
                       .at( spec->name.location, spec->name.length )
                       .arg( "name", name )
                       .arg( "what", std::move( what ) )
                       .arg( "form", std::move( form ) )
                       .arg( "takes", std::move( takes ) ) );
    };
    switch ( spec->form )
    {
    case ir::WithForm::PANE:
      if ( !isPane )
      {
        refuse( "with(NAME)", "a pane" );
        return std::nullopt;
      }
      return Shown{ .window = external->window, .pane = name };
    case ir::WithForm::AT:
    {
      if ( !isFamily && !isWindow )
      {
        refuse( "with(NAME, i)", "a family or a window" );
        return std::nullopt;
      }
      if ( !spec->index.has_value() || !withIndex( name, *spec->index ) )
      {
        return std::nullopt;
      }
      return Shown{ .window = isWindow ? name : external->window, .pane = isFamily ? name : std::string{} };
    }
    case ir::WithForm::STATE:
      if ( !isWindow || !spec->state.has_value() )
      {
        refuse( "with(NAME = STATE)", "a window" );
        return std::nullopt;
      }
      return Shown{ .window = name, .state = mTyping.spellingOf( *spec->state ) };
    }
    return std::nullopt;
  }

  /// The index of `with(NAME, i)`: a `u8` object, or a constant `X` holds.
  bool withIndex( std::string const& name, Token const& index )
  {
    auto const refuse = [&]( std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_WITH_INDEX )
                       .at( index.location, index.length )
                       .arg( "name", name )
                       .arg( "what", std::move( what ) ) );
      return false;
    };
    if ( index.kind == TokenKind::INTEGER_CONSTANT )
    {
      std::int64_t const value = valueOfIntegerConstant( mTyping.spellingOf( index ) ).value_or( 0 );
      return value >= 0 && value <= 255 ? true : refuse( "a constant no `u8` holds" );
    }
    std::optional<Meaning> const meaning = mTyping.lookupSpelling( mTyping.spellingOf( index ) );
    if ( !meaning.has_value() )
    {
      return refuse( "a name nothing defines" );
    }
    if ( meaning->kind != NameKind::OBJECT || meaning->isArray || meaning->type != ir::Type::U8 ||
         meaning->enumeration.has_value() || isAssemblerConstant( *meaning ) )
    {
      return refuse( meaning->kind == NameKind::OBJECT && !meaning->isArray && !isAssemblerConstant( *meaning )
                         ? "a `" + std::string{ ir::spellingOf( meaning->type ) } + "`"
                         : kindOfName( *meaning ) );
    }
    return true;
  }

  /// The block, innermost first, whose data an expression carries the address
  /// of: `&map[i]` of what a block shows, an array of it read as a pointer, a
  /// local of the block holding such an address, and what is computed from
  /// one. The depth of the block, counted from one; nothing for an address
  /// of nothing shown.
  std::optional<std::size_t> carries( Expression const& node )
  {
    switch ( node.kind )
    {
    case ExpressionKind::IDENTIFIER:
    {
      std::optional<Meaning> const meaning = mTyping.lookup( node );
      if ( !meaning.has_value() )
      {
        return std::nullopt;
      }
      if ( meaning->declaredAt.has_value() )
      {
        auto const found = mCarrying.find( meaning->declaredAt->rawOffset() );
        return found != mCarrying.end() ? std::optional{ found->second } : std::nullopt;
      }
      return meaning->isArray ? shownDepth( *meaning ) : std::nullopt;
    }
    case ExpressionKind::ADDRESS:
      return addressed( *node.left );
    case ExpressionKind::BINARY:
      if ( node.token.kind == TokenKind::PLUS || node.token.kind == TokenKind::MINUS )
      {
        std::optional<std::size_t> const left = carries( *node.left );
        std::optional<std::size_t> const right = carries( *node.right );
        return std::max( left, right );
      }
      return std::nullopt;
    case ExpressionKind::CAST:
      return carries( *node.left );
    case ExpressionKind::MEMBER:
    case ExpressionKind::INDEX:
    {
      // An array member, or an element that is one, read as a pointer.
      std::optional<Shape> const shape = mTyping.shape( node );
      return shape.has_value() && shape->isArray ? addressed( node ) : std::nullopt;
    }
    default:
      return std::nullopt;
    }
  }

  /// Whose address `&PLACE` is: the object at the root of a chain of `.` and
  /// `[]`, where a block shows it, or what a pointer along the chain carries.
  std::optional<std::size_t> addressed( Expression const& place )
  {
    Expression const* node = &place;
    while ( true )
    {
      if ( node->kind == ExpressionKind::IDENTIFIER )
      {
        std::optional<Meaning> const meaning = mTyping.lookup( *node );
        if ( !meaning.has_value() )
        {
          return std::nullopt;
        }
        if ( meaning->pointee.has_value() && !meaning->isArray )
        {
          return carries( *node );
        }
        return shownDepth( *meaning );
      }
      if ( node->kind == ExpressionKind::DEREFERENCE ||
           ( node->kind == ExpressionKind::MEMBER && node->token.kind == TokenKind::ARROW ) )
      {
        return carries( *node->left );
      }
      if ( node->kind == ExpressionKind::MEMBER || node->kind == ExpressionKind::INDEX )
      {
        // Read without typing again, which would report again: `p[i]` through
        // a pointer is what the pointer carries.
        std::optional<Shape> const left = mTyping.shape( *node->left );
        if ( node->kind == ExpressionKind::INDEX && left.has_value() && left->pointee.has_value() && !left->isArray )
        {
          return carries( *node->left );
        }
        node = node->left.get();
        continue;
      }
      return std::nullopt;
    }
  }

  /// The depth of the innermost block showing the Pane an object is in.
  [[nodiscard]] std::optional<std::size_t> shownDepth( Meaning const& meaning ) const
  {
    std::string const pane =
        paneOfCallee( meaning.kind == NameKind::OBJECT ? meaning.definition : nullptr, meaning.external );
    ExternalName const* const shownPane = paneNamed( pane );
    if ( shownPane == nullptr )
    {
      return std::nullopt;
    }
    for ( std::size_t depth = mShown.size(); depth > 0; --depth )
    {
      if ( shows( mShown[depth - 1], *shownPane ) )
      {
        return depth;
      }
    }
    return std::nullopt;
  }

  /// The name at the root of what an address is taken of, for a finding.
  [[nodiscard]] std::string rootName( Expression const& node ) const
  {
    Expression const* root = &node;
    while ( root->left != nullptr && root->kind != ExpressionKind::CALL )
    {
      root = root->left.get();
    }
    return mTyping.textOf( root->span );
  }

  /// An address of what a block shows kept: in a local declared in that block
  /// or one inside it, which then carries it; anywhere else it escapes the
  /// block, and is refused — see docs/decisions/0096-panes-in-c.md.
  void addressKept( Expression const& value, Expression const* target, std::optional<Token> declared )
  {
    std::optional<std::size_t> const depth = carries( value );
    if ( !depth.has_value() )
    {
      return;
    }
    std::optional<std::uint32_t> local;
    std::string escapes = "is stored outside a local";
    if ( declared.has_value() )
    {
      local = declared->location.rawOffset();
    }
    else if ( target != nullptr && target->kind == ExpressionKind::IDENTIFIER )
    {
      std::optional<Meaning> const meaning = mTyping.lookup( *target );
      if ( meaning.has_value() && meaning->declaredAt.has_value() )
      {
        local = meaning->declaredAt->rawOffset();
      }
      escapes = "is stored in `" + mTyping.textOf( target->span ) + "`, declared outside the block";
    }
    if ( local.has_value() )
    {
      auto const found = mDeclaredIn.find( *local );
      if ( found != mDeclaredIn.end() && found->second >= *depth )
      {
        mCarrying[*local] = *depth;
        return;
      }
    }
    mTyping.add( diagnostic( diag::DiagnosticId::C_ADDRESS_LEAVES_BLOCK )
                     .at( value.span.begin, value.span.length )
                     .arg( "name", rootName( value ) )
                     .arg( "escapes", escapes ) );
  }

  /// `return;` in a function that returns nothing, and `return x;`, with a
  /// value assignable to the result, in one that returns something.
  void returnStatement( Statement const& statement )
  {
    std::string const name = mTyping.spellingOf( mFunction->name );
    attributesKnown( statement.attributes, AttributeSite::RETURN );
    bool const transition = hasAttribute( statement.attributes, *mSources, "transition" );
    if ( transition && mShown.empty() )
    {
      // A transition never comes back, so there is no value to hand anyone
      // and no caller to hand it to — see docs/decisions/0064-phase-in-c.md.
      // In a block the `return` itself is refused below, which says more.
      std::string what;
      if ( statement.expression != nullptr )
      {
        what = "this one returns a value";
      }
      else if ( mFunction->result.has_value() )
      {
        what = "`" + name + "` returns `" + mTyping.describe( typedAs( *mFunction->result ) ) + "`";
      }
      if ( !what.empty() )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_TRANSITION_RETURNS )
                         .at( statement.span.begin, statement.span.length )
                         .arg( "what", std::move( what ) ) );
        if ( statement.expression != nullptr )
        {
          mTyping.expression( *statement.expression );
        }
        return;
      }
      if ( nameAttribute( statement.attributes, *mSources, "transition" ).empty() )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_ARGUMENTS )
                         .at( statement.span.begin, statement.span.length )
                         .arg( "attribute", std::string{ "transition" } )
                         .arg( "takes", std::string{ "one name, a phase of the project" } ) );
      }
      return;
    }
    if ( !mShown.empty() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_LEAVES_WITH_BLOCK )
                       .at( statement.span.begin, statement.span.length )
                       .arg( "keyword", std::string{ "return" } ) );
      if ( statement.expression != nullptr )
      {
        mTyping.expression( *statement.expression );
      }
      return;
    }
    if ( statement.expression == nullptr )
    {
      if ( mFunction->result.has_value() )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_RETURN_WITHOUT_VALUE )
                         .at( statement.span.begin, statement.span.length )
                         .arg( "name", name )
                         .arg( "type", mTyping.describe( typedAs( *mFunction->result ) ) ) );
      }
      return;
    }
    Typed const value = mTyping.expression( *statement.expression );
    if ( !mFunction->result.has_value() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_RETURN_WITH_VALUE )
                       .at( statement.span.begin, statement.span.length )
                       .arg( "name", name ) );
      return;
    }
    if ( value.valid )
    {
      mTyping.assignable( *statement.expression,
                          value,
                          mFunction->result->type,
                          mFunction->result->enumeration,
                          mFunction->result->pointee,
                          mFunction->result->aggregate );
    }
  }

  void statement( Statement const& statement )
  {
    switch ( statement.kind )
    {
    case StatementKind::IF:
      // What the statement was written with before its condition is in scope
      // in the whole of it, its `else` included, and nowhere else — see
      // docs/decisions/0160-a-declaration-where-a-statement-begins-one.md.
      mTyping.openScope();
      initialiser( statement );
      mTyping.condition( *statement.expression );
      checkCalls();
      for ( Statement const& item : statement.items )
      {
        this->statement( item );
      }
      mTyping.closeScope();
      return;
    case StatementKind::COMPOUND:
    {
      Placed const here{ mPlacement, placementAttribute( statement.attributes, *mSources ) };
      if ( !statement.attributes.empty() && !placementOnly( statement.attributes, *mSources ) )
      {
        withBlock( statement );
        return;
      }
      if ( !statement.attributes.empty() )
      {
        attributesKnown( statement.attributes, AttributeSite::BLOCK );
      }
      mTyping.openScope();
      for ( Statement const& item : statement.items )
      {
        this->statement( item );
      }
      mTyping.closeScope();
      return;
    }
    case StatementKind::CASE:
      // A block is a scope, and so is a case, whose Proc is its own.
      mTyping.openScope();
      for ( Statement const& item : statement.items )
      {
        this->statement( item );
      }
      mTyping.closeScope();
      return;
    case StatementKind::DECLARATION:
      declaration( statement );
      checkCalls();
      return;
    case StatementKind::NULL_STATEMENT:
      return;
    case StatementKind::RETURN:
      returnStatement( statement );
      checkCalls();
      return;
    case StatementKind::EXPRESSION:
      expressionStatement( *statement.expression );
      checkCalls();
      return;
    case StatementKind::WHILE:
    case StatementKind::DO:
    case StatementKind::FOR:
      mTyping.openScope();
      if ( statement.initial != nullptr )
      {
        expressionStatement( *statement.initial );
      }
      if ( statement.declaration != nullptr )
      {
        declaration( statement );
      }
      if ( statement.expression != nullptr )
      {
        mTyping.condition( *statement.expression );
      }
      if ( statement.step != nullptr )
      {
        expressionStatement( *statement.step );
      }
      checkCalls();
      ++mLoops;
      for ( Statement const& item : statement.items )
      {
        this->statement( item );
      }
      --mLoops;
      mTyping.closeScope();
      return;
    case StatementKind::SWITCH:
      mTyping.openScope();
      initialiser( statement );
      switchStatement( statement );
      mTyping.closeScope();
      return;
    case StatementKind::BREAK:
    case StatementKind::CONTINUE:
      jump( statement );
      return;
    }
  }

  /// Locals of a block: each declared once in it, hiding what an outer block
  /// or the program calls the same, and given a value of its type — a
  /// constant, where it is `static`.
  /// The type a declaration written `auto` takes from the value it is given,
  /// where it is written as one name given one value — see
  /// docs/decisions/0161-auto-takes-the-type-of-the-value.md.
  std::optional<Meaning> deducedType( Declaration const& declared )
  {
    DeclaratorShape const& shape = declared.shapes.front();
    Expression const* const value = declared.initialisers.front().get();
    if ( declared.declarators.size() != 1 )
    {
      mTyping.refuse( declared.span, "`auto` of more than one name" );
      return std::nullopt;
    }
    if ( shape.isPointer || shape.isArray || shape.hasList )
    {
      mTyping.refuse( declared.span, "`auto` of a pointer or an array" );
      return std::nullopt;
    }
    if ( value == nullptr )
    {
      mTyping.refuse( declared.span, "`auto` given no value" );
      return std::nullopt;
    }
    return mTyping.deducedType( declared.declarators.front(), *value );
  }

  void declaration( Statement const& statement )
  {
    Declaration const& declared = *statement.declaration;
    attributesKnown( declared.attributes, AttributeSite::LOCAL );
    bool const stripes = isStriped( declared.attributes, *mSources );
    std::optional<model::PlacementClass> const asked = placementAttribute( declared.attributes, *mSources );
    placementAsked( declared, asked, stripes, false );
    std::optional<Meaning> const type =
        declared.type.keyword == Keyword::AUTO ? deducedType( declared ) : mTyping.declaredType( declared.type );
    for ( std::size_t index = 0; index < declared.declarators.size(); ++index )
    {
      Token const& declarator = declared.declarators[index];
      Expression const* const value = declared.initialisers[index].get();
      if ( !type.has_value() )
      {
        if ( value != nullptr )
        {
          mTyping.expression( *value );
        }
        continue;
      }
      DeclaratorShape const& array = declared.shapes[index];
      if ( !shaped( declared, index ) )
      {
        continue;
      }
      if ( type->function.has_value() && !array.isPointer )
      {
        // A function type has no bytes, so no local is of it: it is a type as
        // a pointer alone — see
        // docs/decisions/0103-what-building-handlers-needs.md.
        mTyping.add( diagnostic( diag::DiagnosticId::C_FUNCTION_TYPE_VALUE )
                         .at( declarator.location, declarator.length )
                         .arg( "name", mTyping.spellingOf( declared.type ) ) );
        continue;
      }
      Meaning local = *type;
      local.isConst = declared.isConst;
      local.isVolatile = declared.isVolatile;
      local.declaredAt = declarator.location;
      if ( array.isPointer )
      {
        local.pointee = Pointee{ .type = type->type,
                                 .enumeration = type->enumeration,
                                 .isConst = declared.isConst,
                                 .aggregate = type->aggregate,
                                 .function = type->function,
                                 .isVolatile = declared.isVolatile };
        local.type = ir::Type::POINTER;
        local.enumeration = std::nullopt;
        local.aggregate = std::nullopt;
        local.function = std::nullopt;
        local.isConst = array.isConstPointer;
        local.isVolatile = array.isVolatilePointer;
      }
      if ( array.isArray )
      {
        local.isArray = true;
        local.count = arraySize( declarator, array, local.type, local.aggregate );
      }
      if ( stripes )
      {
        striped( declarator, array, local );
        local.isStriped = array.isArray && !array.isPointer;
      }
      bool const added = mTyping.declareLocal( declarator, local );
      if ( !added )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_REDEFINITION )
                         .at( declarator.location, declarator.length )
                         .arg( "name", mTyping.spellingOf( declarator ) ) );
      }
      mDeclaredIn[declarator.location.rawOffset()] = mShown.size();
      std::optional<std::int64_t> const constant =
          given( declarator, value, array, local.isConst, declared.isStatic, local );
      if ( added && local.isConst && !local.isVolatile && constant.has_value() )
      {
        local.value = constant;
        mTyping.settleLocal( declarator, local );
      }
      if ( value != nullptr && !array.isArray )
      {
        addressKept( *value, nullptr, declarator );
      }
    }
  }

  /// `break` leaves the loop or the `switch` it stands in; `continue` the
  /// loop, from inside a case as from anywhere else — a case is a block of
  /// the function, so there is nothing between it and the loop's step — see
  /// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
  void jump( Statement const& statement )
  {
    bool const isBreak = statement.kind == StatementKind::BREAK;
    if ( mLoops == 0 && !mShown.empty() && ( !isBreak || mSwitches <= mBlockSwitches.back() ) )
    {
      // No loop inside the block, and no `switch` for a `break`: whatever it
      // leaves, it leaves the block.
      mTyping.add( diagnostic( diag::DiagnosticId::C_LEAVES_WITH_BLOCK )
                       .at( statement.span.begin, statement.span.length )
                       .arg( "keyword", std::string{ isBreak ? "break" : "continue" } ) );
      return;
    }
    if ( mLoops > 0 || ( isBreak && mSwitches > 0 ) )
    {
      return;
    }
    mTyping.add( diagnostic( diag::DiagnosticId::C_OUTSIDE_LOOP )
                     .at( statement.span.begin, statement.span.length )
                     .arg( "keyword", std::string{ isBreak ? "break" : "continue" } ) );
  }

  /// The declaration or the expression a statement was written with before
  /// its condition, checked in the scope its names live in.
  void initialiser( Statement const& statement )
  {
    if ( statement.initial != nullptr )
    {
      expressionStatement( *statement.initial );
    }
    if ( statement.declaration != nullptr )
    {
      declaration( statement );
    }
  }

  void switchStatement( Statement const& node )
  {
    Typed const switched = mTyping.expression( *node.expression );
    // A `switch` is the function's own code whatever its shape, so a block
    // that shows a Pane holds one as it holds any other statement: there are
    // no Procs of its own left to have nowhere to stand — see
    // docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
    Typed const& value = switched;
    checkCalls();
    std::optional<std::uint32_t> const enumeration = value.valid ? value.enumeration : std::nullopt;
    // An `enum`, or an ordinary integer, whose labels are then constants —
    // see docs/decisions/0164-a-small-switch-stands-where-it-is.md.
    ir::Type const tested = value.type.value_or( ir::Type::BOOL );
    bool const isInteger =
        !enumeration.has_value() && !value.pointee.has_value() && !value.aggregate.has_value() &&
        ( tested == ir::Type::U8 || tested == ir::Type::I8 || tested == ir::Type::U16 || tested == ir::Type::I16 );
    if ( value.valid && !enumeration.has_value() && !isInteger )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_SWITCH_NOT_ENUMERATION )
                       .at( node.expression->span.begin, node.expression->span.length )
                       .arg( "what", mTyping.describe( value ) ) );
    }

    std::vector<bool> named;
    if ( enumeration.has_value() )
    {
      named.assign( mTyping.enumeration( *enumeration ).enumerators.size(), false );
    }
    bool hasDefault = false;
    std::set<std::int64_t> values;
    ++mSwitches;
    for ( Statement const& clause : node.items )
    {
      label( clause, enumeration, named, hasDefault, tested, values );
      for ( Statement const& item : clause.items )
      {
        statement( item );
      }
    }
    --mSwitches;

    if ( !enumeration.has_value() || hasDefault )
    {
      return;
    }
    std::string missing;
    std::vector<std::string> const& enumerators = mTyping.enumeration( *enumeration ).enumerators;
    for ( std::size_t index = 0; index < named.size(); ++index )
    {
      if ( !named[index] )
      {
        missing.append( missing.empty() ? "" : ", " ).append( "`" + enumerators[index] + "`" );
      }
    }
    if ( !missing.empty() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ENUMERATORS_UNHANDLED )
                       .at( node.span.begin, node.span.length )
                       .arg( "names", missing )
                       .arg( "type", mTyping.enumeration( *enumeration ).name ) );
    }
  }

  /// A label of a `switch`: an enumerator of its type once, or `default`
  /// once.
  void label( Statement const& clause,
              std::optional<std::uint32_t> enumeration,
              std::vector<bool>& named,
              bool& hasDefault,
              ir::Type tested,
              std::set<std::int64_t>& values )
  {
    auto const repeated = [this, &clause]
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_LABEL_REPEATED )
                       .at( clause.span.begin, clause.span.length )
                       .arg( "label",
                             clause.expression == nullptr ? std::string{ "default" }
                                                          : mTyping.textOf( clause.expression->span ) ) );
    };
    if ( clause.expression == nullptr )
    {
      if ( hasDefault )
      {
        repeated();
      }
      hasDefault = true;
      return;
    }
    Typed const kind = mTyping.expression( *clause.expression );
    if ( !kind.valid )
    {
      return;
    }
    if ( !enumeration.has_value() )
    {
      // A label of a `switch` over an integer is a constant of its type — a
      // value the tested type cannot hold matches nothing it could ever hold
      // (0206) — and each value stands once.
      if ( !isKnown( kind ) || kind.pointee.has_value() || kind.enumeration.has_value() )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_NOT_CONSTANT )
                         .at( clause.expression->span.begin, clause.expression->span.length )
                         .arg( "what", mTyping.describe( kind ) ) );
        return;
      }
      if ( !mTyping.constantFits( *clause.expression, tested ) )
      {
        return;
      }
      if ( !values.insert( mTyping.fold( *clause.expression, tested ) ).second )
      {
        repeated();
      }
      return;
    }
    std::optional<std::uint32_t> const index = mTyping.enumeratorIndex( *clause.expression );
    if ( kind.enumeration != enumeration || !index.has_value() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_CASE_NOT_ENUMERATOR )
                       .at( clause.expression->span.begin, clause.expression->span.length )
                       .arg( "label", mTyping.textOf( clause.expression->span ) )
                       .arg( "type", mTyping.enumeration( *enumeration ).name ) );
      return;
    }
    if ( named[*index] )
    {
      repeated();
    }
    named[*index] = true;
  }

  void expressionStatement( Expression const& node )
  {
    switch ( node.kind )
    {
    case ExpressionKind::CALL:
      mTyping.call( node, false );
      return;
    case ExpressionKind::ASSIGNMENT:
      assignment( node );
      return;
    default:
      mTyping.refuse( node.span, "an expression statement that neither assigns nor calls" );
      return;
    }
  }

  /// An assignment statement: what it assigns is held to the same rules
  /// wherever one is written, and only the address a pointer keeps is a
  /// question of where the statement stands.
  void assignment( Expression const& node )
  {
    std::optional<Meaning> const object = mTyping.assignment( node );
    if ( object.has_value() && object->type == ir::Type::POINTER )
    {
      addressKept( *node.right, node.left.get(), std::nullopt );
    }
  }

  diag::SourceManager const* mSources;
  Unit const* mUnit;
  std::size_t mIndex;
  std::vector<CallEdge>* mEdges;
  Typing mTyping;

  /// The function being checked.
  Definition const* mFunction = nullptr;

  /// Its calls as the type check records them, and how many of them the
  /// statements checked so far have been held to the Panes.
  std::vector<CallSite>* mCalls = nullptr;
  std::size_t mChecked = 0;

  /// The `[[with]]` blocks the statement being checked stands in, outermost
  /// first, and the loops each stood in when entered — see
  /// docs/decisions/0096-panes-in-c.md.
  std::vector<Shown> mShown;
  std::vector<std::uint32_t> mBlockLoops;
  std::vector<std::uint32_t> mBlockSwitches;

  /// How many blocks each local was declared in, by where it is declared, and
  /// the depth of the block whose data a local holds the address of.
  std::map<std::uint32_t, std::size_t> mDeclaredIn;
  std::map<std::uint32_t, std::size_t> mCarrying;

  /// The loops the statement being checked stands in within its case or
  /// function, and in all.
  std::uint32_t mLoops = 0;

  /// The `switch`es it stands in.
  std::uint32_t mSwitches = 0;
};

/// A function's blocks as its statements are read. Blocks are made before
/// they are entered, so that a branch can name one it has not reached; the
/// order they are entered in is the order they are written in.
class Blocks
{
public:
  Blocks()
  {
    enter( create() );
  }

  [[nodiscard]] std::uint32_t create()
  {
    mBlocks.emplace_back();
    return static_cast<std::uint32_t>( mBlocks.size() - 1 );
  }

  /// Makes `block` the one instructions go to, the one before having been
  /// ended.
  void enter( std::uint32_t block )
  {
    mCurrent = block;
    mOpen = true;
    mOrder.push_back( block );
  }

  [[nodiscard]] bool isOpen() const
  {
    return mOpen;
  }

  void append( ir::Instruction instruction )
  {
    reopen();
    mBlocks[mCurrent].instructions.push_back( std::move( instruction ) );
  }

  void terminate( ir::Terminator terminator )
  {
    reopen();
    mBlocks[mCurrent].terminator = std::move( terminator );
    mOpen = false;
  }

  /// A `[[with]]` block's blocks, from `begin` up to `end`, which is the one
  /// entered after it: contiguous once ordered, since every block of the
  /// statements inside is entered between the two.
  void region( std::uint32_t begin, std::uint32_t end )
  {
    mRegions.push_back( ir::WithRegion{ .begin = begin, .end = end } );
  }

  /// The regions, by the positions `take` gave their blocks.
  [[nodiscard]] std::vector<ir::WithRegion> const& regions() const
  {
    return mRegions;
  }

  /// The blocks in the order they were entered, their targets renumbered to
  /// match.
  [[nodiscard]] std::vector<ir::Block> take()
  {
    std::vector<std::uint32_t> position( mBlocks.size(), 0 );
    for ( std::uint32_t index = 0; index < mOrder.size(); ++index )
    {
      position[mOrder[index]] = index;
    }
    for ( ir::WithRegion& region : mRegions )
    {
      region.begin = position[region.begin];
      region.end = position[region.end];
    }
    std::vector<ir::Block> ordered;
    ordered.reserve( mOrder.size() );
    for ( std::uint32_t const block : mOrder )
    {
      ir::Block moved = std::move( mBlocks[block] );
      moved.terminator.target = position[moved.terminator.target];
      moved.terminator.otherwise = position[moved.terminator.otherwise];
      for ( std::uint32_t& into : moved.terminator.targets )
      {
        into = position[into];
      }
      ordered.push_back( std::move( moved ) );
    }
    return ordered;
  }

private:
  /// What follows a terminator stands in a block nothing reaches, entered
  /// here.
  void reopen()
  {
    if ( !mOpen )
    {
      enter( create() );
    }
  }

  std::vector<ir::Block> mBlocks;
  std::vector<std::uint32_t> mOrder;
  std::vector<ir::WithRegion> mRegions;
  std::uint32_t mCurrent = 0;
  bool mOpen = false;
};

/// Whether flow from a function's entry reaches `goal`, over the jumps and
/// branches between its blocks.
bool reaches( std::vector<ir::Block> const& blocks, std::uint32_t goal )
{
  std::vector<bool> seen( blocks.size(), false );
  std::vector<std::uint32_t> pending{ 0 };
  seen[0] = true;
  while ( !pending.empty() )
  {
    std::uint32_t const block = pending.back();
    pending.pop_back();
    if ( block == goal )
    {
      return true;
    }
    ir::Terminator const& end = blocks[block].terminator;
    std::vector<std::uint32_t> next;
    if ( end.kind == ir::TerminatorKind::JUMP )
    {
      next = { end.target };
    }
    else if ( end.kind == ir::TerminatorKind::BRANCH )
    {
      next = { end.target, end.otherwise };
    }
    for ( std::uint32_t const target : next )
    {
      if ( !seen[target] )
      {
        seen[target] = true;
        pending.push_back( target );
      }
    }
  }
  return false;
}

ir::Terminator returning( diag::SourceLocation at )
{
  return ir::Terminator{ .kind = ir::TerminatorKind::RETURN, .at = at, .target = 0, .otherwise = 0, .condition = {} };
}

/// The third pass: a checked file in the compiler's own form, a declaration
/// at a time.
class Lowering
{
public:
  /// `sink` takes the one finding only the blocks can decide: a function
  /// that returns something and whose end is reached.
  /// `addressed` holds where each local whose address is taken is declared.
  Lowering( diag::SourceManager const& sources,
            Names const& names,
            Unit const& unit,
            diag::DiagnosticSink& sink,
            std::set<std::uint32_t> const& addressed,
            Membership const& members )
      : mSources( &sources ), mUnit( &unit ), mSink( &sink ), mTyping( sources, names, unit, nullptr ),
        mAddressed( &addressed ), mMembers( &members )
  {
  }

  [[nodiscard]] ir::Unit translationUnit( TranslationUnit const& tree )
  {
    ir::Unit lowered = definitions( tree );
    // An object written `[[in]]` is a Section in its Pane, its stripes each
    // — see docs/decisions/0096-panes-in-c.md.
    for ( ir::Definition& definition : lowered.definitions )
    {
      auto* const object = std::get_if<ir::Global>( &definition );
      if ( object == nullptr )
      {
        continue;
      }
      auto const found = mUnit->definitions.find( object->name );
      if ( found != mUnit->definitions.end() && found->second.kind == NameKind::OBJECT )
      {
        object->pane = found->second.pane;
        object->implements = found->second.implements;
      }
    }
    return lowered;
  }

  [[nodiscard]] ir::Unit definitions( TranslationUnit const& tree )
  {
    ir::Unit lowered;
    for ( ExternalDeclaration const& declaration : tree.declarations )
    {
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration );
           function != nullptr && function->isTypedef )
      {
        lowered.definitions.emplace_back( trampoline( *function ) );
        continue;
      }
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration );
           function != nullptr && function->isTypedef )
      {
        lowered.definitions.emplace_back( trampoline( *function ) );
        continue;
      }
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration );
           function != nullptr && function->isExtern )
      {
        // The Proc is the assembler's — unless the declaration is a Slot,
        // which this program declares and the tool gives a Cell; a function
        // binds it as a `vector`, so a call is a `jsr` to the Cell's `jmp`.
        // See docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md.
        if ( hasAttribute( function->attributes, *mSources, "slot" ) )
        {
          ir::Slot slot{ .name = mTyping.spellingOf( function->name ),
                         .isVector = true,
                         .at = function->name.location };
          Definition const* const self = definitionOf( *mSources, *mUnit, *function );
          for ( std::size_t index = 0; self != nullptr && index < function->parameters.size(); ++index )
          {
            NamedType const& type = self->parameters[index].type;
            slot.parameters.push_back( ir::Local{ .name = mTyping.spellingOf( function->parameters[index].name ),
                                                  .type = type.type,
                                                  .bytes = mTyping.bytes( type.type, type.aggregate ) } );
          }
          if ( self != nullptr && self->result.has_value() )
          {
            slot.result = self->result->type;
            slot.resultBytes = mTyping.bytes( self->result->type, self->result->aggregate );
          }
          lowered.definitions.emplace_back( std::move( slot ) );
        }
        continue;
      }
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration ) )
      {
        lowered.definitions.emplace_back( functionDefinition( *function ) );
        // The Procs a `switch` in it runs, after it.
        for ( ir::Function& extra : mExtra )
        {
          lowered.definitions.emplace_back( std::move( extra ) );
        }
        mExtra.clear();
        continue;
      }
      if ( auto const* specifier = std::get_if<StructSpecifier>( &declaration ) )
      {
        std::optional<Meaning> const type = mTyping.lookupName( mTyping.spellingOf( specifier->name ) );
        ir::Aggregate aggregate{ .name = mTyping.spellingOf( specifier->name ),
                                 .members = {},
                                 .at = specifier->span.begin };
        if ( type.has_value() && type->aggregate.has_value() )
        {
          for ( MemberType const& member : mTyping.aggregate( *type->aggregate ).members )
          {
            aggregate.members.emplace_back( mTyping.spellingOf( member.name ), member.offset );
          }
        }
        lowered.definitions.emplace_back( std::move( aggregate ) );
        continue;
      }
      if ( auto const* specifier = std::get_if<EnumSpecifier>( &declaration ) )
      {
        ir::Enumeration enumeration{ .name = mTyping.spellingOf( specifier->name ),
                                     .enumerators = {},
                                     .at = specifier->span.begin,
                                     .isScoped = specifier->isScoped };
        for ( Token const& enumerator : specifier->enumerators )
        {
          enumeration.enumerators.push_back( mTyping.spellingOf( enumerator ) );
        }
        lowered.definitions.emplace_back( std::move( enumeration ) );
        continue;
      }
      auto const& object = std::get<Declaration>( declaration );
      if ( object.isExtern )
      {
        // The bytes are the assembler's, and the text names them — except a
        // Slot, which this program declares and the tool gives a Cell; the
        // Binding is the type's, a function taking a `jmp` and data an
        // address. See docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md.
        if ( hasAttribute( object.attributes, *mSources, "slot" ) )
        {
          for ( Token const& declarator : object.declarators )
          {
            lowered.definitions.emplace_back(
                ir::Slot{ .name = mTyping.spellingOf( declarator ), .isVector = false, .at = declarator.location } );
          }
        }
        continue;
      }
      Site const fileScope{ .function = nullptr, .blocks = nullptr, .at = object.span.begin };
      // An object at file scope is `absolute` unless it asked otherwise — see
      // docs/decisions/0210-placement-is-declared-in-c-too.md.
      model::PlacementClass const wanted =
          placementAttribute( object.attributes, *mSources ).value_or( model::PlacementClass::ABSOLUTE );
      for ( std::size_t index = 0; index < object.declarators.size(); ++index )
      {
        Token const& declarator = object.declarators[index];
        Expression const* const value = object.initialisers[index].get();
        ir::Type const type = typeNamed( object.type.keyword );
        DeclaratorShape const& array = object.shapes[index];
        if ( mTyping.lookupName( mTyping.spellingOf( declarator ) ).value_or( Meaning{} ).isVolatile )
        {
          mVolatileNames.insert( mTyping.spellingOf( declarator ) );
        }
        // A `struct` or a `union`, or an array of them, is bytes: each member a
        // value given, or zero.
        if ( std::optional<Meaning> const named = mTyping.lookupName( mTyping.spellingOf( declarator ) );
             named.has_value() && named->isStriped && array.isArray )
        {
          lowered.definitions.emplace_back( stripedObject( mTyping.spellingOf( declarator ),
                                                           object.isStatic,
                                                           declarator.location,
                                                           *named,
                                                           array,
                                                           array.hasList,
                                                           fileScope ) );
          continue;
        }
        if ( std::optional<Meaning> const named = mTyping.lookupName( mTyping.spellingOf( declarator ) );
             named.has_value() && named->aggregate.has_value() && !array.isPointer )
        {
          Meaning element = *named;
          element.isArray = false;
          std::uint32_t const count = array.isArray ? named->count.value_or( 0 ) : 1;
          std::uint32_t const bytes = count * mTyping.bytes( ir::Type::BLOCK, named->aggregate );
          lowered.definitions.emplace_back(
              ir::Global{ .name = mTyping.spellingOf( declarator ),
                          .type = ir::Type::U8,
                          .isStatic = object.isStatic,
                          .at = declarator.location,
                          .value = std::nullopt,
                          .count = bytes,
                          .elements = array.hasList ? blockConstants( array, element, count, fileScope )
                                                    : std::vector<ir::Constant>{},
                          .isTemporary = false,
                          .placement = wanted } );
          continue;
        }
        if ( array.isArray )
        {
          std::uint32_t const count =
              mTyping.lookupName( mTyping.spellingOf( declarator ) ).value_or( Meaning{} ).count.value_or( 0 );
          lowered.definitions.emplace_back( ir::Global{ .name = mTyping.spellingOf( declarator ),
                                                        .type = type,
                                                        .isStatic = object.isStatic,
                                                        .at = declarator.location,
                                                        .value = std::nullopt,
                                                        .count = count,
                                                        .elements = elementsOf( array, type, count, fileScope ),
                                                        .isTemporary = false,
                                                        .placement = wanted } );
          continue;
        }
        // A `const` whose value the assembler folds is a Constant; one given an
        // address holds it in bytes.
        if ( object.isConst && !object.isVolatile && value != nullptr && !object.shapes[index].isPointer )
        {
          lowered.definitions.emplace_back( ir::NamedConstant{ .name = mTyping.spellingOf( declarator ),
                                                               .value = constantOf( *value, type, fileScope ),
                                                               .isStatic = object.isStatic,
                                                               .at = declarator.location } );
          continue;
        }
        std::optional<Meaning> const settled = mTyping.lookupName( mTyping.spellingOf( declarator ) );
        if ( object.shapes[index].isPointer && object.shapes[index].isConstPointer && settled.has_value() &&
             settled->value.has_value() && value != nullptr )
        {
          lowered.definitions.emplace_back(
              ir::NamedConstant{ .name = mTyping.spellingOf( declarator ),
                                 .value = constantOf( *value, ir::Type::POINTER, fileScope ),
                                 .isStatic = object.isStatic,
                                 .at = declarator.location } );
          continue;
        }
        ir::Type const objectType = object.shapes[index].isPointer ? ir::Type::POINTER : type;
        lowered.definitions.emplace_back( ir::Global{
            .name = mTyping.spellingOf( declarator ),
            .type = objectType,
            .isStatic = object.isStatic,
            .at = declarator.location,
            .value = value != nullptr ? std::optional{ constantOf( *value, objectType, fileScope ) } : std::nullopt,
            .placement = wanted } );
      }
    }
    for ( ir::Global& literal : mLiterals )
    {
      lowered.definitions.emplace_back( std::move( literal ) );
    }
    lowered.volatiles.insert( mVolatileNames.begin(), mVolatileNames.end() );
    return lowered;
  }

private:
  /// What is being lowered: the function, its blocks, and the statement the
  /// instructions come from.
  struct Site
  {
    ir::Function* function = nullptr;
    Blocks* blocks = nullptr;
    diag::SourceLocation at;
  };

  /// Where `break` and `continue` go in the loop being lowered.
  struct Loop
  {
    std::uint32_t exit = 0;
    std::uint32_t next = 0;
  };

  static ir::Terminator jumpTo( std::uint32_t block, diag::SourceLocation at )
  {
    return ir::Terminator{
      .kind = ir::TerminatorKind::JUMP, .at = at, .target = block, .otherwise = 0, .condition = {}
    };
  }

  /// Flow that runs off the end of the open block into `block`.
  static void fall( Site const& site, std::uint32_t block )
  {
    if ( site.blocks->isOpen() )
    {
      site.blocks->terminate( jumpTo( block, site.at ) );
    }
  }

  static ir::Function function( std::string name, bool isStatic, diag::SourceLocation at )
  {
    return ir::Function{
      .name = std::move( name ), .isStatic = isStatic, .at = at, .values = {}, .blocks = {}, .then = {}
    };
  }

  /// A function type's Proc: the Temporaries a call through a pointer of it
  /// writes, and the jump that reaches the member — see
  /// docs/decisions/0065-handlers.md. Prune drops it where the program points
  /// through no pointer of the type.
  [[nodiscard]] ir::Function trampoline( FunctionDefinition const& definition )
  {
    ir::Function lowered = function( mTyping.spellingOf( definition.name ), false, definition.span.begin );
    lowered.isTrampoline = true;
    Definition const* const self = definitionOf( *mSources, *mUnit, definition );
    for ( std::size_t index = 0; self != nullptr && index < definition.parameters.size(); ++index )
    {
      NamedType const& type = self->parameters[index].type;
      lowered.parameters.push_back( ir::Local{ .name = mTyping.spellingOf( definition.parameters[index].name ),
                                               .type = type.type,
                                               .bytes = mTyping.bytes( type.type, type.aggregate ) } );
    }
    if ( self != nullptr && self->result.has_value() )
    {
      lowered.result = self->result->type;
      lowered.resultBytes = mTyping.bytes( self->result->type, self->result->aggregate );
    }
    return lowered;
  }

  [[nodiscard]] ir::Function functionDefinition( FunctionDefinition const& definition )
  {
    ir::Function lowered =
        function( mTyping.spellingOf( definition.name ), definition.isStatic, definition.span.begin );
    lowered.isInline = definition.isInline;
    lowered.inlineSpan = definition.inlineSpan;
    lowered.placement =
        placementAttribute( definition.attributes, *mSources ).value_or( model::PlacementClass::ZEROPAGE );
    mPlacement.assign( 1, lowered.placement );
    Definition const* const self = definitionOf( *mSources, *mUnit, definition );
    mOwner = lowered.name;
    mPane = self != nullptr ? self->pane : std::string{};
    mUnder = self != nullptr ? self->under : std::string{};
    lowered.implements = self != nullptr ? self->implements : std::string{};
    mShown.clear();
    lowered.pane = mPane;
    lowered.under = mUnder;
    mResult = self != nullptr ? self->result : std::nullopt;
    lowered.result = mResult.has_value() ? std::optional{ mResult->type } : std::nullopt;
    lowered.resultBytes = mResult.has_value() ? mTyping.bytes( mResult->type, mResult->aggregate ) : 0;

    Blocks blocks;
    mLoops.clear();
    mLocals.push_back( 0 );
    mConditions.push_back( 0 );
    mIndexes.push_back( 0 );
    mTested.push_back( 0 );
    // The parameters, and the statements of the body in the same scope, as
    // the check has them.
    mTyping.openScope();

    struct Kept
    {
      std::string from;
      std::string to;
      ir::Type type = ir::Type::U8;
    };

    std::vector<Kept> copied;
    // A member of a function type reads its arguments from the type's own
    // Temporaries and declares none: one convention per function, whoever
    // calls it — see docs/decisions/0065-handlers.md.
    std::string const member = self != nullptr ? typeOfFunction( self ) : std::string{};
    lowered.memberOf = member;
    mMemberOf = member;
    for ( std::size_t index = 0; self != nullptr && index < definition.parameters.size(); ++index )
    {
      Token const& name = definition.parameters[index].name;
      NamedType const& type = self->parameters[index].type;
      if ( member.empty() )
      {
        lowered.parameters.push_back( ir::Local{ .name = mTyping.spellingOf( name ),
                                                 .type = type.type,
                                                 .bytes = mTyping.bytes( type.type, type.aggregate ) } );
      }
      std::string const byte = member.empty() ? mTyping.spellingOf( name ) : member + "." + mTyping.spellingOf( name );
      std::string const owner = member.empty() ? mOwner : std::string{};
      Meaning parameter{ .kind = NameKind::OBJECT,
                         .type = type.type,
                         .enumeration = type.enumeration,
                         .byte = byte,
                         .owner = owner,
                         .definition = nullptr };
      parameter.pointee = type.pointee;
      parameter.aggregate = type.aggregate;
      parameter.isParameter = true;
      mTyping.declareLocal( name, parameter );
    }
    for ( Kept const& keep : copied )
    {
      blocks.append(
          ir::Instruction{ .operation = ir::Store{ .name = keep.to,
                                                   .type = keep.type,
                                                   .value = ir::Object{ .name = keep.from, .type = keep.type } },
                           .at = definition.span.begin } );
    }
    for ( Statement const& item : definition.body.items )
    {
      statement( item, lowered, blocks );
    }
    mTyping.closeScope();
    mConditions.pop_back();
    mIndexes.pop_back();
    mTested.pop_back();
    mLocals.pop_back();
    lowered.sections = std::exchange( mSections, {} );
    // Every Section of the function is in its Pane, so that its code runs in
    // one state of the Window — see docs/decisions/0096-panes-in-c.md.
    for ( ir::Global& object : lowered.sections )
    {
      object.pane = mPane;
    }

    // Falling off the end of a function returns from it as `return;` does,
    // under the line of the closing brace; where the last statement ended its
    // block, nothing runs off the end to need one.
    diag::SourceSpan const body = definition.body.span;
    diag::SourceLocation const closing =
        diag::SourceLocation::fromRawOffset( body.begin.rawOffset() + body.length - 1 );
    bool const fallsOff = blocks.isOpen();
    if ( fallsOff )
    {
      blocks.terminate( returning( closing ) );
    }
    lowered.blocks = blocks.take();
    lowered.withs = blocks.regions();

    // The block entered last is the one that fell off, and a function with a
    // result may not reach it: the caller would read whatever `__ret` held.
    if ( fallsOff && mResult.has_value() &&
         reaches( lowered.blocks, static_cast<std::uint32_t>( lowered.blocks.size() - 1 ) ) )
    {
      mSink->add( diagnostic( diag::DiagnosticId::C_END_REACHED )
                      .at( closing, 1 )
                      .arg( "name", lowered.name )
                      .arg( "type", mTyping.describe( typedAs( *mResult ) ) ) );
    }
    return lowered;
  }

  void statement( Statement const& statement, ir::Function& function, Blocks& blocks )
  {
    Site const site{ .function = &function, .blocks = &blocks, .at = statement.span.begin };
    switch ( statement.kind )
    {
    case StatementKind::COMPOUND:
    {
      // A block carrying only `[[placement]]` is an ordinary block whose
      // declarations lie where it says — see
      // docs/decisions/0210-placement-is-declared-in-c-too.md.
      std::optional<model::PlacementClass> const asked = placementAttribute( statement.attributes, *mSources );
      Placed const here{ mPlacement, asked };
      if ( !statement.attributes.empty() && !placementOnly( statement.attributes, *mSources ) )
      {
        withBlock( statement, site );
        return;
      }
      mTyping.openScope();
      for ( Statement const& item : statement.items )
      {
        this->statement( item, function, blocks );
      }
      mTyping.closeScope();
      return;
    }
    case StatementKind::CASE:
      mTyping.openScope();
      for ( Statement const& item : statement.items )
      {
        this->statement( item, function, blocks );
      }
      mTyping.closeScope();
      return;
    case StatementKind::DECLARATION:
      declaration( statement, site );
      return;
    case StatementKind::NULL_STATEMENT:
      return;
    case StatementKind::RETURN:
      if ( std::optional<NamedType> const result = mResult; statement.expression != nullptr && result.has_value() )
      {
        // Stored before the addresses of the calls it leaves are taken off. A
        // member leaves its result in its type's byte, whoever called it — see
        // docs/decisions/0065-handlers.md.
        std::string returned = mMemberOf + "." + std::string{ RESULT };
        if ( mMemberOf.empty() )
        {
          returned = function.name == mOwner ? std::string{ RESULT } : mOwner + "." + std::string{ RESULT };
        }
        if ( result->aggregate.has_value() )
        {
          copyInto( ir::Place{ .name = returned },
                    *statement.expression,
                    mTyping.bytes( result->type, result->aggregate ),
                    site );
        }
        else
        {
          ir::Operand value = operand( *statement.expression, result->type, site );
          blocks.append( ir::Instruction{
              .operation = ir::Store{ .name = returned, .type = result->type, .value = std::move( value ) },
              .at = site.at } );
        }
      }
      if ( std::string const phase = nameAttribute( statement.attributes, *mSources, "transition" ); !phase.empty() )
      {
        // The Phase is entered here and nothing comes back: the name goes to
        // the text unread, and the assembler resolves it — see
        // docs/decisions/0064-phase-in-c.md.
        blocks.terminate( ir::Terminator{ .kind = ir::TerminatorKind::TRANSITION,
                                          .at = site.at,
                                          .target = 0,
                                          .otherwise = 0,
                                          .condition = {},

                                          .phase = phase } );
        return;
      }
      blocks.terminate( returning( site.at ) );
      return;
    case StatementKind::EXPRESSION:
      expressionStatement( *statement.expression, site );
      return;
    case StatementKind::IF:
      // What the statement declares before its condition lives as long as the
      // statement does — see
      // docs/decisions/0160-a-declaration-where-a-statement-begins-one.md.
      mTyping.openScope();
      initialiser( statement, site );
      ifStatement( statement, site );
      mTyping.closeScope();
      return;
    case StatementKind::WHILE:
    case StatementKind::DO:
    case StatementKind::FOR:
      mTyping.openScope();
      loop( statement, site );
      mTyping.closeScope();
      return;
    case StatementKind::SWITCH:
      mTyping.openScope();
      initialiser( statement, site );
      switchInPlace( statement, site );
      mTyping.closeScope();
      return;
    case StatementKind::BREAK:
    case StatementKind::CONTINUE:
      if ( mLoops.empty() )
      {
        // A `break` of a case: back from the call the `switch` made.
        blocks.terminate( returning( site.at ) );
        return;
      }
      blocks.terminate(
          jumpTo( statement.kind == StatementKind::BREAK ? mLoops.back().exit : mLoops.back().next, site.at ) );
      return;
    }
  }

  /// A local is a byte of the Proc being lowered, named after itself — see
  /// docs/decisions/0080-a-local-is-a-byte-of-its-proc.md. A `const` given a
  /// constant is a Constant of the Proc, named as a local is, and a `static`
  /// one is a Section in a Namespace of its function's name, numbered among
  /// the function's own locals wherever it stands, since that is the scope its
  /// name is in — see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
  void declaration( Statement const& node, Site const& site )
  {
    Declaration const& declared = *node.declaration;
    // The innermost `[[placement]]` over a byte the program names is its own —
    // see docs/decisions/0210-placement-is-declared-in-c-too.md.
    Placed const here{ mPlacement, placementAttribute( declared.attributes, *mSources ) };
    // What `auto` took from the value, read again as the check read it; what
    // the check refused is not lowered at all.
    Meaning const type =
        declared.type.keyword == Keyword::AUTO
            ? mTyping.deducedType( declared.declarators.front(), *declared.initialisers.front() ).value_or( Meaning{} )
            : mTyping.declaredType( declared.type ).value_or( Meaning{} );
    for ( std::size_t index = 0; index < declared.declarators.size(); ++index )
    {
      Token const& declarator = declared.declarators[index];
      Expression const* const value = declared.initialisers[index].get();
      DeclaratorShape const& shape = declared.shapes[index];
      Meaning local = type;
      local.isConst = declared.isConst;
      local.isVolatile = declared.isVolatile;
      local.declaredAt = declarator.location;
      if ( shape.isPointer )
      {
        local.pointee = Pointee{ .type = type.type,
                                 .enumeration = type.enumeration,
                                 .isConst = declared.isConst,
                                 .aggregate = type.aggregate,
                                 .function = type.function,
                                 .isVolatile = declared.isVolatile };
        local.type = ir::Type::POINTER;
        local.enumeration = std::nullopt;
        local.aggregate = std::nullopt;
        local.function = std::nullopt;
        local.isConst = shape.isConstPointer;
        local.isVolatile = shape.isVolatilePointer;
      }
      local.isStriped = shape.isArray && !shape.isPointer && isStriped( declared.attributes, *mSources );
      mTyping.declareLocal( declarator, local );

      if ( shape.isArray )
      {
        localArray( declarator, shape, declared, local, site );
        markVolatile( declarator, local );
        continue;
      }
      if ( local.aggregate.has_value() )
      {
        localBlock( declarator, shape, value, declared, local, site );
        markVolatile( declarator, local );
        continue;
      }

      bool const constant = value != nullptr && isKnown( mTyping.expression( *value ) );
      if ( local.isConst && !local.isVolatile && constant )
      {
        local.byte = "__" + std::to_string( mLocals.back()++ ) + mTyping.spellingOf( declarator );
        local.owner = site.function->name;
        ir::Constant const given = constantOf( *value, local.type, site );
        local.value = given.value;
        site.function->constants.push_back(
            ir::NamedConstant{ .name = local.byte, .value = given, .isStatic = true, .at = declarator.location } );
        mTyping.settleLocal( declarator, local );
        continue;
      }
      // A local whose address is taken is no Temporary: a callee reading
      // through the pointer holds no Reference to it — see
      // docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#pointers--task-3d.
      // Nor is a `volatile` one, which is read and written where the source
      // says, and which no Trace shares — see docs/decisions/0151-volatile.md.
      bool const addressed = mAddressed->contains( declarator.location.rawOffset() );
      if ( declared.isStatic || addressed || local.isVolatile )
      {
        local.byte = "__" + std::to_string( mLocals.front()++ ) + mTyping.spellingOf( declarator );
        local.owner = mOwner;
        mSections.push_back( ir::Global{ .name = local.byte,
                                         .type = local.type,
                                         .isStatic = true,
                                         .at = declarator.location,
                                         .value = declared.isStatic && value != nullptr
                                                      ? std::optional{ constantOf( *value, local.type, site ) }
                                                      : std::nullopt,
                                         .written = mTyping.spellingOf( declarator ) } );
        mTyping.settleLocal( declarator, local );
        markVolatile( declarator, local );
        if ( !declared.isStatic && value != nullptr )
        {
          site.blocks->append( ir::Instruction{ .operation = ir::Store{ .name = nameOf( local, declarator, site ),
                                                                        .type = local.type,
                                                                        .value = operand( *value, local.type, site ) },
                                                .at = site.at } );
        }
        continue;
      }

      local.byte = "__" + std::to_string( mLocals.back()++ ) + mTyping.spellingOf( declarator );
      local.owner = site.function->name;
      site.function->locals.push_back(
          ir::Local{ .name = local.byte, .type = local.type, .placement = mPlacement.back() } );
      mTyping.settleLocal( declarator, local );

      if ( value != nullptr )
      {
        site.blocks->append( ir::Instruction{
            .operation =
                ir::Store{ .name = local.byte, .type = local.type, .value = operand( *value, local.type, site ) },
            .at = site.at } );
      }
    }
  }

  /// A striped array as an object of stripes: a byte of each element to the
  /// stripe of its offset, and the list it is given where `given` — see
  /// docs/decisions/0089-a-stripe-is-a-section.md.
  [[nodiscard]] ir::Global stripedObject( std::string name,
                                          bool isStatic,
                                          diag::SourceLocation at,
                                          Meaning const& array,
                                          DeclaratorShape const& shape,
                                          bool given,
                                          Site const& site )
  {
    Meaning element = array;
    element.isArray = false;
    std::uint32_t const count = array.count.value_or( 0 );
    ir::Type const elementType = array.aggregate.has_value() ? ir::Type::BLOCK : array.type;
    std::uint32_t const size = mTyping.bytes( elementType, array.aggregate );
    std::vector<std::vector<std::string>> const paths = stripePaths( elementType, array.aggregate );
    std::vector<ir::Constant> bytes;
    if ( given )
    {
      bytes = bytesOf( array.aggregate.has_value() ? blockConstants( shape, element, count, site )
                                                   : elementsOf( shape, array.type, count, site ) );
    }
    ir::Global striped{ .name = std::move( name ),
                        .type = ir::Type::U8,
                        .isStatic = isStatic,
                        .at = at,
                        .value = std::nullopt,
                        .count = count,
                        .elements = {},
                        .isTemporary = false };
    for ( std::uint32_t stripe = 0; stripe < size && stripe < paths.size(); ++stripe )
    {
      std::vector<ir::Constant> elements;
      for ( std::uint32_t index = 0; !bytes.empty() && index < count; ++index )
      {
        elements.push_back( bytes.at( ( index * size ) + stripe ) );
      }
      striped.stripes.emplace_back( paths[stripe], std::move( elements ) );
    }
    return striped;
  }

  /// The stripe each byte of an element of a striped array lies in, by the
  /// byte's offset: a member's path, under `__hi` for the high byte of one of
  /// two, and `lo` and `hi` for a `u16` or an `i16` — see
  /// docs/decisions/0089-a-stripe-is-a-section.md.
  [[nodiscard]] std::vector<std::vector<std::string>> stripePaths( ir::Type type,
                                                                   std::optional<std::uint32_t> aggregate ) const
  {
    if ( !aggregate.has_value() )
    {
      return { { "lo" }, { "hi" } };
    }
    std::vector<std::vector<std::string>> paths( mTyping.bytes( type, aggregate ) );
    std::function<void( std::uint32_t, std::vector<std::string> const&, std::uint32_t )> walk;
    walk = [&]( std::uint32_t index, std::vector<std::string> const& prefix, std::uint32_t base )
    {
      for ( MemberType const& member : mTyping.aggregate( index ).members )
      {
        std::vector<std::string> path = prefix;
        path.push_back( mTyping.spellingOf( member.name ) );
        std::uint32_t const at = base + member.offset;
        if ( member.type.aggregate.has_value() && !member.type.pointee.has_value() )
        {
          walk( *member.type.aggregate, path, at );
          continue;
        }
        if ( at < paths.size() )
        {
          paths[at] = path;
        }
        if ( ir::sizeOf( member.type.type ) == 2 && at + 1 < paths.size() )
        {
          path.insert( path.begin(), "__hi" );
          paths[at + 1] = path;
        }
      }
    };
    walk( *aggregate, {}, 0 );
    return paths;
  }

  /// Constants of one or two bytes as bytes: a name as its low and high byte.
  static std::vector<ir::Constant> bytesOf( std::vector<ir::Constant> const& constants )
  {
    std::vector<ir::Constant> bytes;
    for ( ir::Constant const& constant : constants )
    {
      if ( ir::sizeOf( constant.type ) == 1 )
      {
        bytes.push_back( ir::Constant{ .type = ir::Type::U8, .value = constant.value, .name = constant.name } );
        continue;
      }
      auto const bits = static_cast<std::uint64_t>( constant.value );
      bytes.push_back( ir::Constant{ .type = ir::Type::U8,
                                     .value = static_cast<std::int64_t>( bits & 0xFFU ),
                                     .name = constant.name.empty() ? std::string{} : "<(" + constant.name + ")" } );
      bytes.push_back( ir::Constant{ .type = ir::Type::U8,
                                     .value = static_cast<std::int64_t>( ( bits >> 8U ) & 0xFFU ),
                                     .name = constant.name.empty() ? std::string{} : ">(" + constant.name + ")" } );
    }
    return bytes;
  }

  /// The list an array is given, as many constants as it has elements, those
  /// the list leaves out zero as C has them; nothing for an array given none.
  [[nodiscard]] std::vector<ir::Constant>
  elementsOf( DeclaratorShape const& array, ir::Type type, std::uint32_t count, Site const& site )
  {
    std::vector<ir::Constant> elements;
    if ( !array.hasList )
    {
      return elements;
    }
    elements.reserve( count );
    for ( ExpressionPtr const& element : array.list )
    {
      elements.push_back( constantOf( *element, type, site ) );
    }
    elements.resize( count, ir::Constant{ .type = type, .value = 0, .name = {} } );
    return elements;
  }

  /// The name of an element at a constant index: the array's, and the
  /// element's offset in bytes.
  static std::string elementName( std::string const& array, std::int64_t index, ir::Type type )
  {
    std::int64_t const offset = index * ir::sizeOf( type );
    return offset == 0 ? array : array + "+" + std::to_string( offset );
  }

  /// A local array: a Section in the Namespace of its function, numbered among
  /// the function's own locals as a `static` one is. A `static` array, and a
  /// `const` one given constants, hold their bytes between calls; any other is
  /// a Temporary its declaration writes the list into, each time it runs — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#arrays--task-3c.
  void localArray( Token const& declarator,
                   DeclaratorShape const& array,
                   Declaration const& declared,
                   Meaning local,
                   Site const& site )
  {
    auto count = static_cast<std::uint32_t>( array.list.size() );
    if ( array.size != nullptr )
    {
      count = static_cast<std::uint32_t>( mTyping.fold( *array.size, std::nullopt ) );
    }
    local.isArray = true;
    local.count = count;
    local.byte = "__" + std::to_string( mLocals.front()++ ) + mTyping.spellingOf( declarator );
    local.owner = mOwner;
    mTyping.settleLocal( declarator, local );

    if ( local.isStriped )
    {
      localStriped( declarator, local, array, declared, site );
      return;
    }
    if ( local.aggregate.has_value() )
    {
      Meaning element = local;
      element.isArray = false;
      bool const kept =
          declared.isStatic || ( declared.isConst && array.hasList && constantList( array, element, count ) );
      bool const addressedBlock = mAddressed->contains( declarator.location.rawOffset() );
      mSections.push_back( ir::Global{ .name = local.byte,
                                       .type = ir::Type::U8,
                                       .isStatic = true,
                                       .at = declarator.location,
                                       .value = std::nullopt,
                                       .count = count * mTyping.bytes( ir::Type::BLOCK, local.aggregate ),
                                       .elements = kept && array.hasList ? blockConstants( array, element, count, site )
                                                                         : std::vector<ir::Constant>{},
                                       .isTemporary = !kept && !addressedBlock } );
      if ( !kept && array.hasList )
      {
        storeList( nameOf( local, declarator, site ), array, element, count, site );
      }
      return;
    }

    bool const constants = std::ranges::all_of( array.list,
                                                [this]( ExpressionPtr const& element )
                                                {
                                                  Typed const kind = mTyping.expression( *element );
                                                  return isKnown( kind ) || kind.address;
                                                } );
    bool const kept = declared.isStatic || ( declared.isConst && constants );
    bool const addressed = mAddressed->contains( declarator.location.rawOffset() );
    mSections.push_back(
        ir::Global{ .name = local.byte,
                    .type = local.type,
                    .isStatic = true,
                    .at = declarator.location,
                    .value = std::nullopt,
                    .count = count,
                    .elements = kept ? elementsOf( array, local.type, count, site ) : std::vector<ir::Constant>{},
                    .isTemporary = !kept && !addressed } );
    if ( kept || !array.hasList )
    {
      return;
    }
    std::string const name = nameOf( local, declarator, site );
    for ( std::uint32_t element = 0; element < count; ++element )
    {
      ir::Operand value = ir::Constant{ .type = local.type, .value = 0, .name = {} };
      if ( element < array.list.size() )
      {
        value = converted( operand( *array.list[element], local.type, site ), local.type, site );
      }
      site.blocks->append( ir::Instruction{ .operation = ir::Store{ .name = elementName( name, element, local.type ),
                                                                    .type = local.type,
                                                                    .value = std::move( value ) },
                                            .at = site.at } );
    }
  }

  /// A local striped array: its stripes in Namespaces of its name in the
  /// function's Proc, kept as a `static` array or a `const` one given
  /// constants is, and otherwise each a Temporary its declaration writes the
  /// list into — see docs/decisions/0089-a-stripe-is-a-section.md and
  /// docs/decisions/0090-a-namespace-may-stand-in-a-proc.md.
  void localStriped( Token const& declarator,
                     Meaning const& local,
                     DeclaratorShape const& array,
                     Declaration const& declared,
                     Site const& site )
  {
    Meaning element = local;
    element.isArray = false;
    bool const constants = local.aggregate.has_value()
                               ? constantList( array, element, local.count.value_or( 0 ) )
                               : std::ranges::all_of( array.list,
                                                      [this]( ExpressionPtr const& listed )
                                                      { return isKnown( mTyping.expression( *listed ) ); } );
    bool const kept = declared.isStatic || ( declared.isConst && array.hasList && constants );
    ir::Global striped =
        stripedObject( local.byte, true, declarator.location, local, array, kept && array.hasList, site );
    striped.isTemporary = !kept;
    mSections.push_back( std::move( striped ) );
    if ( kept || !array.hasList )
    {
      return;
    }

    ir::Type const elementType = local.aggregate.has_value() ? ir::Type::BLOCK : local.type;
    std::uint32_t const bytes = mTyping.bytes( elementType, local.aggregate );
    Reach const base{ .kind = Reach::Kind::STRIPED,
                      .type = ir::Type::U8,
                      .name = {},
                      .pointer = {},
                      .index = {},
                      .scaled = true,
                      .isArray = false,
                      .array = local.owner.empty() || local.owner == site.function->name
                                   ? local.byte
                                   : local.owner + "." + local.byte,
                      .paths = stripePaths( elementType, local.aggregate ),
                      .offset = 0 };
    Visit const store = [&]( Expression const* value, Meaning const& slot, std::uint32_t offset )
    {
      Reach place = base;
      place.index = ir::Constant{ .type = ir::Type::U8, .value = offset / bytes, .name = {} };
      place.offset = offset % bytes;
      if ( slot.aggregate.has_value() )
      {
        transfer( place, reach( *value, site ), mTyping.bytes( ir::Type::BLOCK, slot.aggregate ), site );
        return;
      }
      place.type = slot.pointee.has_value() ? ir::Type::POINTER : slot.type;
      ir::Operand operand = ir::Constant{ .type = place.type, .value = 0, .name = {} };
      if ( value != nullptr )
      {
        operand = converted( this->operand( *value, place.type, site ), place.type, site );
      }
      write( place, std::move( operand ), site );
    };
    std::uint32_t const count = local.count.value_or( 0 );
    for ( std::uint32_t index = 0; index < count; ++index )
    {
      visitValue( index < array.list.size() ? array.list[index].get() : nullptr, element, index * bytes, store );
    }
  }

  /// Whether a list gives a `struct` or a `union`, or each element of an
  /// array of them, only what a `static` one may be given: constants, known
  /// addresses and `nullptr`, and no whole value, which is copied.
  bool constantList( DeclaratorShape const& shape, Meaning const& element, std::uint32_t count )
  {
    bool constant = true;
    Visit const check = [&]( Expression const* value, Meaning const& slot, std::uint32_t /*offset*/ )
    {
      if ( value == nullptr )
      {
        return;
      }
      Typed const kind = mTyping.expression( *value );
      constant = constant && !slot.aggregate.has_value() && ( isKnown( kind ) || kind.address || kind.isNull );
    };
    if ( !shape.isArray )
    {
      visitAggregate( shape.list, element.aggregate.value_or( 0 ), 0, check );
      return constant;
    }
    std::uint32_t const bytes = mTyping.bytes( ir::Type::BLOCK, element.aggregate );
    for ( std::uint32_t index = 0; index < count && index < shape.list.size(); ++index )
    {
      visitValue( shape.list[index].get(), element, index * bytes, check );
    }
    return constant;
  }

  /// What a list sets, at the offset it sets it, from `base`: every scalar a
  /// value is given for, and every one it leaves out, which is zero; a value
  /// of a `struct` or a `union` itself, which is copied.
  using Visit = std::function<void( Expression const* value, Meaning const& slot, std::uint32_t offset )>;

  void visitValue( Expression const* value, Meaning const& slot, std::uint32_t offset, Visit const& visit )
  {
    if ( !slot.aggregate.has_value() || ( value != nullptr && value->kind != ExpressionKind::LIST ) )
    {
      visit( value, slot, offset );
      return;
    }
    static std::vector<ExpressionPtr> const NO_ELEMENTS;
    visitAggregate( value != nullptr ? value->arguments : NO_ELEMENTS, slot.aggregate.value_or( 0 ), offset, visit );
  }

  void visitAggregate( std::vector<ExpressionPtr> const& elements,
                       std::uint32_t aggregateIndex,
                       std::uint32_t base,
                       Visit const& visit )
  {
    AggregateType const& type = mTyping.aggregate( aggregateIndex );
    std::size_t const members = type.isUnion ? std::min<std::size_t>( 1, type.members.size() ) : type.members.size();
    for ( std::size_t index = 0; index < members; ++index )
    {
      MemberType const& member = type.members[index];
      Meaning slot{ .kind = NameKind::OBJECT,
                    .type = member.type.type,
                    .enumeration = member.type.enumeration,
                    .byte = {},
                    .owner = {} };
      slot.pointee = member.type.pointee;
      slot.aggregate = member.type.aggregate;
      Expression const* const value = index < elements.size() ? elements[index].get() : nullptr;
      std::uint32_t const at = base + member.offset;
      if ( !member.isArray )
      {
        visitValue( value, slot, at, visit );
        continue;
      }
      std::uint32_t const bytes = mTyping.bytes( slot.type, slot.aggregate );
      for ( std::uint32_t element = 0; element < member.count; ++element )
      {
        Expression const* const given =
            value != nullptr && element < value->arguments.size() ? value->arguments[element].get() : nullptr;
        visitValue( given, slot, at + ( element * bytes ), visit );
      }
    }
  }

  /// The bytes a `struct`, a `union` or an array of them is given, as
  /// constants of the widths of their members, zero where nothing is given.
  [[nodiscard]] std::vector<ir::Constant>
  blockConstants( DeclaratorShape const& shape, Meaning const& element, std::uint32_t count, Site const& site )
  {
    std::map<std::uint32_t, ir::Constant> given;
    Visit const collect = [&]( Expression const* value, Meaning const& slot, std::uint32_t offset )
    {
      ir::Type const type = slot.pointee.has_value() ? ir::Type::POINTER : slot.type;
      given.insert_or_assign( offset,
                              value != nullptr ? constantOf( *value, type, site )
                                               : ir::Constant{ .type = type, .value = 0, .name = {} } );
    };
    std::uint32_t const bytes = mTyping.bytes( ir::Type::BLOCK, element.aggregate );
    for ( std::uint32_t index = 0; index < count; ++index )
    {
      Expression const* value = nullptr;
      if ( shape.isArray && index < shape.list.size() )
      {
        value = shape.list[index].get();
      }
      if ( shape.isArray )
      {
        visitValue( value, element, index * bytes, collect );
      }
      else
      {
        visitAggregate( shape.list, element.aggregate.value_or( 0 ), 0, collect );
      }
    }
    std::vector<ir::Constant> elements;
    for ( std::uint32_t offset = 0; offset < count * bytes; )
    {
      if ( auto const found = given.find( offset ); found != given.end() )
      {
        elements.push_back( found->second );
        offset += ir::sizeOf( found->second.type );
        continue;
      }
      elements.push_back( ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} } );
      ++offset;
    }
    return elements;
  }

  /// A list written into bytes where a local is declared, each time it runs:
  /// a store per scalar, zero where nothing is given, and a copy per value of
  /// a `struct` or a `union`.
  void storeList( std::string const& name,
                  DeclaratorShape const& shape,
                  Meaning const& element,
                  std::uint32_t count,
                  Site const& site )
  {
    Visit const store = [&]( Expression const* value, Meaning const& slot, std::uint32_t offset )
    {
      ir::Constant const base{ .type = ir::Type::POINTER, .value = 0, .name = name };
      if ( slot.aggregate.has_value() )
      {
        copyInto( ir::Place{ .name = offsetName( base, offset ) },
                  *value,
                  mTyping.bytes( ir::Type::BLOCK, slot.aggregate ),
                  site );
        return;
      }
      ir::Type const type = slot.pointee.has_value() ? ir::Type::POINTER : slot.type;
      ir::Operand operand = ir::Constant{ .type = type, .value = 0, .name = {} };
      if ( value != nullptr )
      {
        operand = converted( this->operand( *value, type, site ), type, site );
      }
      site.blocks->append( ir::Instruction{
          .operation = ir::Store{ .name = offsetName( base, offset ), .type = type, .value = std::move( operand ) },
          .at = site.at } );
    };
    std::uint32_t const bytes = mTyping.bytes( ir::Type::BLOCK, element.aggregate );
    if ( !shape.isArray )
    {
      visitAggregate( shape.list, element.aggregate.value_or( 0 ), 0, store );
      return;
    }
    for ( std::uint32_t index = 0; index < count; ++index )
    {
      visitValue( index < shape.list.size() ? shape.list[index].get() : nullptr, element, index * bytes, store );
    }
  }

  /// A local `struct` or `union`: a `.ztemp` of four bytes or fewer and a
  /// `.temp` past that, or a Section of its own where it is `static` or its
  /// address is taken — see docs/decisions/0086-a-struct-by-value.md. A list
  /// is written into it, and a value copied, where it is declared.
  void localBlock( Token const& declarator,
                   DeclaratorShape const& shape,
                   Expression const* value,
                   Declaration const& declared,
                   Meaning local,
                   Site const& site )
  {
    std::uint32_t const bytes = mTyping.bytes( ir::Type::BLOCK, local.aggregate );
    bool const addressed = mAddressed->contains( declarator.location.rawOffset() );
    // A `const` one given constants is kept as a `static` one is: nothing
    // can tell them apart, a local's bytes being its Proc's in every call.
    bool const fixed = declared.isStatic || ( declared.isConst && shape.hasList && constantList( shape, local, 1 ) );
    bool const kept = fixed || addressed;
    if ( kept || bytes > SMALL_BLOCK )
    {
      local.byte = "__" + std::to_string( mLocals.front()++ ) + mTyping.spellingOf( declarator );
      local.owner = mOwner;
      mSections.push_back( ir::Global{ .name = local.byte,
                                       .type = ir::Type::U8,
                                       .isStatic = true,
                                       .at = declarator.location,
                                       .value = std::nullopt,
                                       .count = bytes,
                                       .elements = fixed && shape.hasList ? blockConstants( shape, local, 1, site )
                                                                          : std::vector<ir::Constant>{},
                                       .isTemporary = !kept,
                                       .written = mTyping.spellingOf( declarator ) } );
    }
    else
    {
      local.byte = "__" + std::to_string( mLocals.back()++ ) + mTyping.spellingOf( declarator );
      local.owner = site.function->name;
      site.function->locals.push_back(
          ir::Local{ .name = local.byte, .type = ir::Type::BLOCK, .bytes = bytes, .placement = mPlacement.back() } );
    }
    mTyping.settleLocal( declarator, local );
    if ( fixed )
    {
      return;
    }
    std::string const name = nameOf( local, declarator, site );
    if ( shape.hasList )
    {
      storeList( name, shape, local, 1, site );
    }
    else if ( value != nullptr )
    {
      copyInto( ir::Place{ .name = name }, *value, bytes, site );
    }
  }

  /// The name an object is written by from where the lowering stands: a local
  /// of another Proc is named through it, `f.__0i`.
  /// A translated character as the assembler writes it: its own token, or
  /// how a string literal holding it spells it.
  [[nodiscard]] std::string spellingOfCharacter( Expression const& node ) const
  {
    return node.spelling.empty() ? mTyping.spellingOf( node.token ) : node.spelling;
  }

  /// The Section a string literal's bytes and its end lie in, at file scope:
  /// one for equal literals of a file — see
  /// docs/decisions/0095-literals-and-the-runtime.md.
  std::string literalSection( Expression const& node )
  {
    std::string key;
    for ( ExpressionPtr const& character : node.arguments )
    {
      key += character->spelling;
    }
    auto const [found, added] = mLiteralNames.try_emplace( key, "__s" + std::to_string( mLiteralNames.size() ) );
    if ( !added )
    {
      return found->second;
    }
    ir::Global literal{ .name = found->second,
                        .type = ir::Type::U8,
                        .isStatic = true,
                        .at = node.token.location,
                        .value = std::nullopt,
                        .count = static_cast<std::uint32_t>( node.arguments.size() ),
                        .elements = {},
                        .isTemporary = false };
    for ( ExpressionPtr const& character : node.arguments )
    {
      literal.elements.push_back(
          mTyping.isTranslated( *character )
              ? ir::Constant{ .type = ir::Type::U8, .value = 0, .name = spellingOfCharacter( *character ) }
              : ir::Constant{ .type = ir::Type::U8, .value = character->value, .name = {} } );
    }
    mLiterals.push_back( std::move( literal ) );
    return found->second;
  }

  /// A local written `volatile`, by the name its lowered bytes have.
  void markVolatile( Token const& declarator, Meaning const& local )
  {
    if ( !local.isVolatile )
    {
      return;
    }
    std::string const spelling = mTyping.spellingOf( declarator );
    Meaning const settled = mTyping.lookupSpelling( spelling ).value_or( Meaning{} );
    mVolatileNames.insert( settled.byte.empty() ? spelling : settled.byte );
  }

  [[nodiscard]] std::string nameOf( Meaning const& meaning, Token const& token, Site const& site ) const
  {
    if ( meaning.byte.empty() )
    {
      return mTyping.spellingOf( token );
    }
    if ( meaning.owner.empty() || meaning.owner == site.function->name )
    {
      // A byte of no owner is named whole: a member's arguments are the
      // function type's — see docs/decisions/0065-handlers.md.
      return meaning.byte;
    }
    return meaning.owner + "." + meaning.byte;
  }

  /// A `[[with]]` block: its entry, then its statements as a region of
  /// blocks of their own, which the text writes as a macro used once under
  /// `.with` — see docs/decisions/0096-panes-in-c.md. The check held the
  /// attribute to one of the four forms and the index to a `u8` or a
  /// constant; a constant index of a family names the member, `FAMILY + n`.
  void withBlock( Statement const& node, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    std::optional<Attribute> const attribute = withOf( node.attributes, *mSources );
    std::optional<WithSpec> const spec = attribute.has_value() ? withAttribute( *attribute ) : std::nullopt;
    if ( !spec.has_value() )
    {
      return;
    }
    std::string const name = mTyping.spellingOf( spec->name );
    std::optional<Meaning> const meaning = mTyping.lookupName( name );
    ExternalName const* const external =
        meaning.has_value() && meaning->definition == nullptr ? meaning->external : nullptr;
    if ( external == nullptr )
    {
      return;
    }
    bool const isWindow = external->kind == ExternalKind::WINDOW;
    std::string const label = "__w" + std::to_string( mWiths++ );
    if ( !isTrampoline( node.attributes, *mSources ) )
    {
      region( node, *spec, name, isWindow ? name : external->window, label, site );
      return;
    }

    // A Trampoline: a Proc of its own in `fixed`, running `under` the
    // function's Pane, holding the `.with` over the block's body and called
    // by `jsr` — see docs/decisions/0097-a-trampoline-is-declared.md and
    // docs/decisions/0098-a-proc-declares-what-is-shown.md. The block's
    // locals are the Proc's; what it names of the function's is qualified.
    ir::Function proc = function( label, true, site.at );
    proc.under = mPane;
    Blocks inner;
    std::vector<Loop> const outer = std::exchange( mLoops, {} );
    mLocals.push_back( 0 );
    mConditions.push_back( 0 );
    mIndexes.push_back( 0 );
    mTested.push_back( 0 );
    Site const in{ .function = &proc, .blocks = &inner, .at = site.at };
    region( node, *spec, name, isWindow ? name : external->window, label + "b", in );
    inner.terminate( returning( site.at ) );
    proc.blocks = inner.take();
    proc.withs = inner.regions();
    mConditions.pop_back();
    mIndexes.pop_back();
    mTested.pop_back();
    mLocals.pop_back();
    mLoops = outer;
    mExtra.push_back( std::move( proc ) );
    blocks.append( ir::Instruction{ .operation = ir::Call{ .name = label, .arguments = {}, .result = std::nullopt },
                                    .at = site.at } );
  }

  /// The entry of a block and its statements as a region of `site`'s blocks:
  /// the macro `body` used once under `.with`, showing `window`.
  void region( Statement const& node,
               WithSpec const& spec,
               std::string const& name,
               std::string window,
               std::string body,
               Site const& site )
  {
    Blocks& blocks = *site.blocks;
    bool const isWindow = window == name;
    ir::EnterWith enter{ .form = spec.form, .name = name, .body = std::move( body ) };
    Shown shown{ .window = std::move( window ) };
    switch ( spec.form )
    {
    case ir::WithForm::PANE:
      shown.pane = name;
      break;
    case ir::WithForm::AT:
    {
      if ( !spec.index.has_value() )
      {
        return;
      }
      Token const& index = *spec.index;
      if ( !isWindow )
      {
        shown.pane = name;
      }
      if ( index.kind == TokenKind::INTEGER_CONSTANT )
      {
        std::int64_t const value = valueOfIntegerConstant( mTyping.spellingOf( index ) ).value_or( 0 );
        if ( isWindow )
        {
          enter.index = ir::Constant{ .type = ir::Type::U8, .value = value, .name = {} };
        }
        else
        {
          enter.form = ir::WithForm::PANE;
          enter.name = name + " + " + std::to_string( value );
        }
        break;
      }
      Meaning const held = mTyping.lookupSpelling( mTyping.spellingOf( index ) ).value_or( Meaning{} );
      enter.index = ir::Object{ .name = nameOf( held, index, site ), .type = ir::Type::U8 };
      enter.member = !isWindow;
      break;
    }
    case ir::WithForm::STATE:
      if ( !spec.state.has_value() )
      {
        return;
      }
      enter.state = mTyping.spellingOf( *spec.state );
      shown.state = enter.state;
      break;
    }
    blocks.append( ir::Instruction{ .operation = std::move( enter ), .at = site.at } );
    std::uint32_t const first = blocks.create();
    fall( site, first );
    blocks.enter( first );
    mShown.push_back( shown );
    mTyping.openScope();
    for ( Statement const& item : node.items )
    {
      statement( item, *site.function, blocks );
    }
    mTyping.closeScope();
    mShown.pop_back();
    std::uint32_t const after = blocks.create();
    fall( site, after );
    blocks.enter( after );
    blocks.region( first, after );
  }

  /// The Pane a call into `pane` is wrapped in `.with` for: none where the
  /// callee is in the caller's own Pane, in what a block around the call
  /// shows, or in a Pane every Phase of the Module shows as its Window's base
  /// — see docs/decisions/0096-panes-in-c.md.
  [[nodiscard]] std::string withFor( std::string const& pane ) const
  {
    if ( pane.empty() || pane == mPane || pane == mUnder )
    {
      return {};
    }
    std::optional<Meaning> const meaning = mTyping.lookupName( pane );
    ExternalName const* const external =
        meaning.has_value() && meaning->definition == nullptr ? meaning->external : nullptr;
    if ( external == nullptr || external->kind != ExternalKind::PANE )
    {
      return {};
    }
    if ( std::ranges::any_of( mShown, [&]( Shown const& block ) { return shows( block, *external ); } ) )
    {
      return {};
    }
    std::vector<std::string> const& shownByBase = mUnit->source->shownByBase;
    if ( std::ranges::find( shownByBase, pane ) != shownByBase.end() )
    {
      return {};
    }
    return pane;
  }

  void ifStatement( Statement const& node, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    bool const hasElse = node.items.size() > 1;
    std::uint32_t const then = blocks.create();
    std::uint32_t const otherwise = hasElse ? blocks.create() : 0;
    std::uint32_t const join = blocks.create();

    condition( *node.expression, then, hasElse ? otherwise : join, site );
    blocks.enter( then );
    statement( node.items.front(), *site.function, blocks );
    fall( site, join );
    if ( hasElse )
    {
      blocks.enter( otherwise );
      statement( node.items.back(), *site.function, blocks );
      fall( site, join );
    }
    blocks.enter( join );
  }

  /// What a statement was written with before its condition: a declaration
  /// whose stores stand where it was written, or an expression statement.
  void initialiser( Statement const& node, Site const& site )
  {
    if ( node.initial != nullptr )
    {
      expressionStatement( *node.initial, site );
    }
    if ( node.declaration != nullptr )
    {
      declaration( node, site );
    }
  }

  /// A loop's blocks in the order they are written: the test at the head for
  /// `while` and `for`, and at the foot for `do`.
  void loop( Statement const& node, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    initialiser( node, site );

    if ( node.kind == StatementKind::DO )
    {
      std::uint32_t const body = blocks.create();
      std::uint32_t const test = blocks.create();
      std::uint32_t const exit = blocks.create();
      fall( site, body );
      blocks.enter( body );
      governed( node, Loop{ .exit = exit, .next = test }, site );
      fall( site, test );
      blocks.enter( test );
      condition( *node.expression, body, exit, site );
      blocks.enter( exit );
      return;
    }

    std::uint32_t const head = blocks.create();
    std::uint32_t const body = blocks.create();
    std::uint32_t const step = node.step != nullptr ? blocks.create() : head;
    std::uint32_t const exit = blocks.create();
    fall( site, head );
    blocks.enter( head );
    if ( node.expression != nullptr )
    {
      condition( *node.expression, body, exit, site );
    }
    else
    {
      blocks.terminate( jumpTo( body, site.at ) );
    }
    blocks.enter( body );
    governed( node, Loop{ .exit = exit, .next = step }, site );
    if ( node.step != nullptr )
    {
      fall( site, step );
      blocks.enter( step );
      expressionStatement( *node.step, site );
    }
    fall( site, head );
    blocks.enter( exit );
  }

  /// The statement a loop governs, with `break` and `continue` going where
  /// `loop` says.
  void governed( Statement const& node, Loop where, Site const& site )
  {
    mLoops.push_back( where );
    statement( node.items.front(), *site.function, *site.blocks );
    mLoops.pop_back();
  }

  /// What each label of a `switch` stands for: the value, and the body it
  /// runs by its place among the bodies.
  struct Labelled
  {
    std::int64_t value = 0;
    std::size_t body = 0;
  };

  /// A `switch`, written where it stands: the bodies are blocks of the
  /// function's own Proc, `break` a jump past them all and a body that does
  /// not break falling into the next. Few labels are a chain of compares and
  /// branches ([0164](docs/decisions/0164-a-small-switch-stands-where-it-is.md));
  /// more are a `.dispatch` over a table spanning them, which goes to
  /// positions of this Proc and so costs the Proc nothing — see
  /// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
  void switchInPlace( Statement const& node, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    Typed const kind = mTyping.expression( *node.expression );
    ir::Type const type = kind.type.value_or( ir::Type::U8 );

    // The value is read once: where it is an object the compares read it
    // there, and anything else — a call, an operator, a `volatile` byte —
    // goes into a byte of the Proc first.
    ir::Operand control = operand( *node.expression, type, site );
    auto const* const object = std::get_if<ir::Object>( &control );
    if ( object == nullptr || mVolatileNames.contains( baseOf( object->name ) ) )
    {
      std::string const name = "__v" + std::to_string( mTested.back()++ );
      site.function->locals.push_back(
          ir::Local{ .name = name, .type = type, .bytes = 0, .placement = mPlacement.front() } );
      site.blocks->append( ir::Instruction{
          .operation = ir::Store{ .name = name, .type = type, .value = std::move( control ) }, .at = site.at } );
      control = ir::Object{ .name = name, .type = type };
    }

    // Which body each label runs, and which the labels with no body of their
    // own run: the one written next, as a case with no `break` falls into it.
    std::vector<Labelled> labels;
    std::vector<std::size_t> bodies;
    std::optional<std::size_t> defaultBody;
    std::vector<std::size_t> pending;
    bool pendingDefault = false;
    for ( std::size_t index = 0; index < node.items.size(); ++index )
    {
      Statement const& clause = node.items[index];
      if ( clause.expression == nullptr )
      {
        pendingDefault = true;
      }
      else
      {
        pending.push_back( labels.size() );
        labels.push_back( Labelled{ .value = labelValue( *clause.expression, type ), .body = 0 } );
      }
      if ( clause.items.empty() )
      {
        continue;
      }
      for ( std::size_t const at : pending )
      {
        labels[at].body = bodies.size();
      }
      if ( pendingDefault )
      {
        defaultBody = bodies.size();
      }
      pending.clear();
      pendingDefault = false;
      bodies.push_back( index );
    }
    // Labels after the last body run nothing, and so does `default` there.
    for ( std::size_t const at : pending )
    {
      labels[at].body = bodies.size();
    }

    SwitchForm const form = formOf( mTyping.labelValues( node, type ) );

    std::vector<std::uint32_t> blockOf;
    blockOf.reserve( bodies.size() + 1 );
    for ( std::size_t body = 0; body < bodies.size(); ++body )
    {
      blockOf.push_back( blocks.create() );
    }
    std::uint32_t const join = blocks.create();
    blockOf.push_back( join );
    auto const bodyBlock = [&blockOf]( std::size_t body ) { return blockOf[body]; };

    // The table: one entry per value the labels span, holding the body that
    // value runs — a label's, or `default`'s where no label names it, or the
    // way past the whole of it. The clamp puts everything outside the span
    // into one entry more at the end, which holds the same; a byte spanning
    // every value it has is inside whatever it holds, and is clamped to
    // nothing — see docs/decisions/0166-a-tables-span-is-its-labels.md.
    if ( !form.inPlace )
    {
      bool const clamps = ir::sizeOf( type ) == 2 || form.span < MAX_TABLE;
      auto const entries = static_cast<std::size_t>( form.span ) + ( clamps ? 1 : 0 );
      std::vector<std::uint32_t> targets( entries, bodyBlock( defaultBody.value_or( bodies.size() ) ) );
      for ( Labelled const& one : labels )
      {
        targets[static_cast<std::size_t>( one.value - form.first )] = bodyBlock( one.body );
      }
      site.blocks->append( ir::Instruction{ .operation = ir::Switch{ .value = control,
                                                                     .count = static_cast<std::uint32_t>( form.span ),
                                                                     .first = form.first,
                                                                     .type = type },
                                            .at = site.at } );
      blocks.terminate( ir::Terminator{ .kind = ir::TerminatorKind::DISPATCH,
                                        .at = site.at,
                                        .target = 0,
                                        .otherwise = 0,
                                        .condition = {},
                                        .phase = {},
                                        .targets = std::move( targets ) } );
    }

    // The chain: each label a compare and a branch, the last falling through
    // to `default`'s body or past the whole of it.
    for ( std::size_t at = 0; form.inPlace && at < labels.size(); ++at )
    {
      Statement const& clause = node.items[at];
      std::uint32_t const otherwise =
          at + 1 < labels.size() ? blocks.create() : bodyBlock( defaultBody.value_or( bodies.size() ) );
      ir::Value const decided = define( site, ir::Type::BOOL );
      site.blocks->append( ir::Instruction{
          .operation = ir::Compare{ .result = decided,
                                    .op = ir::Comparison::EQUAL,
                                    .type = type,
                                    .left = control,
                                    .right = ir::Constant{ .type = type, .value = labels[at].value, .name = {} } },
          .at = clause.span.begin } );
      blocks.terminate( ir::Terminator{
          .kind = ir::TerminatorKind::BRANCH,
          .at = clause.span.begin,
          .target = bodyBlock( labels[at].body ),
          .otherwise = otherwise,
          .condition = decided,
      } );
      if ( at + 1 < labels.size() )
      {
        blocks.enter( otherwise );
      }
    }
    if ( form.inPlace && labels.empty() )
    {
      fall( site, bodyBlock( defaultBody.value_or( bodies.size() ) ) );
    }

    // The bodies, in the order written: `break` leaves them all, and what
    // runs off the end of one runs into the next.
    // `break` leaves the `switch`; `continue` goes where the loop around it
    // sends one, there being no Proc between a case and that loop.
    mLoops.push_back( Loop{ .exit = join, .next = mLoops.empty() ? join : mLoops.back().next } );
    for ( std::size_t body = 0; body < bodies.size(); ++body )
    {
      blocks.enter( bodyBlock( body ) );
      statement( node.items[bodies[body]], *site.function, blocks );
      fall( site, bodyBlock( body + 1 ) );
    }
    mLoops.pop_back();
    blocks.enter( join );
  }

  /// What a label of a `switch` stands for: an enumerator's place in its
  /// type, or the constant it is written as.
  [[nodiscard]] std::int64_t labelValue( Expression const& label, ir::Type type )
  {
    if ( std::optional<std::uint32_t> const enumerator = mTyping.enumeratorIndex( label ) )
    {
      return *enumerator;
    }
    return wrapped( mTyping.fold( label, type ), type );
  }

  /// A condition as branches: to `whenTrue` where it holds and `whenFalse`
  /// where it does not, `&&` and `||` taking their right side only where the
  /// left has not decided, as C has them.
  void condition( Expression const& node, std::uint32_t whenTrue, std::uint32_t whenFalse, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    if ( node.kind == ExpressionKind::BINARY && isLogical( node.token.kind ) )
    {
      std::uint32_t const middle = blocks.create();
      if ( node.token.kind == TokenKind::AMPERSAND_AMPERSAND )
      {
        condition( *node.left, middle, whenFalse, site );
      }
      else
      {
        condition( *node.left, whenTrue, middle, site );
      }
      blocks.enter( middle );
      condition( *node.right, whenTrue, whenFalse, site );
      return;
    }
    if ( node.kind == ExpressionKind::UNARY && node.token.kind == TokenKind::BANG )
    {
      condition( *node.left, whenFalse, whenTrue, site );
      return;
    }

    Typed const kind = mTyping.expression( node );
    if ( kind.known || kind.decided.has_value() )
    {
      bool const holds = kind.known ? mTyping.fold( node, ir::Type::BOOL ) != 0 : kind.decided.value_or( false );
      blocks.terminate( jumpTo( holds ? whenTrue : whenFalse, site.at ) );
      return;
    }
    ir::Operand value = operand( node, ir::Type::BOOL, site );
    blocks.terminate( ir::Terminator{
        .kind = ir::TerminatorKind::BRANCH,
        .at = site.at,
        .target = whenTrue,
        .otherwise = whenFalse,
        .condition = std::move( value ),
    } );
  }

  /// A call through a pointer of a function type: a call of the type's own
  /// Proc, whose Temporaries hold the arguments and whose `ptr` holds where
  /// to go — see docs/decisions/0065-handlers.md. The pointer is written last,
  /// as any argument is, so a call made while computing one finds it whole.
  std::optional<ir::Value> throughPointer( Expression const& node, std::uint32_t type, Site const& site, bool read )
  {
    FunctionType const& named = mTyping.functionType( type );
    std::vector<ir::Argument> arguments;
    for ( std::size_t index = 0; index < node.arguments.size() && index < named.parameters.size(); ++index )
    {
      ParameterType const& parameter = named.parameters[index];
      std::string const argumentName = named.name + "." + mTyping.spellingOf( parameter.name );
      if ( parameter.type.aggregate.has_value() )
      {
        arguments.push_back( ir::Argument{ .name = argumentName,
                                           .type = ir::Type::BLOCK,
                                           .value = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} },
                                           .from = placeOf( *node.arguments[index], site ),
                                           .bytes = mTyping.bytes( ir::Type::BLOCK, parameter.type.aggregate ) } );
        continue;
      }
      arguments.push_back( ir::Argument{ .name = argumentName,
                                         .type = parameter.type.type,
                                         .value = operand( *node.arguments[index], parameter.type.type, site ) } );
    }
    arguments.push_back( ir::Argument{ .name = named.name + "." + std::string{ TRAMPOLINE_POINTER },
                                       .type = ir::Type::POINTER,
                                       .value = operand( *node.left, ir::Type::POINTER, site ) } );
    std::optional<ir::Value> result;
    if ( read && named.result.has_value() && !named.result->aggregate.has_value() )
    {
      result = define( site, named.result->type );
    }
    site.blocks->append( ir::Instruction{
        .operation = ir::Call{ .name = named.name, .arguments = std::move( arguments ), .result = result, .with = {} },
        .at = site.at } );
    return result;
  }

  /// A call: every argument computed first, in the order written, and each
  /// then written to its parameter's byte by the call itself, so that no call
  /// made while computing one meets a byte already written — see
  /// docs/decisions/0082-a-call-writes-the-callees-bytes.md. Its value, where
  /// `read` asks for one, is the callee's `__ret`, read right after it.
  std::optional<ir::Value> call( Expression const& node, Site const& site, bool read )
  {
    std::string const name = mTyping.spellingOf( node.left->token );
    Meaning const meaning = mTyping.resolve( *node.left ).value_or( Meaning{} );
    Definition const* const callee = meaning.kind == NameKind::FUNCTION ? meaning.definition : nullptr;
    if ( meaning.kind == NameKind::ASSEMBLER_PROC )
    {
      return assemblerCall( node, meaning, read, site );
    }
    if ( meaning.kind == NameKind::OBJECT && meaning.pointee.has_value() && meaning.pointee->function.has_value() )
    {
      return throughPointer( node, *meaning.pointee->function, site, read );
    }

    // A member reads its arguments from its type's Temporaries, so a direct
    // call writes them too — see docs/decisions/0065-handlers.md.
    std::string const bytesOwner = typeOfFunction( callee );
    std::vector<ir::Argument> arguments;
    for ( std::size_t index = 0; callee != nullptr && index < node.arguments.size(); ++index )
    {
      ParameterType const& parameter = callee->parameters.at( index );
      std::string const argumentName =
          ( bytesOwner.empty() ? name : bytesOwner ) + "." + mTyping.spellingOf( parameter.name );
      if ( parameter.type.aggregate.has_value() )
      {
        arguments.push_back( ir::Argument{ .name = argumentName,
                                           .type = ir::Type::BLOCK,
                                           .value = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} },
                                           .from = placeOf( *node.arguments[index], site ),
                                           .bytes = mTyping.bytes( ir::Type::BLOCK, parameter.type.aggregate ) } );
        continue;
      }
      ir::Operand value = operand( *node.arguments[index], parameter.type.type, site );
      arguments.push_back(
          ir::Argument{ .name = argumentName, .type = parameter.type.type, .value = std::move( value ) } );
    }
    std::optional<ir::Value> result;
    if ( read && callee != nullptr && callee->result.has_value() && !callee->result->aggregate.has_value() )
    {
      result = define( site, callee->result->type );
    }
    site.blocks->append( ir::Instruction{
        .operation =
            ir::Call{ .name = name,
                      .arguments = std::move( arguments ),
                      .result = result,
                      .returned = bytesOwner.empty() ? std::string{} : bytesOwner + "." + std::string{ RESULT },
                      .with = withFor( callee != nullptr ? callee->pane : std::string{} ) },
        .at = site.at } );
    return result;
  }

  /// A statement the second pass let through: a call of a name, or a value
  /// assigned to one.
  /// A call of an assembler's `.proc`: each argument written to the byte its
  /// `.declare arg` names, and the result read from its `.declare ret`'s.
  std::optional<ir::Value> assemblerCall( Expression const& node, Meaning const& meaning, bool read, Site const& site )
  {
    ExternalName const& proc = *meaning.external;
    Definition const* const declared = meaning.definition;
    std::vector<ir::Argument> arguments;
    for ( std::size_t index = 0; index < node.arguments.size(); ++index )
    {
      ExternalByte const& byte = proc.arguments.at( index );
      if ( declared != nullptr )
      {
        // As a call of C passes its function's parameters: a `struct` copied.
        NamedType const& parameter = declared->parameters.at( index ).type;
        if ( parameter.aggregate.has_value() && !parameter.pointee.has_value() )
        {
          arguments.push_back( ir::Argument{ .name = byte.name,
                                             .type = ir::Type::BLOCK,
                                             .value = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} },
                                             .from = placeOf( *node.arguments[index], site ),
                                             .bytes = mTyping.bytes( ir::Type::BLOCK, parameter.aggregate ) } );
          continue;
        }
        arguments.push_back( ir::Argument{ .name = byte.name,
                                           .type = parameter.type,
                                           .value = operand( *node.arguments[index], parameter.type, site ),
                                           .from = std::nullopt,
                                           .bytes = 0,
                                           .place = byte.place } );
        continue;
      }
      arguments.push_back( ir::Argument{ .name = byte.name,
                                         .type = byte.type,
                                         .value = operand( *node.arguments[index], byte.type, site ),
                                         .from = std::nullopt,
                                         .bytes = 0,
                                         .place = byte.place } );
    }
    std::optional<ir::Value> result;
    bool const blockResult =
        declared != nullptr && declared->result.has_value() && declared->result->aggregate.has_value();
    if ( read && proc.result.has_value() && !blockResult )
    {
      result = define(
          site, declared != nullptr && declared->result.has_value() ? declared->result->type : proc.result->type );
    }
    site.blocks->append( ir::Instruction{
        .operation = ir::Call{ .name = mTyping.spellingOf( node.left->token ),
                               .arguments = std::move( arguments ),
                               .result = result,
                               .returned = proc.result.has_value() ? proc.result->name : std::string{},
                               .with = withFor( proc.pane ),
                               .resultPlace = proc.result.has_value() ? proc.result->place : std::string{} },
        .at = site.at } );
    return result;
  }

  void expressionStatement( Expression const& node, Site const& site )
  {
    if ( node.kind == ExpressionKind::CALL )
    {
      call( node, site, false );
      return;
    }
    if ( Typed const target = mTyping.expression( *node.left ); target.aggregate.has_value() )
    {
      // Where the bytes go first, then where they come from, as written.
      std::uint32_t const bytes = mTyping.bytes( ir::Type::BLOCK, target.aggregate );
      Reach const destination = reach( *node.left, site );
      if ( destination.kind == Reach::Kind::STRIPED )
      {
        transfer( destination, reach( *node.right, site ), bytes, site );
        return;
      }
      copyInto( placeFrom( destination, site ), *node.right, bytes, site );
      return;
    }
    assigned( node, site, false );
  }

  /// A value read now, which a step written after its target keeps before the
  /// store: a place is a name the text would read again after it, and a value
  /// is already the byte a load left — see
  /// docs/decisions/0198-a-compound-assignment-is-a-value.md.
  static ir::Operand kept( ir::Operand value, ir::Type type, Site const& site )
  {
    if ( !std::holds_alternative<ir::Object>( value ) )
    {
      return value;
    }
    return materialized( std::move( value ), type, site );
  }

  /// An assignment, whose value is read where `wanted` says: what the target
  /// holds after it, or what it held before where the source wrote the step
  /// after the target.
  std::optional<ir::Operand> assigned( Expression const& node, Site const& site, bool wanted )
  {
    bool const before = wanted && node.writtenAfter;
    if ( node.left->kind == ExpressionKind::INDEX || node.left->kind == ExpressionKind::DEREFERENCE ||
         node.left->kind == ExpressionKind::MEMBER )
    {
      // Where the value goes first, then the value, as they are written.
      Reach const place = reach( *node.left, site );
      std::optional<ir::Operand> const held =
          before ? std::optional{ kept( read( place, site ), place.type, site ) } : std::nullopt;
      ir::Operand const value = converted( operand( *node.right, place.type, site ), place.type, site );
      write( place, value, site );
      if ( !wanted )
      {
        return std::nullopt;
      }
      return before ? *held : kept( value, place.type, site );
    }
    Meaning const target = mTyping.resolve( *node.left ).value_or( Meaning{} );
    std::string name = nameOf( target, node.left->token, site );
    std::optional<ir::Operand> const held =
        before ? std::optional{ materialized( ir::Object{ .name = name, .type = target.type }, target.type, site ) }
               : std::nullopt;
    ir::Operand const value = operand( *node.right, target.type, site );
    site.blocks->append( ir::Instruction{
        .operation = ir::Store{ .name = std::move( name ), .type = target.type, .value = value }, .at = site.at } );
    if ( !wanted )
    {
      return std::nullopt;
    }
    return before ? *held : kept( value, target.type, site );
  }

  /// `&&` or `||` read as a value: a byte of its own that either path writes,
  /// since what a condition decides is not one value but two
  /// ([0079](../decisions/0079-a-condition-is-a-bool.md)).
  ir::Operand decided( Expression const& node, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    std::string const byte = "__b" + std::to_string( mConditions.back()++ );
    site.function->locals.push_back(
        ir::Local{ .name = byte, .type = ir::Type::BOOL, .placement = mPlacement.front() } );

    std::uint32_t const whenTrue = blocks.create();
    std::uint32_t const whenFalse = blocks.create();
    std::uint32_t const join = blocks.create();
    condition( node, whenTrue, whenFalse, site );
    for ( std::uint32_t const path : { whenTrue, whenFalse } )
    {
      blocks.enter( path );
      blocks.append( ir::Instruction{
          .operation = ir::Store{ .name = byte,
                                  .type = ir::Type::BOOL,
                                  .value = ir::Constant{ .type = ir::Type::BOOL, .value = path == whenTrue ? 1 : 0 } },
          .at = site.at } );
      blocks.terminate( jumpTo( join, site.at ) );
    }
    blocks.enter( join );
    return ir::Object{ .name = byte, .type = ir::Type::BOOL };
  }

  /// `c ? a : b` read as a value: a byte of its own that either way writes,
  /// as `&&` and `||` take one — see
  /// docs/decisions/0169-a-conditional-operator.md.
  ir::Operand chosen( Expression const& node, ir::Type context, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    ir::Type const type = mTyping.expression( node ).type.value_or( context );
    std::string const byte = "__b" + std::to_string( mConditions.back()++ );
    site.function->locals.push_back(
        ir::Local{ .name = byte, .type = type, .bytes = 0, .placement = mPlacement.front() } );

    std::uint32_t const whenTrue = blocks.create();
    std::uint32_t const whenFalse = blocks.create();
    std::uint32_t const join = blocks.create();
    condition( *node.left, whenTrue, whenFalse, site );
    for ( std::uint32_t const path : { whenTrue, whenFalse } )
    {
      Expression const& way = path == whenTrue ? *node.arguments.front() : *node.arguments.back();
      blocks.enter( path );
      blocks.append( ir::Instruction{
          .operation =
              ir::Store{ .name = byte, .type = type, .value = converted( operand( way, type, site ), type, site ) },
          .at = site.at } );
      blocks.terminate( jumpTo( join, site.at ) );
    }
    blocks.enter( join );
    return ir::Object{ .name = byte, .type = type };
  }

  static ir::Value define( Site const& site, ir::Type type )
  {
    site.function->values.push_back( type );
    return ir::Value{ .index = static_cast<std::uint32_t>( site.function->values.size() - 1 ) };
  }

  /// An expression the check let through, as an operand, the instructions it
  /// needs appended first. A constant takes `context`, the type of where it is
  /// used.
  ir::Operand operand( Expression const& node, ir::Type context, Site const& site )
  {
    if ( node.kind == ExpressionKind::CONDITIONAL )
    {
      return chosen( node, context, site );
    }
    if ( node.kind == ExpressionKind::CALL )
    {
      return call( node, site, true ).value_or( ir::Value{} );
    }
    if ( node.kind == ExpressionKind::ASSIGNMENT )
    {
      return assigned( node, site, true ).value_or( ir::Value{} );
    }
    if ( node.kind == ExpressionKind::QUALIFIED_NAME )
    {
      return ir::Constant{ .type = ir::Type::U8, .value = mTyping.enumeratorIndex( node ).value_or( 0 ) };
    }
    if ( node.kind == ExpressionKind::NULL_POINTER )
    {
      return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = {} };
    }
    if ( node.kind == ExpressionKind::STRING_LITERAL )
    {
      return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = literalSection( node ) };
    }
    if ( node.kind == ExpressionKind::CHARACTER_CONSTANT && mTyping.isTranslated( node ) )
    {
      return ir::Constant{ .type = ir::Type::U8, .value = 0, .name = spellingOfCharacter( node ) };
    }
    if ( node.kind == ExpressionKind::IDENTIFIER )
    {
      std::optional<Meaning> const named = mTyping.lookup( node );
      // An array's name alone is the address of its first element.
      if ( named.has_value() && named->isArray )
      {
        return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = nameOf( *named, node.token, site ) };
      }
      if ( named.has_value() && isOpaqueConstant( *named ) )
      {
        return ir::Constant{ .type = named->type, .value = 0, .name = nameOf( *named, node.token, site ) };
      }
    }
    if ( node.kind == ExpressionKind::BINARY && isLogical( node.token.kind ) )
    {
      return decided( node, site );
    }
    Typed const kind = mTyping.expression( node );
    if ( node.kind == ExpressionKind::PREDEFINED_CONSTANT )
    {
      return ir::Constant{ .type = ir::Type::BOOL, .value = node.value };
    }
    if ( !kind.type.has_value() )
    {
      return ir::Constant{ .type = context, .value = wrapped( mTyping.fold( node, context ), context ) };
    }
    if ( kind.known )
    {
      return constantOf( node, *kind.type, site );
    }
    if ( kind.decided.has_value() )
    {
      return ir::Constant{ .type = ir::Type::BOOL, .value = *kind.decided ? 1 : 0 };
    }

    ir::Type const type = *kind.type;
    if ( node.kind == ExpressionKind::CAST )
    {
      // What is cast is lowered in its own type: an `enum struct`'s is `u8`.
      ir::Type const source = mTyping.expression( *node.left ).type.value_or( type );
      return converted( operand( *node.left, source, site ), type, site );
    }
    if ( node.kind == ExpressionKind::INDEX || node.kind == ExpressionKind::DEREFERENCE )
    {
      return read( reach( node, site ), site );
    }
    if ( node.kind == ExpressionKind::MEMBER )
    {
      Reach const place = reach( node, site );
      // An array member is the address of its first element.
      if ( place.isArray )
      {
        return addressOfReach( place, site );
      }
      ir::Operand value = read( place, site );
      Expression const* root = &node;
      while ( root->kind == ExpressionKind::MEMBER || root->kind == ExpressionKind::INDEX )
      {
        root = root->left.get();
      }
      if ( root->kind == ExpressionKind::CALL && std::holds_alternative<ir::Object>( value ) )
      {
        return materialized( std::move( value ), place.type, site );
      }
      return value;
    }
    if ( node.kind == ExpressionKind::ADDRESS )
    {
      return addressOf( *node.left, site );
    }
    if ( node.kind == ExpressionKind::BINARY )
    {
      Typed const left = mTyping.expression( *node.left );
      Typed const right = mTyping.expression( *node.right );
      if ( left.pointee.has_value() || right.pointee.has_value() || left.isNull || right.isNull )
      {
        if ( isComparison( node.token.kind ) )
        {
          return comparison( node, site );
        }
        return pointerArithmetic( node, left, right, site );
      }
    }
    if ( node.kind == ExpressionKind::IDENTIFIER )
    {
      Meaning const meaning = mTyping.resolve( node ).value_or( Meaning{} );
      if ( meaning.kind == NameKind::FUNCTION || meaning.kind == NameKind::ASSEMBLER_PROC )
      {
        // A function's name alone is its address, and carries the Proc of its
        // type as `&f` does — see docs/decisions/0065-handlers.md.
        return ir::Constant{ .type = ir::Type::POINTER,
                             .value = 0,
                             .name = nameOf( meaning, node.token, site ),
                             .follower = typeOfMember( meaning, node ) };
      }
      return ir::Object{ .name = nameOf( meaning, node.token, site ), .type = type };
    }
    if ( node.kind == ExpressionKind::UNARY )
    {
      ir::Operand inner = operand( *node.left, type, site );
      ir::UnaryOperator op = ir::UnaryOperator::LOGICAL_NOT;
      if ( node.token.kind == TokenKind::MINUS )
      {
        op = ir::UnaryOperator::NEGATE;
      }
      else if ( node.token.kind == TokenKind::TILDE )
      {
        op = ir::UnaryOperator::COMPLEMENT;
      }
      ir::Value const result = define( site, type );
      site.blocks->append( ir::Instruction{
          .operation = ir::Unary{ .result = result, .op = op, .type = type, .operand = std::move( inner ) },
          .at = site.at } );
      return result;
    }
    if ( isComparison( node.token.kind ) )
    {
      return comparison( node, site );
    }

    TokenKind const op = node.token.kind;
    if ( op == TokenKind::STAR || op == TokenKind::SLASH || op == TokenKind::PERCENT )
    {
      return product( node, type, site );
    }
    ir::Operand left = converted( operand( *node.left, type, site ), type, site );
    ir::Operand right;
    if ( !isShift( node.token.kind ) )
    {
      right = converted( operand( *node.right, type, site ), type, site );
    }
    else if ( isKnown( mTyping.expression( *node.right ) ) )
    {
      right = ir::Constant{ .type = ir::Type::U8, .value = mTyping.fold( *node.right, std::nullopt ) };
    }
    else
    {
      // A count is a `u8` whatever it shifts.
      right = operand( *node.right, ir::Type::U8, site );
    }
    ir::Value const result = define( site, type );
    site.blocks->append( ir::Instruction{ .operation = ir::Binary{ .result = result,
                                                                   .op = binaryOperator( node.token.kind ),
                                                                   .type = type,
                                                                   .left = std::move( left ),
                                                                   .right = std::move( right ) },
                                          .at = site.at } );
    return result;
  }

  /// `*`, `/` or `%` of a value, of `type`: shifts and adds by a constant,
  /// a shift and a mask by a power of two, and a call of the runtime
  /// otherwise — see docs/decisions/0095-literals-and-the-runtime.md.
  ir::Operand product( Expression const& node, ir::Type type, Site const& site )
  {
    TokenKind const op = node.token.kind;
    bool const leftKnown = isKnown( mTyping.expression( *node.left ) );
    bool const rightKnown = isKnown( mTyping.expression( *node.right ) );
    std::uint32_t const bits = 8 * ir::sizeOf( type );
    // A constant as the bits of the type, which is what a product is taken in.
    auto const bitsOf = [&]( Expression const& constant )
    {
      auto const value = static_cast<std::uint64_t>( mTyping.fold( constant, type ) );
      return static_cast<std::uint32_t>( value & ( ( std::uint64_t{ 1 } << bits ) - 1U ) );
    };
    if ( op == TokenKind::STAR && ( leftKnown || rightKnown ) )
    {
      Expression const& value = rightKnown ? *node.left : *node.right;
      Expression const& factor = rightKnown ? *node.right : *node.left;
      return multiplied( converted( operand( value, type, site ), type, site ), bitsOf( factor ), type, site );
    }
    std::int64_t const divisor = rightKnown ? wrapped( mTyping.fold( *node.right, type ), type ) : 0;
    bool const powerOfTwo = rightKnown && divisor > 0 && ( divisor & ( divisor - 1 ) ) == 0;
    if ( powerOfTwo && op != TokenKind::STAR )
    {
      auto const shift = static_cast<std::int64_t>( std::countr_zero( static_cast<std::uint64_t>( divisor ) ) );
      ir::Operand value = converted( operand( *node.left, type, site ), type, site );
      if ( !ir::isSigned( type ) )
      {
        return op == TokenKind::SLASH
                   ? ir::Operand{ binaryValue( ir::BinaryOperator::SHIFT_RIGHT,
                                               type,
                                               std::move( value ),
                                               ir::Constant{ .type = ir::Type::U8, .value = shift, .name = {} },
                                               site ) }
                   : ir::Operand{ binaryValue( ir::BinaryOperator::AND,
                                               type,
                                               std::move( value ),
                                               ir::Constant{ .type = type, .value = divisor - 1, .name = {} },
                                               site ) };
      }
      if ( op == TokenKind::SLASH )
      {
        if ( shift == 0 )
        {
          return value;
        }
        // C truncates towards zero, so a negative value is biased by the
        // divisor less one before the shift, which rounds towards minus
        // infinity: the sign spread over the value, masked to the bias.
        ir::Value const sign = binaryValue( ir::BinaryOperator::SHIFT_RIGHT,
                                            type,
                                            value,
                                            ir::Constant{ .type = ir::Type::U8, .value = bits - 1, .name = {} },
                                            site );
        ir::Value const bias = binaryValue(
            ir::BinaryOperator::AND, type, sign, ir::Constant{ .type = type, .value = divisor - 1, .name = {} }, site );
        ir::Value const biased = binaryValue( ir::BinaryOperator::ADD, type, std::move( value ), bias, site );
        return binaryValue( ir::BinaryOperator::SHIFT_RIGHT,
                            type,
                            biased,
                            ir::Constant{ .type = ir::Type::U8, .value = shift, .name = {} },
                            site );
      }
    }

    // The runtime: its Proc per operator, width and sign, called as any Proc
    // of the assembler is.
    std::string const width = std::to_string( bits );
    std::string name;
    std::string left = "dividend";
    std::string right = "divisor";
    std::string result;
    if ( op == TokenKind::STAR )
    {
      name = "__mul" + width;
      left = "left";
      right = "right";
      result = "product";
    }
    else
    {
      bool const signedOperands = ir::isSigned( type );
      name = std::string{ signedOperands ? "__s" : "__u" } + ( op == TokenKind::SLASH ? "div" : "mod" ) + width;
      result = op == TokenKind::SLASH ? "quotient" : "remainder";
      if ( op == TokenKind::SLASH && !signedOperands )
      {
        // An unsigned division builds the quotient where the dividend was,
        // so one byte of it carries both roles — see
        // docs/decisions/0119-one-temporary-carries-two-roles.md.
        left = "dividendAndQuotient";
        result = left;
      }
    }
    std::vector<ir::Argument> arguments;
    arguments.push_back( ir::Argument{ .name = name + "." + left,
                                       .type = type,
                                       .value = converted( operand( *node.left, type, site ), type, site ) } );
    arguments.push_back( ir::Argument{ .name = name + "." + right,
                                       .type = type,
                                       .value = converted( operand( *node.right, type, site ), type, site ) } );
    ir::Value const value = define( site, type );
    site.blocks->append( ir::Instruction{ .operation = ir::Call{ .name = name,
                                                                 .arguments = std::move( arguments ),
                                                                 .result = value,
                                                                 .returned = name + "." + result },
                                          .at = site.at } );
    return value;
  }

  static ir::BinaryOperator binaryOperator( TokenKind kind )
  {
    switch ( kind )
    {
    case TokenKind::MINUS:
      return ir::BinaryOperator::SUBTRACT;
    case TokenKind::AMPERSAND:
      return ir::BinaryOperator::AND;
    case TokenKind::PIPE:
      return ir::BinaryOperator::OR;
    case TokenKind::CARET:
      return ir::BinaryOperator::XOR;
    case TokenKind::LESS_LESS:
      return ir::BinaryOperator::SHIFT_LEFT;
    case TokenKind::GREATER_GREATER:
      return ir::BinaryOperator::SHIFT_RIGHT;
    default:
      return ir::BinaryOperator::ADD;
    }
  }

  /// An expression of constants the check let through, in `type`: named by the
  /// `const` it is, where it is one read in its own type, or by the object and
  /// offset it is the address of.
  [[nodiscard]] ir::Constant constantOf( Expression const& node, ir::Type type, Site const& site )
  {
    ir::Constant constant{ .type = type, .value = wrapped( mTyping.fold( node, type ), type ), .name = {} };
    if ( node.kind == ExpressionKind::STRING_LITERAL )
    {
      return ir::Constant{ .type = type, .value = 0, .name = literalSection( node ) };
    }
    if ( node.kind == ExpressionKind::CHARACTER_CONSTANT && mTyping.isTranslated( node ) )
    {
      return ir::Constant{ .type = type, .value = 0, .name = spellingOfCharacter( node ) };
    }
    if ( std::optional<std::string> const address = addressName( node, site ); address.has_value() )
    {
      constant.value = 0;
      constant.name = *address;
      return constant;
    }
    if ( node.kind != ExpressionKind::IDENTIFIER )
    {
      return constant;
    }
    std::optional<Meaning> const meaning = mTyping.lookup( node );
    if ( meaning.has_value() && isOpaqueConstant( *meaning ) )
    {
      constant.value = 0;
      constant.name = nameOf( *meaning, node.token, site );
      return constant;
    }
    if ( meaning.has_value() && meaning->isConst && meaning->value.has_value() && meaning->type == type &&
         ( meaning->external == nullptr || meaning->definition != nullptr ) )
    {
      constant.name = nameOf( *meaning, node.token, site );
    }
    return constant;
  }

  /// Where an element, or what a pointer points at, is reached: an object at an
  /// address the text names, an element of one through `X`, or `Y` bytes on
  /// from a pointer on the zero page.
  struct Reach
  {
    enum class Kind : std::uint8_t
    {
      OBJECT,
      INDEXED,
      INDIRECT,

      /// In a striped array's stripes, `index` in `X`.
      STRIPED,
    };

    Kind kind = Kind::OBJECT;
    ir::Type type = ir::Type::U8;
    std::string name{};
    ir::Operand pointer{};
    ir::Operand index{};

    /// An INDEXED index already counted in bytes.
    bool scaled = false;

    /// A member that is an array, whose value is its first element's address.
    bool isArray = false;

    /// An INDIRECT place reached through a pointer to `volatile`.
    bool isVolatile = false;

    /// A STRIPED place: the array's name, the stripe of each byte of an
    /// element, and the byte of the element it is at.
    std::string array{};
    std::vector<std::vector<std::string>> paths{};
    std::uint32_t offset = 0;
  };

  /// The Label of the stripe a byte of a striped element lies in.
  static std::string stripeOf( Reach const& place, std::uint32_t byte )
  {
    std::string name = place.array;
    if ( byte < place.paths.size() )
    {
      for ( std::string const& part : place.paths[byte] )
      {
        name += "." + part;
      }
    }
    return name;
  }

  /// A pointer an operand is, on the zero page where `(zp),y` reads through
  /// it: an object of C declared a pointer lies there, and any other object —
  /// a member, a `u16` cast to a pointer, a name of the assembler — is copied
  /// to a pair of scratch bytes first.
  ir::Operand pointerOperand( Expression const& node, Site const& site )
  {
    ir::Operand pointer = operand( node, ir::Type::POINTER, site );
    if ( std::holds_alternative<ir::Object>( pointer ) && !onZeroPage( node ) )
    {
      return materialized( std::move( pointer ), ir::Type::POINTER, site );
    }
    return pointer;
  }

  /// Whether an object read as a pointer is one of C declared a pointer, cast
  /// or not, which is a pair of bytes on the zero page.
  [[nodiscard]] bool onZeroPage( Expression const& node ) const
  {
    Expression const* named = &node;
    while ( named->kind == ExpressionKind::CAST && named->left != nullptr )
    {
      named = named->left.get();
    }
    if ( named->kind != ExpressionKind::IDENTIFIER )
    {
      return false;
    }
    std::optional<Meaning> const meaning = mTyping.lookup( *named );
    return meaning.has_value() && meaning->kind == NameKind::OBJECT && meaning->external == nullptr &&
           meaning->pointee.has_value() && !meaning->isArray;
  }

  /// An operand as a value of its own, read now: a member of a call's result is
  /// read before another call writes the result again.
  static ir::Operand materialized( ir::Operand value, ir::Type type, Site const& site )
  {
    ir::Value const result = define( site, type );
    site.blocks->append( ir::Instruction{
        .operation = ir::Convert{ .result = result, .type = type, .operand = std::move( value ) }, .at = site.at } );
    return result;
  }

  /// `value` times a constant, by shifts and adds of the value — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
  /// `value` times a constant, as shifts and adds from the factor's top bit
  /// down. A value is read once, by its own instruction, so one computed is
  /// kept in a byte of the Proc first, which the adds read as often as they
  /// need.
  ir::Operand multiplied( ir::Operand value, std::uint32_t factor, ir::Type type, Site const& site )
  {
    if ( factor == 0 )
    {
      return ir::Constant{ .type = type, .value = 0, .name = {} };
    }
    auto const shifted = [&]( ir::Operand operand, int count ) -> ir::Operand
    {
      if ( count == 0 )
      {
        return operand;
      }
      return binaryValue( ir::BinaryOperator::SHIFT_LEFT,
                          type,
                          std::move( operand ),
                          ir::Constant{ .type = ir::Type::U8, .value = count, .name = {} },
                          site );
    };
    int const top = std::bit_width( factor ) - 1;
    if ( std::has_single_bit( factor ) )
    {
      return shifted( std::move( value ), top );
    }
    if ( std::holds_alternative<ir::Value>( value ) )
    {
      std::string const byte = "__m" + std::to_string( mConditions.back()++ );
      site.function->locals.push_back( ir::Local{ .name = byte, .type = type, .placement = mPlacement.front() } );
      site.blocks->append( ir::Instruction{
          .operation = ir::Store{ .name = byte, .type = type, .value = std::move( value ) }, .at = site.at } );
      value = ir::Object{ .name = byte, .type = type };
    }
    ir::Operand product = value;
    int pending = 0;
    for ( int bit = top - 1; bit >= 0; --bit )
    {
      ++pending;
      if ( ( ( factor >> static_cast<unsigned>( bit ) ) & 1U ) != 0 )
      {
        product = binaryValue( ir::BinaryOperator::ADD, type, shifted( std::move( product ), pending ), value, site );
        pending = 0;
      }
    }
    return shifted( std::move( product ), pending );
  }

  /// `s.m` and `p->m`: where the member is, `offset` bytes on from where what
  /// holds it is.
  Reach member( Expression const& node, Site const& site )
  {
    Typed const kind = mTyping.expression( node );
    ir::Type const type = kind.type.value_or( ir::Type::U8 );
    Typed const outer = mTyping.expression( *node.left );
    bool const arrow = node.token.kind == TokenKind::ARROW;
    std::uint32_t const aggregateIndex =
        arrow ? outer.pointee.value_or( Pointee{} ).aggregate.value_or( 0 ) : outer.aggregate.value_or( 0 );
    MemberType const* const found =
        memberNamed( mTyping.aggregate( aggregateIndex ), mTyping.spellingOf( node.right->token ), *mSources );
    std::uint32_t const offset = found != nullptr ? found->offset : 0;
    ir::Type memberType = found != nullptr && found->type.pointee.has_value() ? ir::Type::POINTER : type;
    bool const isArray = found != nullptr && found->isArray;
    if ( isArray )
    {
      memberType = found->type.type;
    }
    Reach outerPlace;
    if ( arrow )
    {
      ir::Operand pointer = pointerOperand( *node.left, site );
      // A pointer that is a constant, a number or a name the assembler knows,
      // points at an address the text writes, as an element's does.
      if ( auto const* constant = std::get_if<ir::Constant>( &pointer ) )
      {
        outerPlace = Reach{ .kind = Reach::Kind::OBJECT,
                            .type = memberType,
                            .name = offsetName( *constant, 0 ),
                            .pointer = {},
                            .index = {} };
      }
      else
      {
        outerPlace = Reach{ .kind = Reach::Kind::INDIRECT,
                            .type = memberType,
                            .name = {},
                            .pointer = std::move( pointer ),
                            .index = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} } };
      }
    }
    else
    {
      outerPlace = reach( *node.left, site );
    }
    Reach place = offsetReach( std::move( outerPlace ), offset, memberType, site );
    place.isArray = isArray;
    return place;
  }

  /// A place `offset` bytes on from another.
  static Reach offsetReach( Reach outer, std::uint32_t offset, ir::Type type, Site const& site )
  {
    outer.type = type;
    if ( outer.kind == Reach::Kind::STRIPED )
    {
      outer.offset += offset;
      return outer;
    }
    if ( outer.kind != Reach::Kind::INDIRECT )
    {
      outer.name = offsetName( ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = outer.name }, offset );
      return outer;
    }
    std::int64_t const at = std::get<ir::Constant>( outer.index ).value + offset;
    if ( at + ir::sizeOf( type ) <= 256 )
    {
      outer.index = ir::Constant{ .type = ir::Type::U8, .value = at, .name = {} };
      return outer;
    }
    outer.pointer =
        binaryValue( ir::BinaryOperator::ADD,
                     ir::Type::POINTER,
                     outer.pointer,
                     ir::Constant{ .type = ir::Type::POINTER, .value = wrapped( at, ir::Type::POINTER ), .name = {} },
                     site );
    outer.index = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} };
    return outer;
  }

  /// The address a place is at.
  static ir::Operand addressOfReach( Reach const& place, Site const& site )
  {
    switch ( place.kind )
    {
    case Reach::Kind::STRIPED:
    {
      // A byte of a stripe, at its Label and the index.
      ir::Constant const stripe{ .type = ir::Type::POINTER, .value = 0, .name = stripeOf( place, place.offset ) };
      if ( auto const* constant = std::get_if<ir::Constant>( &place.index ) )
      {
        return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = offsetName( stripe, constant->value ) };
      }
      return binaryValue(
          ir::BinaryOperator::ADD, ir::Type::POINTER, stripe, converted( place.index, ir::Type::POINTER, site ), site );
    }
    case Reach::Kind::OBJECT:
      return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = place.name };
    case Reach::Kind::INDEXED:
    {
      ir::Operand offset = converted( place.index, ir::Type::POINTER, site );
      if ( !place.scaled && ir::sizeOf( place.type ) == 2 )
      {
        offset = binaryValue( ir::BinaryOperator::SHIFT_LEFT,
                              ir::Type::POINTER,
                              std::move( offset ),
                              ir::Constant{ .type = ir::Type::U8, .value = 1, .name = {} },
                              site );
      }
      return binaryValue( ir::BinaryOperator::ADD,
                          ir::Type::POINTER,
                          ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = place.name },
                          std::move( offset ),
                          site );
    }
    case Reach::Kind::INDIRECT:
      if ( auto const* constant = std::get_if<ir::Constant>( &place.index );
           constant != nullptr && constant->value == 0 )
      {
        return place.pointer;
      }
      return binaryValue( ir::BinaryOperator::ADD,
                          ir::Type::POINTER,
                          place.pointer,
                          converted( place.index, ir::Type::POINTER, site ),
                          site );
    }
    return place.pointer;
  }

  /// Where the bytes of a `struct` or a `union` an expression is are.
  ir::Place placeOf( Expression const& node, Site const& site )
  {
    return placeFrom( reach( node, site ), site );
  }

  /// Where a place's bytes are, as a copy reads them: a name, or a pointer on
  /// the zero page and an offset. A striped place has no such bytes, and is
  /// copied a byte at a time instead.
  static ir::Place placeFrom( Reach const& place, Site const& site )
  {
    if ( place.kind == Reach::Kind::OBJECT )
    {
      return ir::Place{ .name = place.name };
    }
    if ( place.kind == Reach::Kind::INDIRECT )
    {
      if ( auto const* constant = std::get_if<ir::Constant>( &place.index ) )
      {
        return ir::Place{ .name = {},
                          .pointer = place.pointer,
                          .offset = static_cast<std::uint32_t>( constant->value ) };
      }
    }
    return ir::Place{ .name = {}, .pointer = addressOfReach( place, site ), .offset = 0 };
  }

  /// The bytes of a `struct` copied where either side lies in stripes: a byte
  /// at a time, each read and then written, an index several instructions read
  /// kept in a byte of its own — see docs/decisions/0089-a-stripe-is-a-section.md.
  void transfer( Reach to, Reach from, std::uint32_t bytes, Site const& site )
  {
    auto const kept = [&]( Reach& place )
    {
      if ( place.kind == Reach::Kind::STRIPED && std::holds_alternative<ir::Value>( place.index ) )
      {
        std::string const name = "__i" + std::to_string( mIndexes.back()++ );
        site.function->locals.push_back(
            ir::Local{ .name = name, .type = ir::Type::U8, .bytes = 0, .placement = mPlacement.front() } );
        site.blocks->append( ir::Instruction{
            .operation = ir::Store{ .name = name, .type = ir::Type::U8, .value = place.index }, .at = site.at } );
        place.index = ir::Object{ .name = name, .type = ir::Type::U8 };
      }
      if ( place.kind == Reach::Kind::INDEXED )
      {
        place = Reach{ .kind = Reach::Kind::INDIRECT,
                       .type = place.type,
                       .name = {},
                       .pointer = addressOfReach( place, site ),
                       .index = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} } };
      }
    };
    kept( to );
    kept( from );
    auto const byteOf = []( Reach place, std::uint32_t byte )
    {
      place.type = ir::Type::U8;
      switch ( place.kind )
      {
      case Reach::Kind::STRIPED:
        place.offset += byte;
        break;
      case Reach::Kind::OBJECT:
        place.name = offsetName( ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = place.name }, byte );
        break;
      default:
        place.index = ir::Constant{ .type = ir::Type::U8,
                                    .value = std::get<ir::Constant>( place.index ).value + byte,
                                    .name = {} };
        break;
      }
      return place;
    };
    for ( std::uint32_t byte = 0; byte < bytes; ++byte )
    {
      write( byteOf( to, byte ), read( byteOf( from, byte ), site ), site );
    }
  }

  /// The bytes of a `struct` or a `union` copied into a place: from where the
  /// expression is, a pointer on either side taking no offset in a loop.
  void copyInto( ir::Place to, Expression const& from, std::uint32_t bytes, Site const& site )
  {
    Reach const origin = reach( from, site );
    if ( origin.kind == Reach::Kind::STRIPED )
    {
      Reach const target =
          to.pointer.has_value()
              ? Reach{ .kind = Reach::Kind::INDIRECT,
                       .type = ir::Type::U8,
                       .name = {},
                       .pointer = *to.pointer,
                       .index = ir::Constant{ .type = ir::Type::U8, .value = to.offset, .name = {} } }
              : Reach{ .kind = Reach::Kind::OBJECT, .type = ir::Type::U8, .name = to.name, .pointer = {}, .index = {} };
      transfer( target, origin, bytes, site );
      return;
    }
    ir::Place source = placeFrom( origin, site );
    auto const unshifted = [&]( ir::Place& place )
    {
      if ( place.pointer.has_value() && place.offset != 0 && bytes > SMALL_BLOCK )
      {
        place.pointer = binaryValue( ir::BinaryOperator::ADD,
                                     ir::Type::POINTER,
                                     *place.pointer,
                                     ir::Constant{ .type = ir::Type::POINTER, .value = place.offset, .name = {} },
                                     site );
        place.offset = 0;
      }
    };
    unshifted( to );
    unshifted( source );
    site.blocks->append( ir::Instruction{
        .operation = ir::Copy{ .from = std::move( source ), .to = std::move( to ), .bytes = bytes }, .at = site.at } );
  }

  /// The name an address constant is written by: the object and the offset,
  /// `NAME+N` or `NAME-N`, or the address itself.
  static std::string offsetName( ir::Constant const& base, std::int64_t offset )
  {
    if ( base.name.empty() )
    {
      return std::to_string( wrapped( base.value + offset, ir::Type::POINTER ) );
    }
    if ( offset == 0 )
    {
      return base.name;
    }
    return base.name + ( offset < 0 ? "-" : "+" ) + std::to_string( offset < 0 ? -offset : offset );
  }

  /// What the address of an object at a name the assembler knows is written
  /// by: an array's name alone, `&x`, `&t[3]`; nothing for any other.
  [[nodiscard]] std::optional<std::string> addressName( Expression const& node, Site const& site )
  {
    Expression const* place = nullptr;
    if ( node.kind == ExpressionKind::ADDRESS )
    {
      place = node.left.get();
    }
    else if ( node.kind == ExpressionKind::IDENTIFIER || node.kind == ExpressionKind::MEMBER )
    {
      // An array alone, a member one among them, is its first element's.
      if ( std::optional<Shape> const shape = mTyping.shape( node ); shape.has_value() && shape->isArray )
      {
        place = &node;
      }
    }
    if ( place == nullptr )
    {
      return std::nullopt;
    }
    // A byte of a stripe at a constant index is at the stripe's Label.
    if ( mTyping.throughStripes( *place ) )
    {
      Reach const stripe = reach( *place, site );
      if ( stripe.kind != Reach::Kind::STRIPED || !std::holds_alternative<ir::Constant>( stripe.index ) )
      {
        return std::nullopt;
      }
      return std::get<ir::Constant>( addressOfReach( stripe, site ) ).name;
    }
    std::optional<std::pair<std::string, std::int64_t>> const fixed = fixedPlace( *place, site );
    if ( !fixed.has_value() )
    {
      return std::nullopt;
    }
    return offsetName( ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = fixed->first }, fixed->second );
  }

  /// The name and offset of a place at an address the assembler knows: an
  /// object, a member of one by `.`, an element by a constant index.
  std::optional<std::pair<std::string, std::int64_t>> fixedPlace( Expression const& node, Site const& site )
  {
    switch ( node.kind )
    {
    case ExpressionKind::IDENTIFIER:
    {
      std::optional<Meaning> const meaning = mTyping.lookup( node );
      if ( !meaning.has_value() || meaning->kind != NameKind::OBJECT || meaning->pointee.has_value() )
      {
        return std::nullopt;
      }
      return std::pair{ nameOf( *meaning, node.token, site ), std::int64_t{ 0 } };
    }
    case ExpressionKind::MEMBER:
    {
      std::optional<Shape> const outer = mTyping.shape( *node.left );
      if ( node.token.kind != TokenKind::DOT || !outer.has_value() || !outer->aggregate.has_value() || outer->isArray )
      {
        return std::nullopt;
      }
      MemberType const* const member =
          memberNamed( mTyping.aggregate( *outer->aggregate ), mTyping.spellingOf( node.right->token ), *mSources );
      std::optional<std::pair<std::string, std::int64_t>> base = fixedPlace( *node.left, site );
      if ( member == nullptr || !base.has_value() )
      {
        return std::nullopt;
      }
      base->second += member->offset;
      return base;
    }
    case ExpressionKind::INDEX:
    {
      std::optional<Shape> const outer = mTyping.shape( *node.left );
      std::optional<Shape> const element = mTyping.shape( node );
      if ( !outer.has_value() || !outer->isArray || !element.has_value() ||
           !isKnown( mTyping.expression( *node.right ) ) )
      {
        return std::nullopt;
      }
      std::optional<std::pair<std::string, std::int64_t>> base = fixedPlace( *node.left, site );
      if ( !base.has_value() )
      {
        return std::nullopt;
      }
      base->second += mTyping.fold( *node.right, std::nullopt ) * mTyping.bytes( element->type, element->aggregate ) *
                      ( element->isArray ? element->count : 1 );
      return base;
    }
    default:
      return std::nullopt;
    }
  }

  /// An integer operand in `ptr`, times `size`: an offset of that many
  /// elements.
  ir::Operand scaled( Expression const& node, std::uint32_t size, Site const& site )
  {
    Typed const kind = mTyping.expression( node );
    if ( isKnown( kind ) )
    {
      std::int64_t const value = mTyping.fold( node, std::nullopt ) * size;
      return ir::Constant{ .type = ir::Type::POINTER, .value = wrapped( value, ir::Type::POINTER ), .name = {} };
    }
    ir::Operand offset =
        converted( operand( node, kind.type.value_or( ir::Type::U8 ), site ), ir::Type::POINTER, site );
    return multiplied( std::move( offset ), size, ir::Type::POINTER, site );
  }

  static ir::Value
  binaryValue( ir::BinaryOperator op, ir::Type type, ir::Operand left, ir::Operand right, Site const& site )
  {
    ir::Value const result = define( site, type );
    site.blocks->append( ir::Instruction{
        .operation =
            ir::Binary{
                .result = result, .op = op, .type = type, .left = std::move( left ), .right = std::move( right ) },
        .at = site.at } );
    return result;
  }

  /// `p + n`, `n + p`, `p - n`, and `p - q` counted in elements.
  ir::Operand pointerArithmetic( Expression const& node, Typed const& left, Typed const& right, Site const& site )
  {
    if ( left.pointee.has_value() && right.pointee.has_value() )
    {
      ir::Operand bytes = converted( binaryValue( ir::BinaryOperator::SUBTRACT,
                                                  ir::Type::POINTER,
                                                  operand( *node.left, ir::Type::POINTER, site ),
                                                  operand( *node.right, ir::Type::POINTER, site ),
                                                  site ),
                                     ir::Type::I16,
                                     site );
      if ( mTyping.bytes( left.pointee->type, left.pointee->aggregate ) == 1 )
      {
        return bytes;
      }
      return binaryValue( ir::BinaryOperator::SHIFT_RIGHT,
                          ir::Type::I16,
                          std::move( bytes ),
                          ir::Constant{ .type = ir::Type::U8, .value = 1, .name = {} },
                          site );
    }
    bool const pointerLeft = left.pointee.has_value();
    Expression const& pointer = pointerLeft ? *node.left : *node.right;
    Expression const& count = pointerLeft ? *node.right : *node.left;
    std::optional<Pointee> const pointee = pointerLeft ? left.pointee : right.pointee;
    std::uint32_t const size =
        mTyping.bytes( pointee.value_or( Pointee{} ).type, pointee.value_or( Pointee{} ).aggregate );
    ir::Operand base = operand( pointer, ir::Type::POINTER, site );
    ir::Operand offset = scaled( count, size, site );
    return binaryValue( node.token.kind == TokenKind::MINUS ? ir::BinaryOperator::SUBTRACT : ir::BinaryOperator::ADD,
                        ir::Type::POINTER,
                        std::move( base ),
                        std::move( offset ),
                        site );
  }

  /// `&x`, `&t[i]` and `&*p`.
  /// The function type a function belongs to, or empty.
  [[nodiscard]] std::string typeOfFunction( Definition const* function ) const
  {
    if ( mMembers == nullptr || function == nullptr )
    {
      return {};
    }
    auto const found = mMembers->ofFunction.find( function );
    return found == mMembers->ofFunction.end() ? std::string{} : mTyping.functionType( found->second ).name;
  }

  /// The function type a name is a member of, or empty: what the taking of
  /// its address is `.own`ed by — see docs/decisions/0065-handlers.md.
  [[nodiscard]] std::string typeOfMember( Meaning const& meaning, Expression const& named ) const
  {
    if ( mMembers == nullptr )
    {
      return {};
    }
    if ( meaning.kind == NameKind::FUNCTION && meaning.definition != nullptr )
    {
      auto const found = mMembers->ofFunction.find( meaning.definition );
      return found == mMembers->ofFunction.end() ? std::string{} : mTyping.functionType( found->second ).name;
    }
    if ( meaning.kind == NameKind::ASSEMBLER_PROC )
    {
      auto const found = mMembers->ofProc.find( mTyping.spellingOf( named.token ) );
      return found == mMembers->ofProc.end() ? std::string{} : mTyping.functionType( found->second ).name;
    }
    return {};
  }

  ir::Operand addressOf( Expression const& operand, Site const& site )
  {
    if ( operand.kind == ExpressionKind::DEREFERENCE )
    {
      return this->operand( *operand.left, ir::Type::POINTER, site );
    }
    if ( operand.kind == ExpressionKind::MEMBER ||
         ( operand.kind == ExpressionKind::INDEX && operand.left->kind != ExpressionKind::IDENTIFIER ) )
    {
      return addressOfReach( reach( operand, site ), site );
    }
    Expression const& named = operand.kind == ExpressionKind::INDEX ? *operand.left : operand;
    Meaning const meaning = mTyping.lookup( named ).value_or( Meaning{} );
    if ( operand.kind == ExpressionKind::IDENTIFIER )
    {
      // The address of a function is followed by the Proc of its type, which
      // the taking says — see docs/decisions/0065-handlers.md.
      return ir::Constant{ .type = ir::Type::POINTER,
                           .value = 0,
                           .name = nameOf( meaning, named.token, site ),
                           .follower = typeOfMember( meaning, named ) };
    }
    ir::Operand base =
        meaning.pointee.has_value()
            ? this->operand( named, ir::Type::POINTER, site )
            : ir::Operand{ ir::Constant{
                  .type = ir::Type::POINTER, .value = 0, .name = nameOf( meaning, named.token, site ) } };
    std::uint32_t const size = meaning.pointee.has_value()
                                   ? mTyping.bytes( meaning.pointee->type, meaning.pointee->aggregate )
                                   : mTyping.bytes( meaning.type, meaning.aggregate );
    if ( isKnown( mTyping.expression( *operand.right ) ) )
    {
      std::int64_t const offset = mTyping.fold( *operand.right, std::nullopt ) * size;
      if ( auto const* constant = std::get_if<ir::Constant>( &base ) )
      {
        return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = offsetName( *constant, offset ) };
      }
      if ( offset == 0 )
      {
        return base;
      }
    }
    ir::Operand offset = scaled( *operand.right, size, site );
    return binaryValue( ir::BinaryOperator::ADD, ir::Type::POINTER, std::move( base ), std::move( offset ), site );
  }

  /// Where `t[i]` or `*p` is: through `X` into an array at most a byte of
  /// index long, at a name and offset where both are constants, through `Y`
  /// from a pointer where the offset fits a byte — and otherwise at an address
  /// computed into a pair of scratch bytes, which are on the zero page.
  Reach reach( Expression const& node, Site const& site )
  {
    Reach place = reachOf( node, site );
    // What a pointer to `volatile` reaches: through the pointer, by the
    // instruction; at an address the text writes, by its name — see
    // docs/decisions/0151-volatile.md.
    if ( mTyping.expression( node ).isVolatile )
    {
      if ( place.kind == Reach::Kind::INDIRECT )
      {
        place.isVolatile = true;
      }
      else if ( !place.name.empty() )
      {
        mVolatileNames.insert( std::string{ baseName( place.name ) } );
      }
    }
    return place;
  }

  /// The object a name of the text is a byte of: `t` of `t+2`.
  static std::string_view baseName( std::string_view name )
  {
    std::size_t const offset = name.find_first_of( "+-" );
    return offset == std::string_view::npos ? name : name.substr( 0, offset );
  }

  Reach reachOf( Expression const& node, Site const& site )
  {
    Typed const kind = mTyping.expression( node );
    ir::Type const type = kind.type.value_or( ir::Type::U8 );
    std::uint32_t const size = mTyping.bytes( type, kind.aggregate );
    if ( node.kind == ExpressionKind::IDENTIFIER )
    {
      Meaning const meaning = mTyping.lookup( node ).value_or( Meaning{} );
      return Reach{ .kind = Reach::Kind::OBJECT,
                    .type = type,
                    .name = nameOf( meaning, node.token, site ),
                    .pointer = {},
                    .index = {} };
    }
    if ( node.kind == ExpressionKind::CALL )
    {
      call( node, site, false );
      // A Proc of the assembler returns in the byte its `.declare ret` names.
      Meaning const callee = mTyping.lookup( *node.left ).value_or( Meaning{} );
      std::string const returned =
          callee.kind == NameKind::ASSEMBLER_PROC && callee.external != nullptr && callee.external->result.has_value()
              ? callee.external->result->name
              : mTyping.spellingOf( node.left->token ) + "." + std::string{ RESULT };
      return Reach{ .kind = Reach::Kind::OBJECT, .type = type, .name = returned, .pointer = {}, .index = {} };
    }
    if ( node.kind == ExpressionKind::MEMBER )
    {
      return member( node, site );
    }
    Expression const* const index = node.kind == ExpressionKind::INDEX ? node.right.get() : nullptr;
    std::optional<Meaning> const array =
        node.kind == ExpressionKind::INDEX && node.left->kind == ExpressionKind::IDENTIFIER
            ? mTyping.lookup( *node.left )
            : std::optional<Meaning>{};

    if ( array.has_value() && array->isStriped )
    {
      Typed const indexKind = mTyping.expression( *index );
      ir::Operand stripedIndex =
          isKnown( indexKind ) ? ir::Operand{ ir::Constant{
                                     .type = ir::Type::U8, .value = mTyping.fold( *index, std::nullopt ), .name = {} } }
                               : operand( *index, ir::Type::U8, site );
      ir::Type const elementType = array->aggregate.has_value() ? ir::Type::BLOCK : array->type;
      return Reach{ .kind = Reach::Kind::STRIPED,
                    .type = type,
                    .name = {},
                    .pointer = {},
                    .index = std::move( stripedIndex ),
                    .scaled = true,
                    .isArray = false,
                    .array = nameOf( *array, node.left->token, site ),
                    .paths = stripePaths( elementType, array->aggregate ),
                    .offset = 0 };
    }

    ir::Operand base;
    if ( array.has_value() && array->isArray )
    {
      std::string const name = nameOf( *array, node.left->token, site );
      Typed const indexKind = mTyping.expression( *index );
      bool const reachedByX = indexKind.type == ir::Type::U8 && !isKnown( indexKind ) &&
                              ( size == 1 || ( array->count.has_value() && *array->count * size <= 256 ) );
      if ( reachedByX && size <= 2 && !kind.aggregate.has_value() )
      {
        return Reach{ .kind = Reach::Kind::INDEXED,
                      .type = type,
                      .name = name,
                      .pointer = {},
                      .index = operand( *index, ir::Type::U8, site ) };
      }
      if ( reachedByX )
      {
        // `X` holds the index times the size, which the array's bytes keep
        // below 256.
        return Reach{ .kind = Reach::Kind::INDEXED,
                      .type = type,
                      .name = name,
                      .pointer = {},
                      .index = multiplied( operand( *index, ir::Type::U8, site ), size, ir::Type::U8, site ),
                      .scaled = true };
      }
      base = ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = name };
    }
    else
    {
      base = pointerOperand( *node.left, site );
    }

    std::optional<std::int64_t> known;
    Typed indexKind = constant();
    if ( index == nullptr )
    {
      known = 0;
    }
    else if ( indexKind = mTyping.expression( *index ); isKnown( indexKind ) )
    {
      known = mTyping.fold( *index, std::nullopt ) * size;
    }

    if ( auto const* constant = std::get_if<ir::Constant>( &base ) )
    {
      if ( known.has_value() )
      {
        return Reach{
          .kind = Reach::Kind::OBJECT, .type = type, .name = offsetName( *constant, *known ), .pointer = {}, .index = {}
        };
      }
      if ( indexKind.type == ir::Type::U8 && size == 1 )
      {
        return Reach{ .kind = Reach::Kind::INDEXED,
                      .type = type,
                      .name = offsetName( *constant, 0 ),
                      .pointer = {},
                      .index = operand( *index, ir::Type::U8, site ) };
      }
    }
    else if ( known.has_value() && *known >= 0 && *known + size <= 256 )
    {
      return Reach{ .kind = Reach::Kind::INDIRECT,
                    .type = type,
                    .name = {},
                    .pointer = std::move( base ),
                    .index = ir::Constant{ .type = ir::Type::U8, .value = *known, .name = {} } };
    }
    else if ( !known.has_value() && indexKind.type == ir::Type::U8 && size == 1 )
    {
      return Reach{ .kind = Reach::Kind::INDIRECT,
                    .type = type,
                    .name = {},
                    .pointer = std::move( base ),
                    .index = operand( *index, ir::Type::U8, site ) };
    }

    ir::Operand const offset =
        known.has_value() ? ir::Operand{ ir::Constant{
                                .type = ir::Type::POINTER, .value = wrapped( *known, ir::Type::POINTER ), .name = {} } }
                          : scaled( *index, size, site );
    ir::Value const address =
        binaryValue( ir::BinaryOperator::ADD, ir::Type::POINTER, std::move( base ), offset, site );
    return Reach{ .kind = Reach::Kind::INDIRECT,
                  .type = type,
                  .name = {},
                  .pointer = address,
                  .index = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} } };
  }

  static ir::Operand read( Reach const& place, Site const& site )
  {
    if ( place.kind == Reach::Kind::OBJECT )
    {
      return ir::Object{ .name = place.name, .type = place.type };
    }
    if ( place.kind == Reach::Kind::STRIPED )
    {
      std::string const stripe = stripeOf( place, place.offset );
      auto const* constant = std::get_if<ir::Constant>( &place.index );
      if ( constant != nullptr && ir::sizeOf( place.type ) == 1 )
      {
        return ir::Object{ .name = offsetName( ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = stripe },
                                               constant->value ),
                           .type = place.type };
      }
      ir::Value const result = define( site, place.type );
      site.blocks->append( ir::Instruction{
          .operation = ir::Load{ .result = result,
                                 .type = place.type,
                                 .name = stripe,
                                 .index = place.index,
                                 .scaled = true,
                                 .high = ir::sizeOf( place.type ) == 2 ? stripeOf( place, place.offset + 1 ) : "" },
          .at = site.at } );
      return result;
    }
    ir::Value const result = define( site, place.type );
    if ( place.kind == Reach::Kind::INDEXED )
    {
      site.blocks->append( ir::Instruction{ .operation = ir::Load{ .result = result,
                                                                   .type = place.type,
                                                                   .name = place.name,
                                                                   .index = place.index,
                                                                   .scaled = place.scaled },
                                            .at = site.at } );
      return result;
    }
    site.blocks->append( ir::Instruction{ .operation = ir::LoadIndirect{ .result = result,
                                                                         .type = place.type,
                                                                         .pointer = place.pointer,
                                                                         .index = place.index,
                                                                         .isVolatile = place.isVolatile },
                                          .at = site.at } );
    return result;
  }

  static void write( Reach const& place, ir::Operand value, Site const& site )
  {
    switch ( place.kind )
    {
    case Reach::Kind::STRIPED:
    {
      std::string const stripe = stripeOf( place, place.offset );
      auto const* constant = std::get_if<ir::Constant>( &place.index );
      if ( constant != nullptr && ir::sizeOf( place.type ) == 1 )
      {
        site.blocks->append( ir::Instruction{
            .operation =
                ir::Store{ .name = offsetName( ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = stripe },
                                               constant->value ),
                           .type = place.type,
                           .value = std::move( value ) },
            .at = site.at } );
        return;
      }
      site.blocks->append( ir::Instruction{
          .operation =
              ir::StoreElement{ .name = stripe,
                                .type = place.type,
                                .index = place.index,
                                .value = std::move( value ),
                                .scaled = true,
                                .high = ir::sizeOf( place.type ) == 2 ? stripeOf( place, place.offset + 1 ) : "" },
          .at = site.at } );
      return;
    }
    case Reach::Kind::OBJECT:
      site.blocks->append( ir::Instruction{
          .operation = ir::Store{ .name = place.name, .type = place.type, .value = std::move( value ) },
          .at = site.at } );
      return;
    case Reach::Kind::INDEXED:
      site.blocks->append( ir::Instruction{ .operation = ir::StoreElement{ .name = place.name,
                                                                           .type = place.type,
                                                                           .index = place.index,
                                                                           .value = std::move( value ),
                                                                           .scaled = place.scaled },
                                            .at = site.at } );
      return;
    case Reach::Kind::INDIRECT:
      site.blocks->append( ir::Instruction{ .operation = ir::StoreIndirect{ .type = place.type,
                                                                            .pointer = place.pointer,
                                                                            .index = place.index,
                                                                            .value = std::move( value ),
                                                                            .isVolatile = place.isVolatile },
                                            .at = site.at } );
      return;
    }
  }

  /// An operand in `type`: a constant held in it, an object read as its low
  /// bytes where it narrows, and anything else converted by an instruction —
  /// see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md.
  static ir::Operand converted( ir::Operand value, ir::Type type, Site const& site )
  {
    ir::Type const source = ir::typeOf( value, *site.function );
    if ( source == type )
    {
      return value;
    }
    if ( auto const* constant = std::get_if<ir::Constant>( &value ) )
    {
      // A constant only the assembler knows keeps its name, narrowed to its
      // low byte where the type is one byte.
      std::string name = constant->name;
      if ( !name.empty() && ir::sizeOf( type ) < ir::sizeOf( source ) )
      {
        name = "<(" + name + ")";
      }
      return ir::Constant{ .type = type, .value = wrapped( constant->value, type ), .name = std::move( name ) };
    }
    if ( auto const* object = std::get_if<ir::Object>( &value );
         object != nullptr && ir::sizeOf( type ) <= ir::sizeOf( source ) )
    {
      return ir::Object{ .name = object->name, .type = type };
    }
    ir::Value const result = define( site, type );
    site.blocks->append( ir::Instruction{
        .operation = ir::Convert{ .result = result, .type = type, .operand = std::move( value ) }, .at = site.at } );
    return result;
  }

  ir::Operand comparison( Expression const& node, Site const& site )
  {
    // Compared in the wider of the two types, a constant taking the other's.
    std::optional<ir::Type> const leftType = mTyping.expression( *node.left ).type;
    std::optional<ir::Type> const rightType = mTyping.expression( *node.right ).type;
    ir::Type const type = leftType.has_value() && rightType.has_value()
                              ? widerOf( *leftType, *rightType )
                              : leftType.value_or( rightType.value_or( ir::Type::U8 ) );
    ir::Operand left = converted( operand( *node.left, type, site ), type, site );
    ir::Operand right = converted( operand( *node.right, type, site ), type, site );

    ir::Comparison op = ir::Comparison::EQUAL;
    bool swapped = false;
    switch ( node.token.kind )
    {
    case TokenKind::BANG_EQUAL:
      op = ir::Comparison::NOT_EQUAL;
      break;
    case TokenKind::LESS:
      op = ir::Comparison::LESS;
      break;
    case TokenKind::GREATER:
      op = ir::Comparison::LESS;
      swapped = true;
      break;
    case TokenKind::LESS_EQUAL:
      op = ir::Comparison::GREATER_OR_EQUAL;
      swapped = true;
      break;
    case TokenKind::GREATER_EQUAL:
      op = ir::Comparison::GREATER_OR_EQUAL;
      break;
    default:
      break;
    }
    if ( swapped )
    {
      std::swap( left, right );
    }

    ir::Value const result = define( site, ir::Type::BOOL );
    site.blocks->append( ir::Instruction{
        .operation =
            ir::Compare{
                .result = result, .op = op, .type = type, .left = std::move( left ), .right = std::move( right ) },
        .at = site.at } );
    return result;
  }

  diag::SourceManager const* mSources;
  Unit const* mUnit;
  diag::DiagnosticSink* mSink;
  Typing mTyping;
  std::vector<Loop> mLoops;

  /// The Pane the function being lowered is in, or empty, and the `[[with]]`
  /// blocks the statement being lowered stands in, outermost first — see
  /// docs/decisions/0096-panes-in-c.md.
  std::string mPane;
  std::string mUnder;

  /// The function type whose Temporaries the function being lowered reads,
  /// or empty — see docs/decisions/0065-handlers.md.
  std::string mMemberOf;
  std::vector<Shown> mShown;

  /// The `[[with]]` blocks of the file so far, which number their macros
  /// and their Trampolines.
  std::uint32_t mWiths = 0;

  /// The function being lowered, which a case of a `switch` in it names its
  /// parameters and its result through, and what it returns.
  std::string mOwner;
  std::optional<NamedType> mResult;

  /// Per Proc being lowered: how many locals it has taken a byte for, which
  /// numbers the next, and how many bytes a `&&` or an `||` has needed.
  /// The classes in force, the function's at the bottom and a block's above
  /// it: what the program names takes the innermost, and what the compiler
  /// takes for itself the function's — see
  /// docs/decisions/0210-placement-is-declared-in-c-too.md.
  std::vector<model::PlacementClass> mPlacement{ model::PlacementClass::ZEROPAGE };

  std::vector<std::uint32_t> mLocals;
  std::vector<std::uint32_t> mConditions;

  /// Per Proc being lowered: how many bytes have kept an index into a striped
  /// array that more than one instruction reads, and how many have kept the
  /// value a `switch` written where it stands tests — see
  /// docs/decisions/0164-a-small-switch-stands-where-it-is.md.
  std::vector<std::uint32_t> mIndexes;
  std::vector<std::uint32_t> mTested;

  /// The Procs the `switch`es of the function being lowered run.
  std::vector<ir::Function> mExtra;

  /// The `static` locals of the function being lowered, its cases' included.
  std::vector<ir::Global> mSections;

  /// The Sections of the file's string literals, by their characters.
  std::vector<ir::Global> mLiterals;
  std::map<std::string, std::string> mLiteralNames;

  /// Where each local whose address is taken is declared.
  std::set<std::uint32_t> const* mAddressed;

  /// The names of what is `volatile`, as the lowered unit names them — see
  /// docs/decisions/0151-volatile.md.
  std::set<std::string> mVolatileNames;

  /// Which function type each member belongs to, gathered while the program
  /// was checked — see docs/decisions/0065-handlers.md.
  Membership const* mMembers = nullptr;
};

bool isValue( ir::Operand const& operand, ir::Value value )
{
  auto const* found = std::get_if<ir::Value>( &operand );
  return found != nullptr && found->index == value.index;
}

std::uint32_t readersOf( ir::Function const& function, ir::Value value );

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

  /// The local the counter is, and what it starts at.
  std::string counter;
  std::int64_t from = 0;

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

  /// The number a `!=` test holds the counter apart from, where it is one.
  std::optional<std::int64_t> apartFrom{};
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
  if ( compare == nullptr || ir::sizeOf( compare->type ) != 1 || ir::isSigned( compare->type ) )
  {
    return std::nullopt;
  }
  auto const* const object = std::get_if<ir::Object>( &compare->left );
  if ( object == nullptr )
  {
    return std::nullopt;
  }
  bool const ordered = compare->op == ir::Comparison::LESS || compare->op == ir::Comparison::GREATER_OR_EQUAL;
  if ( std::holds_alternative<ir::Value>( compare->right ) && !ordered )
  {
    return std::nullopt;
  }

  // What the counter is held below, where the loop runs while it is: the
  // branch on `<` goes into the loop where it holds, and on `>=` where it does
  // not, and either way the body runs with the counter below the bound.
  std::optional<std::int64_t> atMost;
  if ( ordered )
  {
    auto const* const bound = std::get_if<ir::Constant>( &compare->right );
    atMost = bound != nullptr && bound->name.empty() ? bound->value - 1 : 254;
  }
  std::optional<std::int64_t> apartFrom;
  if ( auto const* const bound = std::get_if<ir::Constant>( &compare->right );
       compare->op == ir::Comparison::NOT_EQUAL && bound != nullptr && bound->name.empty() )
  {
    apartFrom = bound->value;
  }
  return CounterTest{ .counter = object->name, .atMost = atMost, .apartFrom = apartFrom };
}

/// Whether the block steps the counter by one and does nothing else.
bool stepsByOne( ir::Block const& block, std::string const& counter )
{
  if ( block.instructions.size() != 2 )
  {
    return false;
  }
  auto const* const add = std::get_if<ir::Binary>( &block.instructions[0].operation );
  auto const* const store = std::get_if<ir::Store>( &block.instructions[1].operation );
  if ( add == nullptr || store == nullptr || add->op != ir::BinaryOperator::ADD || ir::sizeOf( add->type ) != 1 ||
       !isLocal( add->left, counter ) )
  {
    return false;
  }
  auto const* const by = std::get_if<ir::Constant>( &add->right );
  return by != nullptr && by->value == 1 && store->name == counter &&
         std::holds_alternative<ir::Value>( store->value ) &&
         std::get<ir::Value>( store->value ).index == add->result.index;
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
  return indexes > 0 && indexes == readersOf( function, value );
}

std::vector<CountedLoop> countedLoopsOf( ir::Function const& function, std::set<std::string> const& volatiles )
{
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
    if ( !test.has_value() || !stepsByOne( block, test->counter ) )
    {
      continue;
    }
    // A `volatile` counter is stepped in its byte at every turn — see
    // docs/decisions/0151-volatile.md.
    std::string const& counter = test->counter;
    if ( volatiles.contains( counter ) )
    {
      continue;
    }
    std::optional<std::int64_t> const from = startsAt( function.blocks[header - 1], counter, header );
    if ( !from.has_value() )
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
    if ( safe && exit > index )
    {
      found.push_back( CountedLoop{ .header = header,
                                    .step = index,
                                    .preheader = header - 1,
                                    .exit = exit,
                                    .counter = counter,
                                    .from = *from,
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
       readersOf( function, load->result ) != 1 || readersOf( function, binary->result ) != 1 )
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
  if ( !kind.has_value() || readersOf( function, binary->result ) != 1 )
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

/// How many operands of the function read the value.
std::uint32_t readersOf( ir::Function const& function, ir::Value value )
{
  std::uint32_t readers = 0;
  for ( ir::Block const& each : function.blocks )
  {
    for ( ir::Instruction const& instruction : each.instructions )
    {
      std::visit(
          [&]( auto const& operation )
          {
            using Operation = std::decay_t<decltype( operation )>;
            auto const reads = [&]( ir::Operand const& operand )
            {
              auto const* const other = std::get_if<ir::Value>( &operand );
              readers += other != nullptr && other->index == value.index ? 1 : 0;
            };
            if constexpr ( std::is_same_v<Operation, ir::Store> || std::is_same_v<Operation, ir::Switch> )
            {
              reads( operation.value );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Unary> || std::is_same_v<Operation, ir::Convert> )
            {
              reads( operation.operand );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Binary> || std::is_same_v<Operation, ir::Compare> )
            {
              reads( operation.left );
              reads( operation.right );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Load> )
            {
              reads( operation.index );
            }
            else if constexpr ( std::is_same_v<Operation, ir::StoreElement> )
            {
              reads( operation.index );
              reads( operation.value );
            }
            else if constexpr ( std::is_same_v<Operation, ir::LoadIndirect> )
            {
              reads( operation.pointer );
              reads( operation.index );
            }
            else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> )
            {
              reads( operation.pointer );
              reads( operation.index );
              reads( operation.value );
            }
            else if constexpr ( std::is_same_v<Operation, ir::Call> )
            {
              for ( ir::Argument const& argument : operation.arguments )
              {
                reads( argument.value );
              }
            }
            else if constexpr ( std::is_same_v<Operation, ir::EnterWith> )
            {
              if ( operation.index.has_value() )
              {
                reads( *operation.index );
              }
            }
          },
          instruction.operation );
    }
    if ( each.terminator.kind == ir::TerminatorKind::BRANCH )
    {
      auto const* const other = std::get_if<ir::Value>( &each.terminator.condition );
      readers += other != nullptr && other->index == value.index ? 1 : 0;
    }
  }
  return readers;
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

/// The fourth pass: the text a file's IR is emitted as, a definition at a
/// time.
class Writer
{
public:
  Writer( diag::SourceManager const& sources,
          std::string_view sourcePath,
          Cpu cpu = Cpu::MOS6502,
          Intent intent = Intent::FIT )
      : mSources( &sources ), mSourcePath( quoted( sourcePath ) ), mCpu( cpu ), mIntent( intent )
  {
  }

  /// What a function's text says of where its parameters and its result
  /// could be carried: its body with the stores to a byte a register carries
  /// taken out, and what `A` held at each `rts` — see
  /// docs/decisions/0145-an-argument-in-a-register.md.
  struct PlaceFacts
  {
    std::string body;
    std::vector<std::vector<std::string>> returns;
  };

  [[nodiscard]] PlaceFacts placeFacts( ir::Function const& lowered, std::set<std::string> const& volatiles )
  {
    mVolatiles = volatiles;
    function( lowered );
    return PlaceFacts{ .body = std::move( mBody ), .returns = std::move( mReturnHolds ) };
  }

  /// Whether every line of the body that names its own byte stores to it: a
  /// name another Proc's, `bits8.__ret`, is that Proc's byte and not this one.
  [[nodiscard]] static bool onlyStoredIn( std::string const& body, std::string const& name )
  {
    return onlyStoredTo( ownLines( body, name ), name );
  }

  /// Whether some line of the body names its own byte.
  [[nodiscard]] static bool namedIn( std::string const& body, std::string const& name )
  {
    return namesByte( ownLines( body, name ), name );
  }

  /// The body with every name that is `name` qualified by another Proc's
  /// blanked out, so that only its own byte is found.
  [[nodiscard]] static std::string ownLines( std::string body, std::string const& name )
  {
    for ( std::size_t at = body.find( "." + name ); at != std::string::npos; at = body.find( "." + name, at + 1 ) )
    {
      std::size_t begin = at;
      while ( begin > 0 &&
              ( std::isalnum( static_cast<unsigned char>( body[begin - 1] ) ) != 0 || body[begin - 1] == '_' ) )
      {
        --begin;
      }
      if ( begin == at )
      {
        continue;
      }
      std::fill( body.begin() + static_cast<std::ptrdiff_t>( begin ),
                 body.begin() + static_cast<std::ptrdiff_t>( at + 1 + name.size() ),
                 ' ' );
    }
    return body;
  }

  [[nodiscard]] std::string unit( ir::Unit const& lowered )
  {
    mVolatiles = lowered.volatiles;
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
      function( std::get<ir::Function>( definition ) );
    }
    dropUnnamedBytes();
    return std::move( mText );
  }

private:
  /// The path as the assembler's string literal spells it.
  static std::string quoted( std::string_view path )
  {
    std::string out = "\"";
    for ( char const c : path )
    {
      if ( c == '"' || c == '\\' )
      {
        out += '\\';
      }
      out += c;
    }
    return out + "\"";
  }

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
    std::string const written = transferred( text );
    if ( !noteLine( written ) )
    {
      return;
    }
    mText.append( INDENT ).append( written ).append( "\n" );
  }

  void label( std::string_view name )
  {
    forgetA();
    mText.append( name ).append( "\n" );
  }

  /// What `A` holds, by every name the text has for it: the operand of the
  /// `lda` that filled it, an immediate included, and every byte a `sta` has
  /// since written it to. Kept by reading the emitter's own lines, so that no
  /// path that writes `A` or a byte can be missed — the lesson of
  /// docs/decisions/0109-an-induction-variable-in-x.md. See
  /// docs/decisions/0110-a-value-stays-in-a.md.
  std::vector<std::string> mAHolds;

  /// What `X` holds, by every name the text has for it, kept the same way and
  /// forgotten at every label: a `ldx` of one of those names is written as
  /// nothing. See docs/decisions/0136-a-byte-x-holds-is-not-loaded-again.md.
  std::vector<std::string> mXHolds;

  /// And what `Y` holds, the same way: above all the `#0` of `ldy #0` before
  /// `lda (p),y`. See docs/decisions/0141-what-y-holds.md.
  std::vector<std::string> mYHolds;

  /// The scalar bytes of the Proc being written, by the names its text gives
  /// them, which a store through a pointer does not reach.
  std::set<std::string> mOwnScalars;

  /// The unit's names that change or act behind the program's back, which no
  /// register is said to hold and no access to which is moved, merged or
  /// dropped — see docs/decisions/0151-volatile.md.
  std::set<std::string> mVolatiles;

  /// The instruction of `mBlock` being written, by its place in the block.
  std::size_t mAt = 0;

  /// The bytes whose bit 7 the carry holds, as the `asl` that pushed it out of
  /// `A` left it.
  std::vector<std::string> mCarryBit7;

  /// What `A` held at each `rts` of the function being written, and its body
  /// as written, for a pass that asks where its result and its parameters
  /// could be carried — see docs/decisions/0145-an-argument-in-a-register.md.
  std::vector<std::vector<std::string>> mReturnHolds;
  std::string mBody;

  /// What the carry is, as far as the lines written say: `clc` and `sec` set
  /// it, an add, a subtract, a compare or a shift leave it as the machine
  /// decides, and a branch on it says what it is on each way out. A `clc`
  /// where it is known clear, or a `sec` where it is known set, is not
  /// written. See docs/decisions/0113-a-carry-known-is-not-set-again.md.
  enum class Carry : std::uint8_t
  {
    UNKNOWN,
    CLEAR,
    SET,
  };
  Carry mCarry = Carry::UNKNOWN;

  /// Whether `N` and `Z` say what `A` holds, as far as the lines written say:
  /// set by what loads or computes `A`, left by a store, a flag's own
  /// instruction or a branch, and lost to anything that sets them from
  /// something else — `ldx`, `inx`, `cmp`, `bit`, a shift of memory, a `jsr`.
  /// Kept apart from what `A` holds, since a `lda` left out because `A`
  /// already held the byte leaves the flags as whatever set them last. See
  /// docs/decisions/0130-a-test-for-zero-the-flags-already-answer.md.
  bool mFlagsFromA = false;

  /// The same for `X`: set by what loads or steps it, so that `inx` and a
  /// test of the counter against zero is `inx / jne` — see
  /// docs/decisions/0135-a-loop-run-up-to-its-wrap.md.
  bool mFlagsFromX = false;

  /// What the carry is on each way out of a block: where its branch is taken,
  /// and where it falls through or jumps. One and the same but for a branch
  /// on the carry itself, which is what makes them two — and then `jumpsTo`
  /// says which block the branch went to, since the text branches to
  /// whichever of the two successors is not written next, and that is not
  /// always the one the IR calls the target.
  struct EdgeCarry
  {
    std::optional<std::uint32_t> jumpsTo{};
    Carry taken = Carry::UNKNOWN;
    Carry fallen = Carry::UNKNOWN;
  };

  /// What the last branch written said of the carry on each way out of the
  /// block, where it was a branch on the carry; nothing otherwise.
  std::optional<EdgeCarry> mLastBranch;

  /// What `A` holds as each block is entered, once the trial run has said what
  /// it holds at the end of every block that leads there: the names every
  /// predecessor agrees on. Empty for the first block, and in the trial itself.
  std::vector<std::vector<std::string>> mBlockEntry;
  std::vector<Carry> mBlockEntryCarry;

  /// What `A` held, and what the carry was, when each block ended, recorded
  /// by the trial run.
  std::vector<std::vector<std::string>> mBlockEnd;
  std::vector<EdgeCarry> mBlockEndCarry;

  /// A run whose text is thrown away: it exists to fill `mBlockEnd`.
  bool mTrial = false;

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

  void forgetA()
  {
    mAHolds.clear();
    mXHolds.clear();
    mYHolds.clear();
    mCarryBit7.clear();
    mCarry = Carry::UNKNOWN;
    mFlagsFromA = false;
    mFlagsFromX = false;
  }

  /// The Jcc that is taken wherever it stands, or empty where nothing the
  /// lines written say makes one certain.
  ///
  /// A jump whose condition is known to hold **is** a jump, and a branch is a
  /// byte shorter for the same three cycles — one more only where it crosses
  /// a page, which is the trade docs/decisions/0077-a-jcc-is-two-bytes-or-five.md
  /// already makes for every conditional and nobody calls a cost. See
  /// docs/decisions/0181-a-jump-whose-flag-is-known.md.
  [[nodiscard]] std::string_view alwaysTaken() const
  {
    // Where the processor has `bra` there is no flag to know: every jump is a
    // branch where it reaches, and the tracking below is the 6502's business
    // alone — see docs/decisions/0187-a-jump-that-asks-no-flag.md.
    if ( mCpu == Cpu::WDC65SC02 )
    {
      return "jra";
    }
    if ( mFlagsFromA )
    {
      for ( std::string const& name : mAHolds )
      {
        // A literal alone: `#<label` and the like name a value Place settles,
        // and the flags cannot be read off text the assembler has not folded.
        if ( name.size() < 2 || name.front() != '#' )
        {
          continue;
        }
        std::string_view digits{ name };
        digits.remove_prefix( 1 );
        int base = 10;
        if ( digits.front() == '$' )
        {
          digits.remove_prefix( 1 );
          base = 16;
        }
        if ( digits.empty() ||
             !std::ranges::all_of(
                 digits, [base]( char c ) { return base == 16 ? std::isxdigit( c ) != 0 : std::isdigit( c ) != 0; } ) )
        {
          continue;
        }
        unsigned long const value = std::stoul( std::string{ digits }, nullptr, base );
        return ( value & 0xFFU ) == 0 ? "jmpeq" : "jmpne";
      }
    }
    if ( mCarry == Carry::SET )
    {
      return "jmpcs";
    }
    if ( mCarry == Carry::CLEAR )
    {
      return "jmpcc";
    }
    return {};
  }

  /// Keeps `mFlagsFromA` true for one line that is written.
  void noteFlags( std::string_view mnemonic, std::string_view operand )
  {
    static constexpr std::array<std::string_view, 11> FROM_A{ "lda", "and", "ora", "eor", "adc", "sbc",
                                                              "pla", "txa", "tya", "tax", "tay" };
    static constexpr std::array<std::string_view, 35> LEAVES_FLAGS{
      "sta", "stx", "sty", "clc", "sec", "clv",   "cld",   "sed",   "cli",   "sei", "nop", "pha",
      "php", "jmp", "bcc", "bcs", "beq", "bne",   "bmi",   "bpl",   "bvc",   "bvs", "jcc", "jcs",
      "jeq", "jne", "jmi", "jpl", "jvc", "jmpcc", "jmpcs", "jmpeq", "jmpne", "jra",
    };
    auto const among = []( auto const& list, std::string_view what )
    { return std::ranges::find( list, what ) != list.end(); };
    bool const shiftsA = operand.empty() && ( mnemonic == "asl" || mnemonic == "lsr" || mnemonic == "rol" ||
                                              mnemonic == "ror" || mnemonic == "inc" || mnemonic == "dec" );
    static constexpr std::array<std::string_view, 5> FROM_X{ "ldx", "inx", "dex", "tax", "tsx" };
    bool const leaves = among( LEAVES_FLAGS, mnemonic ) || mnemonic == "jvs";
    if ( among( FROM_A, mnemonic ) || shiftsA )
    {
      mFlagsFromA = true;
    }
    else if ( !leaves )
    {
      mFlagsFromA = false;
    }
    // `tax` sets them from the byte both now hold.
    if ( among( FROM_X, mnemonic ) )
    {
      mFlagsFromX = true;
      mFlagsFromA = mnemonic == "tax";
    }
    else if ( !leaves )
    {
      mFlagsFromX = false;
    }
  }

  /// An operand that names one byte, or an immediate: something `A` can be
  /// said to hold. An indexed or indirect operand names a byte only at run
  /// time.
  static bool isOneByte( std::string_view operand )
  {
    return operand.find_first_of( ",(" ) == std::string_view::npos;
  }

  /// The array a store or a load through an index names — `t` of `t+1,x` or
  /// `t-4,y` — where the operand is one: nothing for a byte, and nothing for
  /// a pointer, `(p),y`, which may be aimed anywhere.
  static std::optional<std::string_view> indexedBase( std::string_view operand )
  {
    std::size_t const comma = operand.find( ',' );
    if ( comma == std::string_view::npos || operand.contains( '(' ) )
    {
      return std::nullopt;
    }
    return byteBase( operand.substr( 0, comma ) );
  }

  /// The object a byte of it names: `t` of `t+2`.
  static std::string_view byteBase( std::string_view operand )
  {
    std::size_t const offset = operand.find_first_of( "+-" );
    return offset == std::string_view::npos ? operand : operand.substr( 0, offset );
  }

  /// Forgets what a store to `operand` may have written of what a register
  /// was said to hold: that byte, and an element of its array read through an
  /// index; for a store through an index, whatever of that array was held,
  /// since an element's index stays within its array — see
  /// docs/decisions/0149-a-store-to-an-element-stays-in-its-array.md; and for
  /// a store through a pointer, every byte but a scalar of the Proc's own,
  /// which no pointer is aimed at — see
  /// docs/decisions/0150-a-pointer-does-not-reach-the-procs-own-scalars.md.
  void forgetWritten( std::vector<std::string>& held, std::string_view operand ) const
  {
    auto const baseOfHeld = []( std::string const& name ) -> std::string_view
    {
      std::optional<std::string_view> const indexed = indexedBase( name );
      return indexed.has_value() ? *indexed : byteBase( name );
    };
    if ( isOneByte( operand ) )
    {
      std::string_view const base = byteBase( operand );
      std::erase_if( held,
                     [&]( std::string const& name )
                     { return name == operand || ( indexedBase( name ).has_value() && baseOfHeld( name ) == base ); } );
      return;
    }
    if ( std::optional<std::string_view> const base = indexedBase( operand ); base.has_value() )
    {
      std::erase_if(
          held, [&]( std::string const& name ) { return !name.starts_with( '#' ) && baseOfHeld( name ) == *base; } );
      return;
    }
    std::erase_if( held,
                   [this]( std::string const& name )
                   { return !name.starts_with( '#' ) && !ownScalar( std::string{ byteBase( name ) } ); } );
  }

  /// Whether a name is a scalar byte, or pair, of the Proc being written: a
  /// parameter, the result, a local of a scalar type, or a scratch byte. C
  /// takes the address of none of them — a parameter's is refused, and a
  /// local whose address is taken is a Section of its own — and the
  /// assembler may not count on them, so no pointer is aimed at one.
  [[nodiscard]] bool ownScalar( std::string const& name ) const
  {
    bool const scratch = name.starts_with( "__t" ) && name.size() > 3 &&
                         std::ranges::all_of( name.substr( 3 ), []( char c ) { return c >= '0' && c <= '9'; } );
    return scratch || mOwnScalars.contains( name );
  }

  /// Whether an operand, or an object's name, is of a volatile object: by the
  /// name of what it is a byte or an element of.
  [[nodiscard]] bool isVolatile( std::string_view operand ) const
  {
    if ( mVolatiles.empty() )
    {
      return false;
    }
    std::optional<std::string_view> const indexed = indexedBase( operand );
    std::string_view const base = indexed.has_value() ? *indexed : byteBase( operand );
    return mVolatiles.contains( std::string{ base } );
  }

  [[nodiscard]] bool holdsInA( std::string const& name ) const
  {
    return std::ranges::find( mAHolds, name ) != mAHolds.end();
  }

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

  /// The number an operand is, where it is one the tracker may step: `#7` is
  /// 7, and a name, an address or anything else is nothing.
  [[nodiscard]] static std::optional<int> numberIn( std::string_view operand )
  {
    if ( !operand.starts_with( '#' ) || operand.size() < 2 )
    {
      return std::nullopt;
    }
    std::string_view const digits = operand.substr( 1 );
    int number = 0;
    auto const [end, error] = std::from_chars( digits.data(), digits.data() + digits.size(), number );
    if ( error != std::errc{} || end != digits.data() + digits.size() || number < 0 || number > 255 )
    {
      return std::nullopt;
    }
    return number;
  }

  /// A load an index register is one step away from, written as that step:
  /// `dey` where `Y` holds `#1` and `#0` is wanted, which is one byte where
  /// the load is two and the same two cycles, and sets `N` and `Z` from the
  /// same number the load would. One step only — two would be two bytes
  /// again, and four cycles. `A` has no such step on a 6502, and on a
  /// 65SC02 it is
  /// docs/decisions/0184-one-more-in-the-accumulator.md's. See
  /// docs/decisions/0199-a-byte-another-register-holds-is-transferred.md.
  [[nodiscard]] static std::string_view
  steppedTo( char const reg, std::vector<std::string> const& held, std::optional<int> const wanted )
  {
    if ( !wanted.has_value() )
    {
      return {};
    }
    for ( std::string const& name : held )
    {
      std::optional<int> const number = numberIn( name );
      if ( !number.has_value() )
      {
        continue;
      }
      if ( ( *number + 1 ) % 256 == *wanted )
      {
        return reg == 'x' ? "inx" : "iny";
      }
      if ( ( *number + 255 ) % 256 == *wanted )
      {
        return reg == 'x' ? "dex" : "dey";
      }
    }
    return {};
  }

  /// A load of a byte another register already holds, written as the transfer
  /// that puts it there: one byte and two cycles, where the shortest load is
  /// two and two. Read from what the registers hold, before the line is noted,
  /// so that no path that writes one can be missed — the lesson of
  /// docs/decisions/0110-a-value-stays-in-a.md. Only through `A`: the
  /// processor has no `txy` or `tyx`, and the way round through `A` costs two
  /// instructions and `A` itself. Where no register holds the byte, an index
  /// register one step from the number wanted steps to it instead. See
  /// docs/decisions/0199-a-byte-another-register-holds-is-transferred.md.
  [[nodiscard]] std::string transferred( std::string_view text ) const
  {
    std::size_t const space = text.find( ' ' );
    if ( space == std::string_view::npos )
    {
      return std::string{ text };
    }
    std::string_view const mnemonic = text.substr( 0, space );
    std::string_view const operand = text.substr( space + 1 );
    // What a register may be said to hold at all: an indexed or indirect
    // operand names a byte only at run time, and a volatile one is nobody's.
    if ( !isOneByte( operand ) || isVolatile( operand ) )
    {
      return std::string{ text };
    }
    auto const holds = []( std::vector<std::string> const& held, std::string_view what )
    { return std::ranges::find( held, what ) != held.end(); };
    // Where the register itself holds the byte the load is left out
    // altogether, which is cheaper still, so the transfer is for the rest.
    if ( mnemonic == "lda" && !holds( mAHolds, operand ) )
    {
      if ( holds( mXHolds, operand ) )
      {
        return "txa";
      }
      if ( holds( mYHolds, operand ) )
      {
        return "tya";
      }
    }
    if ( mnemonic == "ldx" && !holds( mXHolds, operand ) )
    {
      if ( holds( mAHolds, operand ) )
      {
        return "tax";
      }
      if ( std::string_view const step = steppedTo( 'x', mXHolds, numberIn( operand ) ); !step.empty() )
      {
        return std::string{ step };
      }
    }
    if ( mnemonic == "ldy" && !holds( mYHolds, operand ) )
    {
      if ( holds( mAHolds, operand ) )
      {
        return "tay";
      }
      if ( std::string_view const step = steppedTo( 'y', mYHolds, numberIn( operand ) ); !step.empty() )
      {
        return std::string{ step };
      }
    }
    return std::string{ text };
  }

  /// Reads one line about to be written and keeps `mAHolds` true; false where
  /// the line is a `lda` of what `A` already holds, which is then not written.
  bool noteLine( std::string_view text )
  {
    std::size_t const space = text.find( ' ' );
    std::string_view const mnemonic = text.substr( 0, space );
    std::string_view const operand = space == std::string_view::npos ? std::string_view{} : text.substr( space + 1 );
    std::vector<std::string> const before = mAHolds;
    if ( !noteIndexes( mnemonic, operand ) || !noteLineOf( mnemonic, operand ) )
    {
      return false;
    }
    noteCarryBit( mnemonic, operand, before );
    // An element read through `X` or `Y` is no longer the one the register
    // now picks, once the register moves.
    bool const movesX =
        mnemonic == "ldx" || mnemonic == "inx" || mnemonic == "dex" || mnemonic == "tax" || mnemonic == "tsx";
    bool const movesY = mnemonic == "ldy" || mnemonic == "iny" || mnemonic == "dey" || mnemonic == "tay";
    if ( movesX || movesY )
    {
      std::string_view const suffix = movesX ? ",x" : ",y";
      std::erase_if( mAHolds, [suffix]( std::string const& name ) { return name.ends_with( suffix ); } );
      std::erase_if( mCarryBit7, [suffix]( std::string const& name ) { return name.ends_with( suffix ); } );
    }
    // A transfer fills the register with what `A` holds, which `A` still holds.
    auto const bytes = [this]
    {
      std::vector<std::string> named = mAHolds;
      std::erase_if( named, []( std::string const& name ) { return name.contains( ',' ); } );
      return named;
    };
    if ( mnemonic == "tax" )
    {
      mXHolds = bytes();
    }
    else if ( mnemonic == "tay" )
    {
      mYHolds = bytes();
    }
    // And the other way: `A` then holds every byte the index register holds,
    // where a `lda` would have left it holding one.
    else if ( mnemonic == "txa" )
    {
      mAHolds = mXHolds;
    }
    else if ( mnemonic == "tya" )
    {
      mAHolds = mYHolds;
    }
    noteFlags( mnemonic, operand );
    return true;
  }

  /// Keeps `mCarryBit7` true for one line written: an `asl` of `A` pushes the
  /// top bit of what `A` held into the carry, which then stands for that bit
  /// of each byte `A` held, until something sets the carry otherwise or
  /// writes the byte — see docs/decisions/0148-a-bit-the-carry-already-holds.md.
  void noteCarryBit( std::string_view mnemonic, std::string_view operand, std::vector<std::string> const& before )
  {
    if ( mnemonic == "asl" && operand.empty() )
    {
      mCarryBit7 = before;
      return;
    }
    static constexpr std::array<std::string_view, 46> LEAVES_CARRY{
      "lda", "ldx", "ldy", "sta", "stx", "sty", "tax", "tay", "txa", "tya", "inx",   "iny",   "dex",   "dey",   "inc",
      "dec", "and", "ora", "eor", "bit", "nop", "pha", "php", "pla", "beq", "bne",   "bmi",   "bpl",   "bvc",   "bvs",
      "jeq", "jne", "jmi", "jpl", "jvc", "jvs", "bcc", "bcs", "jcc", "jcs", "jmpcc", "jmpcs", "jmpeq", "jmpne", "jra",
    };
    if ( std::ranges::find( LEAVES_CARRY, mnemonic ) == LEAVES_CARRY.end() )
    {
      mCarryBit7.clear();
      return;
    }
    bool const writes =
        mnemonic == "sta" || mnemonic == "stx" || mnemonic == "sty" || mnemonic == "inc" || mnemonic == "dec";
    if ( writes )
    {
      forgetWritten( mCarryBit7, operand );
    }
  }

  /// Keeps `mXHolds` and `mYHolds` true for one line about to be written;
  /// false where the line is a `ldx` or a `ldy` of what the register already
  /// holds, which is then not written.
  bool noteIndexes( std::string_view mnemonic, std::string_view operand )
  {
    bool const x = noteIndex( 'x', mXHolds, mnemonic, operand );
    bool const y = noteIndex( 'y', mYHolds, mnemonic, operand );
    return x && y;
  }

  /// What one index register holds, kept as `mAHolds` is: the operand of the
  /// load that filled it, what `A` held where a transfer filled it, and every
  /// byte a store of it has since written it to. A number it holds is still
  /// known after the register is stepped, `#0` and `iny` being `#1`, so a
  /// `ldy #1` after them is not written either. See
  /// docs/decisions/0136-a-byte-x-holds-is-not-loaded-again.md and
  /// docs/decisions/0141-what-y-holds.md.
  bool
  noteIndex( char const reg, std::vector<std::string>& held, std::string_view mnemonic, std::string_view operand ) const
  {
    std::string const own{ reg };
    auto const among = []( auto const& list, std::string_view what )
    { return std::ranges::find( list, what ) != list.end(); };
    auto const holds = [&held]( std::string_view what ) { return std::ranges::find( held, what ) != held.end(); };

    if ( mnemonic == "ld" + own )
    {
      bool const holdable = isOneByte( operand ) && !isVolatile( operand );
      if ( holdable && holds( operand ) )
      {
        return false;
      }
      held.clear();
      if ( holdable )
      {
        held.emplace_back( operand );
      }
      return true;
    }
    if ( mnemonic == "in" + own || mnemonic == "de" + own )
    {
      // A number stepped is the next number; a name is no longer what it holds.
      int const by = mnemonic.starts_with( "in" ) ? 1 : 255;
      std::vector<std::string> stepped;
      for ( std::string const& name : held )
      {
        if ( std::optional<int> const number = numberIn( name ); number.has_value() )
        {
          stepped.push_back( "#" + std::to_string( ( *number + by ) % 256 ) );
        }
      }
      held = std::move( stepped );
      return true;
    }

    // What writes a byte of memory: the byte the register may hold is then no
    // longer what it holds — any byte, where the store goes through an index
    // or a pointer. Its own store writes what it holds, so it holds that byte
    // too.
    static constexpr std::array<std::string_view, 10> WRITES_MEMORY{ "sta", "stx", "sty", "stz", "inc",
                                                                     "dec", "asl", "lsr", "rol", "ror" };
    if ( among( WRITES_MEMORY, mnemonic ) && !operand.empty() )
    {
      forgetWritten( held, operand );
      if ( isOneByte( operand ) && !isVolatile( operand ) && mnemonic == "st" + own && !holds( operand ) )
      {
        held.emplace_back( operand );
      }
      return true;
    }

    // What leaves both registers as they were, and what moves only the other
    // one; anything else — its own transfer or `tsx`, a `jsr`, a directive or
    // a macro — may not. Its own transfer is taken apart, since it fills the
    // register with what `A` holds, which the caller does not see.
    static constexpr std::array<std::string_view, 45> LEAVES_BOTH{
      "lda", "and", "ora", "eor", "adc", "sbc", "cmp", "cpx", "cpy", "bit",   "clc",   "sec",   "clv",   "pha", "pla",
      "php", "nop", "txa", "tya", "asl", "lsr", "rol", "ror", "bcc", "bcs",   "beq",   "bne",   "bmi",   "bpl", "bvc",
      "bvs", "jcc", "jcs", "jeq", "jne", "jmi", "jpl", "jvc", "jvs", "jmpcc", "jmpcs", "jmpeq", "jmpne", "jra",
    };
    static constexpr std::array<std::string_view, 3> ALSO_LEAVES_BOTH{ "jmp", "rts", "txs" };
    std::string const other = reg == 'x' ? "y" : "x";
    bool const moves = mnemonic == "ta" + own || ( reg == 'x' && mnemonic == "tsx" );
    bool const others =
        mnemonic == "ld" + other || mnemonic == "ta" + other || mnemonic == "in" + other || mnemonic == "de" + other;
    if ( moves || ( !among( LEAVES_BOTH, mnemonic ) && !among( ALSO_LEAVES_BOTH, mnemonic ) && !others ) )
    {
      held.clear();
    }
    return true;
  }

  bool noteLineOf( std::string_view mnemonic, std::string_view operand )
  {
    auto const holds = [this]( std::string_view what ) { return std::ranges::find( mAHolds, what ) != mAHolds.end(); };

    if ( !noteCarry( mnemonic, operand ) )
    {
      return false;
    }

    // An element read through an index is held too, `t,x`, for as long as
    // the index register stands and nothing writes the array: a `lda` of it
    // again reads the byte `A` holds — see
    // docs/decisions/0149-a-store-to-an-element-stays-in-its-array.md.
    bool const holdable = ( isOneByte( operand ) || indexedBase( operand ).has_value() ) && !isVolatile( operand );
    if ( mnemonic == "lda" )
    {
      if ( holdable && holds( operand ) )
      {
        return false;
      }
      mAHolds.clear();
      if ( holdable )
      {
        mAHolds.emplace_back( operand );
      }
      return true;
    }
    if ( mnemonic == "sta" )
    {
      forgetWritten( mAHolds, operand );
      if ( holdable && !holds( operand ) )
      {
        mAHolds.emplace_back( operand );
      }
      return true;
    }

    static constexpr std::array<std::string_view, 9> WRITES_MEMORY{ "asl", "lsr", "rol", "ror", "inc",
                                                                    "dec", "stx", "sty", "stz" };
    static constexpr std::array<std::string_view, 20> LEAVES_A{ "cmp", "cpx", "cpy", "tax", "tay", "inx", "iny",
                                                                "dex", "dey", "ldx", "ldy", "clc", "sec", "clv",
                                                                "pha", "php", "nop", "bit", "jmp", "rts" };
    static constexpr std::array<std::string_view, 21> BRANCHES{
      "bcc", "bcs", "beq", "bne", "bmi", "bpl",   "bvc",   "bvs",   "jcc",   "jcs", "jeq",
      "jne", "jmi", "jpl", "jvc", "jvs", "jmpcc", "jmpcs", "jmpeq", "jmpne", "jra",
    };

    auto const among = []( auto const& list, std::string_view what )
    { return std::ranges::find( list, what ) != list.end(); };
    bool const writesMemory = among( WRITES_MEMORY, mnemonic );
    if ( writesMemory && !operand.empty() )
    {
      forgetWritten( mAHolds, operand );
    }
    else if ( !among( LEAVES_A, mnemonic ) && !among( BRANCHES, mnemonic ) )
    {
      // What writes `A`; a shift with no operand, which shifts `A`; and a
      // `jsr`, a directive or a macro, whose doings to `A` and to memory this
      // cannot say. What is in the two lists leaves `A` as it was — a branch
      // not taken falls through with it, and one taken lands on a label, which
      // forgets.
      mAHolds.clear();
    }
    return true;
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
    if ( ( object.type == ir::Type::POINTER && !object.count.has_value() ) ||
         object.placement == model::PlacementClass::ZEROPAGE )
    {
      attributes = "zeropage";
    }
    if ( !object.pane.empty() )
    {
      attributes += ( attributes.empty() ? "" : ", " ) + ( "in " + object.pane );
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
        line( ".res " + std::to_string( *count * ir::sizeOf( object.type ) ) );
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
        line( std::string{ width == 1 ? ".byte " : ".word " } + items );
        at = element;
      }
    }
    else if ( std::optional<ir::Constant> const value = object.value; value.has_value() )
    {
      line( std::string{ ir::sizeOf( object.type ) == 1 ? ".byte " : ".word " } + valueOf( *value ) );
    }
    else
    {
      line( ".res " + std::to_string( ir::sizeOf( object.type ) ) );
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
      mText.append( object.pane.empty() ? "\n.section\n" : "\n.section in " + object.pane + "\n" )
          .append( path.back() )
          .append( "\n" );
      if ( !object.isStatic )
      {
        mText.append( ".export " ).append( path.back() ).append( "\n" );
      }
      if ( elements.empty() )
      {
        line( ".res " + std::to_string( object.count.value_or( 0 ) ) );
      }
      for ( std::size_t at = 0; at < elements.size(); at += TABLE_LINE )
      {
        std::string items;
        for ( std::size_t element = at; element < std::min( elements.size(), at + TABLE_LINE ); ++element )
        {
          items.append( element == at ? "" : ", " ).append( valueOf( elements[element] ) );
        }
        line( ".byte " + items );
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
    mRegions.push_back( mText.size() );
    mRegionOwners.push_back( lowered.name );
    mOwner = lowered.name;
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
    forgetA();
    mLocalLabels = 0;

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
    if ( !mTrial )
    {
      // Which loops are too long to test where they jump back: a Jcc that far
      // is five bytes and a `jmp`, which is what the test at the top cost. Read
      // off the last trial, which rotates every loop it may; where one is too
      // long, the trials are run again without it, so that every trial the
      // text is entered by writes the same blocks.
      mFarLatches.clear();
      for ( int sizing = 0; sizing < 2; ++sizing )
      {
        mBlockEntry.assign( lowered.blocks.size(), {} );
        mBlockEntryCarry.assign( lowered.blocks.size(), Carry::UNKNOWN );
        std::map<std::uint32_t, std::size_t> reach;
        for ( int round = 0; round < MAX_TRIALS; ++round )
        {
          Writer trial{ *this };
          trial.mTrial = true;
          trial.mText.clear();
          trial.function( lowered );
          reach = trial.mRotatedReach;
          std::vector<std::vector<std::string>> const grown =
              entriesFrom( lowered, trial.mBlockEnd, trial.mRotatedStep, trial.mFallsInto );
          std::vector<Carry> const carries =
              carriesFrom( lowered, trial.mBlockEndCarry, trial.mRotatedStep, trial.mFallsInto );
          if ( grown == mBlockEntry && carries == mBlockEntryCarry )
          {
            break;
          }
          mBlockEntry = grown;
          mBlockEntryCarry = carries;
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
    }
    mBlockEnd.assign( lowered.blocks.size(), {} );
    mBlockEndCarry.assign( lowered.blocks.size(), {} );
    mScratchSizes.clear();
    mScratch.assign( lowered.values.size(), std::nullopt );
    mDirect.assign( lowered.values.size(), std::string{} );
    mFused.assign( lowered.blocks.size(), false );
    mLoops = countedLoopsOf( lowered, mVolatiles );
    mLiveIn = liveInOf( lowered );
    mBlockTextAt.assign( lowered.blocks.size(), 0 );
    mRotatedReach.clear();
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
    std::vector<bool> labelled( lowered.blocks.size(), false );
    std::vector<std::vector<bool>> storedDirectly( lowered.blocks.size() );

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
    mOwnScalars.clear();
    for ( ir::Local const& byte : lowered.parameters )
    {
      if ( byte.type != ir::Type::BLOCK )
      {
        mOwnScalars.insert( qualified( byte.name ) );
      }
    }
    for ( ir::Local const& byte : lowered.locals )
    {
      if ( byte.type != ir::Type::BLOCK )
      {
        mOwnScalars.insert( qualified( byte.name ) );
      }
    }
    if ( lowered.result.has_value() && *lowered.result != ir::Type::BLOCK && lowered.memberOf.empty() )
    {
      mOwnScalars.insert( qualified( std::string{ RESULT } ) );
    }
    for ( ir::Global const& object : lowered.sections )
    {
      mProcBytes.insert( object.name );
    }
    for ( ir::NamedConstant const& constant : lowered.constants )
    {
      mProcBytes.insert( constant.name );
    }
    mInBody = 0;
    mBodies.clear();
    mPendingBodies.clear();
    std::vector<std::string> outside;

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
      storedDirectly[index].assign( instructions.size(), false );
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
            storedDirectly[index][at + 1] = true;
          }
          else
          {
            mScratch[defined->index] = scratch( 2 );
          }
          continue;
        }
        bool const kept = at + 1 == instructions.size() ? branchesOnLast : takesFromA( instructions[at + 1], *defined );
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
        if ( throughY( load.index, load.type, load.scaled ) || mDeferred[index][at] ||
             !mAsOperand[load.result.index].empty() || mFallsInto[index].has_value() )
        {
          plan.reset();
        }
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
    for ( std::uint32_t index = 0; index < lowered.blocks.size(); ++index )
    {
      if ( mRotatedStep[index].has_value() )
      {
        labelled[mRotatedStep[index]->body] = true;
        continue;
      }
      for ( std::uint32_t const target : jumpsOf( lowered.blocks[index].terminator, nextOf( index ) ) )
      {
        labelled[target] = true;
      }
    }

    // A parameter that comes in a register is there when the first block is
    // entered, and what reads it from its byte reads it from there — see
    // docs/decisions/0145-an-argument-in-a-register.md.
    mReturnHolds.clear();
    std::size_t const bodyBegin = mText.size();
    for ( ir::Local const& parameter : lowered.parameters )
    {
      if ( parameter.place == "a" )
      {
        mAHolds.push_back( qualified( parameter.name ) );
      }
      else if ( parameter.place == "x" )
      {
        mXHolds.push_back( qualified( parameter.name ) );
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
          forgetA();
          ++mInBody;
        }
      }
      ir::Block const& block = lowered.blocks[index];
      mBlock = index;
      mSkipAfterInPlace = 0;
      mBlockTextAt[index] = mText.size();
      if ( labelled[index] )
      {
        label( blockLabel( index ) );
        mAHolds = mBlockEntry[index];
        mCarry = mBlockEntryCarry[index];
      }

      // Where a counted loop leaves, its counter goes back into its byte —
      // once, and not at every turn of the test, and not at all where nothing
      // after the loop reads it.
      for ( CountedLoop const& loop : mLoops )
      {
        if ( loop.exit == index && mLiveIn[index].contains( loop.counter ) )
        {
          line( "stx " + qualified( loop.counter ) );
        }
      }

      // The step of such a loop is `inx` and nothing else. Written here rather
      // than by skipping the block, so that a `[[with]]` region ending on it is
      // still closed below.
      CountedLoop const* const stepping = loopAt( index );
      bool const isStep = stepping != nullptr && stepping->step == index;
      if ( isStep )
      {
        under( block.instructions.front().at );
        line( "inx" );
      }

      for ( std::size_t at = 0; at < block.instructions.size() && !isStep; ++at )
      {
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
        if ( at == 0 && mShiftedIn[index] )
        {
          // The shift the test before wrote: `A` holds what it computes.
          keep( std::get<ir::Binary>( block.instructions.front().operation ).result );
          continue;
        }
        if ( madeInPlace || ( mFused[index] && at + 1 == block.instructions.size() ) || storedDirectly[index][at] ||
             isFoldedSum( block.instructions[at] ) || mDeferred[index][at] || mMoved[index][at] )
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
        // what the operator in place leaves; the real run takes the way
        // through `A` where a block it leads to was promised the object
        // there, which gives more than was promised and never less. See
        // docs/decisions/0112-an-operator-in-place.md and
        // docs/decisions/0157-a-wide-comparison-high-byte-first.md for the
        // pair whose high byte the promise names.
        // Of a pair, the high byte is asked about too: the way through `A`
        // the trials take leaves that byte in it, and what a block this one
        // leads to was promised is the high byte, not the pair's name.
        std::string const inPlaceHigh =
            inPlace.has_value() && inPlace->wide ? byteOf( qualified( inPlace->object ), 1 ) : std::string{};
        bool const counted = inPlace.has_value() && !mTrial &&
                             ( expectedInA( index, qualified( inPlace->object ) ) ||
                               ( !inPlaceHigh.empty() && expectedInA( index, inPlaceHigh ) ) );
        if ( inPlace.has_value() && !counted && !isVolatile( inPlace->object ) &&
             !holdsInA( qualified( inPlace->object ) ) )
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
      mLastBranch.reset();
      if ( !mFallsInto[index].has_value() )
      {
        terminator( index );
      }
      // An element held through an index is not carried into another block,
      // whose ways in may have the register at different elements.
      mBlockEnd[index] = mAHolds;
      if ( mEarlyHolds.has_value() )
      {
        std::erase_if( mBlockEnd[index],
                       [this]( std::string const& name )
                       { return std::ranges::find( *mEarlyHolds, name ) == mEarlyHolds->end(); } );
      }
      std::erase_if( mBlockEnd[index], []( std::string const& name ) { return name.contains( ',' ); } );
      mBlockEndCarry[index] = mLastBranch.value_or( EdgeCarry{ .taken = mCarry, .fallen = mCarry } );
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
          forgetA();
          --mInBody;
        }
      }
    }

    // The stores to a byte a register carries instead are stores to nothing:
    // the value is where the register leaves it.
    dropStoresToPlaced( lowered, bodyBegin );
    loadBackWhatZeroStored( bodyBegin );
    stepInAccumulator( bodyBegin );
    setBitsInPlace( bodyBegin );
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
        if ( lowered.resultPlace == "a" )
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
    bool const placed = !lowered.resultPlace.empty() ||
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
    for ( std::uint32_t index = 0; index < mScratchSizes.size(); ++index )
    {
      std::size_t const begin = mText.size();
      mText.append( scratchName( index ) )
          .append( lowered.placement == model::PlacementClass::ABSOLUTE ? " .temp " : " .ztemp " )
          .append( std::to_string( mScratchSizes[index] ) )
          .append( "\n" );
      mDeclared.push_back( Declared{ .name = scratchName( index ),
                                     .begin = begin,
                                     .end = mText.size(),
                                     .owner = mOwner,
                                     .region = mRegions.size() - 1 } );
    }
    for ( ir::NamedConstant const& constant : lowered.constants )
    {
      namedConstant( constant, INDENT );
    }

    // Its objects that are no bytes of the Proc stand in it, beside it and in
    // its scope: a local array a `.temp`, and a `static` local, an array given
    // constants or a local whose address is taken a Section of its own — see
    // docs/decisions/0085-a-section-may-stand-in-a-proc.md.
    for ( ir::Global const& object : lowered.sections )
    {
      if ( object.isTemporary && object.stripes.empty() )
      {
        mText.append( object.name )
            .append( " .temp " )
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

  /// A scratch byte, or a pair, taken for the Proc being written.
  std::uint32_t scratch( std::uint32_t size )
  {
    mScratchSizes.push_back( size );
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

  /// The block whose label the operand is, or nothing for a local label such
  /// as `@e0`, which a branch inside a block goes to.
  [[nodiscard]] static std::optional<std::uint32_t> blockOf( std::string_view operand )
  {
    if ( !operand.starts_with( "@l" ) )
    {
      return std::nullopt;
    }
    std::uint32_t index = 0;
    for ( char const digit : operand.substr( 2 ) )
    {
      if ( digit < '0' || digit > '9' )
      {
        return std::nullopt;
      }
      index = ( index * 10 ) + static_cast<std::uint32_t>( digit - '0' );
    }
    return index;
  }

  /// Keeps `mCarry` true for one line about to be written; false where the
  /// line is a `clc` or a `sec` that would set the carry to what it is.
  bool noteCarry( std::string_view mnemonic, std::string_view operand )
  {
    if ( mnemonic == "clc" )
    {
      if ( mCarry == Carry::CLEAR )
      {
        return false;
      }
      mCarry = Carry::CLEAR;
      return true;
    }
    if ( mnemonic == "sec" )
    {
      if ( mCarry == Carry::SET )
      {
        return false;
      }
      mCarry = Carry::SET;
      return true;
    }

    // A branch on the carry says what it is on each way out: taken, it is
    // what the branch tested for; fallen through, it is the other. Every other
    // branch leaves it as it was on both.
    if ( mnemonic == "jcs" || mnemonic == "bcs" )
    {
      mLastBranch = EdgeCarry{ .jumpsTo = blockOf( operand ), .taken = Carry::SET, .fallen = Carry::CLEAR };
      mCarry = Carry::CLEAR;
      return true;
    }
    if ( mnemonic == "jcc" || mnemonic == "bcc" )
    {
      mLastBranch = EdgeCarry{ .jumpsTo = blockOf( operand ), .taken = Carry::CLEAR, .fallen = Carry::SET };
      mCarry = Carry::SET;
      return true;
    }

    static constexpr std::array<std::string_view, 37> LEAVES_CARRY{
      "lda", "ldx", "ldy", "sta", "stx", "sty", "tax", "tay", "txa", "tya", "inx", "iny", "dex",
      "dey", "inc", "dec", "and", "ora", "eor", "bit", "nop", "pha", "php", "pla", "beq", "bne",
      "bmi", "bpl", "bvc", "bvs", "jeq", "jne", "jmi", "jpl", "jvc", "jvs", "jra",
    };
    if ( std::ranges::find( LEAVES_CARRY, mnemonic ) == LEAVES_CARRY.end() )
    {
      // An add, a subtract, a compare, a shift, a `jsr`, a `jmp` or `rts`
      // into nothing, a directive, a macro: the carry is the machine's.
      mCarry = Carry::UNKNOWN;
    }
    return true;
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
    auto const* const constant = std::get_if<ir::Constant>( &operand );
    return constant != nullptr && constant->name.empty() && constant->value == 0;
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

  /// Whether a byte element is read or written through `Y`: under a counted
  /// loop, whose counter `X` holds, where its index is not in `X` already.
  [[nodiscard]] bool throughY( ir::Operand const& index, ir::Type type, bool scaled ) const
  {
    return ir::sizeOf( type ) == 1 && !scaled && loopAt( mBlock ) != nullptr && !indexAlreadyInX( index, scaled );
  }

  [[nodiscard]] bool indexAlreadyInX( ir::Operand const& index, bool scaled ) const
  {
    if ( scaled )
    {
      return false;
    }
    if ( auto const* const object = std::get_if<ir::Object>( &index ) )
    {
      return inX( mBlock, object->name );
    }
    CountedLoop const* const loop = loopAt( mBlock );
    return loop != nullptr && loop->offsetOf( index ).has_value();
  }

  /// Whether an index is the counter of the loop holding this block, or a sum
  /// the address carries — asked at planning, where `mBlock` is not yet set.
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
           !mDirect[call->result->index].empty() || readersOf( function, *call->result ) != 1 ||
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
        if ( object == nullptr || isVolatile( object->name ) )
        {
          continue;
        }
        std::string const base = baseOf( object->name );

        // Every reader is a 16-bit operator, a compare or a store, reading the
        // bytes where they lie; and nothing before the last of them writes
        // what they lie in. `x << 8`'s high byte is `x`'s low one, which a
        // reader writing `x` must not write before it reads — so such a
        // reader is taken only where its low byte writes `x`'s back unchanged.
        std::uint32_t const readers = readersOf( function, shift->result );
        std::uint32_t seen = 0;
        bool fits = true;
        for ( std::size_t later = at + 1; later < body.size() && fits && seen < readers; ++later )
        {
          ir::Instruction const& instruction = body[later];
          ir::Instruction read = instruction;
          std::uint32_t here = 0;
          ir::eachOperand(
              read, [&here, &shift]( ir::Operand& operand ) { here += isValue( operand, shift->result ) ? 1U : 0U; } );
          if ( here != 0 )
          {
            seen += here;
            if ( auto const* const binary = std::get_if<ir::Binary>( &instruction.operation );
                 binary != nullptr && ir::sizeOf( binary->type ) == 2 && binary->op != ir::BinaryOperator::SHIFT_LEFT &&
                 binary->op != ir::BinaryOperator::SHIFT_RIGHT )
            {
              std::string const& into = mDirect[binary->result.index];
              auto const* const same = std::get_if<ir::Object>( &binary->left );
              fits = !left || into.empty() || baseOf( into ) != base ||
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
              fits = !left || baseOf( store->name ) != base;
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
                                           std::holds_alternative<ir::Switch>( instruction.operation ) ||
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
             readersOf( function, *operand ) != 1 )
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
             readersOf( function, *operand ) != 1 )
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
        if ( !before.has_value() || before->index != operand->index || isVolatile( into ) ||
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
          ir::Instruction read = instruction;
          ir::eachOperand(
              read, [&here, &sum]( ir::Operand& operand ) { here += isValue( operand, sum->result ) ? 1U : 0U; } );
          indexOnly = here == 0;
          written = written || writesObject( instruction, object->name );
        }
        if ( !indexOnly || reads == 0 || reads != readersOf( function, sum->result ) )
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
      return baseOf( store->name ) == baseOf( name );
    }
    if ( auto const* const element = std::get_if<ir::StoreElement>( &instruction.operation ) )
    {
      return baseOf( element->name ) == baseOf( name );
    }
    if ( auto const* const copy = std::get_if<ir::Copy>( &instruction.operation ) )
    {
      return baseOf( copy->to.name ) == baseOf( name );
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
           !isValue( to->value, from->result ) || readersOf( function, from->result ) != 1 ||
           isVolatile( from->name ) || isVolatile( to->name ) )
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
           readersOf( function, load->result ) != 1 || isVolatile( load->name ) )
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
                  compare != nullptr && ir::sizeOf( compare->type ) == 1 )
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
  void planIndirectOperands( ir::Function const& function, std::uint32_t index )
  {
    ir::Block const& block = function.blocks[index];
    for ( std::size_t at = 0; at < block.instructions.size(); ++at )
    {
      auto const* const load = std::get_if<ir::LoadIndirect>( &block.instructions[at].operation );
      if ( load == nullptr || load->isVolatile || ir::sizeOf( load->type ) != 1 ||
           readersOf( function, load->result ) != 1 )
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
         !mask->name.empty() || ( mask->value != 128 && mask->value != 64 ) || object == nullptr ||
         readersOf( function, masked->result ) != 1 )
    {
      return;
    }
    bool const high = mask->value == 128;
    mBitTest[index] = BitTest{ .masked = masked->result.index,
                               .object = object->name,
                               .high = high,
                               .shifted = high && shiftsBothWays( function, index, object->name ) };
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
  bool shiftsBothWays( ir::Function const& function, std::uint32_t index, std::string const& tested )
  {
    ir::Terminator const& end = function.blocks[index].terminator;
    // A volatile byte is read once for the test and once for the shift.
    if ( end.target == end.otherwise || end.target == index || end.otherwise == index || isVolatile( tested ) )
    {
      return false;
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
          return false;
        }
      }
    }
    auto const shifts = [&function, &tested, index]( std::uint32_t into )
    {
      ir::Block const& block = function.blocks[into];
      if ( into == 0 || block.instructions.empty() || onlyPredecessorOf( function, into ) != index )
      {
        return false;
      }
      auto const* const shift = std::get_if<ir::Binary>( &block.instructions.front().operation );
      auto const* const object = shift != nullptr ? std::get_if<ir::Object>( &shift->left ) : nullptr;
      auto const* const by = shift != nullptr ? std::get_if<ir::Constant>( &shift->right ) : nullptr;
      return shift != nullptr && shift->op == ir::BinaryOperator::SHIFT_LEFT && ir::sizeOf( shift->type ) == 1 &&
             object != nullptr && object->name == tested && by != nullptr && by->name.empty() && by->value == 1;
    };
    if ( !shifts( end.target ) || !shifts( end.otherwise ) )
    {
      return false;
    }
    mShiftedIn[end.target] = true;
    mShiftedIn[end.otherwise] = true;
    return true;
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
           readersOf( function, *defined ) != 1 )
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
    for ( std::optional<std::uint32_t> const& used : mScratch )
    {
      if ( used.has_value() && !renumbered[*used].has_value() )
      {
        renumbered[*used] = static_cast<std::uint32_t>( sizes.size() );
        sizes.push_back( mScratchSizes[*used] );
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
  [[nodiscard]] bool returned( std::uint32_t target )
  {
    ir::Block const& into = mFunction->blocks[target];
    if ( !into.instructions.empty() || into.terminator.kind != ir::TerminatorKind::RETURN )
    {
      return false;
    }
    under( into.terminator.at );
    mReturnHolds.push_back( mAHolds );
    line( "rts" );
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
      forgetA();
      mCarry = Carry::UNKNOWN;
      return;
    }
    case ir::TerminatorKind::RETURN:
      under( end.at );
      mReturnHolds.push_back( mAHolds );
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
      if ( next != end.target && !returned( end.target ) )
      {
        under( end.at );
        std::string_view const taken = alwaysTaken();
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
        forgetA();
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
        if ( !returned( end.otherwise ) )
        {
          std::string_view const taken = alwaysTaken();
          line( ( taken.empty() ? std::string{ "jmp" } : std::string{ taken } ) + " " + blockLabel( end.otherwise ) );
        }
      }
      if ( mPastBranch.has_value() )
      {
        label( *mPastBranch );
        mPastBranch.reset();
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
      label( *mPastBranch );
      mPastBranch.reset();
    }
  }

  /// What the lines of text come to, roughly: an instruction's opcode and its
  /// operand, a byte for an operand of the Proc's own, which lies on the zero
  /// page, and two for any other name, which may not; a Jcc at two. Enough to
  /// tell a loop that fits a branch from one that does not.
  [[nodiscard]] static std::size_t estimatedBytes( std::string_view text )
  {
    std::size_t bytes = 0;
    while ( !text.empty() )
    {
      std::size_t const end = std::min( text.find( '\n' ), text.size() );
      std::string_view line = text.substr( 0, end );
      text.remove_prefix( std::min( end + 1, text.size() ) );
      if ( line.empty() || line.front() != ' ' )
      {
        continue;
      }
      line.remove_prefix( std::min( line.find_first_not_of( ' ' ), line.size() ) );
      if ( line.empty() || line.front() == '.' )
      {
        continue;
      }
      std::size_t const space = line.find( ' ' );
      if ( space == std::string_view::npos )
      {
        bytes += 1;
        continue;
      }
      std::string_view const operand = line.substr( space + 1 );
      bool const branch = line.front() == 'j' && line.substr( 0, space ) != "jmp" && line.substr( 0, space ) != "jsr";
      bool const own = operand.starts_with( "__" ) || operand.starts_with( '#' ) || operand.starts_with( '@' ) ||
                       operand.starts_with( '(' );
      bytes += branch || line.front() == 'b' || own ? std::size_t{ 2 } : std::size_t{ 3 };
    }
    return bytes;
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
        load( ir::Object{ .name = test->object, .type = ir::Type::U8 } );
        line( "asl" );
        return set ? Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" } : Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" };
      }
      // The carry holds that bit already, where an `asl` pushed it out of `A`
      // since the byte was last written: no `bit`, and nothing read.
      if ( test->high && std::ranges::find( mCarryBit7, qualified( test->object ) ) != mCarryBit7.end() )
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
      // Against zero, where the flags already say what `A` holds, or what
      // `X` holds for a counter: `Z` is the answer, and `cmp #0` or `cpx #0`
      // would only write it again.
      if ( !isZero( *second ) || !( testsX ? mFlagsFromX : mFlagsFromA ) )
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
    if ( !testsX && isZero( *second ) )
    {
      if ( !mFlagsFromA )
      {
        // `ora #0` and not `cmp #0`: both say what `A` holds, and this one
        // leaves the carry alone and counts as setting the flags from `A`,
        // so a second test of the same byte says nothing at all.
        line( "ora #0" );
      }
      return less ? Jumps{ .whenTrue = "jmi", .whenFalse = "jpl" } : Jumps{ .whenTrue = "jpl", .whenFalse = "jmi" };
    }

    // The sign of the difference, turned over where the subtraction
    // overflowed, is set where the first is less. A signed counter is not kept
    // in `X`, so this path always has its value in `A`.
    std::string const skip = localLabel( "v" );
    line( "sec" );
    line( "sbc " + named( *second ) );
    line( "bvc " + skip );
    line( "eor #128" );
    label( skip );
    return less ? Jumps{ .whenTrue = "jmi", .whenFalse = "jpl" } : Jumps{ .whenTrue = "jpl", .whenFalse = "jmi" };
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
      line( "lda " + named( left ) );
      if ( !zero || !mFlagsFromA )
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
        if ( !zero || !mFlagsFromA )
        {
          line( "cmp " + named( right, 8 ) );
        }
        return instruction.op == ir::Comparison::EQUAL ? Jumps{ .whenTrue = "jeq", .whenFalse = "jne" }
                                                       : Jumps{ .whenTrue = "jne", .whenFalse = "jeq" };
      }
      std::string const differ = localLabel( "e" );
      line( "bne " + differ );
      line( "lda " + named( left, 8 ) );
      if ( !zero || !mFlagsFromA )
      {
        line( "cmp " + named( right, 8 ) );
      }
      if ( mWays.has_value() )
      {
        mPastBranch = differ;
      }
      else
      {
        label( differ );
      }
      return instruction.op == ir::Comparison::EQUAL ? Jumps{ .whenTrue = "jeq", .whenFalse = "jne" }
                                                     : Jumps{ .whenTrue = "jne", .whenFalse = "jeq" };
    }

    bool const less = instruction.op == ir::Comparison::LESS;
    if ( !ir::isSigned( instruction.type ) && mWays.has_value() && highFirst( instruction ) )
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
        line( std::string{ bcc } + " " + *past );
      };
      line( "lda " + named( left, 8 ) );
      line( "cmp " + named( right, 8 ) );
      toward( lessTo, "jcc", "bcc" );
      toward( notLessTo, "jne", "bne" );
      line( "lda " + named( left ) );
      line( "cmp " + named( right ) );
      mPastBranch = std::move( past );
      return less ? Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" } : Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" };
    }
    subtractWide( left, right );
    if ( !ir::isSigned( instruction.type ) )
    {
      return less ? Jumps{ .whenTrue = "jcc", .whenFalse = "jcs" } : Jumps{ .whenTrue = "jcs", .whenFalse = "jcc" };
    }
    std::string const skip = localLabel( "v" );
    line( "bvc " + skip );
    line( "eor #128" );
    label( skip );
    return less ? Jumps{ .whenTrue = "jmi", .whenFalse = "jpl" } : Jumps{ .whenTrue = "jpl", .whenFalse = "jmi" };
  }

  /// Whether a 16-bit comparison of order is written high bytes first: where
  /// the bound is a constant of more than a byte, or one the assembler names
  /// and the compiler cannot fold, so that the high bytes differ on most of
  /// the range the other operand walks — see
  /// docs/decisions/0157-a-wide-comparison-high-byte-first.md. Against a
  /// bound below 256 they agree on every turn but the last, and asking about
  /// them first costs four cycles a turn; and what an object's high byte
  /// holds, `limit` of `sieve`, the compiler does not know yet.
  [[nodiscard]] static bool highFirst( ir::Compare const& instruction )
  {
    auto const* const bound = std::get_if<ir::Constant>( &instruction.right );
    return bound != nullptr && ( !bound->name.empty() || bound->value >= 256 );
  }

  /// The high byte of `left - right` in `A`, with the carry, sign and overflow
  /// of the whole subtraction.
  void subtractWide( ir::Operand const& left, ir::Operand const& right )
  {
    line( "lda " + named( left ) );
    line( "cmp " + named( right ) );
    line( "lda " + named( left, 8 ) );
    line( "sbc " + named( right, 8 ) );
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
      if ( instruction.member )
      {
        // A member's state is the family's first plus the index.
        load( *index );
        line( "clc" );
        line( "adc #" + instruction.name );
        line( "tax" );
      }
      else
      {
        line( inA( *index ) ? std::string{ "tax" } : "ldx " + named( *index ) );
      }
    }
    std::string with = ".with " + instruction.name;
    if ( instruction.form == ir::WithForm::AT )
    {
      with += ", x";
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
        store( ir::Store{ .name = argument->name, .type = argument->type, .value = argument->value } );
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
    if ( indexAlreadyInX( index, scaled ) )
    {
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
    if ( throughY( instruction.index, instruction.type, instruction.scaled ) || secondIndexHere() )
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
      bool const y = throughY( instruction.index, instruction.type, instruction.scaled ) || secondIndexHere();
      if ( y || !indexAlreadyInX( instruction.index, instruction.scaled ) )
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

  /// A byte or bytes of a Proc: a `.ztemp`, or for a `struct` or a `union` of
  /// more than four bytes a `.temp` — see docs/decisions/0086-a-struct-by-value.md.
  /// One byte of a Proc's own. `mayGo` where nothing outside the text
  /// requires it: a local, against a parameter or a result, which the caller
  /// names whether this Proc writes it or not.
  /// Where the declaration of a byte a Proc need not have stands in the
  /// text, and the name it declares.
  struct Declared
  {
    std::string name;
    std::size_t begin = 0;
    std::size_t end = 0;

    /// The Proc it is a byte of, and where that Proc's text begins in
    /// `mRegions`: what the Proc writes up to the next function, the macros
    /// of its `[[with]]` blocks included, names it by itself, and anything
    /// else only as `PROC.NAME`.
    std::string owner{};
    std::size_t region = 0;
  };

  /// Where each function's text begins, in the order written, and its name.
  std::vector<std::size_t> mRegions;

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

  /// What `A` held where a comparison branched away before the branch that
  /// reads it — a way out of the block the IR's edges do not know of, since
  /// it goes where one of them goes: the block's end holds no more than
  /// every one of its ways out agrees on. And whether the carry on that way
  /// was another than the branch's, which no edge may then count on.
  std::optional<std::vector<std::string>> mEarlyHolds;
  bool mEarlyCarry = false;

  /// Notes a way out written before the block's own branch.
  void leftEarly( bool carryDiffers )
  {
    if ( !mEarlyHolds.has_value() )
    {
      mEarlyHolds = mAHolds;
    }
    else
    {
      std::erase_if( *mEarlyHolds,
                     [this]( std::string const& name )
                     { return std::ranges::find( mAHolds, name ) == mAHolds.end(); } );
    }
    mEarlyCarry = mEarlyCarry || carryDiffers;
  }

  std::vector<std::string> mRegionOwners;

  /// The function being written.
  std::string mOwner;

  /// The declarations that go where nothing names them, in the order they
  /// were written: a local a counted loop moved into `X`
  /// ([0109](docs/decisions/0109-an-induction-variable-in-x.md)), one whose
  /// store was dropped as dead (0111), a scratch byte the emitter found
  /// another place for (0114 to 0116). See
  /// docs/decisions/0118-a-byte-nothing-names-is-not-declared.md.
  std::vector<Declared> mDeclared;

  /// Whether some line of the Module names the byte: the name with no
  /// letter, digit or `_` beside it, so that `__1i` is not found inside
  /// `__1if`, and `g.__0seen` counts, since a case of a `switch` names its
  /// function's byte through it. A word in a comment or a path counts too,
  /// which keeps a declaration rather than dropping one, and that is the
  /// direction to err in.
  [[nodiscard]] static bool namesByte( std::string const& lines, std::string const& name )
  {
    auto const part = []( char c ) { return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_'; };
    for ( std::size_t at = lines.find( name ); at != std::string::npos; at = lines.find( name, at + 1 ) )
    {
      std::size_t const after = at + name.size();
      if ( ( at == 0 || !part( lines[at - 1] ) ) && ( after == lines.size() || !part( lines[after] ) ) )
      {
        return true;
      }
    }
    return false;
  }

  /// A byte of a Proc's own that no line of the Module names is not
  /// declared: the emitter found somewhere else for it to be, or for nothing
  /// to be there at all, and what is left is a Temporary for Trace to walk
  /// and Place to give an address to. Run once the whole Module stands,
  /// since a byte may be named from a Proc written after the one that
  /// declares it.
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
    if ( gone.empty() && lowered.resultPlace.empty() )
    {
      return;
    }
    std::string kept;
    std::size_t from = begin;
    while ( from < mText.size() )
    {
      std::size_t const end = std::min( mText.find( '\n', from ), mText.size() - 1 ) + 1;
      std::string_view const text{ mText.data() + from, end - from };
      bool drop = std::ranges::any_of( gone, [text]( std::string const& name ) { return isStoreTo( text, name ); } );
      if ( lowered.resultPlace == "a" )
      {
        drop = drop || isStoreTo( text, result );
      }
      else if ( lowered.resultPlace == "ma" )
      {
        drop = drop || isStoreTo( text, result + "+1" );
      }
      if ( !drop )
      {
        kept.append( text );
      }
      from = end;
    }
    mText.replace( begin, mText.size() - begin, kept );
  }

  /// Puts back the load a `stz` cost. A zero stored with the accumulator
  /// leaves the byte in `A`, and the writer leans on that: where the next
  /// instruction reads the byte again, `stz` has bought a byte and paid for a
  /// load. The pair goes back to what it was, which is the same two
  /// instructions and the same flags — see
  /// docs/decisions/0183-a-zero-stored-without-the-accumulator.md.
  void loadBackWhatZeroStored( std::size_t begin )
  {
    std::size_t from = begin;
    while ( from < mText.size() )
    {
      std::size_t const end = std::min( mText.find( '\n', from ), mText.size() - 1 ) + 1;
      std::string_view const text{ mText.data() + from, end - from };
      std::string_view stored = text;
      stored.remove_prefix( std::min( stored.find_first_not_of( ' ' ), stored.size() ) );
      if ( !stored.starts_with( "stz " ) )
      {
        from = end;
        continue;
      }
      while ( !stored.empty() && ( stored.back() == '\n' || stored.back() == ' ' ) )
      {
        stored.remove_suffix( 1 );
      }
      std::string const name{ stored.substr( 4 ) };

      // The next line that is an instruction, a `.source` mark standing
      // between a store and its reader as often as not.
      std::size_t next = end;
      std::string_view following;
      while ( next < mText.size() )
      {
        std::size_t const stop = std::min( mText.find( '\n', next ), mText.size() - 1 ) + 1;
        following = std::string_view{ mText.data() + next, stop - next };
        std::string_view bare = following;
        bare.remove_prefix( std::min( bare.find_first_not_of( ' ' ), bare.size() ) );
        if ( !bare.starts_with( ".source " ) )
        {
          break;
        }
        next = stop;
      }
      std::string_view reader = following;
      reader.remove_prefix( std::min( reader.find_first_not_of( ' ' ), reader.size() ) );
      while ( !reader.empty() && ( reader.back() == '\n' || reader.back() == ' ' ) )
      {
        reader.remove_suffix( 1 );
      }
      if ( reader != "lda " + name )
      {
        from = end;
        continue;
      }

      std::string const put = std::string{ INDENT } + "lda #0\n" + std::string{ INDENT } + "sta " + name + "\n";
      std::size_t const readerEnd = std::min( mText.find( '\n', next ), mText.size() - 1 ) + 1;
      mText.erase( next, readerEnd - next );
      mText.replace( from, end - from, put );
      from += put.size();
    }
  }

  /// `clc / adc #1` is `inc` on a 65SC02, and `sec / sbc #1` is `dec`: one
  /// byte and two cycles against three and four, and the accumulator is left
  /// holding the same value either way, so nothing the writer knows is lost.
  ///
  /// Done to the finished text rather than where the addition is written,
  /// because the pair is what there is to recognise. What the trackers made
  /// of the `adc` stands: they have the carry as unknown after it, and after
  /// an `inc` the carry is whatever it was before the `clc` that is now gone
  /// — unknown is the safe answer to both. See
  /// docs/decisions/0184-one-more-in-the-accumulator.md.
  void stepInAccumulator( std::size_t begin )
  {
    if ( mCpu != Cpu::WDC65SC02 )
    {
      return;
    }
    auto const bare = []( std::string_view text )
    {
      text.remove_prefix( std::min( text.find_first_not_of( ' ' ), text.size() ) );
      while ( !text.empty() && ( text.back() == '\n' || text.back() == ' ' ) )
      {
        text.remove_suffix( 1 );
      }
      return text;
    };

    std::size_t from = begin;
    while ( from < mText.size() )
    {
      std::size_t const end = std::min( mText.find( '\n', from ), mText.size() - 1 ) + 1;
      std::string_view const carry = bare( std::string_view{ mText.data() + from, end - from } );
      bool const up = carry == "clc";
      if ( !up && carry != "sec" )
      {
        from = end;
        continue;
      }

      // Strictly the next line: a label or a mark between the two means they
      // are not one addition, and a label on the second would be left
      // standing on an instruction that is no longer there.
      std::size_t const stop = std::min( mText.find( '\n', end ), mText.size() - 1 ) + 1;
      std::string_view const step = bare( std::string_view{ mText.data() + end, stop - end } );
      if ( step != ( up ? "adc #1" : "sbc #1" ) )
      {
        from = end;
        continue;
      }

      std::string const put = std::string{ INDENT } + ( up ? "inc" : "dec" ) + "\n";
      mText.replace( from, stop - from, put );
      from += put.size();
    }
  }

  /// `ora X / sta X` is `tsb X` on a 65SC02: the bits of `A` set in the byte,
  /// in one instruction instead of two. What it costs is that `A` is left
  /// holding the mask rather than the result, so this fires only where the
  /// **next** instruction fills `A` without reading it — which is the shape
  /// the writer produces anyway, a run of `x |= …` loading its next operand.
  ///
  /// `trb` has no counterpart here: `X &= ~A` would want the mask
  /// complemented, and no program of the set masks a byte in place at all.
  /// See docs/decisions/0188-bits-set-in-a-byte.md.
  void setBitsInPlace( std::size_t begin )
  {
    if ( mCpu != Cpu::WDC65SC02 )
    {
      return;
    }
    auto const bare = []( std::string_view text )
    {
      text.remove_prefix( std::min( text.find_first_not_of( ' ' ), text.size() ) );
      while ( !text.empty() && ( text.back() == '\n' || text.back() == ' ' ) )
      {
        text.remove_suffix( 1 );
      }
      return text;
    };
    auto const fillsA = []( std::string_view text )
    { return text.starts_with( "lda " ) || text == "pla" || text == "txa" || text == "tya"; };

    std::size_t from = begin;
    while ( from < mText.size() )
    {
      std::size_t const end = std::min( mText.find( '\n', from ), mText.size() - 1 ) + 1;
      std::string_view const masks = bare( std::string_view{ mText.data() + from, end - from } );
      if ( !masks.starts_with( "ora " ) )
      {
        from = end;
        continue;
      }
      std::string const name{ masks.substr( 4 ) };

      // Strictly the next line, and strictly the one after it: a label or a
      // mark between means these are not one statement.
      std::size_t const stop = std::min( mText.find( '\n', end ), mText.size() - 1 ) + 1;
      if ( bare( std::string_view{ mText.data() + end, stop - end } ) != "sta " + name )
      {
        from = end;
        continue;
      }
      // The next line that is an instruction: a mark stands between two
      // statements, and the statement after this one is what matters.
      std::size_t next = stop;
      std::string_view following;
      while ( next < mText.size() )
      {
        std::size_t const past = std::min( mText.find( '\n', next ), mText.size() - 1 ) + 1;
        following = bare( std::string_view{ mText.data() + next, past - next } );
        if ( !following.starts_with( ".source " ) )
        {
          break;
        }
        next = past;
      }
      if ( !fillsA( following ) )
      {
        from = end;
        continue;
      }

      std::string const put = std::string{ INDENT } + "tsb " + name + "\n";
      mText.replace( from, stop - from, put );
      from += put.size();
    }
  }

  void dropUnnamedBytes()
  {
    // What the Module's lines say, against what they declare: the text with
    // the declarations that may go left out of it, whole and for each
    // function's own stretch of it.
    auto const without = [this]( std::size_t from, std::size_t to )
    {
      std::string lines;
      std::size_t at = from;
      for ( Declared const& declared : mDeclared )
      {
        if ( declared.begin >= from && declared.end <= to )
        {
          lines.append( mText, at, declared.begin - at );
          at = declared.end;
        }
      }
      lines.append( mText, at, to - at );
      return lines;
    };
    std::string const lines = without( 0, mText.size() );
    std::vector<std::string> own;
    own.reserve( mRegions.size() );
    for ( std::size_t region = 0; region < mRegions.size(); ++region )
    {
      own.push_back( without( mRegions[region], region + 1 < mRegions.size() ? mRegions[region + 1] : mText.size() ) );
    }

    // A byte is named by its own Proc's text as it is, and anywhere else only
    // as `PROC.NAME`: another Proc's byte of the same name, `__t0` in each,
    // is another byte.
    auto const named = [&]( Declared const& declared )
    {
      return namesByte( own[declared.region], declared.name ) ||
             namesByte( lines, declared.owner + "." + declared.name );
    };

    // A byte that every line naming it only stores to is read by nothing:
    // what is kept in it is kept in `A`, where the loads 0110 left out found
    // it. Those stores go, and then the byte is named nowhere. See
    // docs/decisions/0143-a-byte-only-stored-to-is-not-kept.md.
    std::set<std::pair<std::size_t, std::string>> writeOnly;
    for ( Declared const& declared : mDeclared )
    {
      if ( named( declared ) && onlyStoredTo( own[declared.region], declared.name ) &&
           onlyStoredTo( lines, declared.owner + "." + declared.name ) )
      {
        writeOnly.emplace( declared.region, declared.name );
      }
    }

    // The text again, a line at a time: a declaration that goes where
    // nothing names its byte, and a store to a byte only ever stored to.
    std::string kept;
    kept.reserve( mText.size() );
    std::size_t next = 0;
    std::size_t from = 0;
    std::size_t region = 0;
    while ( from < mText.size() )
    {
      std::size_t const end = std::min( mText.find( '\n', from ), mText.size() - 1 ) + 1;
      bool drop = false;
      while ( next < mDeclared.size() && mDeclared[next].end <= from )
      {
        ++next;
      }
      while ( region + 1 < mRegions.size() && mRegions[region + 1] <= from )
      {
        ++region;
      }
      if ( next < mDeclared.size() && mDeclared[next].begin == from )
      {
        Declared const& declared = mDeclared[next];
        drop = !named( declared ) || writeOnly.contains( { declared.region, declared.name } );
      }
      else
      {
        // A store in the byte's own stretch by its name, or anywhere by its
        // Proc's.
        std::string_view const text{ mText.data() + from, end - from };
        drop = std::ranges::any_of( writeOnly,
                                    [&]( std::pair<std::size_t, std::string> const& byte )
                                    {
                                      return ( byte.first == region && !mRegions.empty() && from >= mRegions[region] &&
                                               isStoreTo( text, byte.second ) ) ||
                                             isStoreTo( text, mRegionOwners[byte.first] + "." + byte.second );
                                    } );
      }
      if ( !drop )
      {
        kept.append( mText, from, end - from );
      }
      from = end;
    }
    mText = std::move( kept );
    mDeclared.clear();
    mRegions.clear();
    mRegionOwners.clear();
  }

  /// Whether the line is a store of a register to the byte, or to a byte of
  /// it: `sta name`, `stx name+1`.
  [[nodiscard]] static bool isStoreTo( std::string_view line, std::string const& name )
  {
    std::size_t const first = line.find_first_not_of( ' ' );
    if ( first == std::string_view::npos )
    {
      return false;
    }
    std::string_view text = line.substr( first );
    while ( !text.empty() && ( text.back() == '\n' || text.back() == ' ' ) )
    {
      text.remove_suffix( 1 );
    }
    if ( !( text.starts_with( "sta " ) || text.starts_with( "stx " ) || text.starts_with( "sty " ) ) )
    {
      return false;
    }
    std::string_view const operand = text.substr( 4 );
    if ( !operand.starts_with( name ) )
    {
      return false;
    }
    std::string_view const rest = operand.substr( name.size() );
    return rest.empty() || ( rest.size() > 1 && rest.front() == '+' &&
                             rest.substr( 1 ).find_first_not_of( "0123456789" ) == std::string_view::npos );
  }

  /// Whether every line that names the byte is a store to it.
  [[nodiscard]] static bool onlyStoredTo( std::string const& lines, std::string const& name )
  {
    auto const part = []( char c ) { return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_'; };
    for ( std::size_t at = lines.find( name ); at != std::string::npos; at = lines.find( name, at + 1 ) )
    {
      std::size_t const after = at + name.size();
      if ( ( at != 0 && part( lines[at - 1] ) ) || ( after != lines.size() && part( lines[after] ) ) )
      {
        continue;
      }
      std::size_t const begin = lines.rfind( '\n', at ) == std::string::npos ? 0 : lines.rfind( '\n', at ) + 1;
      std::size_t const end = std::min( lines.find( '\n', at ), lines.size() );
      if ( !isStoreTo( std::string_view{ lines }.substr( begin, end - begin ), name ) )
      {
        return false;
      }
    }
    return true;
  }

  void temporary( std::string const& name,
                  ir::Type type,
                  std::uint32_t bytes,
                  bool mayGo,
                  model::PlacementClass placement = model::PlacementClass::ZEROPAGE )
  {
    std::uint32_t const size = type == ir::Type::BLOCK ? bytes : ir::sizeOf( type );
    // A block of more than a few bytes lies off the zero page whatever was
    // asked: an instruction reaches none of it in two bytes — see
    // docs/decisions/0210-placement-is-declared-in-c-too.md.
    bool const off = size > SMALL_BLOCK || placement == model::PlacementClass::ABSOLUTE;
    std::size_t const begin = mText.size();
    mText.append( name ).append( off ? " .temp " : " .ztemp " ).append( std::to_string( size ) ).append( "\n" );
    if ( mayGo )
    {
      mDeclared.push_back( Declared{
          .name = name, .begin = begin, .end = mText.size(), .owner = mOwner, .region = mRegions.size() - 1 } );
    }
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
    line( "iny" );
    line( through );
    line( "sta " + byteOf( to, 1 ) );
  }

  /// A value written through a pointer on the zero page, the index loaded into
  /// `Y` first so that an 8-bit value may wait in `A`.
  void storeIndirect( ir::StoreIndirect const& instruction )
  {
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
    forgetA();
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
  /// addition: every `bvc` the writer emits follows a shift.
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
    if ( mCarry != ( add ? Carry::SET : Carry::CLEAR ) )
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
      if ( inA( instruction.operand ) ||
           ( std::holds_alternative<ir::Object>( instruction.operand ) && holdsInA( named( instruction.operand ) ) ) )
      {
        line( "eor #255" );
        line( "clc" );
        line( "adc #1" );
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
    return commutes( instruction.op ) && std::holds_alternative<ir::Object>( second ) && holdsInA( named( second ) ) &&
           !inA( first ) && !holdsInA( named( first ) );
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
    if ( instruction.op == ir::BinaryOperator::SHIFT_RIGHT &&
         ir::sizeOf( ir::typeOf( instruction.left, *mFunction ) ) == 2 )
    {
      std::int64_t const count = std::get<ir::Constant>( instruction.right ).value;
      line( "lda " + named( instruction.left, 8 ) );
      for ( std::int64_t done = 8; done < count; ++done )
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
        line( "clc" );
        line( "adc " + named( *second ) );
      }
      break;
    case ir::BinaryOperator::SUBTRACT:
      if ( !foldedCarry( false, *second ) )
      {
        line( "sec" );
        line( "sbc " + named( *second ) );
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
    std::string const to = location( instruction.result );
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
    for ( std::uint32_t index = 0; index < 2; ++index )
    {
      std::string first = named( *left, 8 * index );
      std::string second = named( *right, 8 * index );
      // The byte `A` holds already goes first, where the operator commutes.
      if ( commutes( instruction.op ) && holdsInA( second ) && !holdsInA( first ) )
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
            line( "sta " + byteOf( to, index ) );
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
          line( "sta " + byteOf( to, index ) );
        }
        continue;
      }
      line( "lda " + first );
      line( std::string{ mnemonic } + " " + second );
      line( "sta " + byteOf( to, index ) );
    }
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

  /// A shift by a count held in a `u8`: the count copied to a scratch byte of
  /// its own and counted down, one shift a round. Neither the count nor the
  /// shifts touch the overflow flag, so a branch on it clear closes the loop.
  void shiftByValue( ir::Binary const& instruction )
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
      // Less where the subtraction's sign and overflow differ. Adding $40 to
      // those two bits of the status leaves the top bit set exactly then, and
      // shifts it into carry.
      line( "sec" );
      line( "sbc " + named( *second ) );
      line( "php" );
      line( "pla" );
      line( "and #192" );
      line( "clc" );
      line( "adc #64" );
      line( "asl" );
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
      line( "eor " + named( instruction.right, 8 ) );
      label( differ );
      line( "cmp #1" );
      invert = instruction.op == ir::Comparison::EQUAL;
    }
    else
    {
      subtractWide( instruction.left, instruction.right );
      if ( ir::isSigned( instruction.type ) )
      {
        line( "php" );
        line( "pla" );
        line( "and #192" );
        line( "clc" );
        line( "adc #64" );
        line( "asl" );
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

  diag::SourceManager const* mSources;
  std::string mSourcePath;

  /// What the Target's processor is, and what the program asked to be
  /// optimised for: `stz` needs both, being the 65SC02's and a trade — see
  /// docs/decisions/0183-a-zero-stored-without-the-accumulator.md.
  Cpu mCpu = Cpu::MOS6502;
  Intent mIntent = Intent::FIT;
  std::string mText;

  ir::Function const* mFunction = nullptr;
  std::optional<diag::SourceLocation> mMarked;
  std::uint32_t mLocalLabels = 0;

  /// The bytes of each scratch name of the function being written: one, or a
  /// pair for a 16-bit value.
  std::vector<std::uint32_t> mScratchSizes;

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
  };

  std::vector<std::optional<BitTest>> mBitTest;

  /// The blocks entered with `A` holding what their first instruction, a
  /// shift left of a byte, computes: the test before them wrote it.
  std::vector<bool> mShiftedIn;

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

/// Recursion refused on the program's call graph: each cycle once, at the
/// call closing it that stands last in the order the files were given, with
/// the cycle named — see docs/decisions/0082-a-call-writes-the-callees-bytes.md.
/// The graph's components are found by Tarjan's algorithm, run on a stack of
/// its own so that a long chain of calls costs no depth of the tool's.
/// A member of a function type that calls its own type — through a pointer of
/// it, or by naming another member — writes the Temporaries it is reading
/// itself. The copy 0065 proposed does not save it: the type's jump reaches
/// every member, so the call may come back to this one and write the copy too,
/// which the recursion rule then refuses. The owner gave the shape up rather
/// than keep it — see docs/decisions/0104-handlers-stop-at-one-dispatch.md — so
/// it is refused here, where the finding can name it.
void refuseCallsOfOwnType( std::vector<CallEdge> const& edges, Membership const& members )
{
  auto const typeOf = [&members]( Definition const* function ) -> std::optional<std::uint32_t>
  {
    auto const found = members.ofFunction.find( function );
    return found == members.ofFunction.end() ? std::nullopt : std::optional{ found->second };
  };
  auto const refuse = []( diag::DiagnosticSink* sink, diag::SourceSpan span )
  {
    if ( sink != nullptr )
    {
      sink->add( diagnostic( diag::DiagnosticId::C_CALLS_OWN_TYPE ).at( span.begin, span.length ) );
    }
  };
  for ( Membership::CallOfType const& call : members.callsOfType )
  {
    if ( typeOf( call.caller ) == call.type )
    {
      refuse( call.sink, call.span );
    }
  }
  for ( CallEdge const& edge : edges )
  {
    std::optional<std::uint32_t> const caller = typeOf( edge.caller );
    if ( caller.has_value() && caller == typeOf( edge.callee ) )
    {
      refuse( edge.sink, edge.span );
    }
  }
}

void refuseRecursion( diag::SourceManager const& sources, std::vector<CallEdge> const& edges )
{
  std::map<Definition const*, std::uint32_t> numbers;
  std::vector<Definition const*> functions;
  auto const number = [&]( Definition const* function )
  {
    auto const [found, added] = numbers.try_emplace( function, static_cast<std::uint32_t>( functions.size() ) );
    if ( added )
    {
      functions.push_back( function );
    }
    return found->second;
  };
  std::vector<std::vector<std::size_t>> outgoing;
  for ( std::size_t index = 0; index < edges.size(); ++index )
  {
    std::uint32_t const from = number( edges[index].caller );
    number( edges[index].callee );
    outgoing.resize( functions.size() );
    outgoing[from].push_back( index );
  }
  outgoing.resize( functions.size() );

  constexpr std::uint32_t unvisited = UINT32_MAX;
  std::vector<std::uint32_t> order( functions.size(), unvisited );
  std::vector<std::uint32_t> lowest( functions.size(), 0 );
  std::vector<std::uint32_t> component( functions.size(), unvisited );
  std::vector<bool> stacked( functions.size(), false );
  std::vector<std::uint32_t> stack;
  std::uint32_t visited = 0;
  std::uint32_t components = 0;

  struct Frame
  {
    std::uint32_t function = 0;
    std::size_t next = 0;
  };

  for ( std::uint32_t root = 0; root < functions.size(); ++root )
  {
    if ( order[root] != unvisited )
    {
      continue;
    }
    std::vector<Frame> frames{ Frame{ .function = root, .next = 0 } };
    order[root] = lowest[root] = visited++;
    stack.push_back( root );
    stacked[root] = true;
    while ( !frames.empty() )
    {
      std::uint32_t const at = frames.back().function;
      if ( frames.back().next < outgoing[at].size() )
      {
        std::uint32_t const to = numbers.at( edges[outgoing[at][frames.back().next++]].callee );
        if ( order[to] == unvisited )
        {
          order[to] = lowest[to] = visited++;
          stack.push_back( to );
          stacked[to] = true;
          frames.push_back( Frame{ .function = to, .next = 0 } );
        }
        else if ( stacked[to] )
        {
          lowest[at] = std::min( lowest[at], order[to] );
        }
        continue;
      }
      if ( lowest[at] == order[at] )
      {
        std::uint32_t member = unvisited;
        do
        {
          member = stack.back();
          stack.pop_back();
          stacked[member] = false;
          component[member] = components;
        } while ( member != at );
        ++components;
      }
      frames.pop_back();
      if ( !frames.empty() )
      {
        lowest[frames.back().function] = std::min( lowest[frames.back().function], lowest[at] );
      }
    }
  }

  // The call that closes each cycle: of the calls inside a component, the one
  // written last.
  std::vector<std::optional<std::size_t>> closing( components );
  for ( std::size_t index = 0; index < edges.size(); ++index )
  {
    std::uint32_t const from = component[numbers.at( edges[index].caller )];
    if ( from != component[numbers.at( edges[index].callee )] )
    {
      continue;
    }
    std::optional<std::size_t>& chosen = closing[from];
    auto const place = []( CallEdge const& edge ) { return std::pair{ edge.unit, edge.span.begin.rawOffset() }; };
    if ( !chosen.has_value() || place( edges[*chosen] ) < place( edges[index] ) )
    {
      chosen = index;
    }
  }

  for ( std::optional<std::size_t> const& chosen : closing )
  {
    if ( !chosen.has_value() )
    {
      continue;
    }
    CallEdge const& edge = edges[*chosen];
    std::uint32_t const from = numbers.at( edge.caller );
    std::uint32_t const to = numbers.at( edge.callee );

    // The way back from the callee to the caller, inside the component, by
    // the fewest calls.
    std::vector<std::optional<std::uint32_t>> previous( functions.size() );
    std::vector<bool> reached( functions.size(), false );
    std::vector<std::uint32_t> frontier{ to };
    reached[to] = true;
    for ( std::size_t head = 0; head < frontier.size() && !reached[from]; ++head )
    {
      for ( std::size_t const index : outgoing[frontier[head]] )
      {
        std::uint32_t const next = numbers.at( edges[index].callee );
        if ( !reached[next] && component[next] == component[from] )
        {
          reached[next] = true;
          previous[next] = frontier[head];
          frontier.push_back( next );
        }
      }
    }
    std::vector<std::uint32_t> path{ from };
    for ( std::optional<std::uint32_t> step = previous[from]; step.has_value() && from != to; step = previous[*step] )
    {
      path.push_back( *step );
      if ( *step == to )
      {
        break;
      }
    }
    std::ranges::reverse( path );
    std::string cycle = "`" + std::string{ sources.textOf( edge.caller->name.span() ) } + "`";
    for ( std::uint32_t const function : path )
    {
      cycle += " -> `" + std::string{ sources.textOf( functions[function]->name.span() ) } + "`";
    }
    edge.sink->add(
        diagnostic( diag::DiagnosticId::C_RECURSION ).at( edge.span.begin, edge.span.length ).arg( "cycle", cycle ) );
  }
}

} // namespace

namespace
{

/// A jump or a branch to a block that holds nothing and only jumps on goes
/// where that block goes, and a block nothing reaches then is gone, the rest
/// keeping their order: `if (...) break;` branches to the loop's exit rather
/// than over a `jmp` to it — see
/// docs/decisions/0152-a-jump-to-a-jump-goes-where-it-goes.md. Not in a
/// function with `[[with]]` blocks, whose regions are numbered by block.
void threadJumps( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    auto* const function = std::get_if<ir::Function>( &definition );
    if ( function == nullptr || function->blocks.empty() || !function->withs.empty() )
    {
      continue;
    }
    std::vector<ir::Block>& blocks = function->blocks;
    // The entry is never passed through, since the Proc begins there; and a
    // chain of empty blocks ends in at most as many steps as there are blocks,
    // or is a loop that does nothing forever, which keeps its block.
    auto const landing = [&blocks]( std::uint32_t at )
    {
      for ( std::size_t steps = 0;
            steps < blocks.size() && at != 0 && blocks[at].instructions.empty() &&
            blocks[at].terminator.kind == ir::TerminatorKind::JUMP && blocks[at].terminator.target != at;
            ++steps )
      {
        at = blocks[at].terminator.target;
      }
      return at;
    };
    for ( ir::Block& block : blocks )
    {
      if ( block.terminator.kind == ir::TerminatorKind::JUMP )
      {
        block.terminator.target = landing( block.terminator.target );
      }
      else if ( block.terminator.kind == ir::TerminatorKind::BRANCH )
      {
        block.terminator.target = landing( block.terminator.target );
        block.terminator.otherwise = landing( block.terminator.otherwise );
      }
      else if ( block.terminator.kind == ir::TerminatorKind::DISPATCH )
      {
        for ( std::uint32_t& into : block.terminator.targets )
        {
          into = landing( into );
        }
      }
    }
    std::vector<bool> reached( blocks.size(), false );
    std::vector<std::uint32_t> pending{ 0 };
    while ( !pending.empty() )
    {
      std::uint32_t const at = pending.back();
      pending.pop_back();
      if ( reached[at] )
      {
        continue;
      }
      reached[at] = true;
      ir::Terminator const& end = blocks[at].terminator;
      if ( end.kind == ir::TerminatorKind::JUMP || end.kind == ir::TerminatorKind::BRANCH )
      {
        pending.push_back( end.target );
      }
      if ( end.kind == ir::TerminatorKind::BRANCH )
      {
        pending.push_back( end.otherwise );
      }
      for ( std::uint32_t const into : end.targets )
      {
        pending.push_back( into );
      }
    }
    std::vector<std::uint32_t> renumbered( blocks.size(), 0 );
    std::vector<ir::Block> kept;
    for ( std::uint32_t at = 0; at < blocks.size(); ++at )
    {
      if ( reached[at] )
      {
        renumbered[at] = static_cast<std::uint32_t>( kept.size() );
        kept.push_back( std::move( blocks[at] ) );
      }
    }
    for ( ir::Block& block : kept )
    {
      block.terminator.target = renumbered[block.terminator.target];
      block.terminator.otherwise = renumbered[block.terminator.otherwise];
      for ( std::uint32_t& into : block.terminator.targets )
      {
        into = renumbered[into];
      }
    }
    blocks = std::move( kept );
  }
}

/// The names of a unit whose bytes are the Target's registers: a constant
/// pointer aimed into one, `const u8* const VCOUNT = (u8*)0xD40B`, which C
/// reads and writes through its name — see docs/decisions/0151-volatile.md.
void markVolatiles( ir::Unit& unit, std::span<AddressRange const> registers )
{
  auto const inRegister = [registers]( std::int64_t address )
  {
    return std::ranges::any_of(
        registers,
        [address]( AddressRange const& range )
        { return std::cmp_greater_equal( address, range.begin ) && std::cmp_less( address, range.end ); } );
  };
  auto const mark = [&]( ir::NamedConstant const& constant )
  {
    if ( constant.value.type == ir::Type::POINTER && constant.value.name.empty() && inRegister( constant.value.value ) )
    {
      unit.volatiles.insert( constant.name );
    }
  };
  // A register the Target names, which C reads and writes by that name —
  // `PORTB`, or an element of `io`.
  for ( AddressRange const& range : registers )
  {
    if ( !range.name.empty() )
    {
      unit.volatiles.insert( range.name );
    }
  }
  for ( ir::Definition const& definition : unit.definitions )
  {
    if ( auto const* const constant = std::get_if<ir::NamedConstant>( &definition ) )
    {
      mark( *constant );
    }
    else if ( auto const* const function = std::get_if<ir::Function>( &definition ) )
    {
      for ( ir::NamedConstant const& local : function->constants )
      {
        mark( local );
      }
    }
  }
}

} // namespace

std::vector<std::optional<ir::Unit>> lower( diag::SourceManager const& sources,
                                            std::span<SourceFile const> files,
                                            std::span<ExternalName const> externals,
                                            diag::DiagnosticSink& sink,
                                            std::span<AddressRange const> registers,
                                            Intent intent )
{
  // Deques, since a Unit points at its sink and a vector would move them. A
  // vector of Units would also copy them as it grows wherever moving one may
  // throw, as it may with MSVC's std::map, and a syntax tree has no copy.
  std::deque<diag::DiagnosticSink> found;
  std::deque<Unit> units;
  for ( SourceFile const& file : files )
  {
    diag::DiagnosticSink& own = found.emplace_back( sink.policy() );
    units.push_back(
        Unit{ .source = &file, .tree = parse( sources, file.file, own ), .sink = &own, .definitions = {} } );
  }

  // Every signature of the program first, a file that did not parse
  // included: what it did parse is what it defines, and leaving it out would
  // report its names as undeclared in every other file.
  Types types;
  for ( Unit& unit : units )
  {
    gatherSignatures( sources, unit, types );
  }
  resolveTypeNames( sources, units, types );
  Names names{ sources, units, externals, std::move( types ) };

  // A cycle among constants is reported with what the check finds, and does
  // not keep the file from being checked.
  std::vector<bool> checked;
  checked.reserve( units.size() );
  for ( Unit const& unit : units )
  {
    checked.push_back( !unit.sink->hasErrors() );
  }
  settleConstants( sources, units, names );
  settleExterns( units, names );

  // A file that did not parse is not checked: what the compiler would say
  // about half a tree is noise beside the error that cut it short.
  std::vector<CallEdge> edges;
  std::vector<std::set<std::uint32_t>> addressed( units.size() );
  Membership members;
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    if ( checked[index] )
    {
      Checker{ sources, names, units[index], index, edges, addressed[index], members }.check();
    }
  }
  refuseRecursion( sources, edges );
  refuseCallsOfOwnType( edges, members );

  // Every Unit is lowered before any pass runs over one, so that a pass with
  // the whole program in front of it has an IR of every Unit to read. That is
  // what the gap between this loop and the next is for: wrapping a call into
  // its caller is one — see
  // docs/decisions/0172-a-call-with-one-site-is-wrapped.md — and it belongs
  // there and not after the per-Unit passes, since the larger half of what it
  // is worth is that they then see through the body it spliced.
  std::vector<std::optional<ir::Unit>> lowered( units.size() );
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    Unit const& unit = units[index];
    if ( unit.sink->hasErrors() )
    {
      continue;
    }
    ir::Unit one = Lowering{ sources, names, unit, *unit.sink, addressed[index], members }.translationUnit( unit.tree );
    markVolatiles( one, registers );
    if ( !unit.sink->hasErrors() )
    {
      lowered[index] = std::move( one );
    }
  }

  std::vector<diag::DiagnosticSink*> sinks;
  sinks.reserve( units.size() );
  for ( Unit const& unit : units )
  {
    sinks.push_back( unit.sink );
  }
  // Before any pass moves a store or drops one: what the source says about a
  // `static` local read before anything writes it — see
  // docs/decisions/0212-a-static-local-read-before-it-is-written.md.
  for ( std::size_t index = 0; index < lowered.size(); ++index )
  {
    if ( lowered[index].has_value() && sinks[index] != nullptr )
    {
      checkStaticsAreAssigned( *lowered[index], *sinks[index] );
    }
  }

  wrapCalls( lowered, sinks, intent == Intent::SPEED );

  // The passes over the finished IR, one Unit at a time: what a loop computes
  // the same at every turn moved before it, then what the subset's own
  // widening rules put in two bytes and only one of them needs, the runtime's
  // Procs chosen for what the operands turn out to be, then the stores that
  // nothing reads, and last — since what it looks for is a store that
  // survived all four — the local a function returns becoming its result.
  for ( std::optional<ir::Unit>& one : lowered )
  {
    if ( !one.has_value() )
    {
      continue;
    }
    foldConstants( *one );
    hoistInvariants( *one );
    walkPointers( *one );
    reuseLoads( *one );
    narrowRanges( *one );
    narrowIndexes( *one );
    narrowRuntimeCalls( *one );
    dropDeadStores( *one );
    mergeReturnedLocals( *one );
  }

  // Last of all, and over the whole program: which functions leave their
  // result in a parameter's byte, which is part of the Signature every caller
  // reads — see docs/decisions/0120-a-function-returns-through-a-parameter.md.
  returnThroughParameters( lowered );

  for ( diag::DiagnosticSink& own : found )
  {
    sink.merge( std::move( own ) );
  }
  return lowered;
}

namespace
{

/// Whether a function may carry a parameter or its result in a register: one
/// every call of which is a `jsr` from C that loads what it declares — not a
/// member of a function type, whose bytes are the type's, not a Slot's, not
/// one a `switch` or a `[[with]]` reaches the bytes of, not one called through
/// a `.with`, which may write `A` on the way, and not one entered by falling
/// into it.
bool mayPlace( ir::Function const& function, std::set<std::string> const& fallenInto )
{
  if ( function.isTrampoline || !function.memberOf.empty() || !function.implements.empty() || !function.pane.empty() ||
       !function.under.empty() || !function.then.empty() || !function.withs.empty() ||
       fallenInto.contains( function.name ) || function.blocks.empty() )
  {
    return false;
  }
  for ( ir::Block const& block : function.blocks )
  {
    for ( ir::Instruction const& instruction : block.instructions )
    {
      if ( std::holds_alternative<ir::Switch>( instruction.operation ) ||
           std::holds_alternative<ir::EnterWith>( instruction.operation ) )
      {
        return false;
      }
    }
  }
  return true;
}

/// The parameter a byte of which `A` would be loaded with first: the first
/// object the first block reads into `A`, where that is a parameter of one
/// byte.
ir::Local* firstReadIntoA( ir::Function& function )
{
  auto const parameterNamed = [&function]( std::string const& name ) -> ir::Local*
  {
    auto const found = std::ranges::find_if( function.parameters,
                                             [&name]( ir::Local const& parameter ) { return parameter.name == name; } );
    return found == function.parameters.end() ? nullptr : &*found;
  };
  for ( ir::Instruction const& instruction : function.blocks.front().instructions )
  {
    std::vector<ir::Operand const*> read;
    if ( auto const* const binary = std::get_if<ir::Binary>( &instruction.operation ) )
    {
      read = { &binary->left, &binary->right };
    }
    else if ( auto const* const compare = std::get_if<ir::Compare>( &instruction.operation ) )
    {
      read = { &compare->left, &compare->right };
    }
    else if ( auto const* const unary = std::get_if<ir::Unary>( &instruction.operation ) )
    {
      read = { &unary->operand };
    }
    else if ( auto const* const convert = std::get_if<ir::Convert>( &instruction.operation ) )
    {
      read = { &convert->operand };
    }
    else if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation ) )
    {
      read = { &store->value };
    }
    for ( ir::Operand const* operand : read )
    {
      if ( auto const* const object = std::get_if<ir::Object>( operand ) )
      {
        ir::Local* const parameter = parameterNamed( object->name );
        bool const byte =
            parameter != nullptr && parameter->type != ir::Type::BLOCK && ir::sizeOf( parameter->type ) == 1;
        return byte ? parameter : nullptr;
      }
    }
    if ( !read.empty() )
    {
      return nullptr;
    }
  }
  return nullptr;
}

/// The parameter of one byte the function first indexes an element by.
ir::Local* firstIndex( ir::Function& function, ir::Local const* besides )
{
  for ( ir::Block const& block : function.blocks )
  {
    for ( ir::Instruction const& instruction : block.instructions )
    {
      ir::Operand const* index = nullptr;
      if ( auto const* const load = std::get_if<ir::Load>( &instruction.operation ); load != nullptr && !load->scaled )
      {
        index = &load->index;
      }
      else if ( auto const* const store = std::get_if<ir::StoreElement>( &instruction.operation );
                store != nullptr && !store->scaled )
      {
        index = &store->index;
      }
      auto const* const object = index != nullptr ? std::get_if<ir::Object>( index ) : nullptr;
      if ( object == nullptr )
      {
        continue;
      }
      for ( ir::Local& parameter : function.parameters )
      {
        if ( parameter.name == object->name && &parameter != besides && parameter.type != ir::Type::BLOCK &&
             ir::sizeOf( parameter.type ) == 1 )
        {
          return &parameter;
        }
      }
    }
  }
  return nullptr;
}

/// Whether every call of the function that reads its result stores it whole
/// into an object right after, which a pair whose high byte is in `A` does in
/// as many instructions as before, less the callee's store: a caller that
/// reads the pair as an operand where it lies would first have to set it
/// down — see docs/decisions/0145-an-argument-in-a-register.md.
template <typename Resolve>
bool storedWhole( std::vector<std::optional<ir::Unit>> const& units,
                  Resolve const& resolve,
                  ir::Function const& callee )
{
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    if ( !units[index].has_value() )
    {
      continue;
    }
    for ( ir::Definition const& definition : units[index]->definitions )
    {
      auto const* const caller = std::get_if<ir::Function>( &definition );
      if ( caller == nullptr )
      {
        continue;
      }
      for ( ir::Block const& block : caller->blocks )
      {
        for ( std::size_t at = 0; at < block.instructions.size(); ++at )
        {
          auto const* const call = std::get_if<ir::Call>( &block.instructions[at].operation );
          auto const where = call != nullptr ? resolve( index, call->name ) : decltype( resolve( index, "" ) ){};
          if ( !where.has_value() || where->function != &callee || !call->result.has_value() )
          {
            continue;
          }
          auto const* const store = at + 1 < block.instructions.size()
                                        ? std::get_if<ir::Store>( &block.instructions[at + 1].operation )
                                        : nullptr;
          if ( store == nullptr || !isValue( store->value, *call->result ) || ir::sizeOf( store->type ) != 2 ||
               readersOf( *caller, *call->result ) != 1 )
          {
            return false;
          }
        }
      }
    }
  }
  return true;
}

} // namespace

/// Where each function of C carries its parameters and its result, chosen
/// from its own text, callees before their callers so that the calls in a
/// function's text are written as they will be — and then every call of it
/// told. A parameter goes in `A` where the first block reads it into `A`
/// first, or in `X` where an element is indexed by it, and only where its
/// byte is then named by nothing; the result stays in `A` where `A` holds it
/// at every `rts` — see docs/decisions/0145-an-argument-in-a-register.md.
void choosePlaces( diag::SourceManager const& sources,
                   std::span<SourceFile const> files,
                   std::vector<std::optional<ir::Unit>>& units )
{
  struct Where
  {
    std::size_t unit = 0;
    ir::Function* function = nullptr;
  };

  std::map<std::string, Where> exported;
  std::vector<std::map<std::string, ir::Function*>> own( units.size() );
  std::set<std::string> fallenInto;
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    if ( !units[index].has_value() )
    {
      continue;
    }
    for ( ir::Definition& definition : units[index]->definitions )
    {
      if ( auto* const function = std::get_if<ir::Function>( &definition ) )
      {
        own[index][function->name] = function;
        if ( !function->isStatic )
        {
          exported[function->name] = Where{ .unit = index, .function = function };
        }
        if ( !function->then.empty() )
        {
          fallenInto.insert( function->then );
        }
      }
    }
  }
  auto const resolve = [&]( std::size_t unit, std::string const& name ) -> std::optional<Where>
  {
    if ( auto const found = own[unit].find( name ); found != own[unit].end() )
    {
      return Where{ .unit = unit, .function = found->second };
    }
    if ( auto const found = exported.find( name ); found != exported.end() )
    {
      return found->second;
    }
    return std::nullopt;
  };

  // Callees first: the call graph has no cycle, since recursion is refused.
  // A function no call of C reaches is called from the assembler or not at
  // all, and keeps its bytes: nothing of C would gain, and an assembler
  // caller would be refused for what it writes.
  std::vector<Where> order;
  std::set<ir::Function const*> seen;
  std::set<ir::Function const*> calledFromC;
  std::function<void( Where )> visit = [&]( Where where )
  {
    if ( !seen.insert( where.function ).second )
    {
      return;
    }
    for ( ir::Block const& block : where.function->blocks )
    {
      for ( ir::Instruction const& instruction : block.instructions )
      {
        if ( auto const* const call = std::get_if<ir::Call>( &instruction.operation ) )
        {
          if ( std::optional<Where> const callee = resolve( where.unit, call->name ); callee.has_value() )
          {
            calledFromC.insert( callee->function );
            visit( *callee );
          }
        }
      }
    }
    order.push_back( where );
  };
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    for ( auto const& [name, function] : own[index] )
    {
      visit( Where{ .unit = index, .function = function } );
    }
  }

  for ( Where const& where : order )
  {
    ir::Function& function = *where.function;
    if ( !calledFromC.contains( where.function ) || !mayPlace( function, fallenInto ) )
    {
      continue;
    }
    // Not the parameter whose byte is also the result's: the callers read
    // the result there after the call — see
    // docs/decisions/0120-a-function-returns-through-a-parameter.md.
    ir::Local* inX = firstIndex( function, nullptr );
    if ( inX != nullptr && inX->name == function.resultByte )
    {
      inX = nullptr;
    }
    if ( inX != nullptr )
    {
      inX->place = "x";
    }

    // For `A`, the parameter the first block reads into it first, and then
    // every other byte in turn: one the body reads only as the operand of an
    // operator that commutes is found in `A` there too, `adc a` in place of
    // `lda a / adc b` — see
    // docs/decisions/0147-the-operand-a-holds-goes-first.md.
    std::vector<ir::Local*> candidates;
    if ( ir::Local* const first = firstReadIntoA( function ); first != nullptr && first != inX )
    {
      candidates.push_back( first );
    }
    for ( ir::Local& parameter : function.parameters )
    {
      bool const byte = parameter.type != ir::Type::BLOCK && ir::sizeOf( parameter.type ) == 1;
      if ( byte && &parameter != inX && std::ranges::find( candidates, &parameter ) == candidates.end() )
      {
        candidates.push_back( &parameter );
      }
    }
    std::erase_if( candidates,
                   [&function]( ir::Local const* parameter ) { return parameter->name == function.resultByte; } );
    auto const facts = [&]
    { return Writer{ sources, files[where.unit].path }.placeFacts( function, units[where.unit]->volatiles ); };
    std::optional<Writer::PlaceFacts> found;
    for ( ir::Local* const candidate : candidates )
    {
      candidate->place = "a";
      found = facts();
      if ( !Writer::namedIn( found->body, candidate->name ) )
      {
        break;
      }
      candidate->place.clear();
      found.reset();
    }
    if ( !found.has_value() )
    {
      found = facts();
    }
    if ( inX != nullptr && Writer::namedIn( found->body, inX->name ) )
    {
      inX->place.clear();
      found = facts();
    }

    // The result, where `A` holds it, or a pair's high byte, at every `rts`.
    Writer::PlaceFacts const& written = found.value();
    std::string const result{ RESULT };
    bool const scalar = function.result.has_value() && *function.result != ir::Type::BLOCK &&
                        function.resultByte.empty() && ir::sizeOf( *function.result ) <= 2;
    auto const heldEverywhere = [&written]( std::string const& name )
    {
      return !written.returns.empty() &&
             std::ranges::all_of( written.returns,
                                  [&name]( std::vector<std::string> const& held )
                                  { return std::ranges::find( held, name ) != held.end(); } );
    };
    if ( scalar && Writer::onlyStoredIn( written.body, result ) )
    {
      if ( ir::sizeOf( *function.result ) == 1 && heldEverywhere( result ) )
      {
        function.resultPlace = "a";
      }
      else if ( ir::sizeOf( *function.result ) == 2 && heldEverywhere( result + "+1" ) &&
                storedWhole( units, resolve, function ) )
      {
        function.resultPlace = "ma";
      }
    }

    // Every call of it, told.
    for ( std::size_t index = 0; index < units.size(); ++index )
    {
      if ( !units[index].has_value() )
      {
        continue;
      }
      for ( ir::Definition& definition : units[index]->definitions )
      {
        auto* const caller = std::get_if<ir::Function>( &definition );
        if ( caller == nullptr )
        {
          continue;
        }
        for ( ir::Block& block : caller->blocks )
        {
          for ( ir::Instruction& instruction : block.instructions )
          {
            auto* const call = std::get_if<ir::Call>( &instruction.operation );
            std::optional<Where> const callee = call != nullptr ? resolve( index, call->name ) : std::nullopt;
            if ( !callee.has_value() || callee->function != &function )
            {
              continue;
            }
            for ( ir::Argument& argument : call->arguments )
            {
              for ( ir::Local const& parameter : function.parameters )
              {
                if ( argument.name == function.name + "." + parameter.name )
                {
                  argument.place = parameter.place;
                }
              }
            }
            call->resultPlace = function.resultPlace;
          }
        }
      }
    }
  }
}

/// Every call of a Proc of the assembler told where its bytes are, the ones
/// the compiler writes itself included — a call of the runtime, and one
/// `Select` sent to another Proc of it — so that where the runtime's
/// `.declare`s keep a byte in a register, the call puts it there: see
/// docs/decisions/0146-the-runtime-in-registers.md.
void placeAssemblerCalls( std::vector<std::optional<ir::Unit>>& units, std::span<ExternalName const> externals )
{
  std::map<std::string, ExternalName const*> procs;
  for ( ExternalName const& name : externals )
  {
    if ( name.kind == ExternalKind::PROC )
    {
      procs[name.name] = &name;
    }
  }
  for ( std::optional<ir::Unit>& unit : units )
  {
    if ( !unit.has_value() )
    {
      continue;
    }
    for ( ir::Definition& definition : unit->definitions )
    {
      auto* const function = std::get_if<ir::Function>( &definition );
      if ( function == nullptr )
      {
        continue;
      }
      for ( ir::Block& block : function->blocks )
      {
        for ( ir::Instruction& instruction : block.instructions )
        {
          auto* const call = std::get_if<ir::Call>( &instruction.operation );
          auto const found = call != nullptr ? procs.find( call->name ) : procs.end();
          if ( found == procs.end() )
          {
            continue;
          }
          ExternalName const& proc = *found->second;
          for ( std::size_t index = 0; index < call->arguments.size() && index < proc.arguments.size(); ++index )
          {
            call->arguments[index].place = proc.arguments[index].place;
          }
          if ( std::optional<ExternalByte> const& result = proc.result; result.has_value() )
          {
            call->resultPlace = result.value().place;
          }
        }
      }
    }
  }
}

std::vector<std::optional<std::string>> compile( diag::SourceManager const& sources,
                                                 std::span<SourceFile const> files,
                                                 std::span<ExternalName const> externals,
                                                 diag::DiagnosticSink& sink,
                                                 std::span<AddressRange const> registers,
                                                 Intent intent,
                                                 Cpu cpu )
{
  std::vector<std::optional<ir::Unit>> lowered = lower( sources, files, externals, sink, registers, intent );
  // The blocks as the text will have them, a jump to a jump gone: after every
  // pass over the IR, which the blocks as the source wrote them serve.
  for ( std::optional<ir::Unit>& unit : lowered )
  {
    if ( unit.has_value() )
    {
      threadJumps( *unit );
    }
  }
  placeAssemblerCalls( lowered, externals );
  choosePlaces( sources, files, lowered );
  std::vector<std::optional<std::string>> texts( lowered.size() );
  std::size_t index = 0;
  for ( std::optional<ir::Unit> const& one : lowered )
  {
    if ( one.has_value() )
    {
      texts[index] = Writer{ sources, files[index].path, cpu, intent }.unit( *one );
    }
    ++index;
  }
  return texts;
}

} // namespace nga::c
