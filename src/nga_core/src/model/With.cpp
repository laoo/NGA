#include "nga/model/With.hpp"

#include "nga/model/Evaluate.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/References.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

/// One thing a `.with` shows: a Window, and within it a Pane — a member of
/// a family, or the whole family under `, x` — or a named state, or with
/// `WINDOW, x` nothing that can be named at all.
struct Shown
{
  WindowIndex window;
  std::optional<PaneIndex> pane;
  std::optional<std::uint32_t> state;
};

/// A Reference a Chunk holds, as this check needs it: what it reaches, how,
/// and where it was written.
struct Ref
{
  SectionRef target;
  ReferenceKind kind = ReferenceKind::ESCAPE;
  syntax::Expression const* site = nullptr;
  std::string_view symbol;
};

/// One statement under one or more `.with`: what they show, and what the
/// statement itself names.
struct Site
{
  diag::SourceSpan span;
  std::vector<Shown> shown;
  std::vector<Ref> refs;

  /// Whether some `.with` on the statement could not be resolved, which
  /// Expand reported: nothing about the statement can then be said.
  bool unresolved = false;

  /// Where the outermost `.with` stands in a macro body: the use the body
  /// was instantiated at, which a finding notes.
  std::optional<diag::SourceSpan> use{};
};

/// What a Section holds for the check: the References of its Chunks under no
/// `.with`, and its `.with` sites.
struct Gathered
{
  std::vector<Ref> plain;
  std::vector<Site> sites;
};

/// The Section a Symbol names as memory, or nothing for a Slot, a Region, a
/// Pane and every other kind that is not one.
std::optional<SectionRef> memoryOf( GlobalSymbols const& symbols, SymbolRef where )
{
  Symbol const& symbol = symbols.at( where );
  if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value ); label != nullptr )
  {
    return SectionRef{ .module = where.module, .section = label->section };
  }
  return std::nullopt;
}

class Collector final : public ReferenceVisitor
{
public:
  Collector( GlobalSymbols const& symbols, ModuleIndex from, ReferenceKind kind, syntax::Expression const& site )
      : mSymbols( &symbols ), mFrom( from ), mKind( kind ), mSite( &site )
  {
  }

  void reference( SymbolRef target ) override
  {
    if ( std::optional<SectionRef> const memory = memoryOf( *mSymbols, target ); memory.has_value() )
    {
      found.push_back( Ref{ .target = *memory, .kind = mKind, .site = mSite, .symbol = mSymbols->at( target ).name } );
    }
  }

  void localReference( LabelPosition target ) override
  {
    found.push_back( Ref{ .target = SectionRef{ .module = mFrom, .section = target.section },
                          .kind = mKind,
                          .site = mSite,
                          .symbol = {} } );
  }

  std::vector<Ref> found;

private:
  GlobalSymbols const* mSymbols;
  ModuleIndex mFrom;
  ReferenceKind mKind;
  syntax::Expression const* mSite;
};

class Checker
{
public:
  Checker( Pruned const& build, Freezes& freezes, diag::DiagnosticSink& sink )
      : mBuild( &build ), mFreezes( &freezes ), mSink( &sink )
  {
  }

  void run()
  {
    std::span<Module const> const modules = mBuild->modules();
    for ( std::uint32_t module = 0; module < modules.size(); ++module )
    {
      for ( std::uint32_t index = 0; index < modules[module].sections().size(); ++index )
      {
        SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
        if ( !mBuild->reachable().includes( where ) )
        {
          continue;
        }
        mGathered.emplace( keyOf( where ), gather( where ) );
        // A Proc that runs under a Pane is not seen in the Window's base
        // state: Place keeps it out of the Window.
        if ( std::optional<PaneIndex> const under = underOf( where ); under.has_value() )
        {
          mFreezes->keepOut( where, mBuild->target().panes[under->value].window );
        }
      }
    }
    for ( auto const& [key, gathered] : mGathered )
    {
      SectionRef const where = unkey( key );
      checkPlain( where, gathered.plain );
      for ( Site const& site : gathered.sites )
      {
        checkSite( where, site );
      }
    }
  }

private:
  static std::uint64_t keyOf( SectionRef where )
  {
    return ( static_cast<std::uint64_t>( where.module.value ) << 32U ) | where.section.value;
  }

  static SectionRef unkey( std::uint64_t key )
  {
    return SectionRef{ .module = ModuleIndex{ static_cast<std::uint32_t>( key >> 32U ) },
                       .section = SectionIndex{ static_cast<std::uint32_t>( key & 0xFFFFFFFFU ) } };
  }

