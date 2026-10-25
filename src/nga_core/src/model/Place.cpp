#include "nga/model/Place.hpp"

#include "nga/model/Prune.hpp"
#include "nga/model/ReadOnly.hpp"

#include "nga/model/Solver.hpp"
#include "nga/model/Trace.hpp"
#include "nga/model/TypeCheck.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace nga::model
{

namespace
{

/// A Section waiting to be placed, with what Place knows about it before the
/// solver is asked: in Project order, then declaration order within a Module.
struct Candidate
{
  SectionRef where;
  std::uint32_t size = 0;
  syntax::PlacementClass placement = syntax::PlacementClass::ABSOLUTE;
  diag::SourceSpan span;
  std::optional<std::uint32_t> pinned;

  /// What `align` and `within` declared, as a Claim carries them.
  std::uint32_t alignment = 1;
  std::uint32_t boundary = 0;

  /// Refused where it was declared — larger than its boundary — and so
  /// neither pinned nor handed to the solver, though it still weighs what it
  /// weighs in the sums.
  bool impossible = false;

  /// A Temporary, which may share an address with another the Interference
  /// allows — see docs/decisions/0034-trace.md.
  bool temporary = false;

  /// What the source said with `foreign`: another master of the address space
  /// reads the bytes, so the extent may cross a `register` or a `reserved`
  /// Region and nothing else about the Section changes — see
  /// docs/decisions/0220-a-foreign-section.md.
  bool foreign = false;

  /// What the ReadOnly Step said: nothing writes this Section.
  bool readOnly = false;

  /// The Regions `in NAME` named, the Section standing wholly within one of
  /// them — see docs/decisions/0243-a-section-may-name-the-region-it-stands-in.md.
  std::vector<AddressRange> regions{};

  /// Whether it therefore stands in ROM — the third pool, or a pin inside a
  /// `rom` Region. Then its `live` is every Phase, because nothing loads ROM:
  /// its bytes are there for the whole run and no other Section's may be at
  /// that address, whatever the two Residencies say — see
  /// docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
  bool inRom = false;

  /// The Proc this one falls through into, placed immediately after it.
  std::optional<SectionRef> next;

  /// The Module's, which its Sections inherit. Outlives the Candidate: it is
  /// the Module's own and the Modules outlive Place.
  Residency const* residency = nullptr;

  /// The Phases the Section's bytes are held apart in, see heldAcross: its
  /// Residency, or where the bytes are written once at load and never again
  /// the Phases they are live across. What every Claim and pin is asked
  /// against, and what the sums count a Phase's Sections by.
  Residency live{};

  /// Whether this Section may not live in the Window: a Transition loads it
  /// from a Bank, so that while the Bank is switched in an address there is
  /// the Bank — or it is the routine that does the switching.
  bool outsideWindow = false;

  /// The Pane the Section is in, when it is in one: its pool is then the
  /// Pane's Window, and its address space either the named state the Pane
  /// is pinned to — `group` — or the Bank the solver gives the Pane, asked
  /// for as the PaneClaim at `paneClaim` — see docs/decisions/0054-panes.md.
  std::optional<PaneIndex> pane{};
  std::optional<std::uint32_t> group{};
  std::optional<std::size_t> paneClaim{};

  /// The unit set the Section stands in as the medium sees it: its pool is
  /// the set's Banks end to end, at linear addresses — see
  /// docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
  std::optional<UnitSetIndex> units{};

  /// Whether the Section stands in the medium's memory, a Pane's Bank or a
  /// unit set, and so takes nothing from the CPU's pools.
  [[nodiscard]] bool inMedium() const
  {
    return pane.has_value() || units.has_value();
  }

  /// The ranges the Section may not stand in: the stream's, where a
  /// Transition needs it. Nothing for a Pane's Section, whose Window is its
  /// pool; no other Section has a Window's ranges in its pool at all.
  std::vector<AddressRange> keepOut{};
};

/// The pins accepted so far, kept sorted so that the one an overlap names is
/// the lowest in memory rather than the first that happened to be looked at.
class AcceptedPins
{
public:
  /// The pin already holding any part of this range in a Phase the claimant
  /// is present in and in the same state of memory, or nothing. A pin `shares`
  /// says the claimant may overlap is passed over.
  template <typename Shares>
  [[nodiscard]] std::optional<SectionRef> overlapping( AddressRange range,
                                                       Residency const& residency,
                                                       std::optional<std::uint32_t> group,
                                                       Shares const& shares ) const
  {
    for ( Pin const& pin : mPins )
    {
      if ( pin.range.overlaps( range ) && pin.group == group && pin.residency->intersects( residency ) &&
           !shares( pin.owner ) )
      {
        return pin.owner;
      }
    }
    return std::nullopt;
  }

  void accept( AddressRange range, SectionRef owner, Residency const& residency, std::optional<std::uint32_t> group )
  {
    mPins.push_back( Pin{ .range = range, .owner = owner, .residency = &residency, .group = group } );
    for ( std::size_t index = mPins.size() - 1; index > 0; --index )
    {
      if ( mPins[index - 1].range.begin <= mPins[index].range.begin )
      {
        break;
      }
      std::swap( mPins[index - 1], mPins[index] );
    }
  }

private:
  struct Pin
  {
    AddressRange range;
    SectionRef owner;
    Residency const* residency = nullptr;
    std::optional<std::uint32_t> group;
  };

  std::vector<Pin> mPins;
};

std::string nameOfSection( Merged const& build, SectionRef where )
{
  return build.symbols().moduleAt( where.module ).displayNameOf( where.section );
}

std::vector<Candidate>
candidatesOf( Sized const& build, Storage const& storage, ReadOnly const& readOnly, diag::DiagnosticSink& sink )
{
  Sizes const& sizes = build.sizes();
  std::vector<Candidate> candidates;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      // What decodes while a `.xex` loads is placed by the load Step, in what
      // the load leaves free, once the stored forms are known — see
      // docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md.
      if ( !build.reachable().includes( where ) || placedByTheLoad( build, where ) )
      {
        continue;
      }

      Candidate candidate{ .where = where,
                           .size = sizes.isKnown( where ) ? sizes.sizeOfSection( where ) : 0,
                           .placement = section.placement(),
                           .span = section.span(),
                           .pinned = std::nullopt,
                           .next = std::nullopt,
                           .residency = &one.residency(),
                           .outsideWindow = storage.hasPayload( where ) || one.outsideWindow(),
                           .pane = section.pane(),
                           .group = std::nullopt,
                           .keepOut = {} };
      candidate.live = heldAcrossOf( build, where );
      candidate.units = section.unitSet();
      if ( !candidate.inMedium() && candidate.outsideWindow )
      {
        candidate.keepOut.insert(
            candidate.keepOut.end(), build.target().streamRanges.begin(), build.target().streamRanges.end() );
      }
      candidate.temporary = section.isTemporary();
      candidate.regions.assign( section.regions().begin(), section.regions().end() );
      candidate.foreign = section.saysForeign();
      candidate.readOnly = readOnly.includes( where );
      if ( std::optional<SectionIndex> const next = section.next(); next.has_value() )
      {
        candidate.next = SectionRef{ .module = where.module, .section = *next };
      }

      if ( section.pinnedAddress() != nullptr )
      {
        // A pin is an Integer with a declared value, for the reason that
        // governs an instruction's width: one computed from the layout would
        // be the same cycle one turn wider.
        // In a unit set it is a linear address, which the medium's memory
        // reaches past sixteen bits.
        std::optional<std::int64_t> const address = declaredValueOf(
            build.sources(), build.symbols(), &build.charsets(), where.module, *section.pinnedAddress() );
        std::int64_t const end = candidate.units.has_value() ? std::int64_t{ 1 } << 32 : 0x10000;
        if ( !address.has_value() || *address < 0 || *address >= end )
        {
          // What was written, and why it is no address: the value says
          // nothing where it was not declared, and prints as no address where
          // it lies below zero.
          diag::SourceSpan const at = section.pinnedAddress()->span;
          std::string what = "not a declared value";
          if ( address.has_value() )
          {
            what = candidate.units.has_value() ? "past $FFFFFFFF" : "past $FFFF";
            if ( *address < 0 )
            {
              what = "below $0000";
            }
          }
          sink.add( diag::diagnostic( diag::DiagnosticId::PIN_NOT_AN_ADDRESS )
                        .at( at.begin, at.length )
                        .arg( "text", std::string{ build.sources().textOf( at ) } )
                        .arg( "what", std::move( what ) ) );
        }
        else
        {
          candidate.pinned = static_cast<std::uint32_t>( *address );
        }
      }

      // An alignment and a boundary are declared values for the same reason,
      // and each is a count of bytes, so zero is not one.
      if ( section.alignment() != nullptr )
      {
        std::optional<std::int64_t> const value =
            declaredValueOf( build.sources(), build.symbols(), &build.charsets(), where.module, *section.alignment() );
        // A power of two, because an alignment of 3 is one no address
        // arithmetic of the 6502 wants: a page, a table indexed by a shift, a
        // buffer the hardware fetches from are all aligned to a power of two.
        bool const powerOfTwo = value.has_value() && *value >= 1 && ( *value & ( *value - 1 ) ) == 0;
        if ( !value.has_value() || *value < 1 || *value > 0x10000 || !powerOfTwo )
        {
          diag::SourceSpan const at = section.alignment()->span;
          std::string what = "not a declared value";
          if ( value.has_value() )
          {
            what = *value < 1 || *value > 0x10000 ? "out of that range" : "not a power of two";
          }
          sink.add( diag::diagnostic( diag::DiagnosticId::BAD_ALIGNMENT )
                        .at( at.begin, at.length )
                        .arg( "text", std::string{ build.sources().textOf( at ) } )
                        .arg( "what", std::move( what ) ) );
        }
        else
        {
          candidate.alignment = static_cast<std::uint32_t>( *value );
        }
      }

      if ( section.boundary() != nullptr )
      {
        std::optional<std::int64_t> const value =
            declaredValueOf( build.sources(), build.symbols(), &build.charsets(), where.module, *section.boundary() );
        if ( !value.has_value() || *value < 1 || *value > 0x10000 )
        {
          diag::SourceSpan const at = section.boundary()->span;
          sink.add(
              diag::diagnostic( diag::DiagnosticId::BAD_BOUNDARY )
                  .at( at.begin, at.length )
                  .arg( "text", std::string{ build.sources().textOf( at ) } )
                  .arg( "what", std::string{ value.has_value() ? "out of that range" : "not a declared value" } ) );
        }
        else
        {
          candidate.boundary = static_cast<std::uint32_t>( *value );
        }
      }

      // Where it stands is the ReadOnly Step's answer and not a second one of
      // Place's. A Pane's Section has its Window for a pool whatever else is
      // true of it.
      candidate.inRom = candidate.readOnly && !candidate.inMedium() && readOnly.standsInRom( where );
      if ( candidate.inRom )
      {
        candidate.live = Residency::all( one.residency().phaseCount() );
      }

      // Nothing that large lies within the boundary wherever it starts: a
      // local impossibility, reported once here and asked of nobody.
      if ( candidate.boundary > 0 && candidate.size > candidate.boundary )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::SECTION_EXCEEDS_BOUNDARY )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", nameOfSection( build, where ) )
                      .arg( "size", candidate.size )
                      .arg( "boundary", candidate.boundary ) );
        candidate.impossible = true;
      }

      candidates.push_back( candidate );
    }
  }
  return candidates;
}

