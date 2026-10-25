#include "nga/c/Lines.hpp"

#include "nga/c/Front.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nga::c::detail
{

namespace
{

/// Whether a character may stand in a name, so that one beside a name found
/// makes it part of another.
bool partOfName( char c )
{
  return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
}

/// Where the name next stands in the lines as a word of its own, from `from`
/// on.
std::size_t findName( std::string_view lines, std::string_view name, std::size_t from )
{
  for ( std::size_t at = lines.find( name, from ); at != std::string_view::npos; at = lines.find( name, at + 1 ) )
  {
    std::size_t const after = at + name.size();
    if ( ( at == 0 || !partOfName( lines[at - 1] ) ) && ( after == lines.size() || !partOfName( lines[after] ) ) )
    {
      return at;
    }
  }
  return std::string_view::npos;
}

} // namespace

bool Line::isMark() const
{
  return bare.starts_with( ".source " );
}

std::string_view Line::mnemonic() const
{
  return bare.substr( 0, bare.find( ' ' ) );
}

std::string_view Line::operand() const
{
  std::size_t const space = bare.find( ' ' );
  return space == std::string_view::npos ? std::string_view{} : bare.substr( space + 1 );
}

Line lineAt( std::string_view text, std::size_t from )
{
  if ( from >= text.size() )
  {
    return Line{ .begin = text.size(), .end = text.size(), .bare = {}, .indented = false };
  }
  std::size_t const end = std::min( text.find( '\n', from ), text.size() - 1 ) + 1;
  std::string_view bare = text.substr( from, end - from );
  bool const indented = bare.front() == ' ';
  bare.remove_prefix( std::min( bare.find_first_not_of( ' ' ), bare.size() ) );
  while ( !bare.empty() && ( bare.back() == '\n' || bare.back() == ' ' ) )
  {
    bare.remove_suffix( 1 );
  }
  return Line{ .begin = from, .end = end, .bare = bare, .indented = indented };
}

Line pastMarks( std::string_view text, std::size_t from )
{
  Line line = lineAt( text, from );
  while ( line.isMark() )
  {
    line = lineAt( text, line.end );
  }
  return line;
}

bool isStoreTo( std::string_view line, std::string_view name )
{
  if ( !( line.starts_with( "sta " ) || line.starts_with( "stx " ) || line.starts_with( "sty " ) ) )
  {
    return false;
  }
  std::string_view const operand = line.substr( 4 );
  if ( !operand.starts_with( name ) )
  {
    return false;
  }
  std::string_view const rest = operand.substr( name.size() );
  return rest.empty() || ( rest.size() > 1 && rest.front() == '+' &&
                           rest.substr( 1 ).find_first_not_of( "0123456789" ) == std::string_view::npos );
}

bool namesByte( std::string_view lines, std::string_view name )
{
  return findName( lines, name, 0 ) != std::string_view::npos;
}

bool onlyStoredTo( std::string_view lines, std::string_view name )
{
  for ( std::size_t at = findName( lines, name, 0 ); at != std::string_view::npos;
        at = findName( lines, name, at + 1 ) )
  {
    std::size_t const newline = lines.rfind( '\n', at );
    if ( !isStoreTo( lineAt( lines, newline == std::string_view::npos ? 0 : newline + 1 ).bare, name ) )
    {
      return false;
    }
  }
  return true;
}

std::string ownLines( std::string body, std::string const& name )
{
  for ( std::size_t at = body.find( "." + name ); at != std::string::npos; at = body.find( "." + name, at + 1 ) )
  {
    std::size_t begin = at;
    while ( begin > 0 && partOfName( body[begin - 1] ) )
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

bool onlyStoredIn( std::string const& body, std::string const& name )
{
  return onlyStoredTo( ownLines( body, name ), name );
}

bool namedIn( std::string const& body, std::string const& name )
{
  return namesByte( ownLines( body, name ), name );
}

std::size_t estimatedBytes( std::string_view text )
{
  std::size_t bytes = 0;
  for ( Line line = lineAt( text, 0 ); line.begin < text.size(); line = lineAt( text, line.end ) )
  {
    if ( !line.indented || line.bare.empty() || line.bare.front() == '.' )
    {
      continue;
    }
    std::string_view const mnemonic = line.mnemonic();
    if ( mnemonic.size() == line.bare.size() )
    {
      bytes += 1;
      continue;
    }
    std::string_view const operand = line.operand();
    bool const branch = mnemonic.front() == 'j' && mnemonic != "jmp" && mnemonic != "jsr";
    bool const own = operand.starts_with( "__" ) || operand.starts_with( '#' ) || operand.starts_with( '@' ) ||
                     operand.starts_with( '(' );
    bytes += branch || mnemonic.front() == 'b' || own ? std::size_t{ 2 } : std::size_t{ 3 };
  }
  return bytes;
}

void dropStoresTo( std::string& text, std::size_t begin, std::vector<std::string> const& names )
{
  if ( names.empty() )
  {
    return;
  }
  std::string kept;
  for ( Line line = lineAt( text, begin ); line.begin < text.size(); line = lineAt( text, line.end ) )
  {
    if ( std::ranges::none_of( names, [&line]( std::string const& name ) { return isStoreTo( line.bare, name ); } ) )
    {
      kept.append( text, line.begin, line.end - line.begin );
    }
  }
  text.replace( begin, text.size() - begin, kept );
}

void loadBackWhatZeroStored( std::string& text, std::size_t begin )
{
  std::size_t from = begin;
  while ( from < text.size() )
  {
    Line const stored = lineAt( text, from );
    std::string const name{ stored.operand() };
    if ( stored.mnemonic() != "stz" || name.empty() )
    {
      from = stored.end;
      continue;
    }
    Line const reader = pastMarks( text, stored.end );
    if ( reader.bare != "lda " + name )
    {
      from = stored.end;
      continue;
    }

    std::string const put = std::string{ INDENT } + "lda #0\n" + std::string{ INDENT } + "sta " + name + "\n";
    text.erase( reader.begin, reader.end - reader.begin );
    text.replace( stored.begin, stored.end - stored.begin, put );
    from += put.size();
  }
}

namespace
{

/// Each conditional jump and the one taken where it is not.
constexpr std::array<std::array<std::string_view, 2>, 8> INVERSES{ { { "jcc", "jcs" },
                                                                     { "jcs", "jcc" },
                                                                     { "jeq", "jne" },
                                                                     { "jne", "jeq" },
                                                                     { "jmi", "jpl" },
                                                                     { "jpl", "jmi" },
                                                                     { "jvc", "jvs" },
                                                                     { "jvs", "jvc" } } };

/// Whether the label stands before the next instruction from `from` on, with
/// nothing but this body's labels and marks between.
bool landsAt( std::string_view text, std::size_t from, std::string_view label )
{
  for ( Line next = lineAt( text, from );
        next.begin < text.size() && ( next.isMark() || ( !next.indented && next.bare.starts_with( '@' ) ) );
        next = lineAt( text, next.end ) )
  {
    if ( !next.isMark() && next.bare == label )
    {
      return true;
    }
  }
  return false;
}

} // namespace

void dropJumpsToWhatFollows( std::string& text, std::size_t begin )
{
  static constexpr std::array<std::string_view, 14> JUMPS{ "jmp", "jra", "jmpcc", "jmpcs", "jmpeq", "jmpne", "jcc",
                                                           "jcs", "jeq", "jne",   "jmi",   "jpl",   "jvc",   "jvs" };
  std::size_t from = begin;
  while ( from < text.size() )
  {
    Line const jump = lineAt( text, from );
    std::string_view const target = jump.operand();
    if ( !jump.indented || !target.starts_with( '@' ) || std::ranges::find( JUMPS, jump.mnemonic() ) == JUMPS.end() )
    {
      from = jump.end;
      continue;
    }
    // Only this body's own labels: anything else unindented ends the search.
    if ( landsAt( text, jump.end, target ) )
    {
      text.erase( jump.begin, jump.end - jump.begin );
      continue;
    }

    // A conditional jump over the certain one written right after it.
    auto const inverse =
        std::ranges::find_if( INVERSES, [&jump]( auto const& pair ) { return pair[0] == jump.mnemonic(); } );
    Line const over = pastMarks( text, jump.end );
    std::string_view const onward = over.operand();
    bool const certain = over.mnemonic() == "jmp" || over.mnemonic() == "jra" || over.mnemonic().starts_with( "jmp" );
    if ( inverse != INVERSES.end() && over.indented && certain && onward.starts_with( '@' ) &&
         landsAt( text, over.end, target ) )
    {
      std::string const put =
          std::string{ INDENT } + std::string{ ( *inverse )[1] } + " " + std::string{ onward } + "\n";
      text.replace( jump.begin, over.end - jump.begin, put );
      continue;
    }
    from = jump.end;
  }
}

void setBitsInPlace( std::string& text, std::size_t begin )
{
  auto const fillsA = []( std::string_view line )
  { return line.starts_with( "lda " ) || line == "pla" || line == "txa" || line == "tya"; };

  std::size_t from = begin;
  while ( from < text.size() )
  {
    Line const masks = lineAt( text, from );
    std::string const name{ masks.operand() };
    // `tsb` reaches a byte by its address alone, zero page or absolute, and
    // an element an index register chooses is no such byte.
    if ( masks.mnemonic() != "ora" || name.empty() || name.find_first_of( ",(#" ) != std::string::npos )
    {
      from = masks.end;
      continue;
    }

    // Strictly the next line, and strictly the one after it: a label or a
    // mark between means these are not one statement.
    Line const stored = lineAt( text, masks.end );
    if ( stored.bare != "sta " + name )
    {
      from = masks.end;
      continue;
    }
    // The next line that is an instruction: a mark stands between two
    // statements, and the statement after this one is what matters.
    if ( !fillsA( pastMarks( text, stored.end ).bare ) )
    {
      from = masks.end;
      continue;
    }

    std::string const put = std::string{ INDENT } + "tsb " + name + "\n";
    text.replace( masks.begin, stored.end - masks.begin, put );
    from += put.size();
  }
}

void dropUnnamedBytes( std::string& text,
                       std::vector<Declared> const& declared,
                       std::vector<std::size_t> const& regions,
                       std::vector<std::string> const& owners )
{
  // What the Module's lines say, against what they declare: the text with
  // the declarations that may go left out of it, whole and for each
  // function's own stretch of it.
  auto const without = [&]( std::size_t from, std::size_t to )
  {
    std::string lines;
    std::size_t at = from;
    for ( Declared const& one : declared )
    {
      if ( one.begin >= from && one.end <= to )
      {
        lines.append( text, at, one.begin - at );
        at = one.end;
      }
    }
    lines.append( text, at, to - at );
    return lines;
  };
  std::string const lines = without( 0, text.size() );
  std::vector<std::string> own;
  own.reserve( regions.size() );
  for ( std::size_t region = 0; region < regions.size(); ++region )
  {
    own.push_back( without( regions[region], region + 1 < regions.size() ? regions[region + 1] : text.size() ) );
  }

  // A byte is named by its own Proc's text as it is, and anywhere else only
  // as `PROC.NAME`: another Proc's byte of the same name, `__t0` in each,
  // is another byte.
  auto const named = [&]( Declared const& one )
  { return namesByte( own[one.region], one.name ) || namesByte( lines, one.owner + "." + one.name ); };

  // A byte that every line naming it only stores to is read by nothing:
  // what is kept in it is kept in `A`, where the loads 0110 left out found
  // it. Those stores go, and then the byte is named nowhere. See
  // docs/decisions/0143-a-byte-only-stored-to-is-not-kept.md.
  std::set<std::pair<std::size_t, std::string>> writeOnly;
  for ( Declared const& one : declared )
  {
    if ( named( one ) && onlyStoredTo( own[one.region], one.name ) &&
         onlyStoredTo( lines, one.owner + "." + one.name ) )
    {
      writeOnly.emplace( one.region, one.name );
    }
  }

  // The text again, a line at a time: a declaration that goes where
  // nothing names its byte, and a store to a byte only ever stored to.
  std::string kept;
  kept.reserve( text.size() );
  std::size_t next = 0;
  std::size_t region = 0;
  for ( Line line = lineAt( text, 0 ); line.begin < text.size(); line = lineAt( text, line.end ) )
  {
    bool drop = false;
    while ( next < declared.size() && declared[next].end <= line.begin )
    {
      ++next;
    }
    while ( region + 1 < regions.size() && regions[region + 1] <= line.begin )
    {
      ++region;
    }
    if ( next < declared.size() && declared[next].begin == line.begin )
    {
      drop = !named( declared[next] ) || writeOnly.contains( { declared[next].region, declared[next].name } );
    }
    else
    {
      // A store in the byte's own stretch by its name, or anywhere by its
      // Proc's.
      drop = std::ranges::any_of( writeOnly,
                                  [&]( std::pair<std::size_t, std::string> const& byte )
                                  {
                                    return ( byte.first == region && !regions.empty() &&
                                             line.begin >= regions[region] && isStoreTo( line.bare, byte.second ) ) ||
                                           isStoreTo( line.bare, owners[byte.first] + "." + byte.second );
                                  } );
    }
    if ( !drop )
    {
      kept.append( text, line.begin, line.end - line.begin );
    }
  }
  text = std::move( kept );
}

} // namespace nga::c::detail
