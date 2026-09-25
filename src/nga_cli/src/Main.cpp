// The CLI is deliberately thin: it parses arguments, reads and writes files,
// and hands over to nga_core. Everything testable lives in the library.

#include <CLI/CLI.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "nga/Version.hpp"
#include "nga/diag/Catalogue.hpp"
#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/Renderer.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/ContainerFacts.hpp"
#include "nga/model/Facts.hpp"
#include "nga/model/MemoryMap.hpp"
#include "nga/model/Pipeline.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/ProjectFile.hpp"
#include "nga/model/Symbols.hpp"
#include "nga/model/Tokens.hpp"

// Asking the platform where the executable is takes a platform's header. Both
// of Windows' macro habits are turned off first: `min` and `max` as macros
// break every standard header after this one, and the lean header leaves out
// what nothing here uses.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace
{

/// The directory the running executable stands in, empty where the platform
/// will not say. Only the CLI asks: nga_core is handed a path and never looks
/// one up, which is why the three ways of asking live here.
std::filesystem::path executableDirectory()
{
#ifdef _WIN32
  // Long paths are permitted, so the buffer is the maximum rather than MAX_PATH,
  // and a return equal to its size means the name was truncated.
  std::wstring buffer( 32768, L'\0' );
  DWORD const length = GetModuleFileNameW( nullptr, buffer.data(), static_cast<DWORD>( buffer.size() ) );
  if ( length == 0 || length == buffer.size() )
  {
    return {};
  }
  buffer.resize( length );
  return std::filesystem::path{ buffer }.parent_path();
#else
#ifdef __APPLE__
  // The first call fails and reports the size it wanted; the path it then
  // writes may hold symlinks and `..`, so it is resolved.
  std::uint32_t size = 0;
  _NSGetExecutablePath( nullptr, &size );
  std::string buffer( size, '\0' );
  if ( size == 0 || _NSGetExecutablePath( buffer.data(), &size ) != 0 )
  {
    return {};
  }
  buffer.resize( std::strlen( buffer.c_str() ) );
  std::error_code failed;
  std::filesystem::path const self = std::filesystem::weakly_canonical( buffer, failed );
  return failed ? std::filesystem::path{} : self.parent_path();
#else
  std::error_code failed;
  std::filesystem::path const self = std::filesystem::read_symlink( "/proc/self/exe", failed );
  return failed ? std::filesystem::path{} : self.parent_path();
#endif
#endif
}

/// Where the drivers and decoders the tool ships are, when `--lib` does not
/// say. A release archive carries `lib/` beside the executable and is meant to
/// work unpacked anywhere, so that is looked for first; a build knows the tree
/// it was built from, which is what a developer runs against and what the
/// archive's copy was taken from.
std::string shippedLibrary()
{
  std::filesystem::path const beside = executableDirectory();
  if ( !beside.empty() )
  {
    std::error_code failed;
    std::filesystem::path const library = beside / "lib";
    if ( std::filesystem::is_directory( library, failed ) )
    {
      return library.string();
    }
  }
  return NGA_LIB_DIR;
}

/// One `--deny`, `--allow` or `--off` group, resolved to identifiers once so it
/// can be applied more than once without validating twice.
struct Override
{
  std::vector<nga::diag::DiagnosticId> ids;
  nga::diag::SeverityOverride action;
};

void applyOverrides( std::span<Override const> overrides, nga::diag::SeverityPolicy& policy )
{
  for ( Override const& group : overrides )
  {
    for ( nga::diag::DiagnosticId const id : group.ids )
    {
      policy.set( id, group.action );
    }
  }
}

/// The filesystem, as the loader reaches it. `nga_core` does no I/O of its own,
/// so this is where a Project file's includes and modules are actually read.
class DiskFiles final : public nga::model::FileReader
{
public:
  std::optional<std::string> read( std::filesystem::path const& path ) override
  {
    std::ifstream input{ path, std::ios::binary };
    if ( !input )
    {
      return std::nullopt;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }
};

/// Writes text to a file, or says why not.
/// `a,b,,c` as the three names it holds. An empty piece is nobody's name and
/// is dropped, so a trailing comma is not an error worth a message.
std::vector<std::string> splitOnCommas( std::string_view given )
{
  std::vector<std::string> names;
  while ( !given.empty() )
  {
    std::size_t const comma = given.find( ',' );
    std::string_view const piece = given.substr( 0, comma );
    if ( !piece.empty() )
    {
      names.emplace_back( piece );
    }
    if ( comma == std::string_view::npos )
    {
      break;
    }
    given.remove_prefix( comma + 1 );
  }
  return names;
}

bool writeText( std::string const& path, std::string const& text )
{
  std::ofstream out{ path, std::ios::binary };
  out.write( text.data(), static_cast<std::streamsize>( text.size() ) );
  if ( !out )
  {
    std::fprintf( stderr, "nga: cannot write %s\n", path.c_str() );
    return false;
  }
  return true;
}

int run( int argc, char** argv )
{
  CLI::App app{ "NGA - resource-aware 6502 assembler", "nga" };
  app.set_version_flag( "-V,--version", std::string{ nga::versionString() } );

  // Not `required()`: `--diagnostics-catalogue` describes the tool rather than
  // a build, and asking it for a Project to read would be asking for a file it
  // never opens. What a run without either is, is decided below.
  std::string projectPath;
  app.add_option( "project", projectPath, "The `.ngp` project file to build" );

  std::string output;
  app.add_option( "-o,--output", output, "Where to write the bytes; the project says what they are" );

  // Severity control, the same feature as the Project file's `diagnostics`
  // block seen from the other side — see docs/spec/diagnostics.md.
  std::vector<std::string> denied;
  std::vector<std::string> allowed;
  std::vector<std::string> suppressed;
  app.add_option( "--deny", denied, "Raise these diagnostics to errors" )->take_all();
  app.add_option( "--allow", allowed, "Lower these diagnostics to warnings" )->take_all();
  app.add_option( "--off", suppressed, "Suppress these diagnostics entirely" )->take_all();

  unsigned int jobs = 0;
  app.add_option( "-j,--jobs", jobs, "Number of assembly threads; 0 selects one per core" );

  bool verbose = false;
  app.add_flag( "-v,--verbose", verbose, "Log at debug level" );

  // Where the drivers and decoders the tool ships are found when a document
  // names one that is not beside it — see docs/spec/project-file.md.
  std::string library = shippedLibrary();
  app.add_option( "--lib", library, "The directory of the drivers and decoders the tool ships" );

  bool verifyLayout = false;
  app.add_flag( "--verify-layout", verifyLayout, "Re-read the layout and report any constraint it does not satisfy" );

  bool explain = false;
  app.add_flag( "--explain", explain, "When no layout exists, say which sections cannot be kept apart" );

  std::string mapPath;
  app.add_option( "--map", mapPath, "Write the memory map, phase by phase, as text to this file" );

  std::string emitAsmDir;
  app.add_option(
      "--emit-asm", emitAsmDir, "Write the text of every module the tool wrote rather than read into this directory" );

  std::string cataloguePath;
  app.add_option( "--diagnostics-catalogue",
                  cataloguePath,
                  "Write every diagnostic the tool can raise as JSON to this file, or to `-`; takes no project" );

  std::vector<std::string> classifyPaths;
  app.add_option( "--classify",
                  classifyPaths,
                  "Classify these files for a highlighter and write the result as JSON; takes no project" )
      ->take_all();

  std::string factsPath;
  app.add_option( "--facts",
                  factsPath,
                  "Write what the tool knows about this build as JSON to this file, or to `-`; the format is not "
                  "stable" );
  std::vector<std::string> factNames;
  app.add_option( "--fact", factNames, "Which facts to write: a name, `all`, or several separated by commas" )
      ->take_all();

  CLI11_PARSE( app, argc, argv );

  // The log goes to stderr, because stdout is where `--facts -` and the other
  // machine-readable outputs write: a line of logging in the middle of a JSON
  // document makes it no longer one.
  spdlog::set_default_logger( spdlog::stderr_color_mt( "nga" ) );

  // `--fact a,b` and `--fact a --fact b` are the same thing said twice over,
  // and both are answered before the Project is opened so that a name nobody
  // has is reported beside the run rather than after it.
  std::set<nga::model::FactSet> wantedFacts;
  std::vector<std::string> unknownFacts;
  for ( std::string const& given : factNames )
  {
    for ( auto const& name : splitOnCommas( given ) )
    {
      if ( name == "all" )
      {
        wantedFacts.merge( nga::model::everyFactSet() );
        continue;
      }
      if ( auto const set = nga::model::factSetFor( name ) )
      {
        wantedFacts.insert( *set );
      }
      else
      {
        unknownFacts.push_back( name );
      }
    }
  }
  // The cheapest two, which is what an editor asking after every save wants.
  if ( wantedFacts.empty() )
  {
    wantedFacts = { nga::model::FactSet::DIAGNOSTICS, nga::model::FactSet::PROJECT };
  }

  // Before anything is read: this says what the tool can find, not what it
  // found, so it answers on its own and a Project would be beside the point.
  if ( !cataloguePath.empty() )
  {
    std::string const catalogue = nga::diag::renderCatalogueJson();
    if ( cataloguePath == "-" )
    {
      std::fwrite( catalogue.data(), 1, catalogue.size(), stdout );
      return 0;
    }
    return writeText( cataloguePath, catalogue ) ? 0 : 2;
  }

  // Text nobody built: the shapes of syntax a document is written in, or a
  // file an editor is holding. The classification never needed a build, so
  // neither does this.
  if ( !classifyPaths.empty() )
  {
    nga::diag::SourceManager loose;
    nga::diag::SeverityPolicy allowAll;
    nga::diag::DiagnosticSink quiet{ allowAll };
    std::vector<nga::diag::FileId> files;
    for ( std::string const& path : classifyPaths )
    {
      std::ifstream in{ path, std::ios::binary };
      if ( !in )
      {
        std::fprintf( stderr, "nga: cannot read %s\n", path.c_str() );
        return 2;
      }
      std::string text{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
      files.push_back( loose.addFile( path, std::move( text ) ) );
    }
    std::string const written = nga::model::renderClassificationJson( loose, files, quiet );
    std::fwrite( written.data(), 1, written.size(), stdout );
    return 0;
  }

  if ( projectPath.empty() )
  {
    std::fprintf( stderr, "nga: a project is required\n%s", app.help().c_str() );
    return 1;
  }
  spdlog::set_level( verbose ? spdlog::level::debug : spdlog::level::info );

  nga::diag::SourceManager sources;
  nga::diag::SeverityPolicy policy;
  nga::diag::DiagnosticSink sink{ policy };

  std::vector<std::string> unknownCodes;
  std::vector<Override> const overrides = {
    { .ids = nga::diag::diagnosticIdsFor( denied, unknownCodes ), .action = nga::diag::SeverityOverride::DENY },
    { .ids = nga::diag::diagnosticIdsFor( allowed, unknownCodes ), .action = nga::diag::SeverityOverride::ALLOW },
    { .ids = nga::diag::diagnosticIdsFor( suppressed, unknownCodes ), .action = nga::diag::SeverityOverride::OFF },
  };
  for ( std::string const& code : unknownCodes )
  {
    sink.add( nga::diag::diagnostic( nga::diag::DiagnosticId::UNKNOWN_DIAGNOSTIC_CODE )
                  .arg( "code", code )
                  .sortedBy( code ) );
  }

  // A set nobody has is refused rather than written as an empty one: a name
  // that answers with nothing looks like a set with nothing in it, and sends a
  // consumer looking for the reason it is empty.
  for ( std::string const& name : unknownFacts )
  {
    sink.add( nga::diag::diagnostic( nga::diag::DiagnosticId::UNKNOWN_FACT_SET )
                  .arg( "name", name )
                  .arg( "sets", nga::model::factSetNames() )
                  .sortedBy( name ) );
  }

  // Applied before the Project is read, so that reading it obeys them, and
  // again afterwards, so that they outlast anything its `diagnostics` block
  // set. The command line is closer to the run and visible in the same place
  // as its output, so it is the one that wins.
  applyOverrides( overrides, policy );

  // A run always works on a Project, and the Project is the file it is given:
  // what a Module is called, which Phase it is resident in and what the Target
  // is are facts no source file carries — see
  // docs/decisions/0176-a-run-is-given-a-project.md.
  nga::model::Project project;
  DiskFiles disk;
  if ( std::filesystem::path{ projectPath }.extension() == ".ngp" )
  {
    project = nga::model::loadProject( sources, disk, projectPath, library, policy, sink );
  }
  else
  {
    sink.add( nga::diag::diagnostic( nga::diag::DiagnosticId::NOT_A_PROJECT )
                  .arg( "path", projectPath )
                  .note( nga::diag::diagnostic( nga::diag::DiagnosticId::MODULE_NAMED_BY_PROJECT ) )
                  .sortedBy( projectPath ) );
  }

  applyOverrides( overrides, policy );

  // The bytes are attributed through the Layout the map reports, so asking
  // for them asks for it too.
  bool const wantMap = !mapPath.empty() || wantedFacts.contains( nga::model::FactSet::MAP ) ||
                       wantedFacts.contains( nga::model::FactSet::BYTES );

  // A Project that failed to load names no Modules to build, and building from
  // it would report the absence a second time. What Container is written was
  // settled by the document, `-o` being a path and nothing else — see
  // docs/decisions/0179-a-container-is-chosen-in-the-project.md.
  std::vector<std::uint8_t> bytes;
  std::optional<std::uint32_t> origin;
  std::optional<nga::model::MemoryMap> map;
  std::optional<std::vector<nga::model::SymbolFacts>> symbols;
  std::optional<nga::model::ContainerFacts> container;
  bool const wantSymbols = wantedFacts.contains( nga::model::FactSet::SYMBOLS ) && !factsPath.empty();
  bool const wantBytes = wantedFacts.contains( nga::model::FactSet::BYTES ) && !factsPath.empty();
  if ( !sink.hasErrors() )
  {
    nga::model::BuildOptions const options{ .verifyLayout = verifyLayout, .explain = explain };
    nga::model::build( sources,
                       project,
                       options,
                       sink,
                       [&]( nga::model::Patched const& patched, nga::model::Emitted const& emitted )
                       {
                         // The map is read off the finished build, deciding
                         // nothing — see docs/spec/memory-map.md.
                         if ( wantMap )
                         {
                           map = nga::model::memoryMapOf( patched );
                         }
                         if ( wantSymbols )
                         {
                           symbols = nga::model::symbolsOf( patched );
                         }
                         bytes = emitted.bytes;
                         origin = emitted.origin;
                         if ( wantBytes && map.has_value() )
                         {
                           container = nga::model::containerFactsOf(
                               patched, *map, bytes, nga::model::nameOf( project.container ) );
                         }
                       } );
  }

  // What the tool wrote rather than read — a generator's text, or what a `.ngc`
  // Module compiled to — whether or not a later Step failed on it: an author
  // asking what the tool produced is usually asking because it did not build.
  if ( !emitAsmDir.empty() )
  {
    for ( nga::model::ProjectModule const& entry : project.modules )
    {
      std::optional<nga::diag::FileId> const text = entry.fromGenerator ? entry.file : entry.compiledText;
      if ( !text.has_value() )
      {
        continue;
      }
      std::filesystem::path const path = std::filesystem::path{ emitAsmDir } / ( entry.name + ".asm" );
      std::error_code ignored;
      std::filesystem::create_directories( path.parent_path(), ignored );
      if ( !writeText( path.string(), std::string{ sources.contentsOf( *text ) } ) )
      {
        return 2;
      }
    }
  }

  sink.sortForOutput( sources );
  std::string const report = nga::diag::renderText( sources, sink );
  if ( !report.empty() )
  {
    std::fputs( report.c_str(), stderr );
  }

  // Before the exit code, and whatever it turns out to be: a build that failed
  // still learned something, and the findings are what a reader wants most
  // when it did -- see docs/spec/facts.md.
  if ( !factsPath.empty() )
  {
    nga::model::Facts const facts{ .projectPath = projectPath,
                                   .project = &project,
                                   .map = map.has_value() ? &*map : nullptr,
                                   .symbols = symbols.has_value() ? &*symbols : nullptr,
                                   .bytes = container.has_value() ? &*container : nullptr };
    std::string const written = nga::model::renderFactsJson( sources, sink, facts, wantedFacts );
    if ( factsPath == "-" )
    {
      std::fwrite( written.data(), 1, written.size(), stdout );
    }
    else if ( !writeText( factsPath, written ) )
    {
      return 2;
    }
  }

  if ( sink.hasErrors() )
  {
    return 1;
  }

  if ( map.has_value() && !mapPath.empty() && !writeText( mapPath, nga::model::renderMapText( *map ) ) )
  {
    return 2;
  }

  if ( !output.empty() )
  {
    std::ofstream out{ output, std::ios::binary };
    out.write( reinterpret_cast<char const*>( bytes.data() ), static_cast<std::streamsize>( bytes.size() ) );
    if ( !out )
    {
      std::fprintf( stderr, "nga: cannot write %s\n", output.c_str() );
      return 2;
    }
    if ( origin.has_value() )
    {
      spdlog::info( "{} bytes at ${:04X}", bytes.size(), *origin );
    }
    else
    {
      spdlog::info( "{} bytes of .{}", bytes.size(), nga::model::nameOf( project.container ) );
    }
  }

  return 0;
}

} // namespace

// Nothing may escape main: an unhandled exception would abort without telling
// the user anything useful, and CLI11 reports argument problems by throwing.
// The handlers write to stderr directly rather than through spdlog, because
// logging may allocate or throw and would then be the second failure in a row.
int main( int argc, char** argv )
{
  try
  {
    return run( argc, argv );
  }
  catch ( std::exception const& error )
  {
    std::fprintf( stderr, "nga: %s\n", error.what() );
    return 2;
  }
  catch ( ... )
  {
    std::fputs( "nga: internal error: unknown exception\n", stderr );
    return 2;
  }
}