/// A sum that held, by how little: a Phase, the pool it was taken over, and
/// what the pool holds against what was required of it.
struct LeastRoom
{
  std::string phase;
  std::string pool;
  std::uint32_t required = 0;
  std::uint32_t available = 0;
};

/// What a Section takes of a pool: its size, or for a pin the part of it that
/// lies within the pool — a pin outside the pool is a Constraint the author
/// chose and costs the pool nothing.
std::uint32_t bytesWithin( Candidate const& candidate, std::span<AddressRange const> pool )
{
  if ( !candidate.pinned.has_value() )
  {
    return candidate.size;
  }
  std::uint32_t within = 0;
  for ( AddressRange const& part : pool )
  {
    std::uint32_t const begin = std::max( *candidate.pinned, part.begin );
    std::uint32_t const end = std::min( *candidate.pinned + candidate.size, part.end );
    within += end > begin ? end - begin : 0;
  }
  return within;
}

std::uint32_t sizeOf( std::span<AddressRange const> pool )
{
  std::uint32_t total = 0;
  for ( AddressRange const& part : pool )
  {
    total += part.size();
  }
  return total;
}

/// The first layer of explaining a layout that cannot exist: for every Phase
/// and pool, what the Sections present take against what the pool holds.
/// Arithmetic over the model, before the solver is asked, because the sum
/// names the cause where a solver could only name whichever Section came
/// last — see docs/spec/diagnostics.md. The Sections that weigh most are
/// attached, largest first and in Project order among equals.
/// True when some Phase was refused.
///
/// Where none was, `least` is the sum that came closest: what a search that
/// finds nothing within its budget names, since that sum is the likeliest to
/// be the one that could not see the cause — see
/// docs/decisions/0255-the-portfolio-has-a-budget-too.md.
bool reportCapacity( Sized const& build,
                     std::span<Candidate const> candidates,
                     std::optional<LeastRoom>& least,
                     diag::DiagnosticSink& sink )
{
  PhaseGraph const& phases = build.phases();
  Pools const& pools = build.target().pools;

  constexpr std::size_t largest = 3;
  bool refused = false;

  struct Contribution
  {
    std::uint32_t bytes = 0;
    Candidate const* who = nullptr;
  };

  // Per Candidate, the Sections of a switched ROM its bytes may lie hidden
  // under. Asked once and not per Phase: `hidesUnder` already speaks of every
  // Phase the two are present in together.
  std::vector<std::vector<Candidate const*>> romsOver( candidates.size() );
  if ( canHide( build ) )
  {
    for ( Candidate const& rom : candidates )
    {
      if ( !isSwitchedRom( build, rom.where.module ) )
      {
        continue;
      }
      for ( std::size_t other = 0; other < candidates.size(); ++other )
      {
        if ( &candidates[other] != &rom && hidesUnder( build, candidates[other].where, rom.where ) )
        {
          romsOver[other].push_back( &rom );
        }
      }
    }
  }

  for ( std::uint32_t index = 0; index < phases.phases.size(); ++index )
  {
    PhaseIndex const phase{ index };
    std::string const name = phases.phases[index].displayName();

    auto const report = [&]( syntax::PlacementClass placement,
                             std::span<AddressRange const> pool,
                             std::string_view poolName,
                             diag::Diagnostic finding )
    {
      auto const presentHere = [&]( Candidate const& candidate )
      {
        // A Section in ROM takes nothing of either pool a Phase has: its
        // bytes are in a third, and what that comes to is summed once for
        // the whole run below.
        return candidate.placement == placement && !candidate.inMedium() && !candidate.inRom &&
               candidate.live.includes( phase );
      };
      auto const positionOf = [&]( Candidate const& candidate )
      { return static_cast<std::size_t>( &candidate - candidates.data() ); };
      auto const hidesHere = [&]( Candidate const& candidate )
      {
        return std::ranges::any_of( romsOver[positionOf( candidate )],
                                    [&]( Candidate const* rom ) { return presentHere( *rom ); } );
      };

      // Bytes that may lie hidden under a ROM present here are summed twice,
      // and either sum refuses. In the first they take nothing, since the
      // ROM's Sections take that range already, and a sum that counted both
      // refused a Phase the solver lays out. In the second they take their
      // size and the ROM's Sections they may lie under take nothing, since
      // only a ROM's Section may stand where hidden bytes lie and an address
      // holds one of everything else. The first alone let a Phase through
      // whose hidden bytes outgrow the ROMs, and the solver then searched for
      // a layout that does not exist. See
      // docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
      std::vector<bool> lainUnder( candidates.size(), false );
      for ( Candidate const& candidate : candidates )
      {
        if ( presentHere( candidate ) && hidesHere( candidate ) )
        {
          for ( Candidate const* rom : romsOver[positionOf( candidate )] )
          {
            lainUnder[positionOf( *rom )] = lainUnder[positionOf( *rom )] || presentHere( *rom );
          }
        }
      }

      struct Sum
      {
        std::uint32_t required = 0;
        std::vector<Contribution> contributions;

        void add( std::uint32_t bytes, Candidate const& who )
        {
          required += bytes;
          contributions.push_back( Contribution{ .bytes = bytes, .who = &who } );
        }
      };
      Sum visible;
      Sum hidden;
      for ( Candidate const& candidate : candidates )
      {
        // A Section in the medium takes nothing from the pools: it stands in
        // a Bank.
        if ( !presentHere( candidate ) )
        {
          continue;
        }
        // A Temporary that may share with another present here takes
        // nothing the sum can count: what the pair takes together is the
        // solver's answer, and a sum that counted both would refuse a Phase
        // that fits — the sum is a bound from below or it is no refusal.
        if ( candidate.temporary &&
             std::ranges::any_of( candidates,
                                  [&]( Candidate const& other )
                                  {
                                    return &other != &candidate && other.temporary && presentHere( other ) &&
                                           build.interference().mayShare( candidate.where, other.where );
                                  } ) )
        {
          continue;
        }
        std::uint32_t const bytes = bytesWithin( candidate, pool );
        if ( bytes == 0 )
        {
          continue;
        }
        if ( !hidesHere( candidate ) )
        {
          visible.add( bytes, candidate );
        }
        if ( !lainUnder[positionOf( candidate )] )
        {
          hidden.add( bytes, candidate );
        }
      }
      Sum* over = nullptr;
      if ( visible.required > sizeOf( pool ) )
      {
        over = &visible;
      }
      else if ( hidden.required > sizeOf( pool ) )
      {
        over = &hidden;
      }
      if ( over == nullptr )
      {
        std::uint32_t const required = std::max( visible.required, hidden.required );
        if ( !pool.empty() && ( !least.has_value() || sizeOf( pool ) - required < least->available - least->required ) )
        {
          least = LeastRoom{
            .phase = name, .pool = std::string{ poolName }, .required = required, .available = sizeOf( pool )
          };
        }
        return;
      }

      refused = true;
      std::uint32_t const required = over->required;
      std::vector<Contribution>& contributions = over->contributions;
      std::ranges::stable_sort( contributions, std::ranges::greater{}, &Contribution::bytes );
      finding =
          std::move( finding ).arg( "phase", name ).arg( "required", required ).arg( "available", sizeOf( pool ) );
      for ( Contribution const& contribution : contributions | std::views::take( largest ) )
      {
        // A Section held here only because a later Phase needs it says so:
        // without that, a program with no storage that does not fit reads as
        // a puzzle — see
        // docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.
        Residency const& needed = *contribution.who->residency;
        if ( !needed.includes( phase ) )
        {
          std::string needer = "(implicit)";
          for ( std::uint32_t other = 0; other < needed.phaseCount(); ++other )
          {
            if ( needed.includes( PhaseIndex{ other } ) )
            {
              needer = phases.phases[other].displayName();
              break;
            }
          }
          finding = std::move( finding ).note( diag::diagnostic( diag::DiagnosticId::KEPT_CONTRIBUTOR )
                                                   .at( contribution.who->span.begin, contribution.who->span.length )
                                                   .arg( "section", nameOfSection( build, contribution.who->where ) )
                                                   .arg( "size", contribution.bytes )
                                                   .arg( "needer", needer ) );
          continue;
        }
        finding = std::move( finding ).note( diag::diagnostic( diag::DiagnosticId::LARGEST_CONTRIBUTOR )
                                                 .at( contribution.who->span.begin, contribution.who->span.length )
                                                 .arg( "section", nameOfSection( build, contribution.who->where ) )
                                                 .arg( "size", contribution.bytes ) );
      }
      sink.add( std::move( finding ).sortedBy( name ) );
    };

    // One View until Views exist, and the glossary names it.
    report( syntax::PlacementClass::ABSOLUTE,
            pools.general,
            "view `fixed`",
            diag::diagnostic( diag::DiagnosticId::PHASE_DOES_NOT_FIT ).arg( "view", "fixed" ) );
    report( syntax::PlacementClass::ZEROPAGE,
            pools.zeroPage,
            "zero page",
            diag::diagnostic( diag::DiagnosticId::ZERO_PAGE_DOES_NOT_FIT_IN_PHASE ) );
  }

  // ROM is summed **once** and not per Phase: nothing loads it, so no Section
  // there gives its address up to another Phase's and every one of them adds
  // up in one space — see
  // docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
  std::uint32_t required = 0;
  std::vector<Contribution> contributions;
  for ( Candidate const& candidate : candidates )
  {
    std::uint32_t const bytes = candidate.inRom ? bytesWithin( candidate, pools.readOnly ) : 0;
    if ( bytes == 0 )
    {
      continue;
    }
    required += bytes;
    contributions.push_back( Contribution{ .bytes = bytes, .who = &candidate } );
  }
  if ( required > sizeOf( pools.readOnly ) )
  {
    refused = true;
    std::ranges::stable_sort( contributions, std::ranges::greater{}, &Contribution::bytes );
    diag::Diagnostic finding = diag::diagnostic( diag::DiagnosticId::ROM_DOES_NOT_FIT )
                                   .arg( "required", required )
                                   .arg( "available", sizeOf( pools.readOnly ) );
    for ( Contribution const& contribution : contributions | std::views::take( largest ) )
    {
      finding = std::move( finding ).note( diag::diagnostic( diag::DiagnosticId::LARGEST_CONTRIBUTOR )
                                               .at( contribution.who->span.begin, contribution.who->span.length )
                                               .arg( "section", nameOfSection( build, contribution.who->where ) )
                                               .arg( "size", contribution.bytes ) );
    }
    sink.add( std::move( finding ).sortedBy( "rom" ) );
  }
  return refused;
}

