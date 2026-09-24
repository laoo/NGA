// The CLI is deliberately thin: it parses arguments, reads and writes files,
// and hands over to nga_core. Everything testable lives in the library.

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include "nga/Version.hpp"
#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/Renderer.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/MemoryMap.hpp"
#include "nga/model/Pipeline.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/ProjectFile.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace
{

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

  std::string projectPath;
  app.add_option( "project", projectPath, "The `.ngp` project file to build" )->required();

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

  bool json = false;
  app.add_flag( "--diagnostics-json{true}", json, "Write diagnostics as JSON rather than as text" );

  unsigned int jobs = 0;
  app.add_option( "-j,--jobs", jobs, "Number of assembly threads; 0 selects one per core" );

  bool verbose = false;
  app.add_flag( "-v,--verbose", verbose, "Log at debug level" );

  // Where the drivers and decoders the tool ships are found when a document
  // names one that is not beside it — see docs/spec/project-file.md.
  std::string library = NGA_LIB_DIR;
  app.add_option( "--lib", library, "The directory of the drivers and decoders the tool ships" );

  bool verifyLayout = false;
  app.add_flag( "--verify-layout", verifyLayout, "Re-read the layout and report any constraint it does not satisfy" );

  bool explain = false;
  app.add_flag( "--explain", explain, "When no layout exists, say which sections cannot be kept apart" );

  std::string mapPath;
  app.add_option( "--map", mapPath, "Write the memory map, phase by phase, as text to this file" );
  std::string mapHtmlPath;
  app.add_option( "--map-html", mapHtmlPath, "Write the memory map as a self-contained HTML page to this file" );

  std::string emitAsmDir;
  app.add_option(
      "--emit-asm", emitAsmDir, "Write the text of every module the tool wrote rather than read into this directory" );

  CLI11_PARSE( app, argc, argv );
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

  bool const wantMap = !mapPath.empty() || !mapHtmlPath.empty();

  // A Project that failed to load names no Modules to build, and building from
  // it would report the absence a second time. What Container is written was
  // settled by the document, `-o` being a path and nothing else — see
  // docs/decisions/0179-a-container-is-chosen-in-the-project.md.
  std::vector<std::uint8_t> bytes;
  std::optional<std::uint32_t> origin;
  std::optional<nga::model::MemoryMap> map;
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
                         bytes = emitted.bytes;
                         origin = emitted.origin;
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
  std::string const report = json ? nga::diag::renderJson( sources, sink ) : nga::diag::renderText( sources, sink );
  if ( !report.empty() )
  {
    std::fputs( report.c_str(), stderr );
  }

  if ( sink.hasErrors() )
  {
    return 1;
  }

  if ( map.has_value() )
  {
    if ( !mapPath.empty() && !writeText( mapPath, nga::model::renderMapText( *map ) ) )
    {
      return 2;
    }
    if ( !mapHtmlPath.empty() && !writeText( mapHtmlPath, nga::model::renderMapHtml( *map ) ) )
    {
      return 2;
    }
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
