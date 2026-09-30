#include "nga/model/Emit.hpp"

#include "nga/model/Storage.hpp"
#include "nga/model/Transition.hpp"

namespace nga::model
{

Bs93File emitBs93( Patched const& build, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  diag::SourceManager const& sources = build.sources();
  GlobalSymbols const& symbols = build.symbols();

  // A Payload waits in storage for a Transition to read it, and this Container
  // carries no loader at all: an emulator copies one block into memory and
  // enters it. So a Payload here is bytes nothing would ever put where they
  // belong, which is a refusal and not a silence.
  bool refused = false;
  Storage const& storage = build.storage();
  for ( std::uint32_t module = 0; module < symbols.modules().size(); ++module )
  {
    Module const& one = symbols.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !storage.hasPayload( where ) )
      {
        continue;
      }
      Section const& section = one.sections()[index];
      sink.add( diag::diagnostic( diag::DiagnosticId::BS93_HAS_A_PAYLOAD )
                    .at( section.span().begin, section.span().length )
                    .arg( "section", one.displayNameOf( where.section, sources ) ) );
      refused = true;
    }
  }

  // Before the image, not after: a program refused for waiting in storage would
  // otherwise also be told that its Phases share addresses, which is the same
  // fault read a second way.
  if ( refused )
  {
    return Bs93File{};
  }

  // The image itself is the raw one: this Container adds a header and a rule,
  // and nothing about which bytes are in it.
  RawImage const image = emitRawImage( build, sink );
  if ( sink.hasErrors() || image.bytes.empty() )
  {
    return Bs93File{};
  }

  std::optional<std::uint32_t> const entry = entryAddressOf( build, project.phases.entry, sink );
  if ( !entry.has_value() )
  {
    return Bs93File{};
  }

  // One address in the header is both where the block lands and where it is
  // entered, so an entry above the image's first byte is a program this format
  // cannot carry. A `jmp` the tool put in front would move the address the
  // header has to hold, which is the thing being computed.
  if ( *entry != image.origin )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::BS93_ENTRY_NOT_LOWEST )
                  .arg( "entry", *entry )
                  .arg( "begin", image.origin ) );
    return Bs93File{};
  }

  std::uint32_t const length = BS93_HEADER_SIZE + static_cast<std::uint32_t>( image.bytes.size() );
  Bs93File file;
  file.bytes.reserve( length );
  file.bytes.push_back( 0x80 );
  file.bytes.push_back( 0x08 );
  file.bytes.push_back( static_cast<std::uint8_t>( image.origin >> 8 ) );
  file.bytes.push_back( static_cast<std::uint8_t>( image.origin & 0xFF ) );
  file.bytes.push_back( static_cast<std::uint8_t>( length >> 8 ) );
  file.bytes.push_back( static_cast<std::uint8_t>( length & 0xFF ) );
  file.bytes.push_back( 'B' );
  file.bytes.push_back( 'S' );
  file.bytes.push_back( '9' );
  file.bytes.push_back( '3' );
  file.bytes.insert( file.bytes.end(), image.bytes.begin(), image.bytes.end() );
  return file;
}

} // namespace nga::model
