#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/syntax/Builder.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/Payload.hpp"
#include "nga/syntax/TokenCursor.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace nga::syntax
{

/// The `.asm` grammar of docs/spec/syntax.md.
///
/// Recognises statements and calls a Builder; it produces no syntax tree of its
/// own and resolves no name. What it checks is what the text alone settles —
/// the shape of an operand, whether a block was closed — and nothing that needs
/// a Symbol or the ISA.
///
/// Recovery is what line-oriented syntax gives for free: report, skip to the
/// next line ending, carry on. One run reports every error in the file.
class Parser
{
public:
  Parser( diag::SourceManager const& sources, TokenCursor& cursor, Builder& builder, diag::DiagnosticSink& sink );

  void parseModule();

private:
  void parseLine();

  /// The name it defined, so that what follows on the line can be blamed on a
  /// lost indent and reported against the name rather than against the symptom.
  std::optional<Token> parseLabelDefinition();
  void parseConstantDefinition();

  void parseStatement( std::optional<Token> label );
  void parseDirective();
  void parseInstruction();

  /// `SPACE.NAME [expr, ...]`: a qualified macro use, which no instruction
  /// can be, so its operands are read as a list of expressions outright.
  void parseQualifiedUse();

  /// `.namespace NAME[.NAME]...` and `.endnamespace`; a Namespace holds
  /// Sections, Procs and macros, and closes before any of them may not.
  void parseNamespace( Token directive );
  void parseNamespaceEnd( Token directive, std::string_view spelling );

  /// True when the cursor is on `.endnamespace` or `.endns`.
  [[nodiscard]] bool atNamespaceEnd() const;

  void parseSection( Token directive );

  /// `[ , attribute ]...` after a `.section` or `.proc` name. False when the
  /// line was given up on and reported.
  bool parseSectionAttributes( SectionAttributes& attributes, bool& quiet, bool leadingComma = true );
  void parseSectionEnd( Token directive, std::string_view spelling );
  void parseProc( Token directive );
  void parseProcEnd( Token directive, std::string_view spelling );
  void parseData( Token directive, DataWidth width );
  void parseDispatch( Token directive );

  /// `.hex`, `.binary`, `.base64`: one quoted literal decoded to the bytes of
  /// the Chunk `.byte` would have made — see docs/spec/syntax.md#data-and-assertions.
  void parsePayload( Token directive, Payload encoding );
  void parseReserve( Token directive );

  /// `NAME .ztemp SIZE`, with the cursor on NAME: the one statement a name at
  /// column one belongs to rather than precedes.
  void parseTemporary();
  void parseAssertion( Token directive );
  void parseExport( Token directive );

  /// `.off CODE [ , CODE ]...`, held until the next statement begins.
  void parseOff( Token directive );
  void parseSource( Token directive );

  /// True when the cursor is on `.off`, which takes no pending `.off` for
  /// itself: several before one statement all apply to it.
  /// Whether a statement begins with a qualified macro use, `one.fill`,
  /// rather than a mnemonic with an operand read at the top level,
  /// `lda .count`. The head of a use is one unbroken name.
  [[nodiscard]] bool atQualifiedUse() const;

  /// `.if EXPR`, `.elsif EXPR`, `.else`, `.endif`. A branch holds what a
  /// macro body holds, which the directive dispatch enforces before any of
  /// these is reached.
  void parseConditional( Token directive );
  void parseBranch( Token directive, bool isElse );
  void parseConditionalEnd( Token directive, std::string_view spelling );

  /// Whether the word opens, divides or closes a conditional, which is what
  /// the two content rules let through.
  [[nodiscard]] static bool isConditionalWord( std::string_view word );

  /// Whether the word names a directive that emits bytes, which is what a
  /// macro body and a branch hold beside instructions.
  [[nodiscard]] static bool isDataWord( std::string_view word );

  [[nodiscard]] bool atOffDirective() const;
  [[nodiscard]] bool atWithDirective() const;
  [[nodiscard]] bool atTakingDirective() const;
  [[nodiscard]] bool atDeclareDirective() const;
  void parseWith( Token directive );
  void parseTaking( Token directive, model::Taking kind );
  void parseDeclare( Token directive );

  /// The type a `.declare` may carry, read where one is written; nothing,
  /// and reported, where what stands there is not one of the subset's.
  [[nodiscard]] std::optional<model::DeclaredType> parseDeclaredType();

  /// Whether a word after `arg` or `ret` is a place — `a`, `xy`, `am` — and
  /// not a type.
  [[nodiscard]] static bool isPlace( std::string_view word );

  /// Whether the line at the cursor is an instruction or a data statement,
  /// with or without a label in front: what `.own` and `.root` apply to.
  [[nodiscard]] bool atInstructionOrData() const;

  /// Whether the line at the cursor is a `.ztemp`, which is the one thing a
  /// `.declare` applies to.
  [[nodiscard]] bool atTemporaryStatement() const;

  /// A `.own` or `.root` is waiting for the instruction or data below it.
  std::optional<model::Taking> mPendingTaking;
  Token mTakingToken;

  /// A `.declare` is waiting for the `.ztemp` below it; anything else there
  /// is reported and the Builder drops it.
  bool mDeclarePending = false;
  Token mDeclareToken;

  /// A `.declare` whose bytes are all in registers was the last line, which
  /// no `.ztemp` may follow.
  bool mDeclaredInRegisters = false;
  Token mRegisterToken;

  /// A `.with` is waiting for the instruction or macro use below it; where
  /// something else comes first, that is reported and the Builder drops it.
  bool mWithPending = false;
  Token mWithToken;

  /// What the line at the cursor was handed at its start, so that a line
  /// refused as a whole can tell the Builder to drop it.
  bool mWithsHanded = false;
  bool mTakingHanded = false;

  /// Hands every pending `.off` the statement beginning at `statement`.
  void flushPendingOffs( std::optional<diag::SourceLocation> statement );
  void parseSlot( Token directive );
  void parseImplements( Token directive );
  void parseTransform( Token directive );

  /// `.transform FORMAT LABEL`: two names, the first recognised by position.
  void parseTwoNames( Token directive, void ( Builder::*take )( Token, Token, diag::SourceSpan ) );

  /// `.driver ROLE NAME [NAME]`: a role and the one or two names it takes.
  void parseDriver( Token directive );
  void parseTransition( Token directive );

  /// `.macro NAME [PARAM, ...]` and `.endm`. A body holds instructions and
  /// data and nothing else, which the grammar holds it to, since that is a
  /// fact about the text.
  void parseMacro( Token directive );
  void parseMacroEnd( Token directive, std::string_view spelling );

  /// `.match PACK`, `.case PATTERN` and `.endmatch`, which nest with `.if` as
  /// blocks do: each closer closes the innermost open one of its own kind.
  void parseMatch( Token directive );
  void parseCase( Token directive );
  void parseMatchEnd( Token directive, std::string_view spelling );

  /// A comma-separated list of names, the last of which may be `name...`;
  /// what a `.macro` header and a `.case` share. False when it could not be
  /// read, which has been reported.
  bool parsePattern( Pattern& into, Token after );

  /// One item of a list — an argument of a use, an item of `.byte` or
  /// `.word` — which is an expression, or a pack's name spread: `rest...`.
  ExpressionPtr parseItem();

  /// The whole `.charset` block, closer included. Its body holds entries rather
  /// than statements, so it is read here in one loop and nothing about it is
  /// left open across a line.
  void parseCharset( Token directive );

  /// One `"..." = EXPRESSION` line, appended when it was written well enough to
  /// mean something.
  void parseCharsetEntry( std::vector<model::CharsetEntry>& into );

  /// True when the cursor is on `.endcharset` or `.endch`.
  [[nodiscard]] bool atCharsetEnd() const;

  /// The operand shapes of docs/spec/syntax.md, including the rule that decides
  /// whether a leading parenthesis indirects or merely groups. Both answer
  /// whether the line has already been reported on.
  bool parseOperand( Token mnemonic );
  bool parseParenthesisedOperand( Token mnemonic );
  bool parseIndexSuffix( Token mnemonic, ExpressionPtr operand, diag::SourceSpan span );

  /// Whether the token after the current one touches it, with nothing between.
  [[nodiscard]] bool nextIsAdjacent() const;

  /// How far ahead the parenthesis at the cursor is matched, or nothing when it
  /// is not matched before the line ends. Forward-only: the decision is made
  /// before any expression is parsed, so the expression parser never learns
  /// that addressing modes exist.
  [[nodiscard]] std::optional<std::uint32_t> matchingParenthesis() const;

  ExpressionPtr parseExpression();

  /// `x` or `y` when the token at the cursor is one, recognised by position
  /// rather than by being a keyword.
  [[nodiscard]] std::optional<char> indexRegister() const;

  [[nodiscard]] std::string_view textOf( Token token ) const;

  /// What a message calls this token: its text, or the name of its kind when
  /// the text would be invisible.
  [[nodiscard]] std::string describe( Token token ) const;

  /// Ends a statement: recovers in silence when the line has already produced a
  /// finding, and otherwise insists on nothing following it.
  void endStatement( bool quiet );
  bool expectLineEnd();
  void recover();
  void report( diag::Diagnostic value ) const;

  diag::SourceManager const* mSources;
  TokenCursor* mCursor;
  Builder* mBuilder;
  diag::DiagnosticSink* mSink;

  /// The directive that opened the construct, kept for the diagnostic that
  /// reports it was never closed. Neither construct nests.
  std::optional<Token> mOpenSection;
  std::optional<Token> mOpenProc;
  std::optional<Token> mOpenMacro;

  /// Every `.namespace` directive still open, outermost first.
  std::vector<Token> mOpenNamespaces;

  /// The Namespaces open where the open `.proc` opened: those past it stand in
  /// the Proc — see docs/decisions/0090-a-namespace-may-stand-in-a-proc.md.
  std::size_t mNamespacesAtProc = 0;

  /// One entry per open `.if` or `.match`, so that `.elsif` after `.else` is
  /// refused, a statement before the first `.case` is, and an unclosed one
  /// is reported where it opened.
  struct OpenConditional
  {
    Token directive;
    bool isMatch = false;
    bool sawElse = false;
    bool sawCase = false;
    /// Opened where no Section was. Reported once, at the `.if`; what it
    /// holds is refused in silence, and its branches and end pair with it
    /// without reaching the Builder.
    bool outside = false;
  };

  /// Whether a statement may stand here: not between a `.match` and its
  /// first `.case`. Reports when it may not.
  bool statementAllowed();

  /// Whether a statement that emits, reserves or names a position has a
  /// Section to stand in: a `.section`, a `.proc`, or a macro body, which is
  /// a template for one.
  [[nodiscard]] bool inBody() const;

  /// Refuses `what` when it stands in no Section — reported, recovered, and
  /// whatever a `.with`, `.own` or `.root` held for it dropped — and answers
  /// whether it did. Silent inside a conditional that was itself refused.
  bool refusedOutsideBody( Token at, std::string_view what );

  /// Whether a statement stands in a Namespace opened in a Proc, and in no
  /// Section: where only Sections, Temporaries and Constants stand.
  [[nodiscard]] bool inProcNamespace() const;

  std::vector<OpenConditional> mOpenConditionals;

  /// One `.off CODE` waiting for the statement it applies to.
  struct PendingOff
  {
    Token code;
    diag::SourceSpan span;
  };

  std::vector<PendingOff> mPendingOffs;
};

} // namespace nga::syntax
