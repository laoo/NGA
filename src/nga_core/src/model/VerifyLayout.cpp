#include "nga/model/VerifyLayout.hpp"

#include "nga/model/Prune.hpp"
#include "nga/model/Trace.hpp"

#include "nga/model/Evaluate.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

/// One past the highest address a 6502 can name. The general pool's end is not
/// this: a Target may make a pool small, and a Section outside a small pool is
/// outside its pool and not off the end of the machine.
constexpr std::uint32_t MEMORY_END = 0x10000;

/// One Section as this file needs to see it: where the Layout put it, how far
/// it reaches, and what its source asked for.
///
/// Read from the model and never from Place's own working state, which is the
/// point of the exercise — a Claim carried over from the allocator would carry
/// its mistakes with it.
struct PlacedSection
{
  SectionRef where;
  diag::SourceSpan span;
  std::uint32_t size = 0;
  PlacementClass placement = PlacementClass::ABSOLUTE;
  Residency const* residency = nullptr;
  std::optional<std::uint32_t> pinned;
  std::uint32_t alignment = 1;
  std::uint32_t boundary = 0;
  bool payload = false;
  bool movable = false;
  std::optional<SectionRef> next;

  /// The Pane the Section is in, and the address space its state is — see
  /// addressSpaceOf; nothing for a Section in `fixed` or a base state.
  std::optional<PaneIndex> pane{};
  std::optional<std::uint32_t> space{};

  /// Where the Section stands in each Phase of its Residency — every entry
  /// the same address unless it is Movable — or one entry for a Section in
  /// no Phase, which stands somewhere all the same.
  struct Stand
  {
    std::optional<PhaseIndex> phase;
    AddressRange range;
  };

  std::vector<Stand> stands;
};

/// The last address a range covers. Every message says where a Section *ends*,
/// and a half-open end is one past that.
std::uint32_t lastOf( AddressRange range )
{
  return range.end - 1;
}

bool overlaps( AddressRange one, AddressRange other )
{
  return one.begin < other.end && other.begin < one.end;
}

} // namespace

