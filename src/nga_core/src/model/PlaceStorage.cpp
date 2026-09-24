#include "nga/model/Storage.hpp"

#include "nga/model/Prune.hpp"
#include "nga/model/Slots.hpp"
#include "nga/model/Transition.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model
{

Storage::Storage( std::span<Module const> modules )
{
  mByModule.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    mByModule[module].resize( modules[module].sections().size() );
  }
}

void Storage::markPayload( SectionRef where )
{
  mByModule[where.module.value][where.section.value].payload = true;
}

void Storage::store( SectionRef where, std::uint8_t transform, std::vector<std::uint8_t> form, std::uint32_t landing )
{
  Entry& entry = mByModule[where.module.value][where.section.value];
  entry.transform = transform;
  entry.form = std::move( form );
  entry.landing = landing;
}

void Storage::live( SectionRef where, Residency live )
{
  mByModule[where.module.value][where.section.value].live = std::move( live );
}

Residency const& Storage::liveOf( SectionRef where ) const
{
  return at( where ).live;
}

void Storage::liveFrame( FrameIndex index, Residency live )
{
  mFrames[index.value].live = std::move( live );
}

Residency const& Storage::frameLiveOf( FrameIndex index ) const
{
  return mFrames[index.value].live;
}

void Storage::addPaneImage( SectionRef where, StorageAddress at )
{
  mPaneImages.push_back( PaneImage{ .where = where, .at = at } );
}

void Storage::place( SectionRef where, StorageAddress at )
{
  Entry& entry = mByModule[where.module.value][where.section.value];
  entry.placed = true;
  entry.address = at;
}

Storage::Entry const& Storage::at( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value];
}

bool Storage::hasPayload( SectionRef where ) const
{
  return at( where ).payload;
}

bool Storage::isPlaced( SectionRef where ) const
{
  return at( where ).placed;
}

StorageAddress Storage::addressOf( SectionRef where ) const
{
  return at( where ).address;
}

std::span<std::uint8_t const> Storage::formOf( SectionRef where ) const
{
  return at( where ).form;
}

std::uint32_t Storage::sizeOf( SectionRef where ) const
{
  return static_cast<std::uint32_t>( at( where ).form.size() );
}

std::uint8_t Storage::transformOf( SectionRef where ) const
{
  return at( where ).transform;
}

std::uint32_t Storage::landingOf( SectionRef where ) const
{
  return at( where ).landing;
}

void Storage::declareFrame( FrameIndex index,
                            PhaseIndex from,
                            PhaseIndex to,
                            std::vector<SectionRef> payloads,
                            std::uint32_t cellWrites,
                            std::uint32_t windows )
{
  if ( mFrames.size() <= index.value )
  {
    mFrames.resize( index.value + 1 );
  }
  Frame& frame = mFrames[index.value];
  frame.declared = true;
  frame.from = from;
  frame.to = to;
  frame.payloads = std::move( payloads );
  frame.cellWrites = cellWrites;
  frame.size = sizeOfFrame( static_cast<std::uint32_t>( frame.payloads.size() ), cellWrites, windows );
}

std::optional<FrameIndex> Storage::frameOf( PhaseIndex from, PhaseIndex to ) const
{
  for ( std::uint32_t index = 0; index < mFrames.size(); ++index )
  {
    if ( mFrames[index].declared && mFrames[index].from == from && mFrames[index].to == to )
    {
      return FrameIndex{ index };
    }
  }
  return std::nullopt;
}

std::span<SectionRef const> Storage::framePayloadsOf( FrameIndex index ) const
{
  return mFrames[index.value].payloads;
}

std::uint32_t Storage::frameCellWritesOf( FrameIndex index ) const
{
  return mFrames[index.value].cellWrites;
}

void Storage::placeFrame( FrameIndex index, StorageAddress at )
{
  mFrames[index.value].placed = true;
  mFrames[index.value].address = at;
}

void Storage::storeFrame( FrameIndex index, std::vector<std::uint8_t> bytes )
{
  mFrames[index.value].bytes = std::move( bytes );
}

std::uint32_t Storage::frameCount() const
{
  return static_cast<std::uint32_t>( mFrames.size() );
}

bool Storage::isFrameDeclared( FrameIndex index ) const
{
  return index.value < mFrames.size() && mFrames[index.value].declared;
}

bool Storage::isFramePlaced( FrameIndex index ) const
{
  return index.value < mFrames.size() && mFrames[index.value].placed;
}

StorageAddress Storage::frameAddressOf( FrameIndex index ) const
{
  return mFrames[index.value].address;
}

std::uint32_t Storage::frameSizeOf( FrameIndex index ) const
{
  return mFrames[index.value].size;
}

