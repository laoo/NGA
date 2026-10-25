#pragma once

#include "nga/c/Compile.hpp"
#include "nga/c/Ir.hpp"
#include "nga/c/SyntaxTree.hpp"
#include "nga/c/Token.hpp"
#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"

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

// The compiler's front end, as the passes over one file share it: what a
// name is defined as and what it means where it is used, the types the
// program declares, and the attributes it writes. Not a contract: nothing
// outside the compiler includes it — see Compile.cpp for the order the
// passes run in.
namespace nga::c::detail
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
[[nodiscard]] inline SwitchForm formOf( std::vector<std::int64_t> const& values )
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

/// What a value of the subset is, apart from what it points at: a keyword's
/// type, or that of an `enum struct`, a `struct` or a `union`, or a function
/// type, which nothing is but through a pointer. Every declaration, place and
/// pointee holds its type as one of these, so that one is compared and copied
/// whole — see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md.
struct ValueType
{
  ir::Type type = ir::Type::U8;

  /// The `enum struct` it is of, its type then `u8`.
  std::optional<std::uint32_t> enumeration{};

  /// The `struct` or `union` it is of, its type then `block`.
  std::optional<std::uint32_t> aggregate{};

  /// The function type it is, among the program's: a type only as a pointee,
  /// a pointer to one being the only pointer to a function the subset has —
  /// see docs/decisions/0065-handlers.md and
  /// docs/decisions/0103-what-building-handlers-needs.md.
  std::optional<std::uint32_t> function{};

  friend bool operator==( ValueType const& left, ValueType const& right ) = default;
};

/// What a pointer points at: a type, and whether it is `const` or `volatile`
/// — see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#pointers--task-3d.
struct Pointee : ValueType
{
  bool isConst = false;

  /// Every read and store through the pointer is one in the text — see
  /// docs/decisions/0151-volatile.md.
  bool isVolatile = false;
};

inline Pointee pointeeOf( ValueType const& type, bool isConst = false, bool isVolatile = false )
{
  Pointee pointee;
  static_cast<ValueType&>( pointee ) = type;
  pointee.isConst = isConst;
  pointee.isVolatile = isVolatile;
  return pointee;
}

/// Whether two pointers point at one type, whatever either says of `const`
/// or `volatile`.
inline bool samePointee( Pointee const& left, Pointee const& right )
{
  return static_cast<ValueType const&>( left ) == static_cast<ValueType const&>( right );
}

/// A type as a declaration holds it: a value's, or a pointer's to one, whose
/// type is then `ptr`.
struct TypeRef : ValueType
{
  std::optional<Pointee> pointee{};
};

/// Whether two declarations hold one type, whatever a pointee says of
/// `const` or `volatile`.
inline bool sameType( TypeRef const& left, TypeRef const& right )
{
  return static_cast<ValueType const&>( left ) == static_cast<ValueType const&>( right ) &&
         left.pointee.has_value() == right.pointee.has_value() &&
         ( !left.pointee.has_value() || samePointee( *left.pointee, *right.pointee ) );
}

/// The type alone of anything that holds one.
inline TypeRef typeOf( TypeRef const& holder )
{
  return holder;
}

/// Gives `holder` the type `type`, and touches nothing else of it.
inline void giveType( TypeRef& holder, TypeRef const& type )
{
  holder = type;
}

/// The type of a pointer to `pointee`.
inline TypeRef pointerTo( Pointee const& pointee )
{
  TypeRef pointer;
  pointer.type = ir::Type::POINTER;
  pointer.pointee = pointee;
  return pointer;
}

/// The type of a value of `value`, which points at nothing: what a pointer
/// to it reaches.
inline TypeRef typeOfValue( ValueType const& value )
{
  TypeRef type;
  static_cast<ValueType&>( type ) = value;
  return type;
}