void verifyLayout( Placed const& build, Storage const& storage, diag::DiagnosticSink& sink )
{
  diag::SourceManager const& sources = build.sources();
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Layout const& layout = build.layout();
  Charsets const& charsets = build.charsets();
  Freezes const& freezes = build.freezes();
  PhaseGraph const& phases = build.phases();
  Target const& target = build.target();
  std::span<Module const> const modules = symbols.modules();

  auto const nameOf = [&sources, &symbols]( SectionRef where )
  { return symbols.moduleAt( where.module ).displayNameOf( where.section, sources ); };
  auto const phaseName = [&phases]( std::optional<PhaseIndex> phase )
  { return phase.has_value() ? phases.phases[phase->value].name.value_or( "(implicit)" ) : std::string{ "(none)" }; };

  std::vector<PlacedSection> placed;
  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    Module const& one = modules[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };

      // What Prune dropped has no address, and what it kept has one.
      if ( !build.reachable().includes( where ) )
      {
        if ( layout.isPlaced( where ) )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::LAYOUT_UNREACHABLE_PLACED )
                        .at( section.span().begin, section.span().length )
                        .arg( "section", nameOf( where ) ) );
        }
        continue;
      }
      if ( !layout.isPlaced( where ) )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::LAYOUT_NOT_PLACED )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", nameOf( where ) ) );
        continue;
      }

      PlacedSection entry{ .where = where,
                           .span = section.span(),
                           .size = sizes.isKnown( where ) ? sizes.sizeOfSection( where ) : 0,
                           .placement = section.placement(),
                           .residency = &one.residency(),
                           .pinned = std::nullopt,
                           .alignment = 1,
                           .boundary = 0,
                           .payload = storage.hasPayload( where ),
                           .movable = section.isMovable(),
                           .next = std::nullopt,
                           .pane = section.pane(),
                           .space = section.pane().has_value() ? addressSpaceOf( target, layout, *section.pane() )
                                                               : std::nullopt,
                           .stands = {} };
      if ( std::optional<SectionIndex> const next = section.next(); next.has_value() )
      {
        entry.next = SectionRef{ .module = where.module, .section = *next };
      }

      // Where it stands, Phase by Phase — from the Layout's per-Phase answer,
      // so that a Movable Section at two addresses is seen at both.
      for ( std::uint32_t phase = 0; phase < phases.phases.size(); ++phase )
      {
        if ( one.residency().phaseCount() > phase && one.residency().includes( PhaseIndex{ phase } ) )
        {
          std::uint32_t const at = layout.addressIn( where, PhaseIndex{ phase } );
          entry.stands.push_back( PlacedSection::Stand{
              .phase = PhaseIndex{ phase }, .range = AddressRange{ .begin = at, .end = at + entry.size } } );
        }
      }
      if ( entry.stands.empty() )
      {
        std::uint32_t const at = layout.addressOf( where );
        entry.stands.push_back( PlacedSection::Stand{ .phase = std::nullopt,
                                                      .range = AddressRange{ .begin = at, .end = at + entry.size } } );
      }

      // The pin, the alignment and the boundary come from the declaration,
      // evaluated again here. Taking them from Place would ask the solver
      // whether it did what the solver decided to do.
      if ( section.pinnedAddress() != nullptr )
      {
        std::optional<std::int64_t> const address =
            declaredValueOf( sources, symbols, &charsets, where.module, *section.pinnedAddress() );
        if ( address.has_value() && *address >= 0 && *address < 0x10000 )
        {
          entry.pinned = static_cast<std::uint32_t>( *address );
        }
      }
      if ( section.alignment() != nullptr )
      {
        std::optional<std::int64_t> const value =
            declaredValueOf( sources, symbols, &charsets, where.module, *section.alignment() );
        if ( value.has_value() && *value >= 1 && *value <= 0x10000 )
        {
          entry.alignment = static_cast<std::uint32_t>( *value );
        }
      }
      if ( section.boundary() != nullptr )
      {
        std::optional<std::int64_t> const value =
            declaredValueOf( sources, symbols, &charsets, where.module, *section.boundary() );
        if ( value.has_value() && *value >= 1 && *value <= 0x10000 )
        {
          entry.boundary = static_cast<std::uint32_t>( *value );
        }
      }

      placed.push_back( std::move( entry ) );
    }
  }

  for ( PlacedSection const& one : placed )
  {
    // A Movable Section holds one address across every Residency something
    // refers to it from: the Freezes, re-read against the Layout.
    for ( Residency const& across : freezes.of( one.where ) )
    {
      PlacedSection::Stand const* first = nullptr;
      for ( PlacedSection::Stand const& stand : one.stands )
      {
        if ( !stand.phase.has_value() )
        {
          continue;
        }
        PhaseIndex const phase = stand.phase.value_or( PhaseIndex{} );
        if ( across.phaseCount() <= phase.value || !across.includes( phase ) )
        {
          continue;
        }
        if ( first == nullptr )
        {
          first = &stand;
        }
        else if ( first->range.begin != stand.range.begin )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::LAYOUT_MOVED_UNDER_REFERENCE )
                        .at( one.span.begin, one.span.length )
                        .arg( "section", nameOf( one.where ) )
                        .arg( "address", first->range.begin )
                        .arg( "phase", phaseName( first->phase ) )
                        .arg( "other", stand.range.begin )
                        .arg( "otherPhase", phaseName( stand.phase ) ) );
          break;
        }
      }
    }

    // Every rule about one address, at every address the Section stands at
    // — once per distinct address, so a Section held still is checked once.
    std::vector<std::uint32_t> seen;
    for ( PlacedSection::Stand const& stand : one.stands )
    {
      if ( std::ranges::find( seen, stand.range.begin ) != seen.end() )
      {
        continue;
      }
      seen.push_back( stand.range.begin );
      AddressRange const range = stand.range;

      auto const report = [&one, &nameOf, range]( diag::DiagnosticId id )
      {
        return diag::diagnostic( id )
            .at( one.span.begin, one.span.length )
            .arg( "section", nameOf( one.where ) )
            .arg( "address", range.begin );
      };

      if ( std::optional<std::uint32_t> const pinned = one.pinned; pinned.has_value() && range.begin != *pinned )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_PIN_MOVED ).arg( "pinned", *pinned ) );
      }

      if ( range.begin % one.alignment != 0 )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_NOT_ALIGNED ).arg( "alignment", one.alignment ) );
      }

      // A Section of no bytes covers no address, so every rule below is about
      // one that does.
      if ( range.begin == range.end )
      {
        continue;
      }

      if ( one.boundary > 0 && range.begin / one.boundary != lastOf( range ) / one.boundary )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_CROSSES_BOUNDARY )
                      .arg( "last", lastOf( range ) )
                      .arg( "boundary", one.boundary ) );
      }

      if ( range.end > MEMORY_END )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_PAST_MEMORY ) );
        continue;
      }

      if ( one.placement == PlacementClass::ZEROPAGE && range.end > ZERO_PAGE_END )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_NOT_ZERO_PAGE ).arg( "last", lastOf( range ) ) );
      }

      if ( one.payload && !one.pane.has_value() &&
           std::ranges::any_of( target.streamRanges,
                                [&range]( AddressRange const& window ) { return overlaps( range, window ); } ) )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_PAYLOAD_IN_WINDOW ).arg( "last", lastOf( range ) ) );
      }

      // What runs or is named under a `.with` on a Window stands outside it
      // — see docs/decisions/0055-with.md.
      if ( !one.pane.has_value() )
      {
        for ( WindowIndex const window : freezes.keptOutOf( one.where ) )
        {
          if ( target.windows[window.value].meets( range ) )
          {
            sink.add(
                report( diag::DiagnosticId::LAYOUT_UNDER_WITH ).arg( "window", target.windows[window.value].name ) );
          }
        }
      }

      // Only what the solver placed. A pin outside the pool is the author's
      // Constraint and the solver is obliged to satisfy it, not to refuse it.
      // A Pane's Section has its Window for a pool.
      if ( !one.pinned.has_value() )
      {
        std::span<AddressRange const> pool =
            one.placement == PlacementClass::ZEROPAGE ? target.pools.zeroPage : target.pools.general;
        if ( one.pane.has_value() )
        {
          pool = target.windows[target.panes[one.pane->value].window.value].ranges;
        }
        bool const inside = std::ranges::any_of(
            pool, [&range]( AddressRange const& part ) { return range.begin >= part.begin && range.end <= part.end; } );
        if ( !inside )
        {
          sink.add( report( diag::DiagnosticId::LAYOUT_OUTSIDE_POOL ).arg( "last", lastOf( range ) ) );
        }
      }
    }
  }

  // A Proc another falls through into starts where that one ends, in every
  // Phase both stand in.
  for ( PlacedSection const& one : placed )
  {
    if ( !one.next.has_value() )
    {
      continue;
    }
    auto const following =
        std::ranges::find_if( placed, [&one]( PlacedSection const& other ) { return other.where == *one.next; } );
    if ( following == placed.end() )
    {
      continue;
    }
    for ( PlacedSection::Stand const& stand : one.stands )
    {
      for ( PlacedSection::Stand const& after : following->stands )
      {
        if ( after.phase != stand.phase || after.range.begin == stand.range.end )
        {
          continue;
        }
        sink.add( diag::diagnostic( diag::DiagnosticId::LAYOUT_NOT_FOLLOWING )
                      .at( one.span.begin, one.span.length )
                      .arg( "section", nameOf( one.where ) )
                      .arg( "address", stand.range.end )
                      .arg( "phase", phaseName( stand.phase ) )
                      .arg( "next", nameOf( following->where ) )
                      .arg( "other", after.range.begin ) );
        break;
      }
    }
  }

  // The co-visibility rule as its definition rather than as a search: for
  // every Phase, every pair of Sections it holds, once, with nothing skipped
  // for standing anywhere in particular in a list — and a pair reported once
  // however many Phases it collides in. This is the loop Place cannot afford
  // and does not need to.
  std::vector<std::pair<std::size_t, std::size_t>> reported;
  for ( std::uint32_t phase = 0; phase < phases.phases.size(); ++phase )
  {
    PhaseIndex const here{ phase };
    auto const standIn = [here]( PlacedSection const& one ) -> AddressRange const*
    {
      for ( PlacedSection::Stand const& stand : one.stands )
      {
        if ( stand.phase == here )
        {
          return &stand.range;
        }
      }
      return nullptr;
    };
    for ( std::size_t index = 0; index < placed.size(); ++index )
    {
      AddressRange const* const one = standIn( placed[index] );
      if ( one == nullptr || one->begin == one->end )
      {
        continue;
      }
      for ( std::size_t other = index + 1; other < placed.size(); ++other )
      {
        AddressRange const* const two = standIn( placed[other] );
        // Two states of one Window are never visible together, so two
        // Sections in different address spaces do not collide.
        if ( two == nullptr || two->begin == two->end || !overlaps( *one, *two ) ||
             placed[index].space != placed[other].space ||
             std::ranges::find( reported, std::pair{ index, other } ) != reported.end() ||
             build.interference().mayShare( placed[index].where, placed[other].where ) )
        {
          continue;
        }
        reported.emplace_back( index, other );
        sink.add( diag::diagnostic( diag::DiagnosticId::LAYOUT_OVERLAP )
                      .at( placed[index].span.begin, placed[index].span.length )
                      .arg( "section", nameOf( placed[index].where ) )
                      .arg( "other", nameOf( placed[other].where ) )
                      .arg( "address", std::max( one->begin, two->begin ) )
                      .arg( "phase", phaseName( here ) ) );
      }
    }
  }
}

} // namespace nga::model