std::span<std::uint8_t const> Storage::frameBytesOf( FrameIndex index ) const
{
  return mFrames[index.value].bytes;
}

PhaseIndex Storage::frameFrom( FrameIndex index ) const
{
  return mFrames[index.value].from;
}

PhaseIndex Storage::frameTo( FrameIndex index ) const
{
  return mFrames[index.value].to;
}

/// Which Phases each Phase can reach by one edge or more.
std::vector<std::vector<bool>> reachabilityOf( PhaseGraph const& graph )
{
  std::size_t const count = graph.phases.size();
  std::vector<std::vector<bool>> reaches( count, std::vector<bool>( count, false ) );
  for ( std::uint32_t from = 0; from < count; ++from )
  {
    std::vector<PhaseIndex> pending( graph.phases[from].then.begin(), graph.phases[from].then.end() );
    while ( !pending.empty() )
    {
      PhaseIndex const at = pending.back();
      pending.pop_back();
      if ( reaches[from][at.value] )
      {
        continue;
      }
      reaches[from][at.value] = true;
      pending.insert( pending.end(), graph.phases[at.value].then.begin(), graph.phases[at.value].then.end() );
    }
  }
  return reaches;
}

Residency liveAcross( PhaseGraph const& graph, Residency const& needed )
{
  std::vector<std::vector<bool>> const reaches = reachabilityOf( graph );
  Residency live{ graph.phases.size() };
  for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
  {
    bool still = needed.phaseCount() > phase && needed.includes( PhaseIndex{ phase } );
    for ( std::uint32_t ahead = 0; ahead < graph.phases.size() && !still; ++ahead )
    {
      still = reaches[phase][ahead] && needed.phaseCount() > ahead && needed.includes( PhaseIndex{ ahead } );
    }
    if ( still )
    {
      live.add( PhaseIndex{ phase } );
    }
  }
  return live;
}

/// A `root` Section holding no bytes, evicted between two Phases that need
/// it: what reaches a Root knows no Phases and finds whatever the Phase
/// between put there, and nothing copies it back, since there is nothing to
/// copy. An ordinary reservation is left alone — a gap is an eviction by
/// docs/decisions/0016-phases-and-residency.md, and the code that names it
/// initialises it on entry. One finding per Section, at the first gap in
/// Phase order.
void reportEvictedRoots( Pruned const& build, diag::DiagnosticSink& sink )
{
  PhaseGraph const& graph = build.phases();
  std::vector<std::vector<bool>> const reaches = reachabilityOf( graph );
  auto const nameOf = [&graph]( std::uint32_t phase ) { return graph.phases[phase].name.value_or( "(implicit)" ); };

  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    Residency const& residency = one.residency();
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !section.isRoot() || section.emitsBytes() || !build.reachable().includes( where ) )
      {
        continue;
      }
      for ( std::uint32_t gap = 0; gap < graph.phases.size(); ++gap )
      {
        if ( residency.includes( PhaseIndex{ gap } ) )
        {
          continue;
        }
        std::optional<std::uint32_t> before;
        std::optional<std::uint32_t> after;
        for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
        {
          if ( !residency.includes( PhaseIndex{ phase } ) )
          {
            continue;
          }
          if ( !before.has_value() && reaches[phase][gap] )
          {
            before = phase;
          }
          if ( !after.has_value() && reaches[gap][phase] )
          {
            after = phase;
          }
        }
        if ( !before.has_value() || !after.has_value() )
        {
          continue;
        }
        sink.add( diag::diagnostic( diag::DiagnosticId::ROOT_EVICTED )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", one.displayNameOf( SectionIndex{ index }, build.sources() ) )
                      .arg( "gap", nameOf( gap ) )
                      .arg( "before", nameOf( *before ) )
                      .arg( "after", nameOf( *after ) ) );
        break;
      }
    }
  }
}