/// A type as a signature spells it: a keyword's, or an `enum struct`'s or an
/// aggregate's, whose name is resolved once every file has been gathered.
struct NamedType : TypeRef
{
  std::optional<Token> typeName{};
};

struct ParameterType
{
  Token name;
  NamedType type;

  /// Whether a name was written, which a declaration after `extern` may leave
  /// out.
  bool isNamed = true;
};

/// What a function takes and gives: its result, or nothing for `void`, and
/// its parameters in the order written. A function's, and a function type's,
/// which is the same thing written once — see docs/decisions/0065-handlers.md.
struct Signature
{
  std::optional<NamedType> result{};
  std::vector<ParameterType> parameters{};
};

/// Gives `holder` the signature `signature`, and touches nothing else of it.
inline void giveSignature( Signature& holder, Signature signature )
{
  holder = std::move( signature );
}

/// What a name of a file is defined as. Its type is an OBJECT's; a TYPE
/// holds the `enum struct`, aggregate or function type it is — of a function
/// type, the parameters and the result below are its own — and a FUNCTION
/// holds `u8`. A name in it is resolved once every file has been gathered.
/// A FUNCTION's signature, and a function type's, is its Signature.
struct Definition : NamedType, Signature
{
  NameKind kind = NameKind::FUNCTION;
  bool isStatic = false;
  Token name;

  /// An OBJECT written `const`, and the value an OBJECT is given where it is
  /// declared.
  bool isConst = false;
  Expression const* value = nullptr;

  /// An OBJECT that is an array: its size as written, and its number of
  /// elements where the list it is given decides it.
  bool isArray = false;
  Expression const* size = nullptr;
  std::optional<std::uint32_t> count{};

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
struct FunctionType : Signature
{
  std::string name;
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

/// What a name means where a statement uses it: its type, as a
/// Definition's is, and what else a statement asks of it.
struct Meaning : TypeRef
{
  NameKind kind = NameKind::FUNCTION;

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

  /// A parameter of the function being read, and where a local was declared,
  /// which names it among those whose address is taken.
  bool isParameter = false;
  std::optional<diag::SourceLocation> declaredAt{};

  /// An array written `[[striped]]`, laid out a member per table — see
  /// docs/decisions/0089-a-stripe-is-a-section.md.
  bool isStriped = false;

  /// What an `.asm` Module exports, for a name of the assembler's — see
  /// docs/decisions/0092-what-the-assembler-exports-is-typed-by-its-shape.md.
  ExternalName const* external = nullptr;

