#include "nga/model/Segments.hpp"

#include "nga/model/Place.hpp"

#include "nga/model/Transition.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace nga::model
{

void SegmentWriter::segment( std::uint32_t start, std::span<std::uint8_t const> data )
{
  // Nothing to load is no segment: its header would end a byte before it
  // starts, which a loader reads as a wrap of nearly the whole address space.
  if ( data.empty() )
  {
    return;
  }
  std::uint32_t const end = start + static_cast<std::uint32_t>( data.size() ) - 1;
  if ( mOpen.has_value() && mOpen->end + 1 == start )
  {
    mOpen->end = end;
    mBytes[mOpen->endAt] = static_cast<std::uint8_t>( end & 0xFF );
    mBytes[mOpen->endAt + 1] = static_cast<std::uint8_t>( ( end >> 8 ) & 0xFF );
    mBytes.insert( mBytes.end(), data.begin(), data.end() );
    return;
  }
  address( start );
  mOpen = Open{ .end = end, .endAt = mBytes.size() };
  address( end );
  mBytes.insert( mBytes.end(), data.begin(), data.end() );
}

void SegmentWriter::byte( std::uint32_t at, std::uint8_t value )
{
  std::array<std::uint8_t, 1> const one{ value };
  segment( at, one );
}

void SegmentWriter::word( std::uint32_t at, std::uint32_t value )
{
  std::array<std::uint8_t, 2> const two{ static_cast<std::uint8_t>( value & 0xFF ),
                                         static_cast<std::uint8_t>( ( value >> 8 ) & 0xFF ) };
  segment( at, two );
}

void SegmentWriter::address( std::uint32_t value )
{
  mBytes.push_back( static_cast<std::uint8_t>( value & 0xFF ) );
  mBytes.push_back( static_cast<std::uint8_t>( ( value >> 8 ) & 0xFF ) );
}

std::span<std::uint8_t const> contentOf( Bytes const& bytes, SectionRef where, InitialisedExtent extent )
{
  std::span<std::uint8_t const> const content = bytes.of( where );
  std::size_t const begin = std::min<std::size_t>( extent.begin, content.size() );
  return content.subspan( begin, std::min<std::size_t>( extent.size(), content.size() - begin ) );
}

std::optional<std::uint32_t>
addressOfExported( GlobalSymbols const& symbols, std::string_view name, Sizes const& sizes, Layout const& layout )
{
  std::optional<SymbolRef> const where = symbols.find( name );
  return where.has_value() ? addressOfLabel( symbols, *where, sizes, layout ) : std::nullopt;
}

std::vector<Stored> storedImages( Patched const& build )
{
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Storage const& storage = build.storage();
  Bytes const& bytes = build.bytes();
  std::span<Module const> const modules = symbols.modules();

  std::vector<Stored> stored;
  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    for ( std::uint32_t index = 0; index < modules[module].sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      // The Payload's size is Storage's: what waits in a unit is the stored
      // form, and the Section's own size is what it occupies once loaded.
      if ( !storage.hasPayload( where ) )
      {
        continue;
      }
      // One form unless no unit held the whole of it, and then several, each
      // waiting where it was placed.
      for ( std::uint32_t piece = 0; piece < storage.pieceCountOf( where ); ++piece )
      {
        if ( !storage.isPlaced( where, piece ) || storage.sizeOf( where, piece ) == 0 )
        {
          continue;
        }
        stored.push_back(
            Stored{ .at = storage.addressOf( where, piece ), .form = storage.formOf( where, piece ), .where = where } );
      }
    }
  }
  // And every Pane's Section with bytes, which a load writes into the Pane's
  // unit at its offset within the Window, once — see
  // docs/decisions/0054-panes.md.
  for ( Storage::PaneImage const& image : storage.paneImages() )
  {
    stored.push_back( Stored{ .at = image.at,
                              .form = contentOf( bytes, image.where, sizes.initialisedExtentOf( image.where ) ),
                              .where = image.where } );
  }
  // And every Frame, which waits as a Payload does and is read by the
  // routine alone.
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    if ( storage.isFramePlaced( frame ) && !storage.frameBytesOf( frame ).empty() )
    {
      stored.push_back( Stored{
          .at = storage.frameAddressOf( frame ), .form = storage.frameBytesOf( frame ), .where = std::nullopt } );
    }
  }
  std::ranges::stable_sort(
      stored,
      []( Stored const& a, Stored const& b )
      { return a.at.bank.value != b.at.bank.value ? a.at.bank.value < b.at.bank.value : a.at.offset < b.at.offset; } );
  return stored;
}