  [[nodiscard]] std::optional<PaneIndex> paneOf( SectionRef where ) const
  {
    return mBuild->symbols().moduleAt( where.module ).sectionAt( where.section ).pane();
  }

  /// The Pane a Proc declares it runs under, or nothing — see
  /// docs/decisions/0098-a-proc-declares-what-is-shown.md.
  [[nodiscard]] std::optional<PaneIndex> underOf( SectionRef where ) const
  {
    return mBuild->symbols().moduleAt( where.module ).sectionAt( where.section ).under();
  }

  /// The Pane a Section's own code runs with shown: the one it is in, or
  /// the one it declares it runs under.
  [[nodiscard]] std::optional<PaneIndex> ownPaneOf( SectionRef where ) const
  {
    std::optional<PaneIndex> const pane = paneOf( where );
    return pane.has_value() ? pane : underOf( where );
  }

  /// The same under a site's `.with`: the Pane a Proc runs under is switched
  /// out by a `.with` on its Window, and a Section in a Pane never stands
  /// under one on its own Window.
  [[nodiscard]] std::optional<PaneIndex> ownPaneUnder( SectionRef where, Site const& site ) const
  {
    std::optional<PaneIndex> const pane = paneOf( where );
    if ( pane.has_value() )
    {
      return pane;
    }
    std::optional<PaneIndex> const under = underOf( where );
    if ( !under.has_value() )
    {
      return std::nullopt;
    }
    WindowIndex const window = mBuild->target().panes[under->value].window;
    bool const switched =
        std::ranges::any_of( site.shown, [window]( Shown const& shown ) { return shown.window == window; } );
    return switched ? std::nullopt : under;
  }

  [[nodiscard]] std::string nameOf( SectionRef where ) const
  {
    return mBuild->symbols().moduleAt( where.module ).displayNameOf( where.section, mBuild->sources() );
  }

  /// The state of a Window a Section is in, where that is known before Place:
  /// a Pane pinned to a named state gives it, and so does a pin inside a
  /// Window's ranges, which puts the Section in the base state the variant
  /// declares. Nothing for a Section the solver places, which Place keeps
  /// where every Phase of its Residency shows it, and for a Bank of a set.
  [[nodiscard]] std::optional<std::pair<WindowIndex, std::uint32_t>> stateOf( SectionRef where ) const
  {
    Target const& target = mBuild->target();
    Section const& section = mBuild->symbols().moduleAt( where.module ).sectionAt( where.section );
    if ( section.pane().has_value() )
    {
      Pane const& pane = target.panes[section.pane()->value];
      return pane.state.has_value() ? std::optional{ std::pair{ pane.window, *pane.state } } : std::nullopt;
    }
    if ( section.pinnedAddress() == nullptr )
    {
      return std::nullopt;
    }
    std::optional<std::int64_t> const pinned = declaredValueOf(
        mBuild->sources(), mBuild->symbols(), &mBuild->charsets(), where.module, *section.pinnedAddress() );
    if ( !pinned.has_value() || *pinned < 0 )
    {
      return std::nullopt;
    }
    for ( std::uint32_t index = 0; index < target.windows.size(); ++index )
    {
      Window const& window = target.windows[index];
      if ( window.covers( static_cast<std::uint32_t>( *pinned ) ) && window.base.has_value() )
      {
        return std::pair{ WindowIndex{ index }, *window.base };
      }
    }
    return std::nullopt;
  }

  /// The name of a Window's named state, for a finding.
  [[nodiscard]] std::string stateName( WindowIndex window, std::uint32_t state ) const
  {
    Window const& shown = mBuild->target().windows[window.value];
    std::uint32_t index = 0;
    for ( WindowState const& candidate : shown.states )
    {
      if ( index == state && !candidate.units.has_value() )
      {
        return candidate.name;
      }
      index += candidate.units.has_value() ? mBuild->target().unitSets[candidate.units->value].count : 1;
    }
    return "bank " + std::to_string( state );
  }