/// The starts at which `size` bytes lie within `pool`, or nothing when the
/// pool is too small for them.
std::optional<AddressRange> startsWithin( AddressRange pool, std::uint32_t size )
{
  if ( pool.size() < size )
  {
    return std::nullopt;
  }
  return AddressRange{ .begin = pool.begin, .end = pool.end - size + 1 };
}

/// Where a Section in a unit set may start: anywhere its bytes lie within the
/// set's Banks end to end, whatever boundaries of Banks they run across.
std::vector<AddressRange> linearStartsOf( Target const& target, Candidate const& candidate )
{
  std::optional<AddressRange> const banks =
      candidate.units.has_value() ? target.linearBanksOf( *candidate.units ) : std::nullopt;
  if ( !banks.has_value() )
  {
    return {};
  }
  std::optional<AddressRange> const within = startsWithin( *banks, candidate.size );
  return within.has_value() ? std::vector<AddressRange>{ *within } : std::vector<AddressRange>{};
}

/// The unit set a Claim stands in, as the solver numbers it.
std::optional<std::uint32_t> unitSetOf( Candidate const& candidate )
{
  return candidate.units.has_value() ? std::optional<std::uint32_t>{ candidate.units->value } : std::nullopt;
}

/// The group a Claim in a state of a Window belongs to: one address space
/// per Bank of a set, whichever Window shows it, and one per named state of
/// each Window. `fixed` is the one space with no group.
std::uint32_t groupOfBank( UnitSetIndex set, std::uint32_t bank )
{
  return ( set.value << 16U ) | bank;
}

std::uint32_t groupOfState( WindowIndex window, std::uint32_t state )
{
  return 0x80000000U | ( window.value << 8U ) | state;
}

