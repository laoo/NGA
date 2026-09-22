#include "nga/model/Emit.hpp"

#include <algorithm>

namespace nga::model
{

RawImage emitRawImage( Patched const& build, diag::DiagnosticSink& sink )
{
  diag::SourceManager const& sources = build.sources();
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Layout const& layout = build.layout();
  Bytes const& bytes = build.bytes();
  std::uint32_t lowest = 0x10000;
  std::uint32_t highest = 0;
  bool any = false;

  auto const eachPlacedSection = [&symbols, &sizes, &layout]( auto&& visit )
  {
    for ( std::uint32_t module = 0; module < symbols.modules().size(); ++module )
    {
      Module const& one = symbols.modules()[module];
      for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
      {
        SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
        if ( !layout.isPlaced( where ) || !sizes.isKnown( where ) || sizes.sizeOfSection( where ) == 0 ||
             !one.sections()[index].emitsBytes() || one.sections()[index].pane().has_value() )
        {
          // A reservation puts nothing in an image, and a zero-page one would
          // otherwise stretch it from address zero. A Pane's Section is in a
          // Bank and never in base memory, which is what a raw image is — see
          // docs/decisions/0054-panes.md.
          continue;
        }
        visit( where, layout.addressOf( where ), sizes.sizeOfSection( where ) );
      }
    }
  };

  // What this Container cannot hold: two Sections with bytes at one address.
  // Place was right to allow it — their Residency is disjoint, or Place would
  // have said so — and this is the one Step that knows the image is a single
  // instant. Reported at the later of the two in Project order, as an overlap
  // is, and the run emits nothing.
  struct Content
  {
    SectionRef where;
    AddressRange range;
  };

  std::vector<Content> contents;
  eachPlacedSection(
      [&contents]( SectionRef where, std::uint32_t address, std::uint32_t size )
      { contents.push_back( Content{ .where = where, .range = { .begin = address, .end = address + size } } ); } );

  bool refused = false;
  for ( std::size_t later = 0; later < contents.size(); ++later )
  {
    for ( std::size_t earlier = 0; earlier < later; ++earlier )
    {
      AddressRange const& a = contents[earlier].range;
      AddressRange const& b = contents[later].range;
      if ( a.begin < b.end && b.begin < a.end )
      {
        Section const& section =
            symbols.moduleAt( contents[later].where.module ).sectionAt( contents[later].where.section );
        sink.add( diag::diagnostic( diag::DiagnosticId::OVERLAY_IN_RAW_IMAGE )
                      .at( section.span().begin, section.span().length )
                      .arg( "section",
                            symbols.moduleAt( contents[later].where.module )
                                .displayNameOf( contents[later].where.section, sources ) )
                      .arg( "other",
                            symbols.moduleAt( contents[earlier].where.module )
                                .displayNameOf( contents[earlier].where.section, sources ) )
                      .arg( "address", static_cast<std::int64_t>( std::max( a.begin, b.begin ) ) ) );
        refused = true;
      }
    }
  }
  if ( refused )
  {
    return RawImage{};
  }

  eachPlacedSection(
      [&lowest, &highest, &any]( SectionRef /*where*/, std::uint32_t address, std::uint32_t size )
      {
        lowest = std::min( lowest, address );
        highest = std::max( highest, address + size );
        any = true;
      } );

  if ( !any )
  {
    return RawImage{};
  }

  RawImage image{ .origin = lowest, .bytes = std::vector<std::uint8_t>( highest - lowest, 0 ) };
  eachPlacedSection(
      [&image, &bytes]( SectionRef where, std::uint32_t address, std::uint32_t size )
      {
        std::span<std::uint8_t const> const content = bytes.of( where );
        for ( std::uint32_t at = 0; at < size && at < content.size(); ++at )
        {
          image.bytes[address - image.origin + at] = content[at];
        }
      } );

  return image;
}

} // namespace nga::model