  /// A Section in a known state is seen from `from` in every Phase both are
  /// in only where that Phase shows the state — or where `shown` does.
  /// Reports the first Phase that does not, and says whether it was seen.
  bool checkState( SectionRef from, Ref const& ref, std::span<Shown const> shown, diag::SourceSpan const* site ) const
  {
    std::optional<std::pair<WindowIndex, std::uint32_t>> const state = stateOf( ref.target );
    if ( !state.has_value() )
    {
      return true;
    }
    for ( Shown const& one : shown )
    {
      bool const pinnedPane = one.pane.has_value() && mBuild->target().panes[one.pane->value].state == state->second &&
                              mBuild->target().panes[one.pane->value].window == state->first;
      if ( one.window == state->first && ( one.state == state->second || pinnedPane ) )
      {
        return true;
      }
    }
    Residency const& referrer = mBuild->symbols().moduleAt( from.module ).residency();
    Residency const& referred = mBuild->symbols().moduleAt( ref.target.module ).residency();
    for ( std::uint32_t phase = 0; phase < referrer.phaseCount(); ++phase )
    {
      PhaseIndex const here{ phase };
      if ( !referrer.includes( here ) || referred.phaseCount() <= phase || !referred.includes( here ) )
      {
        continue;
      }
      if ( baseIn( mBuild->project(), here, state->first ) == state->second )
      {
        continue;
      }
      diag::SourceSpan at{};
      if ( site != nullptr )
      {
        at = *site;
      }
      else if ( ref.site != nullptr )
      {
        at = ref.site->span;
      }
      mSink->add( diag::diagnostic( diag::DiagnosticId::STATE_NOT_SHOWN )
                      .at( at.begin, at.length )
                      .arg( "symbol", std::string{ ref.symbol } )
                      .arg( "state", stateName( state->first, state->second ) )
                      .arg( "window", mBuild->target().windows[state->first.value].name )
                      .arg( "phase", mBuild->phases().phases[phase].name.value_or( "(implicit)" ) ) );
      return false;
    }
    return true;
  }

  void collect( SectionRef where, Section const& section, Chunk const& chunk, std::vector<Ref>& into ) const
  {
    ReferenceKind const kind = referenceKindOf( mBuild->sources(), chunk );
    for ( syntax::ExpressionPtr const& item : section.itemsOf( chunk ) )
    {
      Collector collector{ mBuild->symbols(), where.module, kind, *item };
      walkReferences( mBuild->symbols(), mBuild->sources(), where.module, *item, collector );
      into.insert( into.end(), collector.found.begin(), collector.found.end() );
    }
  }

  /// A Chunk's References: a use's are its expansion's, since the arguments
  /// stand cloned inside it.
  void collectChunk(
      SectionRef where, Section const& section, std::uint32_t index, Chunk const& chunk, std::vector<Ref>& into ) const
  {
    if ( std::holds_alternative<MacroUseContent>( chunk.content ) )
    {
      for ( Chunk const& inner : section.innerChunksOf( ChunkIndex{ index } ) )
      {
        collect( where, section, inner, into );
      }
      return;
    }
    collect( where, section, chunk, into );
  }