/// The Panes as the solver is asked about them: every Pane of a unit set
/// that is not pinned to a named state becomes a PaneClaim, and its
/// Sections' Candidates name it; a Pane pinned to a named state has its
/// address space now, and its Sections carry it as a group. Returns which
/// PaneClaim each Pane became, or nothing for a Pane the solver has no say
/// in.
std::vector<std::optional<std::size_t>>
paneClaimsOf( Target const& target, std::vector<Candidate>& candidates, Layout& layout, std::vector<PaneClaim>& into )
{
  std::vector<std::optional<std::size_t>> claimOf( target.panes.size() );
  for ( std::uint32_t index = 0; index < target.panes.size(); ++index )
  {
    Pane const& pane = target.panes[index];
    if ( pane.state.has_value() )
    {
      layout.placePane( PaneIndex{ index }, *pane.state );
      continue;
    }
    std::optional<UnitSetIndex> const set = target.unitSetShownBy( pane.window );
    if ( !set.has_value() )
    {
      continue;
    }
    claimOf[index] = into.size();
    into.push_back( PaneClaim{ .set = set->value,
                               .banks = target.unitSets[set->value].count,
                               .count = pane.count,
                               .window = target.windows[pane.window.value].ranges } );
  }
  for ( Candidate& candidate : candidates )
  {
    if ( !candidate.pane.has_value() )
    {
      continue;
    }
    candidate.paneClaim = claimOf[candidate.pane->value];
    if ( !candidate.paneClaim.has_value() )
    {
      candidate.group = addressSpaceOf( target, layout, *candidate.pane );
    }
  }
  return claimOf;
}

/// The sums for a Pane, before the solver is asked: in every Phase, what
/// its Sections present take against what one Bank holds — a Window's
/// worth — and for a family, whether the set has that many Banks in a row.
/// Arithmetic over the model, as reportCapacity is, because the sum names
/// the cause where the solver could only say that no layout exists.
/// True when some Pane was refused.
bool reportPaneCapacity( Sized const& build, std::span<Candidate const> candidates, diag::DiagnosticSink& sink )
{
  Target const& target = build.target();
  PhaseGraph const& phases = build.phases();
  bool refused = false;
  for ( std::uint32_t index = 0; index < target.panes.size(); ++index )
  {
    Pane const& pane = target.panes[index];
    std::optional<UnitSetIndex> const set = target.unitSetShownBy( pane.window );
    if ( pane.state.has_value() || !set.has_value() )
    {
      continue;
    }
    UnitSet const& units = target.unitSets[set->value];
    if ( pane.count > units.count )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::PANE_FAMILY_EXCEEDS_SET )
                    .at( pane.site.begin, pane.site.length )
                    .arg( "pane", pane.name )
                    .arg( "count", pane.count )
                    .arg( "set", units.name )
                    .arg( "banks", units.count )
                    .sortedBy( pane.name ) );
      refused = true;
      continue;
    }
    std::uint32_t const holds = target.windows[pane.window.value].size();
    for ( std::uint32_t phase = 0; phase < phases.phases.size(); ++phase )
    {
      std::uint32_t needed = 0;
      for ( Candidate const& candidate : candidates )
      {
        if ( candidate.pane == PaneIndex{ index } && candidate.live.includes( PhaseIndex{ phase } ) )
        {
          needed += candidate.size;
        }
      }
      if ( needed <= holds )
      {
        continue;
      }
      std::string const name = phases.phases[phase].displayName();
      sink.add( diag::diagnostic( diag::DiagnosticId::PANE_NO_BANK )
                    .at( pane.site.begin, pane.site.length )
                    .arg( "pane", pane.name )
                    .arg( "size", needed )
                    .arg( "phase", name )
                    .arg( "set", units.name )
                    .arg( "available", holds )
                    .sortedBy( pane.name + " " + name ) );
      refused = true;
      break;
    }
  }
  return refused;
}

/// The sums for a unit set its own Sections stand in: in every Phase, what
/// they and the Sections of the set's Panes take, a family's once per member,
/// against what the set's Banks hold end to end. Asked only of a set some
/// Section stands in, since a set of Panes alone has its sums per Pane.
/// True when some set was refused.
bool reportUnitSetCapacity( Sized const& build, std::span<Candidate const> candidates, diag::DiagnosticSink& sink )
{
  Target const& target = build.target();
  PhaseGraph const& phases = build.phases();
  bool refused = false;
  for ( std::uint32_t set = 0; set < target.unitSets.size(); ++set )
  {
    UnitSetIndex const index{ set };
    std::optional<AddressRange> const banks = target.linearBanksOf( index );
    if ( !banks.has_value() ||
         std::ranges::none_of( candidates, [index]( Candidate const& one ) { return one.units == index; } ) )
    {
      continue;
    }
    for ( std::uint32_t phase = 0; phase < phases.phases.size(); ++phase )
    {
      std::uint64_t needed = 0;
      for ( Candidate const& candidate : candidates )
      {
        if ( candidate.live.phaseCount() <= phase || !candidate.live.includes( PhaseIndex{ phase } ) )
        {
          continue;
        }
        if ( candidate.units == index )
        {
          needed += candidate.size;
        }
        else if ( candidate.paneClaim.has_value() && candidate.pane.has_value() &&
                  target.unitSetShownBy( target.panes[candidate.pane->value].window ) == index )
        {
          needed += std::uint64_t{ candidate.size } * target.panes[candidate.pane->value].count;
        }
      }
      if ( needed <= banks->size() )
      {
        continue;
      }
      std::string const name = phases.phases[phase].displayName();
      sink.add( diag::diagnostic( diag::DiagnosticId::IN_UNITS_FULL )
                    .arg( "phase", name )
                    .arg( "units", target.unitSets[set].name )
                    .arg( "size", static_cast<std::int64_t>( needed ) )
                    .arg( "holds", banks->size() )
                    .sortedBy( target.unitSets[set].name + " " + name ) );
      refused = true;
      break;
    }
  }
  return refused;
}

/// Where an unpinned Section may start: each range of its pool, with the
/// ranges it keeps out of — the stream's, a `.with`'s Window's — cut out of
/// it. The ranges stay sorted, so the lowest is searched first.
std::vector<AddressRange>
allowedStartsOf( Candidate const& candidate, Pools const& pools, std::span<AddressRange const> paneWindow )
{
  std::span<AddressRange const> pool =
      candidate.placement == syntax::PlacementClass::ZEROPAGE ? pools.zeroPage : pools.general;
  if ( candidate.inRom )
  {
    pool = pools.readOnly;
  }
  std::vector<AddressRange> starts;
  if ( candidate.pane.has_value() )
  {
    // A Pane's Section stands within its Window, whatever the pools say:
    // the Window's ranges are its pool, and the stream's ranges are where
    // it lives rather than what it keeps out of.
    for ( AddressRange const& part : paneWindow )
    {
      if ( std::optional<AddressRange> const within = startsWithin( part, candidate.size ); within.has_value() )
      {
        starts.push_back( *within );
      }
    }
    return starts;
  }
  auto const add = [&]( AddressRange part )
  {
    if ( std::optional<AddressRange> const within = startsWithin( part, candidate.size ); within.has_value() )
    {
      starts.push_back( *within );
    }
  };

  // The pool with the kept-out ranges taken out of it, one range at a time,
  // since a Window's ranges need not be contiguous; and cut to the Region the
  // Section named, where it named one.
  std::vector<AddressRange> parts;
  for ( AddressRange const& part : pool )
  {
    if ( candidate.regions.empty() )
    {
      parts.push_back( part );
      continue;
    }
    for ( AddressRange const& region : candidate.regions )
    {
      AddressRange const kept{ .begin = std::max( part.begin, region.begin ), .end = std::min( part.end, region.end ) };
      if ( kept.begin < kept.end )
      {
        parts.push_back( kept );
      }
    }
  }
  std::ranges::sort( parts, {}, &AddressRange::begin );
  {
    for ( AddressRange const& window : candidate.keepOut )
    {
      std::vector<AddressRange> cut;
      for ( AddressRange const& part : parts )
      {
        if ( window.end <= part.begin || window.begin >= part.end )
        {
          cut.push_back( part );
          continue;
        }
        if ( window.begin > part.begin )
        {
          cut.push_back( AddressRange{ .begin = part.begin, .end = std::min( window.begin, part.end ) } );
        }
        if ( window.end < part.end )
        {
          cut.push_back( AddressRange{ .begin = std::max( window.end, part.begin ), .end = part.end } );
        }
      }
      parts = std::move( cut );
    }
  }
  for ( AddressRange const& part : parts )
  {
    add( part );
  }
  return starts;
}

} // namespace