Storage findPayloads( Pruned const& build, diag::DiagnosticSink& sink )
{
  PhaseGraph const& graph = build.phases();
  Storage storage{ build.modules() };
  reportEvictedRoots( build, sink );

  // The load set of every edge, which is the whole of what decides that a
  // Section waits somewhere — nothing declares it. The Phases whose edges
  // load a Section are what it is needed in, and it is live in those and in
  // every Phase that reaches one.
  std::vector<std::vector<Residency>> needed( build.modules().size() );
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    needed[module].assign( build.modules()[module].sections().size(), Residency{ graph.phases.size() } );
  }
  for ( std::uint32_t from = 0; from < graph.phases.size(); ++from )
  {
    for ( PhaseIndex const next : graph.phases[from].then )
    {
      for ( SectionRef const where : loadSetOf( graph, PhaseIndex{ from }, next, build.modules() ) )
      {
        // What Prune dropped is loaded by nothing.
        if ( build.reachable().includes( where ) )
        {
          storage.markPayload( where );
          needed[where.module.value][where.section.value].add( PhaseIndex{ from } );
        }
      }
    }
  }
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    for ( std::uint32_t index = 0; index < needed[module].size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( storage.hasPayload( where ) )
      {
        storage.live( where, liveAcross( graph, needed[module][index] ) );
      }
    }
  }

  // Every edge some reachable `.transition` takes has a Frame: for each
  // statement, one per Phase its Section is present in, since at run time the
  // code does not know which one it is in. Numbered as they are met, which is
  // Project order; sized by what the edge may load and write, which the end
  // of Assemble settled; and placed now, first fit from the start of the
  // Banks, because the statement's bytes name where its Frames wait and Patch
  // writes those before any Payload's stored form exists.
  auto const slotCount = static_cast<std::uint32_t>( slotsOf( build.modules() ).size() );
  std::uint32_t frames = 0;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !build.reachable().includes( where ) )
      {
        continue;
      }
      for ( Chunk const& chunk : one.sections()[index].chunks() )
      {
        auto const* const transition = std::get_if<TransitionContent>( &chunk.content );
        if ( transition == nullptr || !transition->target.has_value() )
        {
          continue;
        }
        PhaseIndex const to = transition->target.value_or( PhaseIndex{} );
        for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
        {
          PhaseIndex const from{ phase };
          std::vector<PhaseIndex> const& then = graph.phases[phase].then;
          if ( !one.residency().includes( from ) || std::ranges::find( then, to ) == then.end() ||
               storage.frameOf( from, to ).has_value() )
          {
            continue;
          }
          storage.declareFrame( FrameIndex{ frames },
                                from,
                                to,
                                loadSetOf( graph, from, to, build.modules() ),
                                slotCount,
                                static_cast<std::uint32_t>( build.target().windows.size() ) );
          // Read when the edge is taken, so needed in the Phase left and
          // live in it and in every Phase that reaches it.
          Residency neededBy{ graph.phases.size() };
          neededBy.add( from );
          storage.liveFrame( FrameIndex{ frames }, liveAcross( graph, neededBy ) );
          ++frames;
        }
      }
    }
  }

  // Storage is one space end to end — an image may run from one unit into
  // the next, since the driver's stream carries on — so the Frames are
  // placed one after the other from the start of it.
  Target const& target = build.target();
  std::uint32_t used = 0;
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    std::uint32_t const size = storage.frameSizeOf( frame );
    if ( used + size <= target.storageSize() )
    {
      storage.placeFrame( frame, target.addressAt( used ) );
      used += size;
    }
  }

  return storage;
}

