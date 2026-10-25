#include "nga/model/Vocabulary.hpp"

#include "nga/Json.hpp"
#include "nga/Version.hpp"
#include "nga/model/Isa.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Tokens.hpp"
#include "nga/syntax/Parser.hpp"

#include <array>
#include <span>
#include <string_view>

namespace nga::model
{

namespace
{

void appendWords( std::string& out, std::span<std::string_view const> words )
{
  out.push_back( '[' );
  for ( std::size_t i = 0; i < words.size(); ++i )
  {
    json::appendString( out, words[i] );
    if ( i + 1 < words.size() )
    {
      out.append( ", " );
    }
  }
  out.push_back( ']' );
}

} // namespace

std::string renderVocabularyJson()
{
  std::string out = "{\n  \"schema\": 1,\n  \"nga\": ";
  json::appendString( out, versionString() );

  out.append( ",\n  \"cpus\": [\n" );
  constexpr std::array<Cpu, 2> cpus{ Cpu::MOS6502, Cpu::WDC65SC02 };
  for ( std::size_t i = 0; i < cpus.size(); ++i )
  {
    out.append( "    { \"name\": " );
    json::appendString( out, nameOf( cpus[i] ) );
    out.append( ", \"mnemonics\": " );
    appendWords( out, mnemonicsOf( cpus[i] == Cpu::WDC65SC02 ) );
    out.append( i + 1 < cpus.size() ? " },\n" : " }\n" );
  }
  out.append( "  ],\n  \"directives\": " );
  appendWords( out, syntax::directiveWords() );
  out.append( ",\n  \"projectWords\": " );
  appendWords( out, projectWords() );
  out.append( "\n}\n" );
  return out;
}

} // namespace nga::model
