#include "nga/model/Panes.hpp"

#include <string>

namespace nga::model
{

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
      if ( !pane.has_value() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::PANE_UNKNOWN )
                      .at( name.location, name.length )
                      .arg( "name", std::string{ text } ) );
        continue;
      }
      if ( section.isMovable() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::PANE_SECTION_MOVABLE )
                      .at( name.location, name.length )
                      .arg( "section", module.displayNameOf( SectionIndex{ index }, sources ) )
                      .arg( "pane", std::string{ text } ) );
        continue;
      }
      if ( section.placement() == PlacementClass::ZEROPAGE )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::PANE_ZERO_PAGE )
                      .at( name.location, name.length )
                      .arg( "section", module.displayNameOf( SectionIndex{ index }, sources ) )
                      .arg( "pane", std::string{ text } ) );
        continue;
      }
      section.bindPane( *pane );
    }
  }
}

} // namespace nga::model