Layout::Layout( std::span<Module const> modules, std::size_t paneCount )
{
  mByModule.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    mByModule[module].resize( modules[module].sections().size() );
  }
  mPaneStates.resize( paneCount );
}

void Layout::placePane( PaneIndex pane, std::uint32_t state )
{
  if ( pane.value >= mPaneStates.size() )
  {
    mPaneStates.resize( pane.value + 1 );
  }
  mPaneStates[pane.value] = state;
}

std::optional<std::uint32_t> Layout::stateOfPane( PaneIndex pane ) const
{
  return pane.value < mPaneStates.size() ? mPaneStates[pane.value] : std::nullopt;
}

std::optional<std::uint32_t> addressSpaceOf( Target const& target, Layout const& layout, PaneIndex pane )
{
  std::optional<std::uint32_t> const state = layout.stateOfPane( pane );
  if ( !state.has_value() )
  {
    return std::nullopt;
  }
  Pane const& one = target.panes[pane.value];
  if ( one.state.has_value() )
  {
    return groupOfState( one.window, *state );
  }
  std::optional<UnitSetIndex> const set = target.unitSetShownBy( one.window );
  if ( !set.has_value() )
  {
    return std::nullopt;
  }
  std::uint32_t const first = target.windows[one.window.value].firstStateOf( *set, target.unitSets ).value_or( 0 );
  return groupOfBank( *set, *state - first );
}

void Layout::place( SectionRef where, std::uint32_t address )
{
  mByModule[where.module.value][where.section.value] = address;
}

bool Layout::isPlaced( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value].has_value();
}

std::uint32_t Layout::addressOf( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value].value_or( 0 );
}

std::optional<std::int64_t> Resolved::linearAddressOf( SectionRef where )
{
  if ( mTarget == nullptr || where.module.value >= mModules.size() || !mLayout->isPlaced( where ) )
  {
    return std::nullopt;
  }
  Section const& section = mModules[where.module.value].sectionAt( where.section );
  // A Section in a unit set was placed at its linear address, which is the
  // only one it has.
  if ( section.unitSet().has_value() )
  {
    return mLayout->addressOf( where );
  }
  std::optional<PaneIndex> const pane = section.pane();
  if ( !pane.has_value() )
  {
    return std::nullopt;
  }
  std::optional<std::uint32_t> const state = mLayout->stateOfPane( *pane );
  if ( !state.has_value() )
  {
    return std::nullopt;
  }
  std::optional<std::uint32_t> const linear = mTarget->linearAddressOf( *pane, *state, mLayout->addressOf( where ) );
  return linear.has_value() ? std::optional<std::int64_t>{ *linear } : std::nullopt;
}

namespace
{

/// While a `.xex` loads, the machine holds what the `load` block names, and
/// the DOS writes every Section the entry Phase has bytes of: the solver
/// keeps them off it. A pin there is the author's word — `INITAD` is in the
/// OS's memory, and writing it is how a program talks to the DOS — and is
/// warned about — see
/// docs/decisions/0246-the-load-of-a-xex-needs-what-the-machine-holds-then.md.
void keepOffWhatTheLoadHolds( Sized const& build, std::vector<Candidate>& candidates, diag::DiagnosticSink& sink )
{
  if ( build.project().container == Container::XEX )
  {
    for ( ModuleIndex const module : build.project().loadNeeds )
    {
      Module const& one = build.modules()[module.value];
      // What the load runs is placed by the load Step, in memory nothing
      // holds while it runs, and is overwritten after: a Phase cannot have it.
      std::optional<PhaseIndex> phase;
      for ( std::uint32_t at = 0; at < one.residency().phaseCount() && !phase.has_value(); ++at )
      {
        if ( one.residency().includes( PhaseIndex{ at } ) )
        {
          phase = PhaseIndex{ at };
        }
      }
      for ( std::uint32_t index = 0; index < one.sections().size() && phase.has_value(); ++index )
      {
        SectionRef const where{ .module = module, .section = SectionIndex{ index } };
        if ( !placedByTheLoad( build, where ) )
        {
          continue;
        }
        Section const& section = one.sections()[index];
        sink.add( diag::diagnostic( diag::DiagnosticId::LOAD_CODE_IN_PHASE )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", nameOfSection( build, where ) )
                      .arg( "module", std::string{ one.name() } )
                      .arg( "phase", build.phases().phases[phase->value].displayName() ) );
        break;
      }
    }
  }
  std::vector<HeldWhileLoading> const held = heldWhileLoading( build );
  for ( Candidate& candidate : candidates )
  {
    if ( held.empty() || candidate.inMedium() || !writtenWhileLoading( build, candidate.where ) )
    {
      continue;
    }
    if ( !candidate.pinned.has_value() )
    {
      for ( HeldWhileLoading const& one : held )
      {
        candidate.keepOut.push_back( one.range );
      }
      continue;
    }
    AddressRange const pinned{ .begin = *candidate.pinned, .end = *candidate.pinned + candidate.size };
    for ( HeldWhileLoading const& one : held )
    {
      if ( pinned.overlaps( one.range ) )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::PIN_HELD_WHILE_LOADING )
                      .at( candidate.span.begin, candidate.span.length )
                      .arg( "section", nameOfSection( build, candidate.where ) )
                      .arg( "address", std::max( pinned.begin, one.range.begin ) )
                      .arg( "holder", nameOfSection( build, one.holder ) ) );
        break;
      }
    }
  }
}

/// What the Frames took of the storage set before any Bank was given: the
/// first half of PlaceStorage put them at the start of storage, and a Pane's
/// Section may not stand on them in a Phase the Frame is live in. A Frame may
/// run from one unit into the next, and takes its part of each.
std::vector<Occupied> occupiedByFrames( Target const& target, Storage const& storage )
{
  std::vector<Occupied> occupied;
  if ( !target.storageUnits.has_value() || target.unitSize == 0 )
  {
    return occupied;
  }
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    if ( !storage.isFramePlaced( frame ) )
    {
      continue;
    }
    std::uint32_t const begin = target.positionOf( storage.frameAddressOf( frame ) );
    std::uint32_t const end = begin + storage.frameSizeOf( frame );
    for ( std::uint32_t at = begin; at < end; at = ( ( at / target.unitSize ) + 1 ) * target.unitSize )
    {
      std::uint32_t const unitEnd = std::min( end, ( ( at / target.unitSize ) + 1 ) * target.unitSize );
      occupied.push_back( Occupied{ .set = target.storageUnits->value,
                                    .bank = at / target.unitSize,
                                    .offset = at % target.unitSize,
                                    .size = unitEnd - at,
                                    .live = storage.frameLiveOf( frame ) } );
    }
  }
  return occupied;
}