  [[nodiscard]] Gathered gather( SectionRef where ) const
  {
    Section const& section = mBuild->symbols().moduleAt( where.module ).sectionAt( where.section );
    Gathered gathered;

    // The `.with` open at this point, in order, and what each shows — one
    // Expand could not resolve shows nothing and is counted instead. A
    // `.with` of a macro body stands among the use's inner Chunks, as a
    // marker either side of its statement, and opens and closes as one on
    // the Section's own line does.
    struct Open
    {
      diag::SourceSpan span;
      bool resolved;
      std::optional<diag::SourceSpan> use;
    };

    std::vector<Open> open;
    std::vector<Shown> shown;
    std::optional<std::size_t> current;
    auto const enter = [&]( MacroUseContent const& use, diag::SourceSpan span, std::optional<diag::SourceSpan> at )
    {
      open.push_back( Open{ .span = span, .resolved = use.window.has_value(), .use = at } );
      if ( use.window.has_value() )
      {
        shown.push_back( Shown{ .window = *use.window, .pane = use.pane, .state = use.shownState } );
      }
      current.reset();
    };
    auto const leave = [&]
    {
      if ( !open.empty() )
      {
        if ( open.back().resolved )
        {
          shown.pop_back();
        }
        open.pop_back();
      }
      current.reset();
    };
    // Where a Chunk's References go: the plain ones under no `.with`, and
    // otherwise the site of the statement, one for every run of Chunks under
    // the same `.with`.
    auto const into = [&]
    {
      if ( open.empty() )
      {
        return std::ref( gathered.plain );
      }
      if ( !current.has_value() )
      {
        current = gathered.sites.size();
        gathered.sites.push_back(
            Site{ .span = open.front().span,
                  .shown = shown,
                  .refs = {},
                  .unresolved = std::ranges::any_of( open, []( Open const& one ) { return !one.resolved; } ),
                  .use = open.front().use } );
      }
      return std::ref( gathered.sites[*current].refs );
    };

    for ( std::uint32_t index = 0; index < section.chunks().size(); ++index )
    {
      Chunk const& chunk = section.chunks()[index];
      auto const* const use = std::get_if<MacroUseContent>( &chunk.content );
      if ( use != nullptr && use->side == WithSide::ENTER )
      {
        // The switch itself runs in what is shown so far; what it calls is
        // the driver's, which stands in no Pane.
        collectChunk( where, section, index, chunk, gathered.plain );
        enter( *use, chunk.span, std::nullopt );
        continue;
      }
      if ( use != nullptr && use->side == WithSide::LEAVE )
      {
        collectChunk( where, section, index, chunk, gathered.plain );
        leave();
        continue;
      }
      if ( use == nullptr )
      {
        collect( where, section, chunk, into().get() );
        continue;
      }
      for ( Chunk const& inner : section.innerChunksOf( ChunkIndex{ index } ) )
      {
        auto const* const marker = std::get_if<MacroUseContent>( &inner.content );
        if ( marker != nullptr && marker->side == WithSide::ENTER )
        {
          // What the switch expands to follows the marker among the inner
          // Chunks and is read as the statement's: the driver's, which
          // stands in no Pane, so nothing is said of it either way.
          enter( *marker, inner.span, chunk.span );
          continue;
        }
        if ( marker != nullptr && marker->side == WithSide::LEAVE )
        {
          leave();
          continue;
        }
        collect( where, section, inner, into().get() );
      }
    }
    return gathered;
  }

  /// Under no `.with`: a Section of a Pane the solver gives a Bank is named
  /// only by the Pane's own code, and one in a known state — a Pane pinned to
  /// it, or a pin inside a Window — only from Phases whose base shows it.
  void checkPlain( SectionRef from, std::vector<Ref> const& refs ) const
  {
    std::optional<PaneIndex> const own = ownPaneOf( from );
    for ( Ref const& ref : refs )
    {
      std::optional<PaneIndex> const pane = paneOf( ref.target );
      if ( pane.has_value() && pane == own )
      {
        continue;
      }
      // A Proc that runs under a Pane is called where the Pane is shown: by
      // the Pane's own code, by another Proc under it, or under a `.with`
      // that shows it, which is the site's to check.
      if ( std::optional<PaneIndex> const runsWith = underOf( ref.target );
           runsWith.has_value() && ref.target != from && runsWith != own && ref.site != nullptr )
      {
        mSink->add( diag::diagnostic( diag::DiagnosticId::UNDER_NOT_SHOWN )
                        .at( ref.site->span.begin, ref.site->span.length )
                        .arg( "symbol", std::string{ ref.symbol } )
                        .arg( "pane", mBuild->target().panes[runsWith->value].name ) );
        continue;
      }
      if ( pane.has_value() && !mBuild->target().panes[pane->value].state.has_value() && ref.site != nullptr )
      {
        mSink->add( diag::diagnostic( diag::DiagnosticId::CROSS_VIEW_REFERENCE )
                        .at( ref.site->span.begin, ref.site->span.length )
                        .arg( "symbol", std::string{ ref.symbol } )
                        .arg( "pane", mBuild->target().panes[pane->value].name ) );
        continue;
      }
      checkState( from, ref, {}, nullptr );
    }
  }

  /// Whether a Pane is shown by one of the site's `.with`.
  [[nodiscard]] bool shows( Site const& site, PaneIndex pane ) const
  {
    Pane const& one = mBuild->target().panes[pane.value];
    return std::ranges::any_of( site.shown,
                                [&one, pane]( Shown const& shown )
                                {
                                  return shown.pane == pane ||
                                         ( shown.state.has_value() && one.window == shown.window &&
                                           one.state == shown.state );
                                } );
  }

