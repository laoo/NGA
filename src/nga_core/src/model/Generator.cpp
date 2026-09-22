#include "nga/model/Generator.hpp"

#include "nga/diag/Diagnostic.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <span>

namespace nga::model
{

namespace
{

/// How many bytes one `.base64` line carries. A fixed count of bytes rather
/// than of characters, so that one changed byte in a generator's input touches
/// one line of a golden — see docs/decisions/0069-generators.md.
constexpr std::size_t BYTES_TO_A_LINE = 48;

/// The column a statement stands in, which is also how wide a label may be
/// before it takes the line to itself.
constexpr std::size_t STATEMENT_COLUMN = 8;

/// How long an `.export` line is allowed to grow before the next name opens
/// another: they sum, as every directive that lists names does.
constexpr std::size_t EXPORT_COLUMNS = 96;

std::string base64Of( std::uint8_t const* bytes, std::size_t count )
{
  static constexpr std::string_view ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string text;
  text.reserve( ( count + 2 ) / 3 * 4 );

  for ( std::size_t at = 0; at < count; at += 3 )
  {
    std::size_t const left = count - at;
    std::uint32_t group = static_cast<std::uint32_t>( bytes[at] ) << 16U;
    if ( left > 1 )
    {
      group |= static_cast<std::uint32_t>( bytes[at + 1] ) << 8U;
    }
    if ( left > 2 )
    {
      group |= static_cast<std::uint32_t>( bytes[at + 2] );
    }

    text += ALPHABET[group >> 18U & 0x3FU];
    text += ALPHABET[group >> 12U & 0x3FU];
    text += left > 1 ? ALPHABET[group >> 6U & 0x3FU] : '=';
    text += left > 2 ? ALPHABET[group & 0x3FU] : '=';
  }

  return text;
}

/// A label and the statement it names share a line, as any label does: the
/// label at column one, the statement at the column the rest of them stand in.
void openStatement( std::string& text, std::optional<std::string> const& label )
{
  if ( !label.has_value() )
  {
    text.append( STATEMENT_COLUMN, ' ' );
    return;
  }

  text += *label;
  if ( label->size() < STATEMENT_COLUMN )
  {
    text.append( STATEMENT_COLUMN - label->size(), ' ' );
    return;
  }
  text += ' ';
}

/// An address as a Project or a Module writes one, so that the emitted text
/// reads like text somebody wrote.
std::string hexOf( std::int64_t value )
{
  static constexpr std::string_view DIGITS = "0123456789ABCDEF";
  auto const bits = static_cast<std::uint64_t>( value );

  std::string text = "$";
  for ( int shift = bits > 0xFFFF ? 28 : 12; shift >= 0; shift -= 4 )
  {
    text += DIGITS[bits >> static_cast<unsigned>( shift ) & 0xFU];
  }
  return text;
}

void emitSectionHeader( std::string& text, GeneratedSection const& section )
{
  text += ".section ";
  text += nameOf( section.placement );

  if ( section.pinnedAddress.has_value() )
  {
    text += " at " + hexOf( *section.pinnedAddress );
  }
  if ( section.alignment.has_value() )
  {
    text += " align " + std::to_string( *section.alignment );
  }
  if ( section.boundary.has_value() )
  {
    text += " within " + std::to_string( *section.boundary );
  }
  if ( section.pane.has_value() )
  {
    text += ", in " + *section.pane;
  }
  if ( section.movable )
  {
    text += ", movable";
  }
  if ( section.root )
  {
    text += ", root";
  }
  if ( section.temporary )
  {
    text += ", temporary";
  }
  text += '\n';
  // The name is the Label at the Section's start, which is what names a
  // Section.
  text += section.name;
  text += '\n';
}

/// Every name the Section defines. A generator exports all of them, because a
/// name generated and not exported is a name for nobody.
std::vector<std::string> namesOf( GeneratedSection const& section )
{
  std::vector<std::string> names{ section.name };
  for ( GeneratedItem const& item : section.items )
  {
    if ( item.label.has_value() )
    {
      names.push_back( *item.label );
    }
    if ( auto const* constant = std::get_if<GeneratedConstant>( &item.content ) )
    {
      names.push_back( constant->name );
    }
  }
  return names;
}

void emitExports( std::string& text, GeneratedSection const& section )
{
  std::vector<std::string> const names = namesOf( section );
  std::size_t at = 0;
  while ( at < names.size() )
  {
    std::string line = ".export ";
    for ( bool first = true; at < names.size(); ++at )
    {
      if ( !first && line.size() + names[at].size() + 2 > EXPORT_COLUMNS )
      {
        break;
      }
      if ( !first )
      {
        line += ", ";
      }
      line += names[at];
      first = false;
    }
    text += line;
    text += '\n';
  }
}

void emitItem( std::string& text, GeneratedItem const& item )
{
  if ( auto const* constant = std::get_if<GeneratedConstant>( &item.content ) )
  {
    // A Constant is a name at column one followed by `=`, so it takes the line
    // a label would have shared.
    if ( item.label.has_value() )
    {
      text += *item.label;
      text += '\n';
    }
    text += constant->name;
    text += " = ";
    text += std::to_string( constant->value );
    text += '\n';
    return;
  }

  if ( auto const* reserve = std::get_if<GeneratedReserve>( &item.content ) )
  {
    openStatement( text, item.label );
    text += ".res ";
    text += std::to_string( reserve->size );
    text += '\n';
    return;
  }

  auto const* bytes = std::get_if<GeneratedBytes>( &item.content );
  if ( bytes == nullptr || bytes->bytes.empty() )
  {
    // A label and nothing else: a position, which is what names the end of a
    // Section.
    if ( item.label.has_value() )
    {
      text += *item.label;
      text += '\n';
    }
    return;
  }

  for ( std::size_t at = 0; at < bytes->bytes.size(); at += BYTES_TO_A_LINE )
  {
    std::size_t const count = std::min( BYTES_TO_A_LINE, bytes->bytes.size() - at );

    // The label goes in front of the first line of the run and not of each.
    openStatement( text, at == 0 ? item.label : std::nullopt );
    text += ".base64 \"";
    text += base64Of( bytes->bytes.data() + at, count );
    text += "\"\n";
  }
}

bool isName( std::string_view text )
{
  if ( text.empty() )
  {
    return false;
  }
  auto const isStart = []( char c ) { return ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || c == '_'; };
  if ( !isStart( text.front() ) )
  {
    return false;
  }
  return std::ranges::all_of( text, [&]( char c ) { return isStart( c ) || ( c >= '0' && c <= '9' ); } );
}

std::string at( std::string const& what, std::size_t index )
{
  return what + "[" + std::to_string( index ) + "]";
}

} // namespace

std::optional<std::string> checkModule( GeneratedModule const& description )
{
  if ( description.namespaceName.has_value() && !isName( *description.namespaceName ) )
  {
    return "namespace: `" + *description.namespaceName + "` is no name";
  }

  if ( description.sections.empty() )
  {
    return "sections: a module holds at least one section";
  }

  for ( std::size_t index = 0; index < description.sections.size(); ++index )
  {
    GeneratedSection const& section = description.sections[index];
    std::string const where = at( "sections", index );

    if ( !isName( section.name ) )
    {
      return where + ".name: `" + section.name + "` is no name";
    }
    if ( section.pane.has_value() && !isName( *section.pane ) )
    {
      return where + ".in: `" + *section.pane + "` is no name";
    }

    for ( std::size_t which = 0; which < section.items.size(); ++which )
    {
      GeneratedItem const& item = section.items[which];
      std::string const itemWhere = where + "." + at( "items", which );

      if ( item.label.has_value() && !isName( *item.label ) )
      {
        return itemWhere + ".label: `" + *item.label + "` is no name";
      }
      if ( auto const* reserve = std::get_if<GeneratedReserve>( &item.content );
           reserve != nullptr && reserve->size < 0 )
      {
        return itemWhere + ".reserve: " + std::to_string( reserve->size ) + " is not a size";
      }
      if ( auto const* constant = std::get_if<GeneratedConstant>( &item.content );
           constant != nullptr && !isName( constant->name ) )
      {
        return itemWhere + ".constant: `" + constant->name + "` is no name";
      }
    }
  }

  return std::nullopt;
}

std::string emitModule( GeneratedModule const& description )
{
  std::string text;

  // One mark for the Module, naming the file the generator worked from: a
  // finding on generated data is a finding on the `.section` line, since an
  // encoded payload cannot hold a byte out of range.
  text += ".source \"";
  text += description.source;
  text += "\", 1\n";

  if ( description.namespaceName.has_value() )
  {
    text += ".namespace " + *description.namespaceName + "\n";
  }

  for ( GeneratedSection const& section : description.sections )
  {
    emitSectionHeader( text, section );
    emitExports( text, section );
    for ( GeneratedItem const& item : section.items )
    {
      emitItem( text, item );
    }
    text += ".ends\n";
  }

  if ( description.namespaceName.has_value() )
  {
    text += ".endns\n";
  }

  return text;
}

namespace
{

/// What a parameter takes, which is what a diagnostic names when it was given
/// something else.
enum class Takes : std::uint8_t
{
  TEXT,   ///< a quoted literal: a path
  NUMBER, ///< an expression with a value
  WORD,   ///< a bare name: a Section's name, a Pane's, a placement class
  FLAG,   ///< the name alone, `root`
};

std::string_view nameOf( Takes takes )
{
  switch ( takes )
  {
  case Takes::TEXT:
    return "a quoted literal";
  case Takes::NUMBER:
    return "a number";
  case Takes::WORD:
    return "a name";
  case Takes::FLAG:
    return "no value";
  }
  return "no value";
}

std::string_view whatWasGiven( GeneratorArgument const& argument )
{
  if ( argument.text.has_value() )
  {
    return "a quoted literal";
  }
  if ( argument.number.has_value() )
  {
    return "a number";
  }
  if ( argument.word.has_value() )
  {
    return "a name";
  }
  return "no value";
}

struct Parameter
{
  std::string_view name;
  Takes takes = Takes::TEXT;
};

/// `binary`: the file, and the attributes of the Section it becomes. The order
/// is the order a message lists them in.
constexpr std::array<Parameter, 10> BINARY_PARAMETERS{
  {
      { .name = "section", .takes = Takes::WORD },
      { .name = "namespace", .takes = Takes::WORD },
      { .name = "placement", .takes = Takes::WORD },
      { .name = "at", .takes = Takes::NUMBER },
      { .name = "align", .takes = Takes::NUMBER },
      { .name = "within", .takes = Takes::NUMBER },
      { .name = "in", .takes = Takes::WORD },
      { .name = "movable", .takes = Takes::FLAG },
      { .name = "root", .takes = Takes::FLAG },
      { .name = "temporary", .takes = Takes::FLAG },
  },
};

/// The arguments of one call, checked against a generator's parameters: each
/// named once, each of the kind its parameter takes, and none the generator
/// does not have.
class Arguments
{
public:
  Arguments( GeneratorCall const& call, std::span<Parameter const> parameters, diag::DiagnosticSink& sink )
      : mCall( &call ), mSink( &sink )
  {
    for ( GeneratorArgument const& argument : call.arguments )
    {
      if ( argument.name.empty() )
      {
        if ( mPositional != nullptr )
        {
          report( diag::DiagnosticId::GENERATOR_ARGUMENT_TWICE, argument.span );
          mFailed = true;
          continue;
        }
        mPositional = &argument;
        continue;
      }

      auto const parameter =
          std::ranges::find_if( parameters, [&]( Parameter const& one ) { return one.name == argument.name; } );
      if ( parameter == parameters.end() )
      {
        reportNamed( diag::DiagnosticId::GENERATOR_ARGUMENT_UNKNOWN, argument.span, argument.name );
        mFailed = true;
        continue;
      }

      if ( !mGiven.emplace( argument.name, &argument ).second )
      {
        report( diag::DiagnosticId::GENERATOR_ARGUMENT_TWICE, argument.span );
        mFailed = true;
        continue;
      }

      if ( whatWasGiven( argument ) != nameOf( parameter->takes ) )
      {
        mSink->add( diag::diagnostic( diag::DiagnosticId::GENERATOR_ARGUMENT_KIND )
                        .at( argument.span.begin, argument.span.length )
                        .arg( "name", argument.name )
                        .arg( "wanted", std::string{ nameOf( parameter->takes ) } )
                        .arg( "given", std::string{ whatWasGiven( argument ) } ) );
        mFailed = true;
      }
    }
  }