/// Why the pin at `range` is impossible on its own, where it can still be
/// said what it was: past the end of memory or of its unit set's Banks, out of
/// the zero page, in the Window, outside its Pane's Window, in a register
/// Region or a ROM, holding bytes where the CPU does not reach, or short of
/// its own Regions, alignment or boundary. Nothing where it stands. A pin in a
/// reserved Region is warned about and stands: the variant and the author
/// disagree about the address, and the tool cannot know the stack's depth or
/// what the author knows about it. Whether it lies on another pin is the
/// caller's, who has the others.
std::optional<diag::Diagnostic> pinRefusal( Sized const& build,
                                            Candidate const& candidate,
                                            AddressRange range,
                                            std::span<AddressRange const> paneWindow,
                                            diag::DiagnosticSink& sink )
{
  Target const& target = build.target();
  auto const pinFinding = [&]( diag::DiagnosticId id )
  {
    return diag::diagnostic( id )
        .at( candidate.span.begin, candidate.span.length )
        .arg( "section", nameOfSection( build, candidate.where ) );
  };

  // In a unit set a pin is a linear address, held to the set's Banks and to
  // nothing of the CPU's memory: no Window, no Region, no pool.
  if ( candidate.units.has_value() )
  {
    std::optional<AddressRange> const banks = target.linearBanksOf( *candidate.units );
    if ( banks.has_value() && ( range.begin < banks->begin || range.end > banks->end ) )
    {
      return pinFinding( diag::DiagnosticId::IN_UNITS_PIN_OUTSIDE )
          .arg( "address", range.begin )
          .arg( "units", target.unitSets[candidate.units->value].name )
          .arg( "begin", banks->begin )
          .arg( "last", banks->end - 1 );
    }
  }
  else if ( range.end > ADDRESS_SPACE_END )
  {
    return pinFinding( diag::DiagnosticId::NO_ROOM ).arg( "size", candidate.size );
  }

  if ( candidate.placement == syntax::PlacementClass::ZEROPAGE && range.end > ZERO_PAGE_END )
  {
    return pinFinding( diag::DiagnosticId::ZERO_PAGE_DOES_NOT_FIT );
  }

  if ( candidate.outsideWindow && !candidate.inMedium() &&
       std::ranges::any_of( target.streamRanges,
                            [range]( AddressRange const& window ) { return range.overlaps( window ); } ) )
  {
    return pinFinding( diag::DiagnosticId::PAYLOAD_PINNED_IN_WINDOW ).arg( "address", range.begin );
  }

  // Pinned inside a Window and in no Pane: what the Window shows there
  // outside a `.with` is undefined.
  if ( !candidate.inMedium() )
  {
    auto const window =
        std::ranges::find_if( target.windows, [range]( Window const& one ) { return one.meets( range ); } );
    if ( window != target.windows.end() )
    {
      return pinFinding( diag::DiagnosticId::PIN_IN_WINDOW )
          .arg( "address", range.begin )
          .arg( "window", window->name );
    }
  }

  // A Pane's Section pinned outside its Window is pinned in a state that
  // does not reach there.
  if ( candidate.pane.has_value() &&
       !std::ranges::any_of( paneWindow,
                             [range]( AddressRange const& part )
                             { return range.begin >= part.begin && range.end <= part.end; } ) )
  {
    return pinFinding( diag::DiagnosticId::PIN_OUTSIDE_PANE_WINDOW )
        .arg( "address", range.begin )
        .arg( "pane", target.panes[candidate.pane->value].name );
  }

  if ( std::optional<RegionIndex> const over =
           candidate.inMedium() ? std::nullopt : target.mostRestrictiveOver( range );
       over.has_value() )
  {
    Region const& region = target.regions[over->value];
    auto const inRegion = [&]( diag::DiagnosticId id )
    {
      diag::Diagnostic finding =
          pinFinding( id ).arg( "address", range.begin ).arg( "region", displayNameOf( region ) );
      if ( region.site.has_value() )
      {
        finding = std::move( finding ).note( diag::diagnostic( diag::DiagnosticId::REGION_DECLARED_HERE )
                                                 .at( region.site->begin, region.site->length )
                                                 .arg( "region", displayNameOf( region ) )
                                                 .arg( "kind", std::string{ "region" } ) );
      }
      return finding;
    };
    // A `foreign` Section is the one thing that may lie over a register or a
    // reservation: those are truths about the address for the CPU, and its
    // bytes are another master's. Nothing else is skipped — the alignment
    // below is exactly what a display buffer needs. What it may not do is
    // hold bytes there: the routine copies with the CPU, and so does a
    // loader, so bytes past what the CPU reaches have nobody to write them.
    // A pin is the only way here, every pool being RAM. See
    // docs/decisions/0220-a-foreign-section.md.
    if ( candidate.foreign && region.property != RegionProperty::RAM &&
         build.symbols().moduleAt( candidate.where.module ).sectionAt( candidate.where.section ).emitsBytes() )
    {
      return inRegion( diag::DiagnosticId::FOREIGN_HOLDS_BYTES ).arg( "last", range.end - 1 );
    }
    if ( region.property == RegionProperty::REGISTER && !candidate.foreign )
    {
      return inRegion( diag::DiagnosticId::PIN_IN_REGISTER );
    }
    if ( region.property == RegionProperty::ROM && !candidate.readOnly )
    {
      // Refused and not warned about: the bytes would be read back as
      // whatever the ROM holds, and there is no author's knowledge that
      // could make that right.
      return inRegion( diag::DiagnosticId::PIN_IN_ROM );
    }
    // What the load needs is the machine's own memory, which a variant
    // reserves as a matter of course.
    bool const machine =
        std::ranges::find( build.project().loadNeeds, candidate.where.module ) != build.project().loadNeeds.end();
    if ( region.property == RegionProperty::RESERVED && !candidate.foreign && !machine )
    {
      sink.add( inRegion( diag::DiagnosticId::PIN_IN_RESERVED ) );
    }
  }

  // A pin has to satisfy the rest of its placement, and whether it does is
  // arithmetic.
  if ( !candidate.regions.empty() &&
       std::ranges::none_of( candidate.regions,
                             [&range]( AddressRange const& region )
                             { return range.begin >= region.begin && range.end <= region.end; } ) )
  {
    return pinFinding( diag::DiagnosticId::PIN_OUTSIDE_REGION ).arg( "address", range.begin );
  }
  if ( range.begin % candidate.alignment != 0 )
  {
    return pinFinding( diag::DiagnosticId::PIN_NOT_ALIGNED )
        .arg( "address", range.begin )
        .arg( "alignment", candidate.alignment );
  }
  if ( candidate.boundary > 0 && candidate.size > 0 &&
       range.begin / candidate.boundary != ( range.end - 1 ) / candidate.boundary )
  {
    return pinFinding( diag::DiagnosticId::PIN_CROSSES_BOUNDARY )
        .arg( "address", range.begin )
        .arg( "last", range.end - 1 )
        .arg( "boundary", candidate.boundary );
  }
  return std::nullopt;
}

/// What the Claims say of each other, beyond each its own room: the
/// Temporaries that may share, what may hide under a switched ROM, and a Proc
/// that another falls through into.
void relateClaims( Sized const& build, std::span<Candidate const* const> claimants, std::vector<Claim>& claims )
{
  // Two Temporaries never live at once may overlap in every Phase both are
  // present in; the Interference says which, and each Claim names the other.
  for ( std::size_t a = 0; a < claims.size(); ++a )
  {
    if ( !claimants[a]->temporary )
    {
      continue;
    }
    for ( std::size_t b = a + 1; b < claims.size(); ++b )
    {
      if ( claimants[b]->temporary && build.interference().mayShare( claimants[a]->where, claimants[b]->where ) )
      {
        claims[a].sharesWith.push_back( b );
        claims[b].sharesWith.push_back( a );
      }
    }
  }

  // Bytes held for a later Phase may lie under a ROM the driver switches out,
  // in every Phase where the ROM is in and nothing needs or names them: their
  // Claim may overlap the ROM's. Asked only of the few Claims that are a ROM's
  // Sections, and written on those alone: a Claim that names a partner leaves
  // the no-overlap for pairs, and a ROM's few Claims doing so cost little where
  // every Claim of a game doing so cost the solver its propagation — see
  // docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
  if ( canHide( build ) )
  {
    for ( std::size_t rom = 0; rom < claims.size(); ++rom )
    {
      if ( !isSwitchedRom( build, claimants[rom]->where.module ) )
      {
        continue;
      }
      for ( std::size_t other = 0; other < claims.size(); ++other )
      {
        if ( other != rom && hidesUnder( build, claimants[other]->where, claimants[rom]->where ) )
        {
          claims[rom].sharesWith.push_back( other );
        }
      }
    }
  }

  // A Proc another falls through into starts where that one ends.
  for ( std::size_t before = 0; before < claims.size(); ++before )
  {
    std::optional<SectionRef> const& next = claimants[before]->next;
    if ( !next.has_value() )
    {
      continue;
    }
    for ( std::size_t index = 0; index < claims.size(); ++index )
    {
      if ( claimants[index]->where == *next )
      {
        claims[index].follows = before;
      }
    }
  }
}

} // namespace

