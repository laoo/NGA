#include "nga/model/Panes.hpp"

#include <string>

namespace nga::model
{

namespace
{

/// A Section `in` a unit set: what the medium reads and nothing the CPU runs,
/// in a set that says where its Banks lie, and named alone. A Temporary never
/// comes here, the parser having dropped its `in`.
void bindUnitSetNamed( diag::SourceManager const& sources,
                       Project const& project,
                       Module& module,
                       SectionIndex index,
                       syntax::Token const& name,
                       UnitSetIndex set,
                       diag::DiagnosticSink& sink )
{
  Section& section = module.sectionAt( index );
  std::string const text{ sources.textOf( name.span() ) };
  if ( !section.alsoIn().empty() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::IN_UNITS_AND_MORE )
                  .at( name.location, name.length )
                  .arg( "name", text ) );
    return;
  }
  if ( !project.target.unitSets[set.value].linear.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::IN_UNITS_NOT_LINEAR )
                  .at( name.location, name.length )
                  .arg( "name", text ) );
    return;
  }
  char const* what = nullptr;
  if ( section.isProc() )
  {
    what = "a proc";
  }
  else if ( section.placement() == syntax::PlacementClass::ZEROPAGE )
  {
    what = "in zero page";
  }
  if ( what != nullptr )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::IN_UNITS_NOT_DATA )
                  .at( name.location, name.length )
                  .arg( "section", module.displayNameOf( index ) )
                  .arg( "what", what ) );
    return;
  }
  section.bindUnitSet( set );
}

} // namespace

void resolvePanes( diag::SourceManager const& sources,
                   Project const& project,
                   std::span<Module> modules,
                   diag::DiagnosticSink& sink )
{
  for ( Module& module : modules )
  {
    for ( std::uint32_t index = 0; index < module.sections().size(); ++index )
    {
      Section& section = module.sectionAt( SectionIndex{ index } );
      if ( std::optional<syntax::Token> const under = section.underName(); under.has_value() )
      {
        // `under` names one Pane that is shown, and a family's member is
        // known at run time alone — see
        // docs/decisions/0098-a-proc-declares-what-is-shown.md.
        std::string_view const text = sources.textOf( under->span() );
        std::optional<PaneIndex> const pane = project.target.paneNamed( text );
        if ( !pane.has_value() )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::PANE_UNKNOWN )
                        .at( under->location, under->length )
                        .arg( "name", std::string{ text } ) );
        }
        else if ( project.target.panes[pane->value].count > 1 )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::UNDER_ATTRIBUTE )
                        .at( under->location, under->length )
                        .arg( "name", std::string{ text } )
                        .arg( "reason", "names a family, whose member is known at run time alone" ) );
        }
        else
        {
          section.bindUnder( *pane );
        }
      }
      if ( !section.paneName().has_value() )
      {
        continue;
      }
      syntax::Token const name = *section.paneName();
      std::string_view const text = sources.textOf( name.span() );
      std::optional<PaneIndex> const pane = project.target.paneNamed( text );

      // A Region's name puts the Section within that Region's memory, which is
      // how it is told to stand under a ROM, and several of them within one of
      // those; a Pane is one, and named alone — see
      // docs/decisions/0243-a-section-may-name-the-region-it-stands-in.md.
      auto const bindRegionNamed = [&]( syntax::Token const& one )
      {
        std::string_view const named = sources.textOf( one.span() );
        if ( project.target.unitSetNamed( named ).has_value() )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::IN_UNITS_AND_MORE )
                        .at( one.location, one.length )
                        .arg( "name", std::string{ named } ) );
          return;
        }
        if ( project.target.paneNamed( named ).has_value() )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::IN_PANE_AND_MORE )
                        .at( one.location, one.length )
                        .arg( "name", std::string{ named } ) );
          return;
        }
        auto const region =
            std::ranges::find_if( project.target.regions, [named]( Region const& r ) { return r.name == named; } );
        if ( region == project.target.regions.end() )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::PANE_UNKNOWN )
                        .at( one.location, one.length )
                        .arg( "name", std::string{ named } ) );
        }
        else if ( region->property != RegionProperty::RAM && region->property != RegionProperty::ROM )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::IN_REGION_NOT_MEMORY )
                        .at( one.location, one.length )
                        .arg( "name", std::string{ named } ) );
        }
        else
        {
          section.bindRegion( region->range );
        }
      };
      // A unit set's name puts the Section in the medium's own memory, at a
      // linear address and across Banks, where only the medium reads it — see
      // docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
      if ( std::optional<UnitSetIndex> const set = project.target.unitSetNamed( text ); set.has_value() )
      {
        bindUnitSetNamed( sources, project, module, SectionIndex{ index }, name, *set, sink );
        continue;
      }
      if ( !pane.has_value() || !section.alsoIn().empty() )
      {
        bindRegionNamed( name );
        for ( syntax::Token const& one : section.alsoIn() )
        {
          bindRegionNamed( one );
        }
        continue;
      }
      if ( section.placement() == syntax::PlacementClass::ZEROPAGE )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::PANE_ZERO_PAGE )
                      .at( name.location, name.length )
                      .arg( "section", module.displayNameOf( SectionIndex{ index } ) )
                      .arg( "pane", std::string{ text } ) );
        continue;
      }
      section.bindPane( *pane );
    }
  }
}

} // namespace nga::model
