#include "nga/model/Project.hpp"

#include "nga/model/Runtime.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>
#include <unordered_set>

namespace nga::model
{

std::optional<Container> containerNamed( std::string_view word )
{
  if ( word == "raw" )
  {
    return Container::RAW_IMAGE;
  }
  if ( word == "xex" )
  {
    return Container::XEX;
  }
  if ( word == "atr" )
  {
    return Container::ATR;
  }
  return std::nullopt;
}

std::string_view nameOf( Container container )
{
  switch ( container )
  {
  case Container::XEX:
    return "xex";
  case Container::ATR:
    return "atr";
  case Container::RAW_IMAGE:
    break;
  }
  return "raw";
}

std::optional<Cpu> cpuNamed( std::string_view word )
{
  if ( word == "6502" )
  {
    return Cpu::MOS6502;
  }
  if ( word == "65sc02" )
  {
    return Cpu::WDC65SC02;
  }
  return std::nullopt;
}

std::string_view nameOf( Cpu cpu )
{
  return cpu == Cpu::WDC65SC02 ? "65sc02" : "6502";
}

std::optional<Intent> intentNamed( std::string_view word )
{
  if ( word == "speed" )
  {
    return Intent::SPEED;
  }
  if ( word == "size" )
  {
    return Intent::SIZE;
  }
  if ( word == "fit" )
  {
    return Intent::FIT;
  }
  return std::nullopt;
}

std::string_view nameOf( Intent intent )
{
  switch ( intent )
  {
  case Intent::SPEED:
    return "speed";
  case Intent::SIZE:
    return "size";
  case Intent::FIT:
    break;
  }
  return "fit";
}

std::string_view nameOf( RegionProperty property )
{
  switch ( property )
  {
  case RegionProperty::RAM:
    return "ram";
  case RegionProperty::RESERVED:
    return "reserved";
  case RegionProperty::REGISTER:
    return "register";
  }
  return "region";
}

std::string displayNameOf( Region const& region )
{
  if ( !region.name.empty() )
  {
    return region.name;
  }
  return fmt::format( "${:04X}..${:04X}", region.range.begin, region.range.end - 1 );
}

namespace
{

std::uint32_t sumOf( std::span<AddressRange const> ranges )
{
  std::uint32_t total = 0;
  for ( AddressRange const& range : ranges )
  {
    total += range.size();
  }
  return total;
}

} // namespace

std::uint32_t Pools::zeroPageSize() const
{
  return sumOf( zeroPage );
}

std::uint32_t Pools::generalSize() const
{
  return sumOf( general );
}

Pools poolsOf( std::span<Region const> regions )
{
  // One entry per address, holding the most restrictive property declared
  // over it, or nothing where no Region reaches. Sixty-four thousand bytes
  // once per Project is cheaper than any interval arithmetic that would have
  // to be right about overlaps.
  std::array<std::optional<RegionProperty>, ADDRESS_SPACE_END> covered;
  for ( Region const& region : regions )
  {
    for ( std::uint32_t address = region.range.begin; address < region.range.end; ++address )
    {
      std::optional<RegionProperty>& here = covered[address];
      if ( !here.has_value() || *here < region.property )
      {
        here = region.property;
      }
    }
  }

  Pools pools;
  auto const runsOf = [&covered]( std::uint32_t from, std::uint32_t to, std::vector<AddressRange>& into )
  {
    std::optional<std::uint32_t> start;
    for ( std::uint32_t address = from; address <= to; ++address )
    {
      bool const ram = address < to && covered[address] == RegionProperty::RAM;
      if ( ram && !start.has_value() )
      {
        start = address;
      }
      else if ( !ram && start.has_value() )
      {
        into.push_back( AddressRange{ .begin = *start, .end = address } );
        start.reset();
      }
    }
  };
  runsOf( 0, 0x100, pools.zeroPage );
  runsOf( 0x100, ADDRESS_SPACE_END, pools.general );
  return pools;
}

std::vector<Region> standInRegions()
{
  return { Region{ .nameSpan = std::nullopt,
                   .name = {},
                   .range = AddressRange{ .begin = 0x0080, .end = 0x0090 },
                   .property = RegionProperty::RAM,
                   .site = std::nullopt },
           Region{ .nameSpan = std::nullopt,
                   .name = {},
                   .range = AddressRange{ .begin = 0x2000, .end = 0xA000 },
                   .property = RegionProperty::RAM,
                   .site = std::nullopt } };
}

std::uint32_t Window::size() const
{
  std::uint32_t total = 0;
  for ( AddressRange const& range : ranges )
  {
    total += range.size();
  }
  return total;
}

bool Window::covers( std::uint32_t address ) const
{
  return std::ranges::any_of(
      ranges, [address]( AddressRange const& range ) { return address >= range.begin && address < range.end; } );
}

bool Window::meets( AddressRange other ) const
{
  return std::ranges::any_of(
      ranges, [other]( AddressRange const& range ) { return range.begin < other.end && other.begin < range.end; } );
}

std::optional<std::uint32_t> Window::firstStateOf( UnitSetIndex set, std::span<UnitSet const> sets ) const
{
  std::uint32_t index = 0;
  for ( WindowState const& state : states )
  {
    if ( state.units.has_value() && *state.units == set )
    {
      return index;
    }
    index += state.units.has_value() ? sets[state.units->value].count : 1;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> Window::namedStateOf( std::string_view wanted, std::span<UnitSet const> sets ) const
{
  std::uint32_t index = 0;
  for ( WindowState const& state : states )
  {
    if ( !state.units.has_value() && state.name == wanted )
    {
      return index;
    }
    index += state.units.has_value() ? sets[state.units->value].count : 1;
  }
  return std::nullopt;
}

std::optional<UnitSetIndex> Target::unitSetNamed( std::string_view name ) const
{
  for ( std::uint32_t index = 0; index < unitSets.size(); ++index )
  {
    if ( unitSets[index].name == name )
    {
      return UnitSetIndex{ index };
    }
  }
  return std::nullopt;
}

std::optional<WindowIndex> Target::windowNamed( std::string_view name ) const
{
  for ( std::uint32_t index = 0; index < windows.size(); ++index )
  {
    if ( windows[index].name == name )
    {
      return WindowIndex{ index };
    }
  }
  return std::nullopt;
}

std::optional<PaneIndex> Target::paneNamed( std::string_view name ) const
{
  for ( std::uint32_t index = 0; index < panes.size(); ++index )
  {
    if ( panes[index].name == name )
    {
      return PaneIndex{ index };
    }
  }
  return std::nullopt;
}

std::optional<UnitSetIndex> Target::unitSetShownBy( WindowIndex window ) const
{
  for ( WindowState const& state : windows[window.value].states )
  {
    if ( state.units.has_value() )
    {
      return state.units;
    }
  }
  return std::nullopt;
}

StorageAddress Target::addressAt( std::uint32_t position ) const
{
  if ( unitSize == 0 )
  {
    return StorageAddress{ .bank = BankIndex{ 0 }, .offset = position };
  }
  return StorageAddress{ .bank = BankIndex{ position / unitSize }, .offset = position % unitSize };
}

std::uint32_t Target::positionOf( StorageAddress at ) const
{
  return ( at.bank.value * unitSize ) + at.offset;
}

std::optional<RegionIndex> Target::mostRestrictiveOver( AddressRange range ) const
{
  // The winner over a range is the winner over some address of it, and that
  // is the most restrictive Region touching the range at all; among equals
  // the first declared, so that the finding is the same on every run.
  std::optional<RegionIndex> found;
  for ( std::uint32_t index = 0; index < regions.size(); ++index )
  {
    Region const& region = regions[index];
    if ( region.range.begin >= range.end || range.begin >= region.range.end )
    {
      continue;
    }
    if ( !found.has_value() || regions[found->value].property < region.property )
    {
      found = RegionIndex{ index };
    }
  }
  return found;
}

std::optional<std::uint32_t> baseIn( Project const& project, PhaseIndex phase, WindowIndex window )
{
  Phase const& one = project.phases.phases[phase.value];
  if ( window.value < one.bases.size() && one.bases[window.value].has_value() )
  {
    return one.bases[window.value];
  }
  return project.target.windows[window.value].base;
}

bool Residency::intersects( Residency const& other ) const
{
  std::size_t const common = std::min( mPhases.size(), other.mPhases.size() );
  for ( std::size_t phase = 0; phase < common; ++phase )
  {
    if ( mPhases[phase] && other.mPhases[phase] )
    {
      return true;
    }
  }
  return false;
}

bool Residency::isEmpty() const
{
  return std::ranges::none_of( mPhases, []( bool present ) { return present; } );
}

bool Residency::isAll() const
{
  return std::ranges::all_of( mPhases, []( bool present ) { return present; } );
}

void deriveResidency( Project& project )
{
  std::size_t const count = project.phases.phases.size();
  for ( ProjectModule& module : project.modules )
  {
    module.residency = Residency{ count };
  }
  for ( std::uint32_t phase = 0; phase < count; ++phase )
  {
    for ( ModuleIndex const module : project.phases.phases[phase].needs )
    {
      project.modules[module.value].residency.add( PhaseIndex{ phase } );
    }
  }
}

Project
synthesiseProject( diag::SourceManager& sources, std::span<diag::FileId const> files, diag::DiagnosticSink& sink )
{
  Project project;
  std::unordered_set<std::string> taken;

  for ( diag::FileId const file : files )
  {
    std::filesystem::path const path{ sources.pathOf( file ) };
    std::string name = path.stem().string();

    if ( !taken.insert( name ).second )
    {
      // No location: this is a fact about the run rather than about a place in
      // a file, so it sorts by identifier and by the name it is about.
      sink.add( diag::diagnostic( diag::DiagnosticId::MODULE_NAME_COLLISION ).arg( "module", name ).sortedBy( name ) );
      continue;
    }

    project.modules.push_back( ProjectModule{
        .name = std::move( name ), .file = file, .residency = {}, .path = std::string{ sources.pathOf( file ) } } );
  }

  // One Phase, unnamed, with everything in it: the same Project a document
  // with no `phase` block describes, so nothing downstream can tell them
  // apart.
  Phase everything;
  for ( std::uint32_t module = 0; module < project.modules.size(); ++module )
  {
    everything.needs.push_back( ModuleIndex{ module } );
  }
  project.phases.phases.push_back( std::move( everything ) );
  project.phases.entry = PhaseIndex{ 0 };
  deriveResidency( project );
  if ( holdsC( sources, project ) )
  {
    addCRuntime( project, sources );
  }

  return project;
}

} // namespace nga::model
