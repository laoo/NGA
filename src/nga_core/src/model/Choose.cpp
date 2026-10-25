#include "nga/model/Choose.hpp"

#include "nga/model/Assemble.hpp"
#include "nga/model/Expand.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Place.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/Size.hpp"
#include "nga/model/Trace.hpp"
#include "nga/model/Transition.hpp"
#include "nga/model/TypeCheck.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace nga::model
{

namespace
{

/// What a later build found that the first did not: its errors, since its
/// warnings are the first build's a second time.
void reportErrorsOf( diag::DiagnosticSink const& quiet, diag::DiagnosticSink& sink )
{
  for ( diag::Finding const& finding : quiet.findings() )
  {
    if ( finding.severity == diag::Severity::ERROR )
    {
      sink.add( finding.diagnostic );
    }
  }
}

/// Where a later build is not the first's program but for its bytes: a
/// Section it reaches and the first does not, or the other way about, or one
/// of another size. The first such Section, since every one after it is
/// usually the same difference again.
bool sameProgram( Placed const& first, Sized const& later, Choice const& choice, diag::DiagnosticSink& sink )
{
  auto const differs = [&]( SectionRef where, std::string what )
  {
    Module const& module = first.modules()[where.module.value];
    Section const& section = module.sectionAt( where.section );
    sink.add( diag::diagnostic( diag::DiagnosticId::CHOICES_DIFFER )
                  .at( section.span().begin, section.span().length )
                  .arg( "document", choice.document )
                  .arg( "section", module.displayNameOf( where.section ) )
                  .arg( "what", std::move( what ) ) );
    return false;
  };
  if ( later.modules().size() != first.modules().size() )
  {
    diag::SourceSpan const site = first.project().chooseSite.value_or( diag::SourceSpan{} );
    sink.add( diag::diagnostic( diag::DiagnosticId::CHOICES_DIFFER )
                  .at( site.begin, site.length )
                  .arg( "document", choice.document )
                  .arg( "section", std::string{ "the program" } )
                  .arg( "what", std::string{ "has other modules" } ) );
    return false;
  }
  for ( std::uint32_t module = 0; module < first.modules().size(); ++module )
  {
    if ( later.modules()[module].sections().size() != first.modules()[module].sections().size() )
    {
      return differs( SectionRef{ .module = ModuleIndex{ module }, .section = SectionIndex{ 0 } },
                      "is not the same section" );
    }
    for ( std::uint32_t index = 0; index < first.modules()[module].sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      bool const reached = first.reachable().includes( where );
      if ( reached != later.reachable().includes( where ) )
      {
        return differs( where,
                        reached ? "is not reached, and under the first it is"
                                : "is reached, and under the first it is not" );
      }
      if ( !reached || !first.sizes().isKnown( where ) || !later.sizes().isKnown( where ) )
      {
        continue;
      }
      std::uint32_t const size = first.sizes().sizeOfSection( where );
      std::uint32_t const other = later.sizes().sizeOfSection( where );
      if ( size != other )
      {
        return differs( where, "is " + std::to_string( other ) + " bytes rather than " + std::to_string( size ) );
      }
    }
  }
  return true;
}

/// What the first `init` of the `load` block reaches: nothing where there is
/// none, or where its name is no Label, which the `.xex` reports.
Reachable firstInitReaches( Placed const& build )
{
  Project const& project = build.project();
  std::vector<SectionRef> from;
  if ( !project.loadInits.empty() )
  {
    for ( ModuleIndex const module : project.loadNeeds )
    {
      Symbol const* const found = build.symbols().moduleAt( module ).symbols().find( project.loadInits.front().name );
      if ( found == nullptr )
      {
        continue;
      }
      if ( auto const* const label = std::get_if<LabelPosition>( &found->value ); label != nullptr )
      {
        from.push_back( SectionRef{ .module = module, .section = label->section } );
      }
    }
  }
  return reachedFrom( build, from );
}

/// Whatever the load's code reaches holds `ngaChoice`, or nothing the file
/// runs while it loads can choose.
void warnUnchosen( Placed const& build, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  std::optional<SymbolRef> const choice = build.symbols().find( CHOICE_NAME );
  if ( !choice.has_value() || project.choices.empty() )
  {
    return;
  }
  std::optional<SectionRef> const cell = sectionNamedBy( build.symbols(), *choice );
  std::vector<SectionRef> code;
  for ( ModuleIndex const module : project.loadNeeds )
  {
    for ( std::uint32_t index = 0; index < build.modules()[module.value].sections().size(); ++index )
    {
      code.push_back( SectionRef{ .module = module, .section = SectionIndex{ index } } );
    }
  }
  std::optional<diag::SourceSpan> const site = project.chooseSite;
  if ( !site.has_value() || ( cell.has_value() && reachedFrom( build, code ).includes( *cell ) ) )
  {
    return;
  }
  sink.add( diag::diagnostic( diag::DiagnosticId::CHOICE_NEVER_MADE )
                .at( site->begin, site->length )
                .arg( "document", project.choices.front().document ) );
}

} // namespace

std::vector<Project> laterChoicesOf( diag::SourceManager const& sources, Project const& project )
{
  std::vector<Project> later;
  if ( !project.chooseModule.has_value() )
  {
    return later;
  }
  for ( std::size_t index = 1; index < project.choices.size(); ++index )
  {
    Choice const& choice = project.choices[index];
    Project& one = later.emplace_back( project );
    for ( ChoiceRegister const& value : choice.registers )
    {
      for ( Region& region : one.target.regions )
      {
        if ( region.property == RegionProperty::REGISTER && region.name == value.name )
        {
          region.range = value.range;
        }
      }
    }
    for ( ProjectConstant const& value : choice.constants )
    {
      std::string_view const name = sources.textOf( value.nameSpan );
      for ( ProjectConstant& constant : one.constants )
      {
        if ( sources.textOf( constant.nameSpan ) == name )
        {
          constant = value;
        }
      }
    }
  }
  return later;
}

std::vector<Storage::ChoiceSite> findChoiceSites( diag::SourceManager& sources,
                                                  std::span<Project const> later,
                                                  Placed const& build,
                                                  Storage const& storage,
                                                  Bytes const& bytes,
                                                  diag::DiagnosticSink& sink )
{
  if ( later.empty() )
  {
    return {};
  }
  warnUnchosen( build, sink );

  // What the load Step placed, its own code among it, is patched after the
  // rest and so here too.
  std::vector<SectionRef> byTheLoad;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    for ( std::uint32_t index = 0; index < build.modules()[module].sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( placedByTheLoad( build, where ) && build.layout().isPlaced( where ) )
      {
        byTheLoad.push_back( where );
      }
    }
  }

  std::size_t const count = later.size() + 1;
  // By Module, Section and offset, which is the order the list is written in.
  std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::vector<std::uint8_t>> sites;
  for ( std::size_t index = 0; index < later.size(); ++index )
  {
    Project project = later[index];
    Choice const& choice = build.project().choices[index + 1];
    diag::DiagnosticSink quiet{ sink.policy() };
    std::vector<Module> modules = assembleProject( sources, project, quiet );
    GlobalSymbols const symbols = merge( modules, project, quiet );
    Charsets const charsets = resolveCharsets( sources, symbols, quiet );
    Merged const merged{ sources, project, symbols, charsets };
    expand( sources, symbols, charsets, project, modules, quiet );
    if ( quiet.hasErrors() )
    {
      reportErrorsOf( quiet, sink );
      continue;
    }
    markRoots( sources, symbols, modules, quiet );
    Reachable const reachable = prune( merged, quiet );
    Pruned const pruned{ merged, reachable };
    auto const [interference, holds] = trace( pruned, quiet );
    Traced const traced{ pruned, interference, holds };
    Referrers const referrers = checkTypes( pruned, quiet );
    Sizes const sizes = computeSizes( pruned, quiet );
    if ( quiet.hasErrors() )
    {
      reportErrorsOf( quiet, sink );
      continue;
    }
    Sized const sized{ traced, referrers, sizes };
    if ( !sameProgram( build, sized, choice, sink ) )
    {
      continue;
    }

    // Over the first build's Layout, which every Choice shares.
    Placed const placed{ sized, build.layout() };
    Bytes made = patch( placed, storage, quiet );
    patchSections( placed, storage, byTheLoad, made, quiet );
    if ( quiet.hasErrors() )
    {
      reportErrorsOf( quiet, sink );
      continue;
    }
    for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
    {
      for ( std::uint32_t section = 0; section < build.modules()[module].sections().size(); ++section )
      {
        SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ section } };
        if ( !build.reachable().includes( where ) )
        {
          continue;
        }
        std::span<std::uint8_t const> const first = bytes.of( where );
        std::span<std::uint8_t const> const other = made.of( where );
        for ( std::size_t offset = 0; offset < first.size() && offset < other.size(); ++offset )
        {
          if ( first[offset] == other[offset] )
          {
            continue;
          }
          std::vector<std::uint8_t>& values = sites[{ module, section, static_cast<std::uint32_t>( offset ) }];
          if ( values.empty() )
          {
            values.assign( count, first[offset] );
          }
          values[index + 1] = other[offset];
        }
      }
    }
  }

  // What the first `init` reaches runs before anything is chosen, so a byte
  // of it that a Choice changes is changed too late; once a Section, at its
  // first. The load's code a later `init` runs is changed before it.
  Reachable const first = firstInitReaches( build );
  std::vector<Storage::ChoiceSite> found;
  std::optional<SectionRef> reported;
  for ( auto& [key, values] : sites )
  {
    auto const [module, section, offset] = key;
    SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ section } };
    if ( isLoadCode( build, where ) && first.includes( where ) )
    {
      if ( reported != where )
      {
        Module const& holder = build.modules()[module];
        Section const& code = holder.sectionAt( where.section );
        std::size_t const choice = static_cast<std::size_t>(
            std::ranges::find_if( values.begin() + 1,
                                  values.end(),
                                  [&values]( std::uint8_t value ) { return value != values.front(); } ) -
            values.begin() );
        sink.add( diag::diagnostic( diag::DiagnosticId::CHOICE_IN_LOAD_CODE )
                      .at( code.span().begin, code.span().length )
                      .arg( "document", build.project().choices[choice].document )
                      .arg( "section", holder.displayNameOf( where.section ) )
                      .arg( "offset", static_cast<std::int64_t>( offset ) ) );
        reported = where;
      }
      continue;
    }
    found.push_back( Storage::ChoiceSite{ .where = where, .offset = offset, .values = std::move( values ) } );
  }
  return found;
}

} // namespace nga::model
