#include "nga/model/Emit.hpp"

#include "nga/model/Segments.hpp"
#include "nga/model/Storage.hpp"
#include "nga/model/Transition.hpp"

namespace nga::model
{

Bs93File emitBs93( Patched const& build, diag::DiagnosticSink& sink )
{
  // A Payload waits in storage for a Transition to read it, and this Container
  // carries no loader at all: an emulator copies one block into memory and
  // enters it. So a Payload here is bytes nothing would ever put where they
  // belong, which is a refusal and not a silence. Before the image, not
  // after: a program refused for waiting in storage would otherwise also be
  // told that its Phases share addresses, which is the same fault read a
  // second way.
  if ( refusedPayloads( build, diag::DiagnosticId::BS93_HAS_A_PAYLOAD, sink ) )
  {
    return Bs93File{};
  }

  // The image itself is the raw one: this Container adds a header and a rule,
  // and nothing about which bytes are in it.
  RawImage const image = emitRawImage( build, sink );
  if ( sink.hasErrors() )
  {
    return Bs93File{};
  }
  if ( image.bytes.empty() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::NOTHING_TO_CARRY ).arg( "container", "bs93" ) );
    return Bs93File{};
  }

  std::optional<std::uint32_t> const entry = startAddressOf( build, sink );
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
  // The header as one list, which GCC 14 does not take for a free of memory
  // that was never allocated, as it took a `reserve` and ten `push_back`s.
  Bs93File file;
  file.bytes = { 0x80,
                 0x08,
                 static_cast<std::uint8_t>( image.origin >> 8 ),
                 static_cast<std::uint8_t>( image.origin & 0xFF ),
                 static_cast<std::uint8_t>( length >> 8 ),
                 static_cast<std::uint8_t>( length & 0xFF ),
                 'B',
                 'S',
                 '9',
                 '3' };
  file.bytes.insert( file.bytes.end(), image.bytes.begin(), image.bytes.end() );
  return file;
}

} // namespace nga::model
