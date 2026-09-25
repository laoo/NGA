#include "nga/diag/Renderer.hpp"
#include "nga/Json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace nga::diag
{

namespace
{

std::string_view severityLabel( Severity severity )
{
  switch ( severity )
  {
  case Severity::ERROR:
    return "error";
  case Severity::WARNING:
    return "warning";
  case Severity::NOTE:
    return "note";
  }
  return "error";
}

std::size_t digitCount( std::uint32_t value )
{
  std::size_t digits = 1;
  while ( value >= 10 )
  {
    value /= 10;
    ++digits;
  }
  return digits;
}

/// Tab stop used when echoing a source line. Assembly is routinely indented
/// with tabs, and a caret padded with one space per tab would sit under the
/// wrong character in every such excerpt.
constexpr std::size_t TAB_WIDTH = 8;

/// A source line laid out for display, with the caret run measured in the same
/// space. The column *reported* stays a byte column, as docs/spec/diagnostics.md
/// defines it; only the excerpt is laid out in display columns.
struct DisplayLine
{
  std::string text;
  std::size_t caretStart = 0;
  std::size_t caretWidth = 1;
};

DisplayLine layOutLine( std::string_view line, std::uint32_t byteColumn, std::uint32_t byteLength )
{
  std::size_t const beginByte = std::min<std::size_t>( byteColumn - 1, line.size() );
  std::size_t const endByte = std::min( beginByte + std::max<std::size_t>( byteLength, 1 ), line.size() );

  DisplayLine display;
  std::size_t width = 0;
  std::size_t beginWidth = 0;
  std::size_t endWidth = 0;

  for ( std::size_t i = 0; i <= line.size(); ++i )
  {
    if ( i == beginByte )
    {
      beginWidth = width;
    }
    if ( i == endByte )
    {
      endWidth = width;
    }
    if ( i == line.size() )
    {
      break;
    }

    char const c = line[i];
    if ( c == '\t' )
    {
      std::size_t const stop = ( ( width / TAB_WIDTH ) + 1 ) * TAB_WIDTH;
      display.text.append( stop - width, ' ' );
      width = stop;
      continue;
    }

    display.text.push_back( c );

    // UTF-8 continuation bytes belong to the character before them and occupy
    // no column of their own.
    if ( ( static_cast<unsigned char>( c ) & 0xC0U ) != 0x80U )
    {
      ++width;
    }
  }

  display.caretStart = beginWidth;
  display.caretWidth = std::max<std::size_t>( endWidth - beginWidth, 1 );
  return display;
}

void appendLocated( std::string& out, SourceManager const& sources, Diagnostic const& value, std::size_t gutter )
{
  ExpandedLocation const where = sources.expand( value.span.begin );
  std::string const pad( gutter, ' ' );
  std::string const lineNumber = std::to_string( where.line );

  out.append( pad ).append( "--> " ).append( where.path );
  out.append( ":" ).append( lineNumber ).append( ":" ).append( std::to_string( where.column ) ).append( "\n" );

  out.append( pad ).append( " |\n" );

  DisplayLine const display = layOutLine( sources.lineTextAt( value.span.begin ), where.column, value.span.length );

  out.append( lineNumber ).append( std::string( gutter - lineNumber.size(), ' ' ) ).append( " | " );
  out.append( display.text ).append( "\n" );

  out.append( pad ).append( " | " ).append( std::string( display.caretStart, ' ' ) );
  out.append( std::string( display.caretWidth, '^' ) ).append( "\n" );

  // Generated text says where it came from; the line above stays the
  // position, since it is what exists, and this is the note under it.
  if ( std::optional<SourceReference> const source = sources.sourceOf( value.span.begin ); source.has_value() )
  {
    out.append( pad ).append( " = from " ).append( source->path ).append( ":" );
    out.append( std::to_string( source->line ) ).append( "\n" );
  }
}

void appendDiagnostic( std::string& out, SourceManager const& sources, Diagnostic const& value, Severity severity )
{
  out.append( severityLabel( severity ) );

  // Notes print without an identifier: they cannot be controlled separately, so
  // a number the reader cannot act on would be noise. It survives in the JSON,
  // where a machine does read it.
  if ( severity != Severity::NOTE )
  {
    out.append( "[" ).append( catalogEntryFor( value.id ).code ).append( "]" );
  }

  out.append( ": " ).append( renderMessage( value ) ).append( "\n" );

  if ( value.span.begin.isValid() )
  {
    appendLocated( out, sources, value, digitCount( sources.expand( value.span.begin ).line ) );
  }

  for ( Diagnostic const& child : value.notes )
  {
    appendDiagnostic( out, sources, child, Severity::NOTE );
  }
}

void appendJsonArgument( std::string& out, Argument const& argument )
{
  nga::json::appendString( out, argument.name );
  out.append( ": " );

  if ( auto const* text = std::get_if<std::string>( &argument.value ) )
  {
    nga::json::appendString( out, *text );
  }
  else if ( auto const* flag = std::get_if<bool>( &argument.value ) )
  {
    out.append( *flag ? "true" : "false" );
  }
  else
  {
    out.append( std::to_string( std::get<std::int64_t>( argument.value ) ) );
  }
}

void appendJsonLocation( std::string& out,
                         SourceManager const& sources,
                         SourceSpan const& span,
                         std::string const& indent )
{
  ExpandedLocation const where = sources.expand( span.begin );
  std::uint32_t const length = span.length == 0 ? 1 : span.length;

  out.append( indent ).append( R"("location": { "file": )" );
  nga::json::appendString( out, where.path );
  out.append( ", \"line\": " ).append( std::to_string( where.line ) );
  out.append( ", \"column\": " ).append( std::to_string( where.column ) );
  out.append( ", \"endLine\": " ).append( std::to_string( where.line ) );
  out.append( ", \"endColumn\": " ).append( std::to_string( where.column + length ) );
  out.append( " },\n" );

  // Absent, never null, when no `.source` covers the position.
  if ( std::optional<SourceReference> const source = sources.sourceOf( span.begin ); source.has_value() )
  {
    out.append( indent ).append( R"("source": { "file": )" );
    nga::json::appendString( out, source->path );
    out.append( ", \"line\": " ).append( std::to_string( source->line ) );
    out.append( " },\n" );
  }
}

void appendJsonDiagnostic( std::string& out,
                           SourceManager const& sources,
                           Diagnostic const& value,
                           Severity severity,
                           std::string const& indent )
{
  CatalogEntry const& entry = catalogEntryFor( value.id );

  out.append( indent ).append( "{\n" );

  out.append( indent ).append( "  \"id\": " );
  nga::json::appendString( out, entry.code );
  out.append( ",\n" );

  out.append( indent ).append( "  \"name\": " );
  nga::json::appendString( out, entry.name );
  out.append( ",\n" );

  out.append( indent ).append( "  \"severity\": " );
  nga::json::appendString( out, severityLabel( severity ) );
  out.append( ",\n" );

  out.append( indent ).append( "  \"message\": " );
  nga::json::appendString( out, renderMessage( value ) );
  out.append( ",\n" );

  // Absent, never null, when a finding has no source position.
  if ( value.span.begin.isValid() )
  {
    appendJsonLocation( out, sources, value.span, indent + "  " );
  }

  out.append( indent ).append( "  \"arguments\": {" );
  for ( std::size_t i = 0; i < value.arguments.size(); ++i )
  {
    out.append( i == 0 ? " " : ", " );
    appendJsonArgument( out, value.arguments[i] );
  }
  out.append( value.arguments.empty() ? "}" : " }" );

  if ( value.notes.empty() )
  {
    out.append( "\n" ).append( indent ).append( "}" );
    return;
  }

  out.append( ",\n" ).append( indent ).append( "  \"notes\": [\n" );
  for ( std::size_t i = 0; i < value.notes.size(); ++i )
  {
    appendJsonDiagnostic( out, sources, value.notes[i], Severity::NOTE, indent + "    " );
    out.append( i + 1 < value.notes.size() ? ",\n" : "\n" );
  }
  out.append( indent ).append( "  ]\n" ).append( indent ).append( "}" );
}

} // namespace

std::string renderText( SourceManager const& sources, DiagnosticSink const& sink )
{
  std::string out;
  for ( Finding const& finding : sink.findings() )
  {
    appendDiagnostic( out, sources, finding.diagnostic, finding.severity );
    out.push_back( '\n' );
  }
  return out;
}

void appendJsonFindings( std::string& out,
                         SourceManager const& sources,
                         DiagnosticSink const& sink,
                         std::string_view indent )
{
  out.append( indent ).append( "\"diagnostics\": [\n" );

  auto const findings = sink.findings();
  std::string const deeper = std::string{ indent } + "  ";
  for ( std::size_t i = 0; i < findings.size(); ++i )
  {
    appendJsonDiagnostic( out, sources, findings[i].diagnostic, findings[i].severity, deeper );
    out.append( i + 1 < findings.size() ? ",\n" : "\n" );
  }

  out.append( indent ).append( "],\n" ).append( indent );
  out.append( R"("summary": { "errors": )" ).append( std::to_string( sink.errorCount() ) );
  out.append( R"(, "warnings": )" ).append( std::to_string( sink.warningCount() ) ).append( " }" );
}

} // namespace nga::diag