  /// The one positional argument, which every generator takes and which names
  /// the file it works from.
  [[nodiscard]] std::optional<std::string> file()
  {
    if ( mPositional == nullptr || !mPositional->text.has_value() )
    {
      diag::SourceSpan const where = mPositional == nullptr ? mCall->generatorSpan : mPositional->span;
      mSink->add( diag::diagnostic( diag::DiagnosticId::GENERATOR_NEEDS_FILE )
                      .at( where.begin, where.length )
                      .arg( "generator", mCall->generator ) );
      mFailed = true;
      return std::nullopt;
    }
    return mPositional->text;
  }

  [[nodiscard]] std::optional<std::int64_t> number( std::string_view name ) const
  {
    GeneratorArgument const* argument = find( name );
    return argument == nullptr ? std::nullopt : argument->number;
  }

  [[nodiscard]] std::optional<std::string> word( std::string_view name ) const
  {
    GeneratorArgument const* argument = find( name );
    return argument == nullptr ? std::nullopt : argument->word;
  }

  [[nodiscard]] bool flag( std::string_view name ) const
  {
    return find( name ) != nullptr;
  }

  [[nodiscard]] bool failed() const
  {
    return mFailed;
  }

private:
  [[nodiscard]] GeneratorArgument const* find( std::string_view name ) const
  {
    auto const found = mGiven.find( std::string{ name } );
    return found == mGiven.end() ? nullptr : found->second;
  }

