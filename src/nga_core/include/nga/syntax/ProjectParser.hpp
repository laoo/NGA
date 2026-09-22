#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/ProjectBuilder.hpp"
#include "nga/syntax/TokenCursor.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace nga::syntax
{

/// The `.ngp` grammar of docs/spec/project-file.md.
///
/// The second grammar over the one lexer, and the first code to exercise what
/// [0006](docs/decisions/0006-one-lexer-two-grammars.md) claims: line endings
/// are produced by the lexer and dropped here, so every construct has
/// self-delimiting arity and layout means nothing.
///
/// Recovery is by block. A malformed entry is reported and the block is read on
/// to its closing brace, so one run reports every mistake in the file.
class ProjectParser
{
public:
  ProjectParser( diag::SourceManager const& sources,
                 TokenCursor& cursor,
                 ProjectBuilder& builder,
                 diag::DiagnosticSink& sink );

  void parseDocument();

private:
  void parseBlock( Token name );
  void parseModules( Token name );
  void parseDiagnostics( Token name );
  void parseConstants( Token name );
  void parseContainer( Token keyword );
  void parseIntent( Token keyword );
  void parsePhase( Token keyword );
  void parseResident( Token name );
  void parseGroup( Token keyword );
  void parseTransform( Token keyword );

  /// The body of a block that holds one list of names — `resident`, `group`
  /// — comma-separated, where two names with nothing between them are
  /// reported once and both taken. `entryError` is what is said of an entry
  /// that is not a name.
  void parseNameBody( Token name,
                      diag::DiagnosticId entryError,
                      std::function<void( Token )> const& take,
                      std::function<void( Token, Token )> const* base = nullptr );

  /// `base WINDOW = STATE`, in a `phase` or a `group` block.
  void parseBase( Token keyword, std::function<void( Token, Token )> const& take );
  void parseTarget( Token name );
  void parseStorage( Token name );
  void parseRegion( Token keyword );
  void parseRegister( Token keyword );
  void parseUnitSet( Token keyword );
  void parseWindow( Token keyword );
  void parsePanes( Token name );

  /// Reads on to a block's body and through it, for a header that could not
  /// be read: one finding for the block, not one per entry.
  void skipBody( Token name );

  /// `start .. end`, both parsed and neither evaluated; nothing when either
  /// could not be read, which has been reported.
  [[nodiscard]] std::optional<std::pair<ExpressionPtr, ExpressionPtr>> parseRange( Token after );
  void parseInclude( Token keyword );
  void parseEntry( Token keyword );

  /// One value: an expression of the shared language, parsed by the parser
  /// the assembler uses. Null when it could not be read, or when it named
  /// something — a value in the Project is written out, since there are no
  /// Symbols for a name to mean — and both have been reported.
  ExpressionPtr parseValue( Token after );

  /// A comma-separated list of values, which ends where the commas stop.
  void parseValueList( Token after, std::function<void( ExpressionPtr )> const& take );

  /// Whether the current token can begin a value.
  [[nodiscard]] bool atValueStart() const;

  /// Reports every name in the tree, and says whether there was one.
  [[nodiscard]] bool rejectNames( Expression const& node ) const;

  /// A comma-separated list of names, which ends where the commas stop. The
  /// first name is required, and `after` is what the finding says it was
  /// expected after.
  void parseNameList( Token after, std::function<void( Token )> const& take );

  /// A comma-separated list of `module.section` names, with any number of
  /// Namespaces between the two. The dot is the lexer's own token, so
  /// nothing here is a lexical special case.
  void parseQualifiedNameList( Token after, std::function<void( Token, std::vector<Token> )> const& take );

  /// `NAME[.NAME]...`: a name, or a dotted one into a Namespace. Empty when
  /// the first was not there, which has been reported after `after`.
  std::vector<Token> parseDottedName( Token after );

  /// Consumes `{`, reporting when it is not there. False when the block cannot
  /// be read at all.
  bool openBody( Token name );

  /// True once the body has ended, having consumed the `}`. Reports the block
  /// as unclosed at end of file.
  bool bodyEnded( Token name );

  /// Reports one finding for a run of entries that could not be read, rather
  /// than one per token, and always consumes something so the loop advances.
  void skipUnreadable( diag::Diagnostic value, bool& alreadyReported );

  /// `NAME ( [ argument { , argument } ] )` of a `modules` entry, with the
  /// cursor on the `(`. False where the call was reported on and skipped.
  bool parseGeneratorArguments( Token generator, std::vector<ProjectBuilder::GeneratorArgument>& into );

  /// Reads to the `)` that closes a call, or to the brace that ends the body.
  void skipCallEnd();

  [[nodiscard]] std::string_view textOf( Token token ) const;
  [[nodiscard]] std::string describe( Token token ) const;
  void report( diag::Diagnostic value ) const;

  diag::SourceManager const* mSources;
  TokenCursor* mCursor;
  ProjectBuilder* mBuilder;
  diag::DiagnosticSink* mSink;
};

} // namespace nga::syntax