Layout placeSections(
    Sized const& build, Storage const& storage, ReadOnly const& readOnly, bool explain, diag::DiagnosticSink& sink )
{
  Target const& target = build.target();
  Layout layout{ build.modules(), target.panes.size() };
  std::vector<Candidate> candidates = candidatesOf( build, storage, readOnly, sink );
  Pools const& pools = target.pools;

  keepOffWhatTheLoadHolds( build, candidates, sink );
  Problem problem;
  std::vector<std::optional<std::size_t>> const paneClaimOf = paneClaimsOf( target, candidates, layout, problem.panes );

  // Every unit set as one axis, which its Panes' Banks and its own Sections
  // share: from its linear start, a Bank's size each — see
  // docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
  for ( std::uint32_t set = 0; set < target.unitSets.size(); ++set )
  {
    UnitSet const& units = target.unitSets[set];
    bool const storageSet = target.storageUnits.has_value() && target.storageUnits->value == set;
    problem.axes.push_back(
        Axis{ .begin = units.linear.has_value() ? units.linear->begin : 0,
              .bankSize = target.bankSizeOf( UnitSetIndex{ set } ).value_or( storageSet ? target.unitSize : 0 ) } );
  }

  problem.occupied = occupiedByFrames( target, storage );
  auto const paneWindowOf = [&target]( Candidate const& candidate ) -> std::span<AddressRange const>
  {
    if ( !candidate.pane.has_value() )
    {
      return {};
    }
    return target.windows[target.panes[candidate.pane->value].window.value].ranges;
  };

  std::optional<LeastRoom> least;
  bool const refusedByPools = reportCapacity( build, candidates, least, sink );
  bool const refusedByPanes = reportPaneCapacity( build, candidates, sink );
  bool const refusedByUnitSets = reportUnitSetCapacity( build, candidates, sink );
  bool const refusedBySums = refusedByPools || refusedByPanes || refusedByUnitSets;

  // What the solver is asked, and which Section each Claim stands for. Pins
  // go first: their start is decided, so deciding it first costs the search
  // nothing and everything else is fitted around them.
  std::vector<Claim>& claims = problem.claims;
  std::vector<Candidate const*> claimants;
  AcceptedPins accepted;

  // Which pins can be told apart before the solver: two in one address space,
  // or two in one Pane, whose Bank is one whatever it turns out to be. Pins
  // in two Panes of a set may or may not share a Bank, which the solver says.
  auto const pinSpaceOf = []( Candidate const& candidate ) -> std::optional<std::uint32_t>
  {
    if ( candidate.paneClaim.has_value() )
    {
      return 0x40000000U | static_cast<std::uint32_t>( *candidate.paneClaim );
    }
    if ( candidate.units.has_value() )
    {
      return 0x20000000U | candidate.units->value;
    }
    return candidate.group;
  };

  // A pin that is impossible is refused here, where the message can still
  // say what it was: on its own, or on another pin in a Phase both are
  // present in. Two pins at one address in disjoint Phases are not an
  // overlap: the two are never in memory together, and a pin across Phases is
  // exactly what the rule allows.
  for ( Candidate const& candidate : candidates )
  {
    if ( !candidate.pinned.has_value() || candidate.impossible )
    {
      continue;
    }

    AddressRange const range{ .begin = *candidate.pinned, .end = *candidate.pinned + candidate.size };
    if ( std::optional<diag::Diagnostic> refusal =
             pinRefusal( build, candidate, range, paneWindowOf( candidate ), sink );
         refusal.has_value() )
    {
      sink.add( std::move( *refusal ) );
      continue;
    }

    // Bytes pinned under a ROM where they hide there, by 0241.
    auto const hides = [&]( SectionRef pin )
    { return hidesUnder( build, candidate.where, pin ) || hidesUnder( build, pin, candidate.where ); };
    if ( std::optional<SectionRef> const other =
             accepted.overlapping( range, candidate.live, pinSpaceOf( candidate ), hides );
         other.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::SECTION_OVERLAP )
                    .at( candidate.span.begin, candidate.span.length )
                    .arg( "section", nameOfSection( build, candidate.where ) )
                    .arg( "other", nameOfSection( build, *other ) )
                    .arg( "address", range.begin ) );
      continue;
    }

    accepted.accept( range, candidate.where, candidate.live, pinSpaceOf( candidate ) );
    claims.push_back( Claim{ .size = candidate.size,
                             .allowedStarts = { AddressRange{ .begin = range.begin, .end = range.begin + 1 } },
                             .alignment = candidate.alignment,
                             .boundary = candidate.boundary,
                             .residency = candidate.live,
                             .sharesWith = {},
                             .follows = std::nullopt,
                             .group = candidate.group,
                             .pane = candidate.paneClaim,
                             .linear = unitSetOf( candidate ) } );
    claimants.push_back( &candidate );
  }

  for ( Candidate const& candidate : candidates )
  {
    if ( candidate.pinned.has_value() || candidate.impossible )
    {
      continue;
    }
    claims.push_back( Claim{ .size = candidate.size,
                             .allowedStarts = candidate.units.has_value()
                                                  ? linearStartsOf( target, candidate )
                                                  : allowedStartsOf( candidate, pools, paneWindowOf( candidate ) ),
                             .alignment = candidate.alignment,
                             .boundary = candidate.boundary,
                             .residency = candidate.live,
                             .sharesWith = {},
                             .follows = std::nullopt,
                             .group = candidate.group,
                             .pane = candidate.paneClaim,
                             .linear = unitSetOf( candidate ) } );
    claimants.push_back( &candidate );
  }

  relateClaims( build, claimants, claims );

  problem.refused = refusedBySums;
  Solution const solution = solve( problem );

  // Every Pane its state, as the solver gave it: the Window's index of the
  // Bank.
  for ( std::uint32_t index = 0; index < target.panes.size(); ++index )
  {
    std::optional<std::size_t> const claim = paneClaimOf[index];
    std::optional<std::uint32_t> const bank = claim.has_value() ? solution.paneBanks[*claim] : std::nullopt;
    if ( !bank.has_value() )
    {
      continue;
    }
    Pane const& pane = target.panes[index];
    std::optional<UnitSetIndex> const set = target.unitSetShownBy( pane.window );
    std::uint32_t const first =
        set.has_value() ? target.windows[pane.window.value].firstStateOf( *set, target.unitSets ).value_or( 0 ) : 0;
    layout.placePane( PaneIndex{ index }, first + *bank );
  }

  // No layout at all is one finding and not one per Section: which Section
  // to name is exactly what the solver cannot say in this form, and naming
  // all of them would say nothing.
  if ( !solution.feasible && solution.decided )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::NO_LAYOUT ) );
  }

  // A search that ran out says so, and not that no layout exists, which it
  // did not show. The sum that came closest is named with it: where a layout
  // is missing that no sum refused, that is where to look first — see
  // docs/decisions/0255-the-portfolio-has-a-budget-too.md.
  if ( !solution.decided )
  {
    diag::Diagnostic finding = diag::diagnostic( diag::DiagnosticId::LAYOUT_NOT_FOUND );
    if ( least.has_value() )
    {
      finding = std::move( finding ).note( diag::diagnostic( diag::DiagnosticId::LEAST_ROOM )
                                               .arg( "phase", least->phase )
                                               .arg( "pool", least->pool )
                                               .arg( "free", least->available - least->required )
                                               .arg( "available", least->available ) );
    }
    sink.add( std::move( finding ) );
  }

  // The pairs no layout can keep apart — asked for, and only where the sums
  // did not already say why: a pair explains nothing a Phase over capacity
  // did not, and only where the search showed that no layout exists, since
  // the explanation is a search as long again. A pair has no position of its
  // own, so each Section gets a note.
  if ( !solution.feasible && solution.decided && explain && !refusedBySums )
  {
    Explanation const explanation = model::explain( problem );
    for ( auto const& [a, b] : explanation.conflicts )
    {
      Candidate const& first = *claimants[a];
      Candidate const& second = *claimants[b];
      std::string const firstName = nameOfSection( build, first.where );
      std::string const secondName = nameOfSection( build, second.where );
      std::string key = firstName;
      key.append( " " ).append( secondName );
      sink.add( diag::diagnostic( diag::DiagnosticId::CANNOT_BE_KEPT_APART )
                    .arg( "section", firstName )
                    .arg( "other", secondName )
                    .note( diag::diagnostic( diag::DiagnosticId::SECTION_IS_HERE )
                               .at( first.span.begin, first.span.length )
                               .arg( "section", firstName ) )
                    .note( diag::diagnostic( diag::DiagnosticId::SECTION_IS_HERE )
                               .at( second.span.begin, second.span.length )
                               .arg( "section", secondName ) )
                    .sortedBy( std::move( key ) ) );
    }
    if ( !explanation.minimal )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::EXPLANATION_NOT_MINIMAL ) );
    }
  }

  for ( std::size_t index = 0; index < claims.size(); ++index )
  {
    Candidate const& candidate = *claimants[index];

    std::optional<std::uint32_t> const at = solution.addresses[index];
    if ( at.has_value() )
    {
      layout.place( candidate.where, *at );
      continue;
    }

    // A Section nothing could hold on its own is its own finding, whether or
    // not the rest had a layout.
    if ( std::ranges::all_of( claims[index].allowedStarts,
                              []( AddressRange range ) { return range.begin >= range.end; } ) )
    {
      sink.add( diag::diagnostic( candidate.placement == syntax::PlacementClass::ZEROPAGE
                                      ? diag::DiagnosticId::ZERO_PAGE_DOES_NOT_FIT
                                      : diag::DiagnosticId::NO_ROOM )
                    .at( candidate.span.begin, candidate.span.length )
                    .arg( "section", nameOfSection( build, candidate.where ) )
                    .arg( "size", candidate.size ) );
    }
  }

  return layout;
}

