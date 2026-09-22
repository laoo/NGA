#pragma once

#include "nga/c/Token.hpp"
#include "nga/diag/SourceLocation.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nga::c
{

/// The syntax tree of a `.ngc` Module, named by the C standard's own terms.
///
/// These are the language's words and not the model's, which is why they are
/// not in the glossary — see its preamble. The tree holds what the text says
/// and nothing it means: no name is resolved and no type is checked here.

enum class ExpressionKind : std::uint8_t
{
  INTEGER_CONSTANT,

  /// `true` or `false`, C23's predefined constants, with `value` one or zero.
  PREDEFINED_CONSTANT,

  IDENTIFIER,

  /// `NAME::NAME`, an enumerator of an `enum struct`: the type's name in
  /// `left` and the enumerator's in `right`, both identifiers, and the
  /// enumerator's token in `token`.
  QUALIFIED_NAME,

  /// `callee(arguments)`: the callee in `left`, and the arguments in
  /// `arguments` in the order written.
  CALL,

  /// `-left`, `~left` or `!left`.
  UNARY,

  /// `(type)left`, with the type's keyword in `token`.
  CAST,

  /// `left[right]`: an element of an array, with the `[` in `token`.
  INDEX,

  /// `sizeof left`, or `sizeof(type)` with the type's keyword in `token` and
  /// no `left`.
  SIZEOF,

  /// `*left`.
  DEREFERENCE,

  /// `&left`.
  ADDRESS,

  /// `nullptr`, C23's predefined constant.
  NULL_POINTER,

  /// `left.right` or `left->right`: a member, its name an identifier in
  /// `right`, and the `.` or `->` in `token`.
  MEMBER,

  /// `{ ... }` inside a list a declaration gives: the values in `arguments`.
  LIST,

  /// `left OP right`, for the operators of C's precedence levels from `*` to
  /// `|`.
  BINARY,

  /// `left ? arguments[0] : arguments[1]`, with the `?` in `token` — see
  /// docs/decisions/0169-a-conditional-operator.md.
  CONDITIONAL,

  /// `left = right`. A compound assignment, `left += right`, and `++left`,
  /// `left--` and their kin are this too, `compound` saying so: the tree
  /// holds `left = left OP right` with the target written twice, and the
  /// text reaches it once — see
  /// docs/decisions/0158-a-compound-assignment-and-a-step.md.
  ASSIGNMENT,

  /// `"..."` or `screen"..."`, adjacent ones joined: in `arguments` a
  /// CHARACTER_CONSTANT per character and one for the end, `\0` — see
  /// docs/decisions/0095-literals-and-the-runtime.md.
  STRING_LITERAL,

  /// `'a'`, with its ASCII value in `value`, or `screen'a'`, whose value the
  /// Charset gives; one a string literal holds is spelled in `spelling`.
  CHARACTER_CONSTANT,
};

struct Expression;
using ExpressionPtr = std::unique_ptr<Expression>;

/// A copy of an expression and everything under it, which a compound
/// assignment's `left = left OP right` needs for the target it names twice.
[[nodiscard]] ExpressionPtr copyOf( Expression const& node );

struct Expression
{
  ExpressionKind kind = ExpressionKind::INTEGER_CONSTANT;

  /// True when this node came directly from a parenthesised group, which the
  /// tree does not otherwise keep.
  bool parenthesised = false;

  /// An ASSIGNMENT the source wrote as `+=`, `++` or one of their kin, whose
  /// `right` the parser built: the operator's token is the one written, and
  /// its left is the target again.
  bool compound = false;

  /// A step the source wrote after its target, `a++` or `a--`: its value is
  /// what the target held before it, where `++a` is what it holds after — see
  /// docs/decisions/0198-a-compound-assignment-is-a-value.md.
  bool writtenAfter = false;

  /// How many operators deep the tree goes from here, none for a leaf. Held
  /// on the node so that the parser can refuse a tree past
  /// MAX_EXPRESSION_DEPTH as it builds one, without walking it.
  std::uint32_t depth = 0;

  /// The constant or identifier for a leaf, the operator for the rest: `=`,
  /// the `(` of a call, or the operator's own token.
  Token token;

  /// The whole of what this node covers, which is what a diagnostic underlines.
  diag::SourceSpan span;

  /// What an INTEGER_CONSTANT stands for.
  std::int64_t value = 0;

  /// Where a CAST, or a SIZEOF of a type, names a pointer type: to what its
  /// keyword names, `const` and `volatile` where they are written.
  bool pointer = false;
  bool pointeeConst = false;
  bool pointeeVolatile = false;

  ExpressionPtr left;
  ExpressionPtr right;

  /// A CALL's arguments.
  std::vector<ExpressionPtr> arguments{};

  /// A CHARACTER_CONSTANT a STRING_LITERAL holds: how the assembler writes
  /// it, `screen'a'`, since no token spells it alone.
  std::string spelling{};
};

/// Defined below, and held by a statement that declares locals.
struct Declaration;

/// `[[NAME]]`, `[[PREFIX::NAME]]` or `[[NAME(TOKENS)]]`, one of a list before a
/// declaration: what the text says, whether or not the subset has it — see
/// docs/decisions/0088-an-attribute-has-no-prefix.md.
struct Attribute
{
  Token name;
  std::optional<Token> prefix{};
  bool hasArguments = false;
  diag::SourceSpan span;

  /// The tokens between the parentheses, which an attribute reads as it will.
  std::vector<Token> arguments{};
};

enum class StatementKind : std::uint8_t
{
  /// `{ ... }`, holding `items`.
  COMPOUND,

  /// An expression followed by `;`.
  EXPRESSION,

  /// `;` alone.
  NULL_STATEMENT,

  /// `return;`, or `return expression;` with the value in `expression`.
  RETURN,

  /// `if ( expression ) items[0]`, and `else items[1]` where one is written.
  IF,

  /// `while ( expression ) items[0]`.
  WHILE,

  /// `do items[0] while ( expression ) ;`.
  DO,

  /// `for ( initial ; expression ; step ) items[0]`, any of the three perhaps
  /// absent.
  FOR,

  /// `break;`.
  BREAK,

  /// `continue;`.
  CONTINUE,

  /// A declaration of locals in a block, in `declaration`.
  DECLARATION,

  /// `switch ( expression ) { items }`, every item a CASE.
  SWITCH,

  /// `case expression :`, or `default:` where `expression` is null, and the
  /// statements up to the next label in `items`. Stands in a SWITCH alone.
  CASE,
};

struct Statement
{
  StatementKind kind = StatementKind::NULL_STATEMENT;
  diag::SourceSpan span;

  /// An EXPRESSION's expression, and the condition of an IF, a WHILE, a DO,
  /// and of a FOR that has one.
  ExpressionPtr expression{};

  /// A FOR's first clause and its third, where they are written.
  ExpressionPtr initial{};
  ExpressionPtr step{};

  /// What a COMPOUND holds, in order, and the statements a control statement
  /// governs.
  std::vector<Statement> items{};

  /// What a DECLARATION declares. A pointer, since a Statement holding one by
  /// value would hold a vector in every statement that is not one.
  std::unique_ptr<Declaration> declaration{};

  /// The attributes written before a COMPOUND, `[[with(...)]]` — see
  /// docs/decisions/0096-panes-in-c.md — or before a RETURN,
  /// `[[transition(PHASE)]]` — see docs/decisions/0064-phase-in-c.md.
  std::vector<Attribute> attributes{};
};

/// `TYPE NAME` in a function's parameter list.
struct Parameter
{
  Token type;
  Token name;

  /// A pointer, `TYPE* NAME`, or an array, `TYPE NAME[]`, which is one; to
  /// `const` and `volatile` where they are written, `volatile` perhaps where
  /// there is no pointer, which the compiler refuses; and the size an array
  /// was written with.
  bool isPointer = false;
  bool pointeeConst = false;
  bool pointeeVolatile = false;
  std::optional<diag::SourceSpan> size{};

  /// False in a declaration after `extern`, which may leave a parameter
  /// unnamed; `name` is then the type's token.
  bool isNamed = true;
};

/// `TYPE NAME(PARAMETERS) { ... }`, where `()` and `(void)` are both a list of
/// none, as C23 reads them, perhaps after `static`.
struct FunctionDefinition
{
  Token name;
  diag::SourceSpan span;
  Statement body;

  /// The attributes written before it.
  std::vector<Attribute> attributes{};

  /// Written after `static`, which keeps the name in its file — see
  /// docs/decisions/0072-static-is-module-privacy.md.
  bool isStatic = false;

  /// Written `inline`, in either order with `static`: every call of it the
  /// program's C writes is wrapped into its caller, and a shape that cannot
  /// be is a finding rather than a silence — see
  /// docs/decisions/0172-a-call-with-one-site-is-wrapped.md.
  bool isInline = false;
  diag::SourceSpan inlineSpan{};

  /// The type written before the name — a keyword, or the name of an
  /// `enum struct` — or nothing for `void`; and whether it is a pointer to it,
  /// to `const` where it is written.
  std::optional<Token> result{};
  bool resultPointer = false;
  bool resultPointeeConst = false;
  bool resultPointeeVolatile = false;

  std::vector<Parameter> parameters{};

  /// `extern RESULT NAME(PARAMETERS);`: how C calls a Proc of the assembler,
  /// with no body — see docs/decisions/0094-extern-declares-how-c-calls-a-proc.md.
  bool isExtern = false;

  /// `typedef RESULT NAME(PARAMETERS);`: a function type, whose name is the
  /// Proc a pointer to it goes through — see docs/decisions/0065-handlers.md.
  /// It declares no function, so nothing calls the name itself.
  bool isTypedef = false;
};

/// What a declarator declares beyond its name: a pointer, perhaps itself
/// `const` or `volatile`; or an array of `size` elements, its size perhaps
/// left to the list it is given, and that list.
struct DeclaratorShape
{
  bool isPointer = false;
  bool isConstPointer = false;
  bool isVolatilePointer = false;

  bool isArray = false;
  ExpressionPtr size{};

  /// Whether `{ ... }` was written, and what it holds.
  bool hasList = false;
  std::vector<ExpressionPtr> list{};
  diag::SourceSpan listSpan{};

  /// A list a string literal gave, its end last, which an array exactly as
  /// long as its characters leaves out, as C does.
  bool fromLiteral = false;
};

/// `u8 NAME, NAME;`, perhaps after `static`, and `const` before or after the
/// type: the type specifier — a keyword, or the name of an `enum struct` — and
/// every declarator.
struct Declaration
{
  Token type;
  std::vector<Token> declarators;

  /// What each declarator is given, where it is given one: as many entries as
  /// there are declarators, and null where a declarator has none or is given
  /// a list.
  std::vector<ExpressionPtr> initialisers;

  /// Each declarator's array, as many entries as there are declarators.
  std::vector<DeclaratorShape> shapes{};

  diag::SourceSpan span;
  bool isStatic = false;

  /// The attributes written before it, which apply to every declarator.
  std::vector<Attribute> attributes{};

  /// Written with `const`, which no statement assigns — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
  bool isConst = false;

  /// Written with `volatile`: every read and store of the object is one in
  /// the text, in its order — see docs/decisions/0151-volatile.md. Of a
  /// pointer's declarator, what it points at is.
  bool isVolatile = false;

  /// Written `extern`: how C reads a name the assembler exports — see
  /// docs/decisions/0093-extern-declares-how-c-reads-the-assembler.md.
  bool isExtern = false;
};

/// `enum struct NAME { A, B };`: the type's name, and its enumerators in the
/// order written — see docs/decisions/0078-switch-over-an-enum-struct.md.
struct EnumSpecifier
{
  Token name;
  std::vector<Token> enumerators;
  diag::SourceSpan span;

  /// Written `enum struct`, whose enumerators are named through the type,
  /// `Light::RED`, against `enum`, whose are named by themselves as well —
  /// see docs/decisions/0162-an-enum-without-struct.md.
  bool isScoped = true;
};

/// `struct NAME { MEMBERS };` or `union NAME { MEMBERS };`: the type's name,
/// and its members as the declarations that declare them — see
/// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
struct StructSpecifier
{
  Token name;
  bool isUnion = false;
  std::vector<Declaration> members;
  diag::SourceSpan span;
};

using ExternalDeclaration = std::variant<FunctionDefinition, Declaration, EnumSpecifier, StructSpecifier>;

/// One `.ngc` file, its external declarations in the order written.
struct TranslationUnit
{
  std::vector<ExternalDeclaration> declarations;
};

/// How deep operators may nest in one expression, and how deep parentheses
/// may: 0070's number and its reason, since every walk the compiler makes over
/// an expression recurses once per level — see
/// docs/decisions/0070-expression-depth.md.
constexpr std::uint32_t MAX_EXPRESSION_DEPTH = 64;

/// How deep blocks may nest, for the same reason: the tree of statements is
/// walked recursively too. The number is C's own translation limit for blocks
/// (C17 5.2.4.1).
constexpr std::uint32_t MAX_BLOCK_DEPTH = 127;

} // namespace nga::c