std::uint32_t storageUsedOf( Patched const& build )
{
  std::uint32_t used = 0;
  for ( Stored const& one : storedImages( build ) )
  {
    used = std::max( used, build.target().positionOf( one.at ) + static_cast<std::uint32_t>( one.form.size() ) );
  }
  return used;
}

bool refusedPayloads( Patched const& build, diag::DiagnosticId id, diag::DiagnosticSink& sink )
{
  bool refused = false;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !build.storage().hasPayload( where ) )
      {
        continue;
      }
      Section const& section = one.sections()[index];
      sink.add( diag::diagnostic( id )
                    .at( section.span().begin, section.span().length )
                    .arg( "section", one.displayNameOf( where.section ) ) );
      refused = true;
    }
  }
  return refused;
}

std::vector<StateStored> stateImagesOf( Patched const& build )
{
  Sizes const& sizes = build.sizes();
  Bytes const& bytes = build.bytes();
  std::vector<StateStored> found;
  for ( Storage::StateImage const& image : build.storage().stateImages() )
  {
    found.push_back( StateStored{ .state = image.state,
                                  .offset = image.offset,
                                  .form = contentOf( bytes, image.where, sizes.initialisedExtentOf( image.where ) ),
                                  .where = image.where } );
  }
  std::ranges::stable_sort( found,
                            []( StateStored const& a, StateStored const& b )
                            { return a.state != b.state ? a.state < b.state : a.offset < b.offset; } );
  return found;
}

std::vector<Resident> residentSections( Patched const& build, std::span<ModuleIndex const> except )
{
  Project const& project = build.project();
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Layout const& layout = build.layout();
  Bytes const& bytes = build.bytes();
  std::span<Module const> const modules = symbols.modules();

  // What is in memory as the entry Phase begins: what it needs, and in a
  // program without storage every Section with bytes a later Phase needs too,
  // since nothing could bring it afterwards — see heldAcross.
  PhaseIndex const entry = project.phases.entry;
  std::vector<bool> excepted( modules.size(), false );
  for ( ModuleIndex const module : except )
  {
    excepted[module.value] = true;
  }

  std::vector<Resident> found;
  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    if ( excepted[module] )
    {
      continue;
    }
    Module const& one = modules[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      // A Section decoded while the file loads is written by the decode.
      // And one the cold start brings is written by the routine.
      if ( !heldAcrossOf( build, where ).includes( entry ) || build.storage().isDecodedAtLoad( where ) ||
           build.storage().isHiddenAtLoad( where ) || build.storage().isLoadedByColdStart( where, entry ) )
      {
        continue;
      }
      // A Pane's Section is in its unit and not in memory the program addresses
      // without a switch, and a unit set's in the medium alone.
      if ( !layout.isPlaced( where ) || !sizes.isKnown( where ) || sizes.sizeOfSection( where ) == 0 ||
           !one.sections()[index].emitsBytes() || one.sections()[index].inMedium() )
      {
        continue;
      }
      // The initialised extent and not the Section: reserved space at either
      // end is not in the file, because a load writing it would write nothing
      // the program may rely on.
      InitialisedExtent const extent = sizes.initialisedExtentOf( where );
      if ( extent.size() == 0 )
      {
        continue;
      }
      found.push_back( Resident{ .where = where,
                                 .address = layout.addressOf( where ) + extent.begin,
                                 .content = contentOf( bytes, where, extent ) } );
    }
  }
  return found;
}

void writeResidentSegments( Patched const& build, SegmentWriter& out, std::span<ModuleIndex const> except )
{
  for ( Resident const& one : residentSections( build, except ) )
  {
    out.segment( one.address, one.content );
  }
}

} // namespace nga::model