void checkAssertions( Placed const& build, diag::DiagnosticSink& sink )
{
  GlobalSymbols const& symbols = build.symbols();
  Resolved resolved{ build.sizes(), build.layout(), build.target(), build.modules() };

  for ( std::uint32_t module = 0; module < symbols.modules().size(); ++module )
  {
    for ( Assertion const& assertion : symbols.modules()[module].assertions() )
    {
      if ( containsError( *assertion.condition ) )
      {
        continue;
      }

      // An expression the type check refused has been reported, and evaluating
      // it anyway would answer a question the rules just said is not one: a
      // comparison across two Sections has a value here and is still refused.
      diag::SeverityPolicy policy;
      diag::DiagnosticSink quiet{ policy };
      if ( !typeOf( build, ModuleIndex{ module }, *assertion.condition, quiet ).isKnown() )
      {
        continue;
      }

      std::optional<std::int64_t> const value = evaluate(
          build.sources(), symbols, &build.charsets(), ModuleIndex{ module }, *assertion.condition, resolved );
      if ( !value.has_value() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::ASSERTION_NOT_DECIDABLE )
                      .at( assertion.span.begin, assertion.span.length ) );
        continue;
      }

      if ( *value == 0 )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::ASSERTION_FAILED )
                      .at( assertion.span.begin, assertion.span.length ) );
      }
    }
  }
}

Residency heldAcrossOf( Traced const& build, SectionRef where )
{
  Module const& module = build.modules()[where.module.value];
  Residency const& residency = module.residency();
  Residency needed = build.holds().phasesOf( where, residency.phaseCount() );
  for ( std::uint32_t phase = 0; phase < residency.phaseCount(); ++phase )
  {
    if ( residency.includes( PhaseIndex{ phase } ) )
    {
      needed.add( PhaseIndex{ phase } );
    }
  }
  return heldAcross( build.phases(), build.target(), module.sectionAt( where.section ), needed );
}

bool isMachineAtLoad( Merged const& build, SectionRef section )
{
  Project const& project = build.project();
  Section const& one = build.modules()[section.module.value].sections()[section.section.value];
  // A pin in a Pane or a unit set is the medium's address, and no memory of
  // the machine's — see
  // docs/decisions/0261-a-section-of-the-load-in-a-medium-is-written-with-the-units.md.
  return project.container == Container::XEX &&
         std::ranges::find( project.loadNeeds, section.module ) != project.loadNeeds.end() &&
         one.pinnedAddress() != nullptr && !one.emitsBytes() && !one.inMedium();
}

bool placedByTheLoad( Traced const& build, SectionRef section )
{
  Project const& project = build.project();
  if ( project.loadModule == section.module || project.chooseModule == section.module )
  {
    return true;
  }
  // A Section in a Pane or a unit set is no code: its address is the
  // medium's, and the units write it as any other — see
  // docs/decisions/0261-a-section-of-the-load-in-a-medium-is-written-with-the-units.md.
  return project.container == Container::XEX &&
         std::ranges::find( project.loadNeeds, section.module ) != project.loadNeeds.end() &&
         !isMachineAtLoad( build, section ) &&
         !build.modules()[section.module.value].sectionAt( section.section ).inMedium() &&
         build.holds().phasesOf( section, project.phases.phases.size() ).isEmpty();
}

bool isLoadCode( Traced const& build, SectionRef section )
{
  Project const& project = build.project();
  return placedByTheLoad( build, section ) && section.module != project.loadModule &&
         section.module != project.chooseModule;
}

std::vector<HeldWhileLoading> heldWhileLoading( Sized const& build )
{
  std::vector<HeldWhileLoading> held;
  if ( build.project().container != Container::XEX )
  {
    return held;
  }
  Sizes const& sizes = build.sizes();
  for ( ModuleIndex const module : build.project().loadNeeds )
  {
    Module const& one = build.modules()[module.value];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = module, .section = SectionIndex{ index } };
      if ( !isMachineAtLoad( build, where ) || !sizes.isKnown( where ) )
      {
        continue;
      }
      std::optional<std::int64_t> const address =
          declaredValueOf( build.sources(), build.symbols(), &build.charsets(), module, *section.pinnedAddress() );
      if ( !address.has_value() || *address < 0 || std::cmp_greater_equal( *address, ADDRESS_SPACE_END ) )
      {
        continue;
      }
      auto const begin = static_cast<std::uint32_t>( *address );
      held.push_back( HeldWhileLoading{
          .range = AddressRange{ .begin = begin, .end = begin + sizes.sizeOfSection( where ) }, .holder = where } );
    }
  }
  return held;
}

bool writtenWhileLoading( Sized const& build, SectionRef section )
{
  Module const& module = build.modules()[section.module.value];
  Section const& one = module.sections()[section.section.value];
  return one.emitsBytes() && heldAcrossOf( build, section ).includes( build.phases().entry );
}

bool canHide( Merged const& build )
{
  Project const& project = build.project();
  std::optional<Driver> const& driver = project.driver;
  return project.container == Container::XEX && !project.target.hasStorage() && project.loadModule.has_value() &&
         driver.has_value() && driver->select.has_value() && driver->mask.has_value() && driver->unmask.has_value();
}

bool isSwitchedRom( Merged const& build, ModuleIndex module )
{
  std::optional<Driver> const& driver = build.project().driver;
  return driver.has_value() &&
         std::ranges::any_of( driver->roms,
                              [module]( RomRole const& rom ) { return rom.module == module || rom.also == module; } );
}

bool hidesUnder( Sized const& build, SectionRef section, SectionRef rom )
{
  if ( !canHide( build ) || !isSwitchedRom( build, rom.module ) || isSwitchedRom( build, section.module ) ||
       placedByTheLoad( build, section ) )
  {
    return false;
  }
  Module const& one = build.modules()[section.module.value];
  Section const& hidden = one.sectionAt( section.section );
  if ( !hidden.emitsBytes() || hidden.pane().has_value() )
  {
    return false;
  }
  Module const& under = build.modules()[rom.module.value];
  Residency const held = heldAcrossOf( build, section );
  Residency const hardware = build.holds().phasesOf( section, held.phaseCount() );
  Residency const romIn =
      heldAcross( build.phases(), build.target(), under.sectionAt( rom.section ), under.residency() );
  Residency const& named = build.referrers().of( section );
  bool shared = false;
  for ( std::uint32_t index = 0; index < held.phaseCount() && index < romIn.phaseCount(); ++index )
  {
    PhaseIndex const phase{ index };
    if ( !held.includes( phase ) || !romIn.includes( phase ) )
    {
      continue;
    }
    // Seen there — needed, named, or held by the hardware — so not hidden
    // there.
    if ( one.residency().includes( phase ) || hardware.includes( phase ) || named.includes( phase ) )
    {
      return false;
    }
    shared = true;
  }
  return shared;
}

} // namespace nga::model
