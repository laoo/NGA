#include "nga/model/Driver.hpp"

#include "nga/model/Merge.hpp"
#include "nga/model/Symbol.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model
{

namespace
{

std::optional<std::uint32_t> symbolIndexOf( Module const& module, std::string_view name )
{
  std::vector<Symbol> const& symbols = module.symbols().symbols();
  for ( std::uint32_t index = 0; index < symbols.size(); ++index )
  {
    if ( symbols[index].name == name )
    {
      return index;
    }
  }
  return std::nullopt;
}

/// The macro a role resolved to, or nothing where the Module reported the
/// name as not a macro of its own.
std::optional<MacroIndex> macroOf( Module const& module, DriverRole const& role )
{
  std::optional<std::uint32_t> const symbol = symbolIndexOf( module, role.label );
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
      // A decoder runs while a Bank is in, from any Phase's Transition.
      module.keepOutsideWindow();
      if ( !module.residency().isAll() )
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
  if ( !copyDeclared && anyTransitionTaken( modules ) )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::NO_COPY_DECODER ).at( first.span.begin, first.span.length ) );
  }

  Target& target = project.target;
  Driver resolved{ .module = *driver, .open = {}, .read = {}, .windows = {}, .stream = std::nullopt };
  resolved.windows.resize( target.windows.size() );
  bool complete = true;

  // The stream's own roles, which every driver has.
  for ( auto const& [name, into] : { std::pair{ std::string_view{ "open" }, &resolved.open },
                                     std::pair{ std::string_view{ "read" }, &resolved.read } } )
  {
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
    ( role.role == "show" ? resolved.windows[window->value].show : resolved.windows[window->value].showAt ) = *macro;
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
}

} // namespace nga::model