  void report( diag::DiagnosticId id, diag::SourceSpan where )
  {
    mSink->add( diag::diagnostic( id ).at( where.begin, where.length ) );
  }

  void reportNamed( diag::DiagnosticId id, diag::SourceSpan where, std::string name )
  {
    mSink->add( diag::diagnostic( id ).at( where.begin, where.length ).arg( "name", std::move( name ) ) );
  }

  GeneratorCall const* mCall;
  diag::DiagnosticSink* mSink;
  GeneratorArgument const* mPositional = nullptr;
  std::map<std::string, GeneratorArgument const*> mGiven;
  bool mFailed = false;
};

/// `placement = zeropage`, and `absolute` where nothing said.
std::optional<PlacementClass> placementNamed( std::string const& word )
{
  if ( word == "absolute" )
  {
    return PlacementClass::ABSOLUTE;
  }
  if ( word == "zeropage" )
  {
    return PlacementClass::ZEROPAGE;
  }
  return std::nullopt;
}

std::optional<GeneratedModule>
runBinary( GeneratorCall const& call, GeneratorFiles const& files, diag::DiagnosticSink& sink )
{
  Arguments arguments{ call, BINARY_PARAMETERS, sink };
  std::optional<std::string> const path = arguments.file();

  GeneratedSection section;
  section.name = arguments.word( "section" ).value_or( call.module );
  section.pinnedAddress = arguments.number( "at" );
  section.alignment = arguments.number( "align" );
  section.boundary = arguments.number( "within" );
  section.pane = arguments.word( "in" );
  section.movable = arguments.flag( "movable" );
  section.root = arguments.flag( "root" );
  section.temporary = arguments.flag( "temporary" );

  if ( std::optional<std::string> const placement = arguments.word( "placement" ) )
  {
    std::optional<PlacementClass> const named = placementNamed( *placement );
    if ( !named.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::GENERATOR_ARGUMENT_KIND )
                    .at( call.generatorSpan.begin, call.generatorSpan.length )
                    .arg( "name", "placement" )
                    .arg( "wanted", "`absolute` or `zeropage`" )
                    .arg( "given", "`" + *placement + "`" ) );
      return std::nullopt;
    }
    section.placement = *named;
  }

