#include "nga/model/Driver.hpp"

#include "nga/model/Merge.hpp"
#include "nga/model/Symbol.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model
{

namespace
{

/// The macro a role resolved to, or nothing where the Module reported the
/// name as not a macro of its own.
std::optional<MacroIndex> macroOf( Module const& module, DriverRole const& role )
{
  std::optional<std::uint32_t> const symbol = module.symbols().indexOf( role.label );
  if ( !symbol.has_value() || module.symbols().symbols()[*symbol].kind != SymbolKind::MACRO )
  {
    return std::nullopt;
  }
  return std::get<MacroIndex>( module.symbols().symbols()[*symbol].value );
}

DriverRole const* roleOf( Module const& module, std::string_view role, std::string_view window )
{
  for ( DriverRole const& one : module.driverRoles() )
  {
    if ( one.role == role && one.window == window )
    {
      return &one;
    }
  }
  return nullptr;
}

bool anyTransitionTaken( std::span<Module const> modules )
{
  for ( Module const& module : modules )
  {
    for ( Section const& section : module.sections() )
    {
      for ( Chunk const& chunk : section.chunks() )
      {
        auto const* const transition = std::get_if<TransitionContent>( &chunk.content );
        if ( transition != nullptr && transition->target.has_value() )
        {
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace

void resolveDriver( diag::SourceManager const& /*sources*/,
                    Project& project,
                    std::span<Module> modules,
                    diag::DiagnosticSink& sink )
{
  std::optional<ModuleIndex> driver;
  for ( std::uint32_t index = 0; index < modules.size(); ++index )
  {
    Module& module = modules[index];
    if ( !module.transforms().empty() )
    {
      // A decoder runs while a Bank is in, from any Phase's Transition — but
      // one that decodes while a `.xex` loads runs before any Phase, and is
      // in none — see docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md.
      module.keepOutsideWindow();
      if ( !module.residency().isAll() && project.loadModule != ModuleIndex{ index } )
      {
        TransformDeclaration const& first = module.transforms().front();
        sink.add(
            diag::diagnostic( diag::DiagnosticId::DECODER_NOT_RESIDENT ).at( first.span.begin, first.span.length ) );
      }
    }
    if ( module.driverRoles().empty() )
    {
      continue;
    }
    DriverRole const& first = module.driverRoles().front();
    if ( driver.has_value() )
    {
      DriverRole const& other = modules[driver->value].driverRoles().front();
      sink.add(
          diag::diagnostic( diag::DiagnosticId::SECOND_DRIVER )
              .at( first.span.begin, first.span.length )
              .arg( "module", std::string{ modules[driver->value].name() } )
              .note(
                  diag::diagnostic( diag::DiagnosticId::DRIVER_IS_HERE ).at( other.span.begin, other.span.length ) ) );
      continue;
    }
    driver = ModuleIndex{ index };
  }

  if ( !driver.has_value() )
  {
    if ( project.transitionRoutine.has_value() && anyTransitionTaken( modules ) )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::NO_DRIVER ).sortedBy( "driver" ) );
    }
    // With no driver nothing switches a ROM, so no Phase can do without one.
    for ( Phase const& phase : project.phases.phases )
    {
      for ( PhaseWithout const& without : phase.without )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::WITHOUT_NOT_A_ROM )
                      .at( without.span.begin, without.span.length )
                      .arg( "module", project.modules[without.module.value].name ) );
      }
    }
    return;
  }

  Module& module = modules[driver->value];
  module.keepOutsideWindow();
  DriverRole const& first = module.driverRoles().front();
  if ( !module.residency().isAll() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_NOT_RESIDENT ).at( first.span.begin, first.span.length ) );
  }
  bool const copyDeclared =
      std::ranges::any_of( project.decoders, []( Decoder const& decoder ) { return decoder.format == "copy"; } );
  // Without storage an edge is a jump to a stub and copies nothing, so only a
  // program with storage needs the decoder every Transition copies with — see
  // docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.
  if ( !copyDeclared && anyTransitionTaken( modules ) && project.target.hasStorage() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::NO_COPY_DECODER ).at( first.span.begin, first.span.length ) );
  }

  Target& target = project.target;
  Driver resolved{ .module = *driver,
                   .open = {},
                   .read = {},
                   .unit = 0,
                   .offset = 0,
                   .init = std::nullopt,
                   .finalize = std::nullopt,
                   .roms = {},
                   .select = std::nullopt,
                   .windows = {},
                   .stream = std::nullopt,
                   .seeksForward = false };
  resolved.windows.resize( target.windows.size() );
  bool complete = true;

  // The stream's own roles, which every driver of storage has. One that
  // declares none of them in a program without storage switches ROMs and has
  // no stream at all — see
  // docs/decisions/0242-a-driver-may-switch-roms-and-have-no-stream.md.
  resolved.streams =
      project.target.hasStorage() ||
      std::ranges::any_of( std::array{ "open", "read", "unit", "offset", "stream" },
                           [&module]( std::string_view name ) { return roleOf( module, name, {} ) != nullptr; } );
  for ( auto const& [name, into] : { std::pair{ std::string_view{ "open" }, &resolved.open },
                                     std::pair{ std::string_view{ "read" }, &resolved.read } } )
  {
    if ( !resolved.streams )
    {
      break;
    }
    DriverRole const* const role = roleOf( module, name, {} );
    if ( role == nullptr )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_ROLE_MISSING )
                    .at( first.span.begin, first.span.length )
                    .arg( "role", std::string{ name } ) );
      complete = false;
      continue;
    }
    std::optional<MacroIndex> const macro = macroOf( module, *role );
    if ( !macro.has_value() )
    {
      // Reported by the Module.
      complete = false;
      continue;
    }
    *into = *macro;
  }

  // The cells the stream stands in, which the routine writes before it
  // expands `open`. The Module has said whether each is a label of its own;
  // how much the reservation under it holds is the type check's to say.
  for ( auto const& [name, into] : { std::pair{ std::string_view{ "unit" }, &resolved.unit },
                                     std::pair{ std::string_view{ "offset" }, &resolved.offset } } )
  {
    if ( !resolved.streams )
    {
      break;
    }
    DriverRole const* const role = roleOf( module, name, {} );
    if ( role == nullptr )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_ROLE_MISSING )
                    .at( first.span.begin, first.span.length )
                    .arg( "role", std::string{ name } ) );
      complete = false;
      continue;
    }
    std::optional<std::uint32_t> const symbol = module.symbols().indexOf( role->label );
    if ( !symbol.has_value() || module.symbols().symbols()[*symbol].kind != SymbolKind::LABEL )
    {
      // Reported by the Module.
      complete = false;
      continue;
    }
    *into = *symbol;
  }

  // `init` is what the hardware takes before the driver can be used at all —
  // a cartridge on the Atari's PBI answers on the bus only once it has been
  // selected, and until then a write to the register that switches its window
  // is a write to nothing. Optional, because every other medium this tool
  // ships a driver for takes nothing: where it is absent no call is emitted
  // and no Container segment is written. Only the `.xex` calls it, so a
  // Container that cannot is refused here rather than silently leaving the
  // hardware unreached — see
  // docs/decisions/0227-a-driver-may-need-the-hardware-reached-first.md.
  if ( DriverRole const* const role = roleOf( module, "init", {} ); role != nullptr )
  {
    // A raw image is no Container: nothing of the tool starts it, so there is
    // nothing to call `init` and nothing to refuse, as for `finalize`.
    if ( project.container != Container::XEX && project.container != Container::RAW_IMAGE )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::CONTAINER_CANNOT_INIT )
                    .at( role->roleSpan.begin, role->roleSpan.length )
                    .arg( "container", std::string{ nameOf( project.container ) } ) );
      complete = false;
    }
    else if ( std::optional<MacroIndex> const macro = macroOf( module, *role ); macro.has_value() )
    {
      resolved.init = *macro;
    }
    else
    {
      // Reported by the Module.
      complete = false;
    }
  }

  // `finalize` is what leaves the machine as the program found it. Optional,
  // and not refused anywhere: a Container whose program cannot end never
  // calls it, and a role nobody calls leaves nothing broken — see
  // docs/decisions/0229-no-window-has-a-base.md.
  if ( DriverRole const* const role = roleOf( module, "finalize", {} ); role != nullptr )
  {
    if ( std::optional<MacroIndex> const macro = macroOf( module, *role ); macro.has_value() )
    {
      resolved.finalize = *macro;
    }
    else
    {
      // Reported by the Module.
      complete = false;
    }
  }

  // What keeps interrupts away while the load has the ROMs out, and lets them
  // in again. Optional both, as `finalize` is: a driver that says nothing
  // hides nothing under a ROM — see
  // docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
  for ( std::string_view const name : { std::string_view{ "mask" }, std::string_view{ "unmask" } } )
  {
    if ( DriverRole const* const role = roleOf( module, name, {} ); role != nullptr )
    {
      if ( std::optional<MacroIndex> const macro = macroOf( module, *role ); macro.has_value() )
      {
        ( name == "mask" ? resolved.mask : resolved.unmask ) = *macro;
      }
      else
      {
        // Reported by the Module.
        complete = false;
      }
    }
  }

  // The ROMs the driver switches out, each a Module of the Project, and what
  // switches them: a bit each, in the order declared — see
  // docs/decisions/0229-no-window-has-a-base.md.
  auto const moduleNamed = [&project]( std::string_view name ) -> std::optional<ModuleIndex>
  {
    for ( std::uint32_t index = 0; index < project.modules.size(); ++index )
    {
      if ( project.modules[index].name == name )
      {
        return ModuleIndex{ index };
      }
    }
    return std::nullopt;
  };
  for ( DriverRole const& role : module.driverRoles() )
  {
    if ( role.role != "rom" )
    {
      continue;
    }
    // A Module the Project does not list is a ROM no Phase needs, whose bit
    // stays clear — and it keeps its place, since the bits are the driver's.
    std::optional<ModuleIndex> const rom = moduleNamed( role.label );
    std::optional<ModuleIndex> const also = role.also.empty() ? std::nullopt : moduleNamed( role.also );
    resolved.roms.push_back( RomRole{ .module = rom, .also = also } );
  }
  if ( resolved.roms.size() > 8 )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_ROMS_TOO_MANY )
                  .at( first.span.begin, first.span.length )
                  .arg( "count", static_cast<std::int64_t>( resolved.roms.size() ) ) );
    complete = false;
  }
  if ( DriverRole const* const role = roleOf( module, "select", {} ); role != nullptr )
  {
    if ( std::optional<MacroIndex> const macro = macroOf( module, *role ); macro.has_value() )
    {
      resolved.select = *macro;
    }
    else
    {
      // Reported by the Module.
      complete = false;
    }
  }
  else if ( !resolved.roms.empty() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_ROLE_MISSING )
                  .at( first.span.begin, first.span.length )
                  .arg( "role", std::string{ "select" } ) );
    complete = false;
  }

  // What a Phase does without is a ROM the driver switches out; a ROM it needs
  // that shows another with it shows that one, so the Phase cannot do without
  // it; and a `.xex`'s entry Phase is loaded by a DOS with every ROM in. These
  // are the Project's errors and not the driver's, which stays resolved.
  for ( std::uint32_t index = 0; index < project.phases.phases.size(); ++index )
  {
    Phase const& phase = project.phases.phases[index];
    for ( PhaseWithout const& without : phase.without )
    {
      std::string const name = project.modules[without.module.value].name;
      bool const switched = std::ranges::any_of(
          resolved.roms, [&without]( RomRole const& rom ) { return rom.module == without.module; } );
      if ( !switched )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::WITHOUT_NOT_A_ROM )
                      .at( without.span.begin, without.span.length )
                      .arg( "module", name ) );
        continue;
      }
      for ( RomRole const& rom : resolved.roms )
      {
        if ( rom.module.has_value() && rom.also == without.module &&
             std::ranges::find( phase.needs, *rom.module ) != phase.needs.end() )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::WITHOUT_SHOWN_ANYWAY )
                        .at( without.span.begin, without.span.length )
                        .arg( "phase", phase.displayName() )
                        .arg( "module", name )
                        .arg( "needed", project.modules[rom.module->value].name ) );
        }
      }
      if ( project.container == Container::XEX && PhaseIndex{ index } == project.phases.entry )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::XEX_ENTRY_WITHOUT )
                      .at( without.span.begin, without.span.length )
                      .arg( "module", name ) );
      }
    }
  }

  // `seek` says what the medium can do about an offset, and `forward` is the
  // one word there is: a driver that reaches an offset by reading up to it.
  for ( DriverRole const& role : module.driverRoles() )
  {
    if ( role.role != "seek" )
    {
      continue;
    }
    if ( role.label != "forward" )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_SEEK_UNKNOWN )
                    .at( role.labelSpan.begin, role.labelSpan.length )
                    .arg( "word", std::string{ role.label } ) );
      complete = false;
      continue;
    }
    resolved.seeksForward = true;
  }

  // `span` says whether an image may cross from one unit into the next, and
  // `none` is the one word there is: a driver that is promised none does, and
  // may then know nothing of which unit follows which or of where one ends.
  for ( DriverRole const& role : module.driverRoles() )
  {
    if ( role.role != "span" )
    {
      continue;
    }
    if ( role.label != "none" )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_SPAN_UNKNOWN )
                    .at( role.labelSpan.begin, role.labelSpan.length )
                    .arg( "word", std::string{ role.label } ) );
      complete = false;
      continue;
    }
    resolved.spansNothing = true;
  }

  // The roles that name a Window: each names one the Target declares, and
  // every Window the Target declares has both, since the variant ships the
  // driver and knows its Windows — see
  // docs/decisions/0053-a-window-names-its-units.md.
  for ( DriverRole const& role : module.driverRoles() )
  {
    if ( role.window.empty() )
    {
      continue;
    }
    std::optional<WindowIndex> const window = target.windowNamed( role.window );
    if ( !window.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_WINDOW_UNKNOWN )
                    .at( role.windowSpan.begin, role.windowSpan.length )
                    .arg( "window", std::string{ role.window } ) );
      complete = false;
      continue;
    }
    if ( role.role == "stream" )
    {
      resolved.stream = window;
      continue;
    }
    std::optional<MacroIndex> const macro = macroOf( module, role );
    if ( !macro.has_value() )
    {
      complete = false;
      continue;
    }
    if ( role.role == "show" )
    {
      resolved.windows[window->value].show = *macro;
      continue;
    }
    resolved.windows[window->value].showAt = *macro;

    // Where `showAt` takes the state: `X` alone, or `X` and `Y` for a Window
    // of more states than a byte counts. Fewer than the Window needs is
    // refused; more is the driver's to have, and then every caller passes the
    // high byte too — see
    // docs/decisions/0228-a-driver-names-the-cells-its-stream-stands-in.md.
    if ( !role.place.empty() && role.place != "x" && role.place != "xy" )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_PLACE_UNKNOWN )
                    .at( role.placeSpan.begin, role.placeSpan.length )
                    .arg( "place", std::string{ role.place } ) );
      complete = false;
      continue;
    }
    std::uint32_t const bytes = role.place == "xy" ? 2 : 1;
    resolved.windows[window->value].stateBytes = bytes;
    std::uint64_t const states = target.windows[window->value].stateCount( target.unitSets );
    if ( bytes == 1 && states > 256 )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_PLACE_TOO_NARROW )
                    .at( role.labelSpan.begin, role.labelSpan.length )
                    .arg( "window", std::string{ role.window } )
                    .arg( "count", static_cast<std::int64_t>( states ) ) );
      complete = false;
    }
  }
  for ( Window const& window : target.windows )
  {
    for ( std::string_view const name : { std::string_view{ "show" }, std::string_view{ "showAt" } } )
    {
      if ( roleOf( module, name, window.name ) == nullptr )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_WINDOW_ROLE_MISSING )
                      .at( first.span.begin, first.span.length )
                      .arg( "role", std::string{ name } )
                      .arg( "window", window.name ) );
        complete = false;
      }
    }
  }

  // The stream reads through a Window that shows the units storage is; a
  // medium with no Window names none, and then storage is by count. A
  // Project with no storage at all has nothing to hold the stream to.
  if ( resolved.stream.has_value() )
  {
    Window const& window = target.windows[resolved.stream->value];
    DriverRole const* const role = roleOf( module, "stream", window.name );
    bool const shows = target.storageUnits.has_value()
                           ? window.firstStateOf( *target.storageUnits, target.unitSets ).has_value()
                           : target.unitCount == 0;
    if ( !shows )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::STREAM_NOT_STORAGE )
                    .at( role->windowSpan.begin, role->windowSpan.length )
                    .arg( "window", window.name ) );
      complete = false;
    }
    else
    {
      target.streamRanges = window.ranges;
      target.streamUnits = target.unitSetShownBy( *resolved.stream );
      target.streamUnitSize = window.size();
    }
  }
  else if ( target.storageUnits.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::DRIVER_STREAM_MISSING )
                  .at( first.span.begin, first.span.length )
                  .arg( "name", target.unitSets[target.storageUnits->value].name ) );
    complete = false;
  }

  if ( complete )
  {
    project.driver = resolved;
  }

  // A driver with no stream runs only where the program is entered or left:
  // `init` and `mask` while it loads, `select` on an edge, `finalize` on the
  // return. A `final` Phase other than the entry has none of those, and keeps
  // none of the driver — see
  // docs/decisions/0245-a-final-phase-keeps-nothing-only-an-exit-runs.md.
  // Unless the Target has a Window: then the driver shows it, and every
  // `.with` in any Phase expands its `show` — see
  // docs/decisions/0253-a-driver-that-shows-a-window-stays.md.
  if ( complete && !resolved.streams && target.windows.empty() )
  {
    Residency const kept = notFinal( project.phases );
    module.keepOnlyIn( kept );
    project.modules[driver->value].residency = module.residency();
    for ( std::uint32_t phase = 0; phase < project.phases.phases.size(); ++phase )
    {
      if ( !kept.includes( PhaseIndex{ phase } ) )
      {
        std::erase( project.phases.phases[phase].needs, *driver );
      }
    }
  }
}

} // namespace nga::model