  /// One statement and everything it reaches: no Pane but the ones shown is
  /// named or stood in, and what is in no Pane keeps out of the Windows.
  void checkSite( SectionRef home, Site const& site )
  {
    if ( site.unresolved )
    {
      return;
    }
    Target const& target = mBuild->target();
    auto const keepOut = [&]( SectionRef where )
    {
      if ( paneOf( where ).has_value() )
      {
        return;
      }
      for ( Shown const& shown : site.shown )
      {
        mFreezes->keepOut( where, shown.window );
      }
    };
    auto const check = [&]( SectionRef from, Ref const& ref )
    {
      // Such a Proc's own labels are its own; a call of it from elsewhere
      // stands where the Pane it runs under is shown.
      if ( std::optional<PaneIndex> const runsWith = underOf( ref.target );
           runsWith.has_value() && ref.target != from && runsWith != ownPaneUnder( from, site ) &&
           !shows( site, *runsWith ) )
      {
        diag::Diagnostic finding = diag::diagnostic( diag::DiagnosticId::UNDER_NOT_SHOWN )
                                       .at( site.span.begin, site.span.length )
                                       .arg( "symbol", std::string{ ref.symbol } )
                                       .arg( "pane", target.panes[runsWith->value].name );
        if ( ref.site != nullptr )
        {
          finding = std::move( finding ).note( diag::diagnostic( diag::DiagnosticId::NAMED_HERE )
                                                   .at( ref.site->span.begin, ref.site->span.length )
                                                   .arg( "symbol", std::string{ ref.symbol } ) );
        }
        mSink->add( std::move( finding ) );
        return false;
      }
      std::optional<PaneIndex> const pane = paneOf( ref.target );
      std::optional<PaneIndex> const own = ownPaneUnder( from, site );
      if ( pane.has_value() && pane != own && target.panes[pane->value].state.has_value() )
      {
        // Pinned to a named state: seen where the state is shown, by the
        // `.with` or by the Phase's base.
        if ( !checkState( from, ref, site.shown, &site.span ) )
        {
          return false;
        }
        keepOut( ref.target );
        return true;
      }
      if ( !pane.has_value() && !checkState( from, ref, site.shown, &site.span ) )
      {
        return false;
      }
      if ( pane.has_value() && pane != own && !shows( site, *pane ) )
      {
        diag::Diagnostic finding = diag::diagnostic( diag::DiagnosticId::WITH_REACHES_OTHER_PANE )
                                       .at( site.span.begin, site.span.length )
                                       .arg( "symbol", std::string{ ref.symbol } )
                                       .arg( "pane", target.panes[pane->value].name );
        if ( ref.site != nullptr )
        {
          finding = std::move( finding ).note( diag::diagnostic( diag::DiagnosticId::NAMED_HERE )
                                                   .at( ref.site->span.begin, ref.site->span.length )
                                                   .arg( "symbol", std::string{ ref.symbol } ) );
        }
        if ( site.use.has_value() )
        {
          finding = std::move( finding ).note(
              diag::diagnostic( diag::DiagnosticId::EXPANSION_BEGAN_HERE ).at( site.use->begin, site.use->length ) );
        }
        mSink->add( std::move( finding ) );
        return false;
      }
      keepOut( ref.target );
      return true;
    };

    keepOut( home );
    std::vector<SectionRef> pending;
    std::vector<std::uint64_t> visited;
    auto const follow = [&]( Ref const& ref )
    {
      if ( ( ref.kind == ReferenceKind::CALL || ref.kind == ReferenceKind::JUMP ) &&
           std::ranges::find( visited, keyOf( ref.target ) ) == visited.end() )
      {
        visited.push_back( keyOf( ref.target ) );
        pending.push_back( ref.target );
      }
    };
    for ( Ref const& ref : site.refs )
    {
      if ( check( home, ref ) )
      {
        follow( ref );
      }
    }
    // The closure over Calls and Jumps, with Prune's walk: a callee's own
    // References, under no `.with` of its own — one it has is checked as
    // its own site.
    while ( !pending.empty() )
    {
      SectionRef const reached = pending.back();
      pending.pop_back();
      keepOut( reached );
      auto const gathered = mGathered.find( keyOf( reached ) );
      if ( gathered == mGathered.end() )
      {
        continue;
      }
      for ( Ref const& ref : gathered->second.plain )
      {
        if ( check( reached, ref ) )
        {
          follow( ref );
        }
      }
    }
  }

  Pruned const* mBuild;
  Freezes* mFreezes;
  diag::DiagnosticSink* mSink;
  std::map<std::uint64_t, Gathered> mGathered;
};

} // namespace

void checkWiths( Pruned const& build, Freezes& freezes, diag::DiagnosticSink& sink )
{
  Checker checker{ build, freezes, sink };
  checker.run();
}

} // namespace nga::model