  if ( arguments.failed() || !path.has_value() )
  {
    return std::nullopt;
  }

  std::string resolved;
  std::optional<std::string> const contents = files.read( *path, resolved );
  if ( !contents.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::CANNOT_READ_FILE )
                  .at( call.generatorSpan.begin, call.generatorSpan.length )
                  .arg( "path", resolved ) );
    return std::nullopt;
  }

  section.items.push_back( GeneratedItem{
      .label = std::nullopt,
      .content = GeneratedBytes{ .bytes = std::vector<std::uint8_t>{ contents->begin(), contents->end() } },
  } );

  GeneratedModule description;

  // The path as the entry wrote it, not the one it resolved to: the mark is for
  // a reader, and an absolute path would make the emitted text depend on where
  // the tree is checked out, which is what a golden file cannot have.
  description.source = *path;
  description.namespaceName = arguments.word( "namespace" );
  description.sections.push_back( std::move( section ) );
  return description;
}

} // namespace

std::optional<GeneratedModule>
runGenerator( GeneratorCall const& call, GeneratorFiles const& files, diag::DiagnosticSink& sink )
{
  if ( call.generator == "binary" )
  {
    return runBinary( call, files, sink );
  }
  if ( call.generator == "script" )
  {
    return runScript( call, files, sink );
  }

  sink.add( diag::diagnostic( diag::DiagnosticId::GENERATOR_UNKNOWN )
                .at( call.generatorSpan.begin, call.generatorSpan.length )
                .arg( "name", call.generator ) );
  return std::nullopt;
}

} // namespace nga::model
