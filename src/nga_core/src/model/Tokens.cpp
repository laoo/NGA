#include "nga/model/Tokens.hpp"

#include "nga/Json.hpp"
#include "nga/Version.hpp"
#include "nga/c/Lexer.hpp"
#include "nga/c/Token.hpp"
#include "nga/model/Isa.hpp"
#include "nga/syntax/Lexer.hpp"
#include "nga/syntax/Token.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace nga::model
{

namespace
{

/// Where in a statement the next token stands. The grammar is positional --
/// 0007 marks a Label by its column and nothing else -- so this is all the
/// state a classification needs.
enum class Place : std::uint8_t
{
  LINE_START, ///< Nothing of this line has been read.
  HEAD,       ///< A statement may begin here; a Label may not.
  BODY,       ///< Inside a statement: operands, arguments, an expression.
};

std::uint32_t offsetIn( diag::SourceManager const& sources, diag::FileId file, diag::SourceLocation where )
{
  return where.rawOffset() - sources.locationOf( file, 0 ).rawOffset();
}

TokenClass ofPunctuation( syntax::TokenKind kind )
{
  switch ( kind )
  {
  case syntax::TokenKind::COMMENT:
    return TokenClass::COMMENT;
  case syntax::TokenKind::NUMBER:
    return TokenClass::NUMBER;
  case syntax::TokenKind::STRING:
    return TokenClass::STRING;
  case syntax::TokenKind::CHARACTER:
    return TokenClass::CHARACTER;
  case syntax::TokenKind::UNKNOWN:
    return TokenClass::UNKNOWN;
  default:
    return TokenClass::PUNCTUATION;
  }
}

// Every word the Project grammar knows, which
// src/nga_core/src/syntax/ProjectParser.cpp compares against by name rather
// than from a table. Listed here so that a reader of a `.ngp` sees its
// grammar, and kept honest by what a miss costs: a word this misses is drawn
// as an ordinary name, which is a colour too few and never a colour that
// lies. A word added to the grammar and forgotten here degrades the same way.
constexpr std::array<std::string_view, 33> PROJECT_WORDS = {
  "after",       "allow",  "as",    "base",      "constants", "container", "containers", "cpu",      "deny",
  "diagnostics", "entry",  "group", "in",        "include",   "modules",   "needs",      "next",     "off",
  "optimize",    "panes",  "phase", "previous",  "region",    "register",  "resident",   "reserved", "size",
  "storage",     "target", "then",  "transform", "units",     "views",
};

bool isProjectWord( std::string_view word )
{
  return std::ranges::find( PROJECT_WORDS, word ) != PROJECT_WORDS.end();
}

/// The assembler and the Project file, which share a lexer and differ only in
/// what stands at the head of a statement: a mnemonic or a directive there, a
/// word of the grammar here.
std::vector<ClassifiedToken>
classifyAssembler( diag::SourceManager const& sources, diag::FileId file, Grammar grammar, diag::DiagnosticSink& sink )
{
  std::vector<ClassifiedToken> out;
  Place place = Place::LINE_START;
  bool afterDot = false;

  for ( syntax::Token const& token : syntax::tokenize( sources, file, sink ) )
  {
    if ( token.kind == syntax::TokenKind::END_OF_FILE )
    {
      break;
    }

    if ( token.kind == syntax::TokenKind::LINE_END )
    {
      place = Place::LINE_START;
      afterDot = false;
      out.push_back( { .offset = offsetIn( sources, file, token.location ),
                       .length = token.length,
                       .classification = TokenClass::PUNCTUATION } );
      continue;
    }

    TokenClass found = ofPunctuation( token.kind );

    // The dot belongs to the directive it opens: `.section` is one word to a
    // reader, and two tokens only because DOT is never resolved by the lexer.
    if ( token.kind == syntax::TokenKind::DOT && place != Place::BODY && grammar != Grammar::PROJECT )
    {
      found = TokenClass::DIRECTIVE;
    }
    else if ( token.kind == syntax::TokenKind::IDENTIFIER )
    {
      if ( afterDot && place != Place::BODY )
      {
        // `.section`, and `.ends` -- a dot that opened the statement.
        found = TokenClass::DIRECTIVE;
      }
      else if ( grammar == Grammar::PROJECT )
      {
        // The Project file has no Labels and no statement head worth the
        // name: `phase intro { needs intro  then game }` is one line holding
        // four of its words. So a word of the grammar is a word of the
        // grammar wherever it stands, and everything else is a name.
        found = isProjectWord( sources.textOf( token.span() ) ) ? TokenClass::KEYWORD : TokenClass::NAME;
      }
      else if ( token.startsLine )
      {
        // Column one, first of its line: a definition, and the one thing a
        // name at the head of a line can be -- see 0007.
        found = TokenClass::LABEL;
      }
      else if ( place != Place::BODY )
      {
        found = isMnemonic( sources.textOf( token.span() ) ) ? TokenClass::MNEMONIC : TokenClass::MACRO;
      }
      else
      {
        found = TokenClass::NAME;
      }
    }
    else if ( token.kind == syntax::TokenKind::LOCAL_IDENTIFIER || token.kind == syntax::TokenKind::ANONYMOUS_LABEL )
    {
      found = token.startsLine && grammar != Grammar::PROJECT ? TokenClass::LABEL : TokenClass::NAME;
    }

    out.push_back(
        { .offset = offsetIn( sources, file, token.location ), .length = token.length, .classification = found } );

    // A comment ends nothing: it stands beside a statement rather than in it.
    if ( token.kind == syntax::TokenKind::COMMENT )
    {
      continue;
    }

    afterDot = token.kind == syntax::TokenKind::DOT && place != Place::BODY;
    if ( found == TokenClass::LABEL )
    {
      // A Label leaves the statement still to come: `name  lda #0` is one
      // line holding both.
      place = Place::HEAD;
    }
    else if ( !afterDot )
    {
      place = Place::BODY;
    }
  }

  return out;
}

std::vector<ClassifiedToken>
classifyC( diag::SourceManager const& sources, diag::FileId file, diag::DiagnosticSink& sink )
{
  std::vector<ClassifiedToken> out;
  for ( c::Token const& token : c::tokenize( sources, file, sink ) )
  {
    if ( token.kind == c::TokenKind::END_OF_FILE )
    {
      break;
    }

    TokenClass found = TokenClass::PUNCTUATION;
    switch ( token.kind )
    {
    case c::TokenKind::COMMENT:
      found = TokenClass::COMMENT;
      break;
    case c::TokenKind::IDENTIFIER:
      found = TokenClass::NAME;
      break;
    case c::TokenKind::KEYWORD:
      found = TokenClass::KEYWORD;
      break;
    case c::TokenKind::INTEGER_CONSTANT:
      found = TokenClass::NUMBER;
      break;
    case c::TokenKind::STRING_LITERAL:
      found = TokenClass::STRING;
      break;
    case c::TokenKind::CHARACTER_CONSTANT:
      found = TokenClass::CHARACTER;
      break;
    case c::TokenKind::UNKNOWN:
      found = TokenClass::UNKNOWN;
      break;
    default:
      break;
    }

    out.push_back(
        { .offset = offsetIn( sources, file, token.location ), .length = token.length, .classification = found } );
  }
  return out;
}

} // namespace

std::string_view nameOf( Grammar grammar )
{
  switch ( grammar )
  {
  case Grammar::PROJECT:
    return "project";
  case Grammar::C:
    return "c";
  case Grammar::ASSEMBLER:
    return "assembler";
  }
  return "assembler";
}

std::string_view nameOf( TokenClass value )
{
  switch ( value )
  {
  case TokenClass::COMMENT:
    return "comment";
  case TokenClass::LABEL:
    return "label";
  case TokenClass::DIRECTIVE:
    return "directive";
  case TokenClass::MNEMONIC:
    return "mnemonic";
  case TokenClass::MACRO:
    return "macro";
  case TokenClass::KEYWORD:
    return "keyword";
  case TokenClass::NUMBER:
    return "number";
  case TokenClass::STRING:
    return "string";
  case TokenClass::CHARACTER:
    return "character";
  case TokenClass::NAME:
    return "name";
  case TokenClass::PUNCTUATION:
    return "punctuation";
  case TokenClass::UNKNOWN:
    return "unknown";
  }
  return "unknown";
}

Grammar grammarOf( std::string_view path )
{
  if ( path.ends_with( ".ngp" ) )
  {
    return Grammar::PROJECT;
  }
  if ( path.ends_with( ".ngc" ) )
  {
    return Grammar::C;
  }
  return Grammar::ASSEMBLER;
}

std::vector<ClassifiedToken>
classify( diag::SourceManager const& sources, diag::FileId file, Grammar grammar, diag::DiagnosticSink& sink )
{
  return grammar == Grammar::C ? classifyC( sources, file, sink ) : classifyAssembler( sources, file, grammar, sink );
}

std::string renderClassificationJson( diag::SourceManager const& sources,
                                      std::vector<diag::FileId> const& files,
                                      diag::DiagnosticSink& sink )
{
  std::string out;
  out.append( "{\n  \"schema\": 1,\n  \"nga\": " );
  json::appendString( out, versionString() );
  out.append( ",\n  \"tokens\": [\n" );

  for ( std::size_t i = 0; i < files.size(); ++i )
  {
    std::string_view const path = sources.pathOf( files[i] );
    Grammar const grammar = grammarOf( path );

    out.append( "    { \"path\": " );
    json::appendString( out, path );
    out.append( ", \"grammar\": " );
    json::appendString( out, nameOf( grammar ) );
    out.append( ",\n      \"spans\": [" );

    std::vector<ClassifiedToken> const spans = classify( sources, files[i], grammar, sink );
    for ( std::size_t s = 0; s < spans.size(); ++s )
    {
      out.append( "[" ).append( std::to_string( spans[s].offset ) );
      out.append( "," ).append( std::to_string( spans[s].length ) ).append( "," );
      json::appendString( out, nameOf( spans[s].classification ) );
      out.append( s + 1 < spans.size() ? "], " : "]" );
    }
    out.append( "] }" );
    out.append( i + 1 < files.size() ? ",\n" : "\n" );
  }

  out.append( "  ]\n}\n" );
  return out;
}

} // namespace nga::model