void placeStorage( Placed const& build, Storage& storage, diag::DiagnosticSink& sink )
{
  diag::SourceManager const& sources = build.sources();
  Target const& target = build.target();
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();

  // Bytes of storage that are taken: for good, by what is written at load —
  // the Frames, a Pane's Section with bytes, and every image placed so far —
  // or in the Phases a reservation in a Pane is present, which an image may
  // lie under only while it is dead. Positions end to end, as an image may
  // run from one unit into the next.
  struct Taken
  {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    /// Nothing for bytes taken in every Phase.
    std::optional<Residency> present;
  };

  std::vector<Taken> taken;
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    if ( storage.isFramePlaced( frame ) )
    {
      std::uint32_t const begin = target.positionOf( storage.frameAddressOf( frame ) );
      taken.push_back( Taken{ .begin = begin, .end = begin + storage.frameSizeOf( frame ), .present = std::nullopt } );
    }
  }

  // A Pane's Section with bytes waits in its Bank at its own offset within
  // the Window, written by the Container; one pinned to a named state has
  // no Bank a loader fills. A Pane's reservation takes the same bytes of
  // the same Bank — of every member's, for a family — while it is present.
  bool noBanksReported = false;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !section.pane().has_value() || !build.reachable().includes( where ) || !layout.isPlaced( where ) ||
           !sizes.isKnown( where ) || sizes.sizeOfSection( where ) == 0 )
      {
        continue;
      }
      Pane const& pane = target.panes[section.pane()->value];
      Window const& window = target.windows[pane.window.value];
      std::optional<UnitSetIndex> const set = target.unitSetShownBy( pane.window );
      std::optional<std::uint32_t> const state = layout.stateOfPane( *section.pane() );
      if ( pane.state.has_value() || !set.has_value() || !state.has_value() )
      {
        if ( pane.state.has_value() && section.emitsBytes() )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::PANE_STATE_PAYLOAD )
                        .at( section.span().begin, section.span().length )
                        .arg( "section", one.displayNameOf( where.section, sources ) )
                        .arg( "pane", pane.name ) );
        }
        continue;
      }
      // The offset within the Window: the ranges laid end to end, which is
      // what an address in a Bank means whichever Window shows it.
      auto const offsetOf = [&window]( std::uint32_t address )
      {
        std::uint32_t offset = 0;
        for ( AddressRange const& range : window.ranges )
        {
          if ( address >= range.begin && address < range.end )
          {
            return offset + ( address - range.begin );
          }
          offset += range.size();
        }
        return offset;
      };
      std::uint32_t const first = window.firstStateOf( *set, target.unitSets ).value_or( 0 );
      std::uint32_t const bank = *state - first;
      if ( section.emitsBytes() )
      {
        std::uint32_t const address = layout.addressOf( where ) + sizes.initialisedExtentOf( where ).begin;
        // One image per member, as the reservation below takes one run per
        // member. A family's Sections stand in every member at one offset so
        // that code shown any member finds them there, and a Container fills
        // each Bank on its own, so each needs its own copy of the bytes.
        for ( std::uint32_t member = bank; member < bank + pane.count && member < target.unitCount; ++member )
        {
          storage.addPaneImage( where, StorageAddress{ .bank = BankIndex{ member }, .offset = offsetOf( address ) } );
        }
      }
      // Only the storage's own set has positions an image could take.
      if ( target.storageUnits != set )
      {
        continue;
      }
      std::uint32_t const offset = offsetOf( layout.addressOf( where ) );
      for ( std::uint32_t member = bank; member < bank + pane.count && member < target.unitCount; ++member )
      {
        std::uint32_t const begin = ( member * target.unitSize ) + offset;
        taken.push_back( Taken{ .begin = begin,
                                .end = begin + sizes.sizeOfSection( where ),
                                .present = section.emitsBytes() ? std::nullopt : std::optional{ one.residency() } } );
      }
    }
  }

  // Each Payload takes the first run of bytes free for it, in Project
  // order, which is what keeps one run's storage the same as the next
  // one's.
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };

      // What the Payload occupies in storage: the length of the stored form,
      // which the Transform Step recorded from the bytes Patch produced. A
      // Section an earlier Step could not size has no form and nothing to
      // wait for.
      std::uint32_t const size = storage.sizeOf( where );
      if ( !storage.hasPayload( where ) || size == 0 )
      {
        continue;
      }
      std::string const name = one.displayNameOf( where.section, sources );

      if ( target.unitCount == 0 )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::NO_STORAGE )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", name )
                      .arg( "size", size )
                      .note( diag::diagnostic( diag::DiagnosticId::NO_BANKS ) ) );
        noBanksReported = true;
        continue;
      }

      // The bytes this image may not lie on: everything taken for good, and
      // every reservation present in a Phase the image is live in.
      Residency const& live = storage.liveOf( where );
      std::vector<AddressRange> blocked;
      for ( Taken const& held : taken )
      {
        if ( !held.present.has_value() || held.present->intersects( live ) )
        {
          blocked.push_back( AddressRange{ .begin = held.begin, .end = held.end } );
        }
      }
      std::ranges::sort( blocked, []( AddressRange const& a, AddressRange const& b ) { return a.begin < b.begin; } );
      std::uint32_t candidate = 0;
      for ( AddressRange const& block : blocked )
      {
        if ( candidate + size <= block.begin )
        {
          break;
        }
        candidate = std::max( candidate, block.end );
      }
      if ( candidate + size > target.storageSize() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::NO_STORAGE )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", name )
                      .arg( "size", size ) );
        continue;
      }
      storage.place( where, target.addressAt( candidate ) );
      taken.push_back( Taken{ .begin = candidate, .end = candidate + size, .present = std::nullopt } );
    }
  }

  // A Frame the first half could not place: no Bank at all, or none with
  // room for it. A Frame has no source of its own, so a finding about one
  // names the edge and sorts by it; no Bank at all is one finding, and where
  // a Payload has already said it, the Frames say nothing more.
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    if ( !storage.isFrameDeclared( frame ) || storage.isFramePlaced( frame ) )
    {
      continue;
    }
    std::uint32_t const size = storage.frameSizeOf( frame );
    std::string const name = nameOfFrame( build.phases(), storage.frameFrom( frame ), storage.frameTo( frame ) );
    if ( target.unitCount == 0 )
    {
      if ( !noBanksReported )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::NO_STORAGE )
                      .arg( "section", name )
                      .arg( "size", size )
                      .note( diag::diagnostic( diag::DiagnosticId::NO_BANKS ) )
                      .sortedBy( name ) );
        noBanksReported = true;
      }
      continue;
    }
    sink.add( diag::diagnostic( diag::DiagnosticId::NO_STORAGE )
                  .arg( "section", name )
                  .arg( "size", size )
                  .sortedBy( name ) );
  }
}

} // namespace nga::model
