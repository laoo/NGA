#include "nga/model/Emit.hpp"

#include "nga/model/Segments.hpp"
#include "nga/model/Storage.hpp"
#include "nga/model/Transition.hpp"

namespace nga::model
{

namespace
{

/// The thirteen bytes BASIC reads before the program does: a link to the line
/// after this one, the line's number, the `SYS` token, five digits, the byte
/// that ends the line and the two zeroes that end the program. The link points
/// at those two zeroes, which is where the next line would begin.
///
/// The digits are five whatever the address needs, and that is the whole reason
/// the stub has one length: a four-digit `SYS` ends one byte lower, so the code
/// behind it begins at $040D, and a program written by hand has to put a pad
/// byte there or be entered one byte into its first instruction. Nothing here
/// can forget it, because there is nothing to forget.
std::vector<std::uint8_t> stubFor( std::uint32_t entry )
{
  constexpr std::uint8_t sysToken = 0x9E;
  std::uint32_t const endOfProgram = PRG_LOAD_ADDRESS + PRG_STUB_SIZE - 2;

  std::vector<std::uint8_t> stub;
  stub.reserve( PRG_STUB_SIZE );
  stub.push_back( static_cast<std::uint8_t>( endOfProgram & 0xFF ) );
  stub.push_back( static_cast<std::uint8_t>( endOfProgram >> 8 ) );
  stub.push_back( 10 ); // the line number, low byte
  stub.push_back( 0 );
  stub.push_back( sysToken );
  for ( int digit = 4; digit >= 0; --digit )
  {
    std::uint32_t divisor = 1;
    for ( int step = 0; step < digit; ++step )
    {
      divisor *= 10;
    }
    stub.push_back( static_cast<std::uint8_t>( '0' + ( ( entry / divisor ) % 10 ) ) );
  }
  stub.push_back( 0 ); // the line ends
  stub.push_back( 0 ); // and so does the program
  stub.push_back( 0 );
  return stub;
}

} // namespace

PrgFile emitPrg( Patched const& build, diag::DiagnosticSink& sink )
{
  diag::SourceManager const& sources = build.sources();
  GlobalSymbols const& symbols = build.symbols();

  // A Payload waits in storage for a Transition to read it, and this Container
  // carries no loader: a PET is handed one contiguous block and a line of
  // BASIC. So a Payload here is bytes nothing would ever put where they belong,
  // which is a refusal and not a silence — as it is in a `bs93`.
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
      sink.add( diag::diagnostic( diag::DiagnosticId::PRG_HAS_A_PAYLOAD )
                    .at( section.span().begin, section.span().length )
                    .arg( "section", one.displayNameOf( where.section, sources ) ) );
      refused = true;
    }
  }

  // Before the image, for the reason a `bs93` reports it before the image: a
  // program refused for waiting in storage would otherwise also be told that
  // its Phases share addresses, which is the same fault read a second way.
  if ( refused )
  {
    return PrgFile{};
  }

  return prgWith( build, sink );
}

PrgFile prgWith( Patched const& build, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();

  // What the block carries: the Sections the entry Phase needs that emit
  // bytes, the driver and the decoders among them, laid end to end from the
  // lowest to the highest with the holes between them zero. Not the raw image,
  // which is memory at one instant and would carry the Sections of every Phase
  // — the ones another Phase loads wait in storage, where a diskette has room
  // for them.
  std::vector<Resident> const carried = residentSections( build, {} );
  if ( carried.empty() )
  {
    return PrgFile{};
  }
  std::uint32_t begin = 0xFFFFFFFF;
  std::uint32_t end = 0;
  for ( Resident const& one : carried )
  {
    begin = std::min( begin, one.address );
    end = std::max( end, one.address + static_cast<std::uint32_t>( one.content.size() ) );
  }

  RawImage image;
  image.origin = begin;
  image.bytes.assign( end - begin, 0 );
  for ( Resident const& one : carried )
  {
    std::ranges::copy( one.content, image.bytes.begin() + static_cast<std::ptrdiff_t>( one.address - begin ) );
  }

  // `LOAD "NAME",8` puts the block at BASIC's start whatever the file's own
  // header says, so the stub is at $0401 and the program begins behind it. A
  // Section lower than that would be written over BASIC's own pointers by the
  // load itself, before a byte of the program has run.
  if ( image.origin < PRG_ENTRY_FLOOR )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::PRG_BELOW_THE_STUB )
                  .arg( "begin", image.origin )
                  .arg( "floor", PRG_ENTRY_FLOOR ) );
    return PrgFile{};
  }

  std::optional<std::uint32_t> const entry = entryAddressOf( build, project.phases.entry, sink );
  if ( !entry.has_value() )
  {
    return PrgFile{};
  }

  PrgFile file;
  file.bytes.reserve( PRG_STUB_SIZE + 2 + image.bytes.size() );
  file.bytes.push_back( static_cast<std::uint8_t>( PRG_LOAD_ADDRESS & 0xFF ) );
  file.bytes.push_back( static_cast<std::uint8_t>( PRG_LOAD_ADDRESS >> 8 ) );
  std::vector<std::uint8_t> const stub = stubFor( *entry );
  file.bytes.insert( file.bytes.end(), stub.begin(), stub.end() );

  // The block is one run, so a program whose lowest Section stands well above
  // the stub pays the distance in file bytes, as a raw image does.
  file.bytes.insert( file.bytes.end(), image.origin - PRG_ENTRY_FLOOR, 0 );
  file.bytes.insert( file.bytes.end(), image.bytes.begin(), image.bytes.end() );
  return file;
}

} // namespace nga::model
