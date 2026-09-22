#include "nga/model/Regions.hpp"

#include "nga/model/Symbol.hpp"
#include "nga/syntax/Expression.hpp"

namespace nga::model
{

Module buildRegionModule( diag::SourceManager const& sources, ProjectModule const& entry, Target const& target )
{
  Module module{ entry.name, entry.file, entry.residency };
  for ( std::uint32_t index = 0; index < target.regions.size(); ++index )
  {
    Region const& region = target.regions[index];
    if ( !region.nameSpan.has_value() )
    {
      continue;
    }
    // Two names of the Target alike were refused where the second was read,
    // so nothing here can fail to be added.
    module.symbols().add(
        Symbol{ .name = sources.textOf( *region.nameSpan ),
                .kind = SymbolKind::REGION,
                .definition = *region.nameSpan,
                .exported = true,
                .value = RegionValue{ .region = RegionIndex{ index }, .address = region.range.begin } } );
  }
  // A unit set's name is its count: an `Integer` the variant wrote out, as a
  // Region's address is, so that a driver can hold its table of values to
  // the variant's number — see docs/decisions/0053-a-window-names-its-units.md.
  for ( UnitSet const& set : target.unitSets )
  {
    syntax::Token const token{ .kind = syntax::TokenKind::NUMBER,
                               .location = set.nameSpan.begin,
                               .length = set.nameSpan.length };
    syntax::ExpressionPtr count = syntax::makeExpression( syntax::ExpressionKind::VALUE, token, set.nameSpan );
    count->value = static_cast<std::int64_t>( set.count );
    module.symbols().add( Symbol{ .name = sources.textOf( set.nameSpan ),
                                  .kind = SymbolKind::CONSTANT,
                                  .definition = set.nameSpan,
                                  .exported = true,
                                  .value = std::move( count ) } );
  }
  for ( std::uint32_t index = 0; index < target.windows.size(); ++index )
  {
    Window const& window = target.windows[index];
    module.symbols().add( Symbol{ .name = sources.textOf( window.nameSpan ),
                                  .kind = SymbolKind::WINDOW,
                                  .definition = window.nameSpan,
                                  .exported = true,
                                  .value = WindowIndex{ index } } );
  }
  // A Pane's name is the first name a Project declares that the source
  // speaks: an expression of type `Pane`, whose value is the index of the
  // state the solver gave it — see docs/decisions/0054-panes.md.
  for ( std::uint32_t index = 0; index < target.panes.size(); ++index )
  {
    Pane const& pane = target.panes[index];
    module.symbols().add( Symbol{ .name = sources.textOf( pane.nameSpan ),
                                  .kind = SymbolKind::PANE,
                                  .definition = pane.nameSpan,
                                  .exported = true,
                                  .value = PaneIndex{ index } } );
  }
  return module;
}

} // namespace nga::model