  /// An OBJECT written `volatile`, itself and not what it points at.
  bool isVolatile = false;
};

/// What one element of an array is: the same, and no array.
inline Meaning elementOf( Meaning array )
{
  array.isArray = false;
  return array;
}

/// Whether what a declaration declares never changes: `const`, and not
/// `volatile`, which something else may change.
inline bool isReadOnly( Declaration const& declared )
{
  return declared.isConst && !declared.isVolatile;
}

/// Whether a name is a Constant of the assembler that folds to no number C
/// knows: a `u16` written by its name.
inline bool isOpaqueConstant( Meaning const& meaning )
{
  return meaning.external != nullptr && meaning.external->kind == ExternalKind::CONSTANT &&
         !meaning.external->value.has_value();
}

/// Whether a name is a Constant of the assembler that folds, which is a
/// constant of C as a literal is.
inline bool isFoldedConstant( Meaning const& meaning )
{
  return meaning.definition == nullptr && meaning.external != nullptr &&
         meaning.external->kind == ExternalKind::CONSTANT && meaning.external->value.has_value();
}

/// Whether a name is a Constant of the assembler, typed by its shape or
/// declared: a value, and never a place.
inline bool isAssemblerConstant( Meaning const& meaning )
{
  return meaning.external != nullptr && meaning.external->kind == ExternalKind::CONSTANT;
}

/// Whether an object lies at an address the assembler knows: one at file
/// scope, of the program or of the assembler.
inline bool atFixedAddress( Meaning const& meaning )
{
  return meaning.definition != nullptr || meaning.external != nullptr;
}

inline ir::Type typeNamed( Keyword keyword )
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

inline NamedType namedType( Token const& token )
{
  NamedType named;
  if ( token.kind == TokenKind::IDENTIFIER )
  {
    named.typeName = token;
  }
  else
  {
    named.type = typeNamed( token.keyword );
  }
  return named;
}

/// A type as a signature spells it, or a pointer to it where `pointer` says.
inline NamedType namedType( Token const& token, bool pointer, bool pointeeConst, bool pointeeVolatile )
{
  NamedType named = namedType( token );
  if ( pointer )
  {
    named.pointee = pointeeOf( ValueType{ .type = named.type }, pointeeConst, pointeeVolatile );
    named.type = ir::Type::POINTER;
  }
  return named;
}

/// The name `token` spells, as nameSpelled reads it.
inline std::string spellingOf( diag::SourceManager const& sources, Token const& token )
{
  return nameSpelled( sources.textOf( token.span() ) );
}

/// The definition gathered for a function of `unit`, which is the one of its
/// name unless the name was defined twice, which has been reported.
inline Definition const*
definitionOf( diag::SourceManager const& sources, Unit const& unit, FunctionDefinition const& function )
{
  auto const found = unit.definitions.find( nameSpelled( sources.textOf( function.name.span() ) ) );
  if ( found == unit.definitions.end() || found->second.name.location != function.name.location )
  {
    return nullptr;
  }
  return &found->second;
}

inline std::int64_t lowestOf( ir::Type type )
{
  if ( !ir::isSigned( type ) )
  {
    return 0;
  }
  return -( std::int64_t{ 1 } << ( ( 8 * ir::sizeOf( type ) ) - 1 ) );
}

inline std::int64_t highestOf( ir::Type type )
{
  if ( type == ir::Type::BOOL )
  {
    return 1;
  }
  std::int64_t const span = std::int64_t{ 1 } << ( 8 * ir::sizeOf( type ) );
  return ir::isSigned( type ) ? ( span / 2 ) - 1 : span - 1;
}

using ir::wrapped;

inline bool isShift( TokenKind kind )
{
  return kind == TokenKind::LESS_LESS || kind == TokenKind::GREATER_GREATER;
}

inline bool isComparison( TokenKind kind )
{
  return kind == TokenKind::LESS || kind == TokenKind::GREATER || kind == TokenKind::LESS_EQUAL ||
         kind == TokenKind::GREATER_EQUAL || kind == TokenKind::EQUAL_EQUAL || kind == TokenKind::BANG_EQUAL;
}

inline bool isLogical( TokenKind kind )
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
inline std::int64_t folded( Expression const& node, std::optional<ir::Type> type, NameValue const& valueOf )
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
    // folded. By -1 it is the negation, which wraps where the host's division
    // would trap: the one quotient that does not fit is the type's to wrap.
    if ( right == -1 )
    {
      return static_cast<std::int64_t>( 0 - l );
    }
    return right == 0 ? 0 : static_cast<std::int64_t>( left / right );
  case TokenKind::PERCENT:
    return right == 0 || right == -1 ? 0 : static_cast<std::int64_t>( left % right );
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
struct Shape : TypeRef
{
  bool isArray = false;
  std::uint32_t count = 1;
};

/// The bytes one value of a type takes.
inline std::uint32_t bytesOf( ir::Type type, std::optional<std::uint32_t> aggregate, AggregateOf const& aggregateOf )
{
  if ( type == ir::Type::BLOCK && aggregate.has_value() )
  {
    return aggregateOf( *aggregate ).size;
  }
  return ir::sizeOf( type );
}

/// The bytes a place takes: its type's, times its elements.
inline std::uint32_t bytesOf( Shape const& shape, AggregateOf const& aggregateOf )
{
  return bytesOf( shape.type, shape.aggregate, aggregateOf ) * ( shape.isArray ? shape.count : 1 );
}

/// The shape of a member.
inline Shape shapeOf( MemberType const& member )
{
  return Shape{ typeOf( member.type ), member.isArray, member.count };
}

/// The member of a `struct` or a `union` that `name` names, or nothing.
inline MemberType const*
memberNamed( AggregateType const& aggregate, std::string_view name, diag::SourceManager const& sources )
{
  auto const found = std::ranges::find_if(
      aggregate.members, [&]( MemberType const& member ) { return sources.textOf( member.name.span() ) == name; } );
  return found == aggregate.members.end() ? nullptr : &*found;
}

/// The shape of what a name, an element, a pointee or a member is, where
/// the names in it name what they must.
inline std::optional<Shape> shapeOf( Expression const& node,
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
    return Shape{ typeOf( *meaning ), meaning->isArray, meaning->count.value_or( 0 ) };
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
    return Shape{ typeOfValue( *outer->pointee ), false, 1 };
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
inline std::optional<std::int64_t> sizeCounted( Expression const& node,
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
  /// where it is used — and for a `?:` whose ways are both constants, which
  /// takes it so too, though a condition decides it where the program runs —
  /// see docs/decisions/0169-a-conditional-operator.md.
  std::optional<ir::Type> type;

  /// A comparison its constant decides: the outcome it always has.
  std::optional<bool> decided;

  /// The `enum struct` a value is of, whose type is then `u8`.
  std::optional<std::uint32_t> enumeration;

  /// Whether its value is folded where it is compiled: a constant, or a
  /// typed expression of constants — a cast of one, or computed from one.
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
};

inline Typed refused()
{
  return Typed{
    .valid = false, .type = std::nullopt, .decided = std::nullopt, .enumeration = std::nullopt, .known = false
  };
}

inline Typed constant()
{
  return Typed{
    .valid = true, .type = std::nullopt, .decided = std::nullopt, .enumeration = std::nullopt, .known = true
  };
}

inline Typed typed( ir::Type type )
{
  return Typed{ .valid = true, .type = type, .decided = std::nullopt, .enumeration = std::nullopt, .known = false };
}

inline Typed enumerated( std::uint32_t enumeration )
{
  return Typed{
    .valid = true, .type = ir::Type::U8, .decided = std::nullopt, .enumeration = enumeration, .known = false
  };
}

/// A value of a `struct` or a `union`.
inline Typed blockOf( std::uint32_t aggregate )
{
  Typed result = typed( ir::Type::BLOCK );
  result.aggregate = aggregate;
  return result;
}

/// A pointer to `pointee`.
inline Typed pointing( Pointee const& pointee )
{
  Typed result = typed( ir::Type::POINTER );
  result.pointee = pointee;
  return result;
}

/// What a value of `type` is.
inline Typed typedAs( TypeRef const& type )
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

/// What a pointer to `pointee` reaches, `const` where the pointee is.
inline Typed reached( Pointee const& pointee )
{
  Typed result = typedAs( typeOfValue( pointee ) );
  result.readOnly = pointee.isConst;
  result.isVolatile = pointee.isVolatile;
  result.throughPointer = true;
  return result;
}

/// The type a value of `kind` is of, as an object holding it would be
/// declared: `u8` for a constant, which takes the type of where it is used.
inline TypeRef typeOfTyped( Typed const& kind )
{
  TypeRef type;
  type.type = kind.type.value_or( ir::Type::U8 );
  type.enumeration = kind.enumeration;
  type.aggregate = kind.aggregate;
  type.pointee = kind.pointee;
  return type;
}

/// Whether an expression's value is known where it is compiled: a constant,
/// or a typed expression of constants.
inline bool isKnown( Typed const& kind )
{
  return kind.known;
}

/// Whether a value is one bytes laid down before the program runs may hold,
/// as a `static` object's: a value known here, an address the assembler
/// knows, or `nullptr`.
inline bool isLaidDown( Typed const& kind )
{
  return isKnown( kind ) || kind.address || kind.isNull;
}

/// The wider of two types, the first where they are as wide.
inline ir::Type widerOf( ir::Type left, ir::Type right )
{
  return ir::sizeOf( right ) > ir::sizeOf( left ) ? right : left;
}

/// The one name an attribute is written with — `[[in(NAME)]]`,
/// `[[under(NAME)]]`, `[[transition(NAME)]]` — where it has exactly one;
/// empty otherwise, which the check reports.
inline std::string
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

/// A count and its noun as a finding says them: "1 byte", "2 bytes".
inline std::string counted( std::uint64_t count, std::string_view noun )
{
  return std::to_string( count ) + " " + std::string{ noun } + ( count == 1 ? "" : "s" );
}

/// A type's name with the article it is read with: "an `i8`", "a `u8`". An
/// initial `u` is read as a `u8` is, "you", and so takes "a".
inline std::string articled( std::string_view name )
{
  bool const vowel = !name.empty() && std::string_view{ "aeioAEIO" }.contains( name.front() );
  return std::string{ vowel ? "an `" : "a `" } + std::string{ name } + "`";
}

/// The class `[[placement(zeropage)]]` or `[[placement(absolute)]]` asks for,
/// and nothing where the attribute does not stand — see
/// docs/decisions/0210-placement-is-declared-in-c-too.md.
inline std::optional<ir::Placement> placementAttribute( std::vector<Attribute> const& attributes,
                                                        diag::SourceManager const& sources )
{
  std::string const said = nameAttribute( attributes, sources, "placement" );
  if ( said == "zeropage" )
  {
    return ir::Placement::ZEROPAGE;
  }
  if ( said == "absolute" )
  {
    return ir::Placement::ABSOLUTE;
  }
  return std::nullopt;
}

/// Whether a block says nothing but where its declarations lie, in which case
/// it is an ordinary block and not one of the `[[with]]` family — see
/// docs/decisions/0210-placement-is-declared-in-c-too.md.
inline bool placementOnly( std::vector<Attribute> const& attributes, diag::SourceManager const& sources )
{
  return !attributes.empty() && std::ranges::all_of( attributes,
                                                     [&]( Attribute const& one )
                                                     { return sources.textOf( one.name.span() ) == "placement"; } );
}

/// The class of a local that keeps its place for the whole program — a
/// `static` one, a `volatile` one, or one whose address is taken: the
/// innermost `[[placement]]` over it, and with none `absolute`, as an object
/// at file scope is, which it is in all but the scope of its name. `inForce`
/// holds the function's class at the bottom, which counts as asked for only
/// where `functionAsked` says the function carries the attribute.
inline ir::Placement keptClass( std::vector<ir::Placement> const& inForce, bool functionAsked )
{
  return inForce.size() > 1 || functionAsked ? inForce.back() : ir::Placement::ABSOLUTE;
}

/// The class a local's bytes lie in, which the lowering gives it and the
/// check holds an attribute to: off the zero page whatever is asked where
/// `offZeroPage` says — read-only data, or a block past a few bytes — the
/// kept class where it is `kept`, and otherwise the class in force, the
/// function's or the block's — see
/// docs/decisions/0210-placement-is-declared-in-c-too.md.
inline ir::Placement
localPlacement( std::vector<ir::Placement> const& inForce, bool functionAsked, bool kept, bool offZeroPage )
{
  if ( offZeroPage )
  {
    return ir::Placement::ABSOLUTE;
  }
  return kept ? keptClass( inForce, functionAsked ) : inForce.back();
}

/// A class in force for as long as the block that asked for it is being
/// lowered. Nothing is pushed where the block asked for nothing, so the
/// innermost entry is always the one that answers.
class Placed
{
public:
  Placed( std::vector<ir::Placement>& stack, std::optional<ir::Placement> asked )
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
  std::vector<ir::Placement>* mStack;
};

/// The attribute of this name written alone, with no prefix and nothing
/// given it, as `[[striped]]` and `[[trampoline]]` are; or nothing.
inline Attribute const*
bareAttribute( std::vector<Attribute> const& attributes, diag::SourceManager const& sources, std::string_view which )
{
  auto const found = std::ranges::find_if( attributes,
                                           [&sources, which]( Attribute const& attribute )
                                           {
                                             return !attribute.prefix.has_value() && !attribute.hasArguments &&
                                                    sources.textOf( attribute.name.span() ) == which;
                                           } );
  return found == attributes.end() ? nullptr : &*found;
}

/// Whether a block is written `[[with(...), trampoline]]` — see
/// docs/decisions/0097-a-trampoline-is-declared.md and
/// docs/decisions/0098-a-proc-declares-what-is-shown.md.
inline bool isTrampoline( std::vector<Attribute> const& attributes, diag::SourceManager const& sources )
{
  return bareAttribute( attributes, sources, "trampoline" ) != nullptr;
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

inline std::optional<WithSpec> withAttribute( Attribute const& attribute )
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
inline std::optional<Attribute> withOf( std::vector<Attribute> const& attributes, diag::SourceManager const& sources )
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
inline bool shows( Shown const& shown, ExternalName const& pane )
{
  return shown.pane == pane.name ||
         ( !shown.state.empty() && shown.window == pane.window && pane.paneState == shown.state );
}

/// What a name is, for a finding that expected a Pane, a family or a Window
/// of it: the assembler's own word for what it exports, or the kind of C.
inline std::string kindOfName( Meaning const& meaning )
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
inline std::string paneOfCallee( Definition const* callee, ExternalName const* external )
{
  if ( callee != nullptr )
  {
    return callee->pane;
  }
  return external != nullptr ? external->pane : std::string{};
}

/// The Pane the callee runs under, or empty.
inline std::string underOfCallee( Definition const* callee, ExternalName const* external )
{
  if ( callee != nullptr )
  {
    return callee->under;
  }
  return external != nullptr ? external->under : std::string{};
}

/// Whether an attribute of this name is written, whatever it is given.
inline bool
hasAttribute( std::vector<Attribute> const& attributes, diag::SourceManager const& sources, std::string_view which )
{
  return std::ranges::any_of(
      attributes,
      [&]( Attribute const& attribute )
      { return !attribute.prefix.has_value() && sources.textOf( attribute.name.span() ) == which; } );
}

/// Whether a declaration is written `[[striped]]`, as the subset spells it.
inline bool isStriped( std::vector<Attribute> const& attributes, diag::SourceManager const& sources )
{
  return bareAttribute( attributes, sources, "striped" ) != nullptr;
}

/// A call of a function of the program, where the checking pass saw it.
struct CallSite
{
  Definition const* callee = nullptr;
  diag::SourceSpan span;

  /// The Proc of the assembler called, where the callee is one.
  ExternalName const* external = nullptr;
};

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
      Meaning meaning;
      meaning.kind = NameKind::ASSEMBLER_OTHER;
      meaning.type = external.type;
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

  /// The bytes one value of a type takes.
  [[nodiscard]] std::uint32_t bytes( ir::Type type, std::optional<std::uint32_t> aggregateIndex ) const
  {
    return bytesOf( type,
                    aggregateIndex,
                    [this]( std::uint32_t index ) -> AggregateType const& { return mAggregates.at( index ); } );
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
      std::optional<Meaning> const named = resolve( unit, nameSpelled( mSources->textOf( node.token.span() ) ) );
      return named.has_value() && named->definition != nullptr ? named->definition->enumerator : std::nullopt;
    }
    if ( node.kind != ExpressionKind::QUALIFIED_NAME )
    {
      return std::nullopt;
    }
    std::optional<Meaning> const meaning = resolve( unit, nameSpelled( mSources->textOf( node.left->token.span() ) ) );
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
    Meaning meaning;
    giveType( meaning, definition );
    meaning.kind = definition.kind;
    meaning.definition = &definition;
    meaning.isConst = definition.isConst;
    if ( definition.enumerator.has_value() )
    {
      meaning.value = static_cast<std::int64_t>( *definition.enumerator );
    }
    meaning.isArray = definition.isArray;
    meaning.count = definition.count;
    meaning.isStriped = definition.isStriped;
    meaning.isVolatile = definition.isVolatile;
    return meaning;
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

/// What `folded` reads a name as where `unit` reads it, every name through
/// `meaningOf`: an enumerator's number, what `sizeof` counts, or a value.
inline NameValue
constantValues( Names const& names, Unit const& unit, MeaningOf meaningOf, diag::SourceManager const& sources )
{
  return [&names, &unit, read = std::move( meaningOf ), &sources]( Expression const& node ) -> std::int64_t
  {
    if ( node.kind == ExpressionKind::QUALIFIED_NAME )
    {
      return names.enumeratorIndex( unit, node ).value_or( 0 );
    }
    if ( node.kind == ExpressionKind::SIZEOF )
    {
      return sizeCounted( node, read, names.aggregateOf(), sources ).value_or( 0 );
    }
    std::optional<Meaning> const meaning = read( node );
    return meaning.has_value() ? meaning->value.value_or( 0 ) : 0;
  };
}

/// The most bytes a run holds: the address space.
constexpr std::uint64_t MAX_RUN_BYTES = 65536;

/// The bytes a run of the assembler's holds before what stops it.
inline std::uint32_t runBytes( ExternalName const& external )
{
  std::uint64_t bytes = 0;
  for ( ExternalCell const& cell : external.cells )
  {
    bytes += cell.bytes;
  }
  return static_cast<std::uint32_t>( std::min<std::uint64_t>( bytes, MAX_RUN_BYTES ) );
}

/// Whether a definition is a `const` given a value, which is a constant of the
/// assembler where the check finds the value is one. Not a `volatile` one,
/// which is read wherever the source reads it — see
/// docs/decisions/0151-volatile.md.
inline bool isConstant( Definition const& definition )
{
  return definition.kind == NameKind::OBJECT && definition.isConst && !definition.isVolatile &&
         definition.value != nullptr && !definition.isArray && definition.type != ir::Type::BLOCK;
}

/// Whether a definition is settled before the check: a constant, an array
/// whose size is written, and a `struct` or a `union`, since a constant may
/// count the bytes of any of them.
inline bool isSettled( Definition const& definition )
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
inline bool containsAddress( Expression const& node )
{
  if ( node.kind == ExpressionKind::ADDRESS )
  {
    return true;
  }
  std::array<Expression const*, 2> const children{ node.left.get(), node.right.get() };
  return std::ranges::any_of( children,
                              []( Expression const* child ) { return child != nullptr && containsAddress( *child ); } );
}

/// What a local is, as its declarator declares it of `type`: what the
/// declaration says of it, and a pointer to `type` where the declarator is
/// one. The check and the lowering read a local alike from this.
inline Meaning
localMeaning( Meaning const& type, Declaration const& declared, DeclaratorShape const& shape, Token const& declarator )
{
  Meaning local = type;
  local.isConst = declared.isConst;
  local.isVolatile = declared.isVolatile;
  local.declaredAt = declarator.location;
  if ( shape.isPointer )
  {
    giveType( local, pointerTo( pointeeOf( type, declared.isConst, declared.isVolatile ) ) );
    local.isConst = shape.isConstPointer;
    local.isVolatile = shape.isVolatilePointer;
  }
  return local;
}

/// Every name an expression reads, in the order written.
inline void namesIn( Expression const& node, std::vector<Expression const*>& into )
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

} // namespace nga::c::detail
