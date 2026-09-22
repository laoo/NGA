#include "nga/model/ModuleBuilder.hpp"

#include "nga/model/Isa.hpp"
#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <map>
#include <ranges>
#include <string>
#include <utility>

namespace nga::model
{

namespace
{

/// The bare name of a local label: `@-loop` names `loop`, `@+` and `@` name
/// nothing at all. Named and anonymous labels are separate populations, and an
/// empty name is what marks the second.
std::string_view bareLocalName( std::string_view text )
{
  text.remove_prefix( 1 ); // `@`
  if ( !text.empty() && ( text.front() == '+' || text.front() == '-' ) )
  {
    text.remove_prefix( 1 );
  }
  return text;
}

/// The three attributes of a Section, which `docs/spec/syntax.md` closes.
bool isAttributeName( std::string_view text )
{
  return text == "runtimeSectionAddress" || text == "runtimeSectionSize" || text == "storagePayloadAddress" ||
         text == "storagePayloadSize" || text == "resident";
}

bool isLocal( syntax::Token token )
{
  return token.kind == syntax::TokenKind::LOCAL_IDENTIFIER || token.kind == syntax::TokenKind::ANONYMOUS_LABEL;
}

} // namespace

ModuleBuilder::ModuleBuilder( diag::SourceManager const& sources, diag::DiagnosticSink& sink, Module& module )
    : mSources( &sources ), mSink( &sink ), mModule( &module )
{
}

std::string_view ModuleBuilder::textOf( syntax::Token token ) const
{
  return mSources->textOf( token.span() );
}

void ModuleBuilder::report( diag::Diagnostic value ) const
{
  mSink->add( std::move( value ) );
}

SectionIndex ModuleBuilder::currentSection( diag::SourceSpan /*span*/ )
{
  // A Section opened in a Proc stands beside it, and what follows goes there
  // until it closes — see docs/decisions/0085-a-section-may-stand-in-a-proc.md.
  if ( mOpenSection.has_value() )
  {
    return *mOpenSection;
  }
  if ( mOpenProcSection.has_value() )
  {
    return *mOpenProcSection;
  }
  // The grammar has refused every statement that stands in no Section, so
  // nothing reaches this line.
  assert( false && "a statement outside every section reached the builder" );
  return SectionIndex{};
}

std::string_view ModuleBuilder::currentScope() const
{
  return mScopes.empty() ? std::string_view{} : mScopes.back();
}

Symbol const* ModuleBuilder::findScoped( std::string_view scope, std::string_view name ) const
{
  // From the innermost Namespace outwards to the top level: the scope, each
  // prefix of it that ends in a dot, then nothing.
  std::string_view rest = scope;
  while ( true )
  {
    std::string candidate{ rest };
    candidate += name;
    if ( Symbol const* const found = mModule->symbols().find( candidate ); found != nullptr )
    {
      return found;
    }
    if ( rest.empty() )
    {
      return nullptr;
    }
    rest.remove_suffix( 1 );
    std::size_t const dot = rest.rfind( '.' );
    rest = dot == std::string_view::npos ? std::string_view{} : rest.substr( 0, dot + 1 );
  }
}

void ModuleBuilder::beginNamespace( std::vector<syntax::Token> path, diag::SourceSpan /*span*/ )
{
  for ( syntax::Token const& segment : path )
  {
    std::string_view const text = textOf( segment );
    if ( mScopes.empty() && text == "nga" )
    {
      report( diag::diagnostic( diag::DiagnosticId::NAMESPACE_RESERVED ).at( segment.location, segment.length ) );
    }
    std::string prefix{ currentScope() };
    prefix += text;
    prefix += '.';
    mScopes.push_back( mModule->intern( std::move( prefix ) ) );
  }
  mScopeDepths.push_back( path.size() );
}

void ModuleBuilder::endNamespace( diag::SourceSpan /*span*/ )
{
  // The grammar has refused an end with nothing open.
  if ( mScopeDepths.empty() )
  {
    return;
  }
  mScopes.resize( mScopes.size() - mScopeDepths.back() );
  mScopeDepths.pop_back();
}

Conditional& ModuleBuilder::openConditionalAt( OpenConditional const& open )
{
  Section& holder = open.macro.has_value() ? mModule->macroAt( *open.macro ).body : mModule->sectionAt( open.section );
  return holder.conditionalAt( open.index );
}

void ModuleBuilder::closeBranch()
{
  OpenConditional const& open = mOpenConditionals.back();
  Section const& holder =
      open.macro.has_value() ? mModule->macroAt( *open.macro ).body : mModule->sectionAt( open.section );
  ChunkIndex const end = holder.nextChunkIndex();
  openConditionalAt( open ).branches.back().last = end;
}

void ModuleBuilder::beginConditional( syntax::ExpressionPtr condition, diag::SourceSpan span )
{
  collectReferences( condition.get() );
  markParameters( condition.get() );

  // The Section is fixed for the whole conditional: the grammar refuses every
  // directive that would open or close one inside a branch.
  Section& holder = currentTarget( span );
  OpenConditional open{ .section = mOpenMacro.has_value() ? SectionIndex{} : currentSection( span ),
                        .macro = mOpenMacro,
                        .index = holder.openConditional(),
                        .branch = 0,
                        .parent = currentBranch() };
  mBranchParents.push_back( open.parent );
  open.branch = static_cast<std::uint32_t>( mBranchParents.size() - 1 );

  holder.conditionalAt( open.index )
      .branches.push_back( Branch{ .condition = std::move( condition ),
                                   .pattern = {},
                                   .first = holder.nextChunkIndex(),
                                   .last = holder.nextChunkIndex(),
                                   .span = span } );
  mOpenConditionals.push_back( open );
}

void ModuleBuilder::nextBranch( syntax::ExpressionPtr condition, diag::SourceSpan span )
{
  // The grammar has refused a branch with no conditional open.
  if ( mOpenConditionals.empty() )
  {
    return;
  }
  collectReferences( condition.get() );
  markParameters( condition.get() );
  closeBranch();

  OpenConditional& open = mOpenConditionals.back();
  Section& holder = open.macro.has_value() ? mModule->macroAt( *open.macro ).body : mModule->sectionAt( open.section );
  mBranchParents.push_back( open.parent );
  open.branch = static_cast<std::uint32_t>( mBranchParents.size() - 1 );

  holder.conditionalAt( open.index )
      .branches.push_back( Branch{ .condition = std::move( condition ),
                                   .pattern = {},
                                   .first = holder.nextChunkIndex(),
                                   .last = holder.nextChunkIndex(),
                                   .span = span } );
}

void ModuleBuilder::endConditional( diag::SourceSpan /*span*/ )
{
  if ( mOpenConditionals.empty() )
  {
    return;
  }
  closeBranch();
  mOpenConditionals.pop_back();
}

void ModuleBuilder::beginMatch( syntax::Token subject, diag::SourceSpan span )
{
  // The grammar has refused a `.match` outside a body.
  if ( !mOpenMacro.has_value() )
  {
    return;
  }
  Section& holder = currentTarget( span );
  OpenConditional const open{ .section = SectionIndex{},
                              .macro = mOpenMacro,
                              .index = holder.openConditional(),
                              .branch = currentBranch(),
                              .parent = currentBranch() };
  Conditional& conditional = holder.conditionalAt( open.index );
  conditional.subject = subject;

  // The subject is a pack the body binds here: the macro's own, or one an
  // enclosing `.case` bound. Anything else has no elements to count.
  std::string_view const text = textOf( subject );
  std::optional<BoundName> const bound = boundName( text );
  if ( !bound.has_value() || !bound->pack )
  {
    report( diag::diagnostic( diag::DiagnosticId::MATCH_NOT_A_PACK )
                .at( subject.location, subject.length )
                .arg( "name", std::string{ text } ) );
  }
  else
  {
    conditional.subjectCase = bound->caseName;
  }
  mOpenConditionals.push_back( open );
}

void ModuleBuilder::beginCase( syntax::Pattern pattern, diag::SourceSpan span )
{
  // The grammar has refused a `.case` outside a `.match`.
  if ( mOpenConditionals.empty() || !mOpenMacro.has_value() )
  {
    return;
  }
  OpenConditional& open = mOpenConditionals.back();
  if ( !open.macro.has_value() )
  {
    return;
  }
  Section& holder = mModule->macroAt( *open.macro ).body;
  if ( !holder.conditionalAt( open.index ).branches.empty() )
  {
    closeBranch();
  }

  for ( std::size_t later = 0; later < pattern.names.size(); ++later )
  {
    syntax::Token const& name = pattern.names[later];
    std::string_view const text = textOf( name );
    bool repeated = false;
    for ( std::size_t earlier = 0; earlier < later; ++earlier )
    {
      repeated = repeated || textOf( pattern.names[earlier] ) == text;
    }
    if ( repeated )
    {
      report( diag::diagnostic( diag::DiagnosticId::MACRO_PARAMETER_REPEATED )
                  .at( name.location, name.length )
                  .arg( "name", std::string{ text } ) );
      continue;
    }
    // A name a case binds may not be one already bound around it: inside
    // the case it would mean one thing and just outside another, and a body
    // is small enough that hiding buys nothing.
    if ( boundName( text ).has_value() )
    {
      report( diag::diagnostic( diag::DiagnosticId::CASE_NAME_SHADOWS )
                  .at( name.location, name.length )
                  .arg( "name", std::string{ text } ) );
      continue;
    }
    // Nor a label of the body, in either order of writing.
    auto const isLabel = [&]( LocalDefinition const& definition )
    { return definition.proc == *mOpenScope && definition.name == text; };
    if ( std::ranges::any_of( mLocalDefinitions, isLabel ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::LABEL_IS_A_PARAMETER )
                  .at( name.location, name.length )
                  .arg( "name", std::string{ text } ) );
    }
  }

  mBranchParents.push_back( open.parent );
  open.branch = static_cast<std::uint32_t>( mBranchParents.size() - 1 );
  holder.conditionalAt( open.index )
      .branches.push_back( Branch{ .condition = nullptr,
                                   .pattern = std::move( pattern ),
                                   .first = holder.nextChunkIndex(),
                                   .last = holder.nextChunkIndex(),
                                   .span = span } );
}

void ModuleBuilder::endMatch( diag::SourceSpan /*span*/ )
{
  if ( mOpenConditionals.empty() )
  {
    return;
  }
  OpenConditional const& open = mOpenConditionals.back();
  Conditional const& conditional = openConditionalAt( open );
  if ( !conditional.branches.empty() )
  {
    closeBranch();
  }

  // A case an earlier one always fits before is never taken: the lengths it
  // fits are a subset of what stands above it. Structural, so said here and
  // not once per use.
  for ( std::size_t later = 1; later < conditional.branches.size(); ++later )
  {
    syntax::Pattern const& pattern = conditional.branches[later].pattern;
    bool covered = false;
    for ( std::size_t earlier = 0; earlier < later && !covered; ++earlier )
    {
      syntax::Pattern const& before = conditional.branches[earlier].pattern;
      covered = before.pack ? before.fixed() <= pattern.fixed() : !pattern.pack && before.fixed() == pattern.fixed();
    }
    if ( covered )
    {
      diag::SourceSpan const span = conditional.branches[later].span;
      report( diag::diagnostic( diag::DiagnosticId::CASE_NEVER_FITS ).at( span.begin, span.length ) );
    }
  }
  mOpenConditionals.pop_back();
}

std::optional<ModuleBuilder::BoundName> ModuleBuilder::boundName( std::string_view name ) const
{
  if ( !mOpenMacro.has_value() )
  {
    return std::nullopt;
  }
  MacroDefinition const& macro = mModule->macroAt( *mOpenMacro );

  // The open `.case` of each enclosing `.match`, innermost first. A case's
  // names may not repeat a parameter's or an enclosing case's, so the order
  // decides nothing for a program that assembles; it decides what a finding
  // about one that does not says.
  for ( std::size_t depth = mOpenConditionals.size(); depth-- > 0; )
  {
    OpenConditional const& open = mOpenConditionals[depth];
    Conditional const& conditional = macro.body.conditionals()[open.index];
    if ( !conditional.isMatch() || conditional.branches.empty() )
    {
      continue;
    }
    syntax::Pattern const& pattern = conditional.branches.back().pattern;
    for ( std::uint32_t slot = 0; slot < pattern.names.size(); ++slot )
    {
      if ( textOf( pattern.names[slot] ) == name )
      {
        return BoundName{
          .parameter = std::nullopt,
          .caseName = CaseBinding{ .conditional = open.index, .branch = conditional.branches.size() - 1, .slot = slot },
          .pack = pattern.pack && slot + 1 == pattern.names.size()
        };
      }
    }
  }
  for ( std::uint32_t index = 0; index < macro.parameters.names.size(); ++index )
  {
    if ( textOf( macro.parameters.names[index] ) == name )
    {
      return BoundName{ .parameter = index,
                        .caseName = std::nullopt,
                        .pack = macro.parameters.pack && index + 1 == macro.parameters.names.size() };
    }
  }
  return std::nullopt;
}

bool ModuleBuilder::boundByAnyCase( std::string_view name ) const
{
  if ( !mOpenMacro.has_value() )
  {
    return false;
  }
  for ( Conditional const& conditional : mModule->macroAt( *mOpenMacro ).body.conditionals() )
  {
    if ( !conditional.isMatch() )
    {
      continue;
    }
    for ( Branch const& branch : conditional.branches )
    {
      for ( syntax::Token const& bound : branch.pattern.names )
      {
        if ( textOf( bound ) == name )
        {
          return true;
        }
      }
    }
  }
  return false;
}

bool ModuleBuilder::branchReaches( std::uint32_t definition, std::uint32_t reference ) const
{
  for ( std::uint32_t at = reference; at != 0; at = mBranchParents[at] )
  {
    if ( at == definition )
    {
      return true;
    }
  }
  return definition == 0;
}

bool ModuleBuilder::refusedInTemporary( diag::SourceSpan span )
{
  // A Temporary holds reservations only: its bytes are another's while it
  // is not active, so nothing written into them at load would survive. A
  // macro body is no Section, and what it expands into is checked where it
  // lands.
  if ( mOpenMacro.has_value() || !mOpenSection.has_value() || !mModule->sectionAt( *mOpenSection ).isTemporary() )
  {
    return false;
  }
  report( diag::diagnostic( diag::DiagnosticId::TEMPORARY_HOLDS_BYTES ).at( span.begin, span.length ) );
  return true;
}

Section& ModuleBuilder::currentTarget( diag::SourceSpan span )
{
  if ( mOpenMacro.has_value() )
  {
    return mModule->macroAt( *mOpenMacro ).body;
  }
  return mModule->sectionAt( currentSection( span ) );
}

void ModuleBuilder::defineSymbol( syntax::Token name, SymbolKind kind, SymbolValue value )
{
  std::string_view text = textOf( name );
  if ( text == "nga" )
  {
    // The tool's namespace: `nga.read` is the driver's macro for that role,
    // and a Module's `nga` would stand where the namespace does.
    report( diag::diagnostic( diag::DiagnosticId::NAMESPACE_RESERVED ).at( name.location, name.length ) );
    return;
  }
  // A Proc's name stands for its Section on the left of a dot, so the three
  // attributes have to keep meaning what they mean: `worker.runtimeSectionSize` is
  // its size, and a Symbol of that name inside it would win, since a dotted
  // name that is a Symbol is read before an attribute of what stands left.
  if ( mProcScopes.contains( currentScope() ) && isAttributeName( text ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::NAME_IS_AN_ATTRIBUTE )
                .at( name.location, name.length )
                .arg( "name", std::string{ text } ) );
    return;
  }
  // Inside a Namespace the Symbol's name is the qualified one, which is what
  // Merge and every lookup see; the definition's span stays the token's.
  if ( std::string_view const scope = currentScope(); !scope.empty() )
  {
    text = mModule->intern( std::string{ scope } + std::string{ text } );
  }
  Symbol symbol{
    .name = text, .kind = kind, .definition = name.span(), .exported = false, .value = std::move( value )
  };

  Symbol const* const existing = mModule->symbols().add( std::move( symbol ) );
  if ( existing == nullptr )
  {
    return;
  }

  report( diag::diagnostic( diag::DiagnosticId::SYMBOL_REDEFINED )
              .at( name.location, name.length )
              .arg( "symbol", std::string{ text } )
              .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_DEFINITION )
                         .at( existing->definition.begin, existing->definition.length )
                         .arg( "symbol", std::string{ text } ) ) );
}

void ModuleBuilder::beginSection( syntax::SectionAttributes attributes, diag::SourceSpan span )
{
  collectReferences( attributes.pinnedAddress.get() );
  collectReferences( attributes.alignment.get() );
  collectReferences( attributes.boundary.get() );

  SectionIndex const index =
      mModule->addSection( Section{ attributes.placement,
                                    std::move( attributes.pinnedAddress ),
                                    std::move( attributes.alignment ),
                                    std::move( attributes.boundary ),
                                    attributes.movable,
                                    attributes.root,
                                    attributes.temporary ? SectionKind::TEMPORARY : SectionKind::PLAIN,
                                    span } );
  mModule->sectionAt( index ).setScope( currentScope() );
  if ( attributes.pane.has_value() )
  {
    mModule->sectionAt( index ).setPaneName( *attributes.pane );
  }
  mOpenSection = index;
  mOpenSectionLabelled = false;
  mOpenSectionSpan = span;

  // Its local labels are its own: code does not branch into data, nor out.
  if ( mOpenScope.has_value() )
  {
    mProcLocalScope = mOpenScope;
    mOpenScope = mScopeCount++;
  }
}

void ModuleBuilder::endSection( diag::SourceSpan /*span*/ )
{
  // A Section is named by its first Label, and one with none is a Section
  // nothing can name: not source, not the Project, not the memory map.
  if ( mOpenSection.has_value() && !mOpenSectionLabelled )
  {
    report( diag::diagnostic( diag::DiagnosticId::SECTION_WITHOUT_LABEL )
                .at( mOpenSectionSpan.begin, mOpenSectionSpan.length ) );
  }
  mOpenSection.reset();
  if ( mProcLocalScope.has_value() )
  {
    mOpenScope = mProcLocalScope;
    mProcLocalScope.reset();
  }
}

void ModuleBuilder::beginProc( syntax::Token name, syntax::SectionAttributes attributes, diag::SourceSpan span )
{
  mOpenScope = mScopeCount;
  ++mScopeCount;

  // A Proc's name is a Label: it names the position its code begins at, which
  // is what makes it referenceable and a candidate Root.
  std::string_view const text = textOf( name );
  if ( isMnemonic( text ) )
  {
    // A legal construct warned about rather than a name reserved: this is what
    // a lost indent looks like, and it is suppressible like any other warning.
    report( diag::diagnostic( diag::DiagnosticId::LABEL_NAMED_LIKE_MNEMONIC )
                .at( name.location, name.length )
                .arg( "name", std::string{ text } ) );
  }

  collectReferences( attributes.pinnedAddress.get() );
  collectReferences( attributes.alignment.get() );
  collectReferences( attributes.boundary.get() );

  // A Section of its own, so that Prune drops it when nothing calls it and
  // Trace sees it as one node, named by the Label at its start as every
  // Section is named by its first.
  SectionIndex const section = mModule->addSection( Section{ attributes.placement,
                                                             std::move( attributes.pinnedAddress ),
                                                             std::move( attributes.alignment ),
                                                             std::move( attributes.boundary ),
                                                             attributes.movable,
                                                             attributes.root,
                                                             SectionKind::PROC,
                                                             span } );
  mModule->sectionAt( section ).setScope( currentScope() );
  if ( attributes.pane.has_value() )
  {
    mModule->sectionAt( section ).setPaneName( *attributes.pane );
  }
  if ( attributes.under.has_value() )
  {
    mModule->sectionAt( section ).setUnderName( *attributes.under );
  }
  if ( attributes.as.has_value() )
  {
    mModule->sectionAt( section ).setAsName( *attributes.as );
    mModule->addMember( Member{ .section = section,
                                .type = textOf( *attributes.as ),
                                .span = attributes.as->span(),
                                .scope = currentScope() } );
  }
  mOpenProcSection = section;
  defineSymbol(
      name, SymbolKind::LABEL, LabelPosition{ .section = section, .chunk = ChunkIndex{ 0 }, .inner = std::nullopt } );

  // The Label stands in the enclosing scope and the Proc opens one of its own
  // beneath it, so `worker` and `worker.inner` are one name and its scope —
  // see docs/decisions/0046-a-proc-is-a-scope.md.
  std::string prefix{ currentScope() };
  prefix += text;
  prefix += '.';
  std::string_view const scope = mModule->intern( std::move( prefix ) );
  mScopes.push_back( scope );
  mProcScopes.insert( scope );
}

void ModuleBuilder::endProc( std::optional<syntax::Token> then, diag::SourceSpan /*span*/ )
{
  // Before `then` is recorded: the Proc it names is a sibling of this one and
  // is read in the scope this one stood in, not inside it.
  if ( mOpenProcSection.has_value() && !mScopes.empty() )
  {
    mScopes.pop_back();
  }
  if ( then.has_value() && mOpenProcSection.has_value() )
  {
    mModule->sectionAt( *mOpenProcSection ).setThen( *then, currentScope() );
    mThens.push_back( *mOpenProcSection );
  }
  mOpenScope.reset();
  mOpenProcSection.reset();
}

void ModuleBuilder::resolveThens()
{
  auto const nameOf = [this]( SectionIndex index ) { return mModule->displayNameOf( index, *mSources ); };
  std::vector<bool> followed( mModule->sections().size(), false );
  for ( SectionIndex const from : mThens )
  {
    syntax::Token const name = mModule->sectionAt( from ).then().value_or( syntax::Token{} );
    std::string_view const text = textOf( name );
    auto const refuse = [&]( diag::DiagnosticId id )
    {
      report( diag::diagnostic( id )
                  .at( name.location, name.length )
                  .arg( "name", std::string{ text } )
                  .arg( "previous", nameOf( from ) ) );
    };

    // The name is a Proc of this Module: the Label at the start of one,
    // looked for in the Namespace the `.endp` stood in.
    Symbol const* const symbol = findScoped( mModule->sectionAt( from ).thenScope(), text );
    auto const* const label = symbol != nullptr ? std::get_if<LabelPosition>( &symbol->value ) : nullptr;
    if ( label == nullptr || label->chunk != ChunkIndex{ 0 } || !mModule->sectionAt( label->section ).isProc() )
    {
      refuse( diag::DiagnosticId::THEN_NOT_A_PROC );
      continue;
    }
    SectionIndex const to = label->section;
    Section const& target = mModule->sectionAt( to );

    // One predecessor, since a Section stands immediately after one thing;
    // no pin and no alignment, since adjacency decides its start; and the
    // same movability, since the two move together or not at all.
    if ( to == from )
    {
      refuse( diag::DiagnosticId::THEN_CYCLE );
      continue;
    }
    if ( followed[to.value] )
    {
      refuse( diag::DiagnosticId::THEN_ALREADY_FOLLOWED );
      continue;
    }
    if ( target.pinnedAddress() != nullptr || target.alignment() != nullptr )
    {
      refuse( diag::DiagnosticId::THEN_PLACED );
      continue;
    }
    if ( target.isMovable() != mModule->sectionAt( from ).isMovable() )
    {
      refuse( diag::DiagnosticId::THEN_MOVABILITY );
      continue;
    }
    // And in one Pane or in none: the bytes after one state's are not the
    // bytes of another, whatever the offsets say. By name, since the names
    // are what the Module wrote and a name that is no Pane is refused later.
    std::optional<syntax::Token> const& paneOfFrom = mModule->sectionAt( from ).paneName();
    std::optional<syntax::Token> const& paneOfTo = target.paneName();
    if ( paneOfFrom.has_value() != paneOfTo.has_value() ||
         ( paneOfFrom.has_value() && textOf( *paneOfFrom ) != textOf( *paneOfTo ) ) )
    {
      refuse( diag::DiagnosticId::THEN_PANE );
      continue;
    }
    followed[to.value] = true;
    mModule->sectionAt( from ).setNext( to );
  }

  // A chain that comes back to where it started has no first Section, and
  // the solver would be asked for a start that follows itself. Reported once
  // per cycle, at its lowest Section.
  for ( SectionIndex const start : mThens )
  {
    std::optional<SectionIndex> at = mModule->sectionAt( start ).next();
    bool lowest = true;
    while ( at.has_value() && *at != start )
    {
      lowest = lowest && at->value > start.value;
      at = mModule->sectionAt( *at ).next();
    }
    if ( at.has_value() && lowest )
    {
      syntax::Token const name = mModule->sectionAt( start ).then().value_or( syntax::Token{} );
      report( diag::diagnostic( diag::DiagnosticId::THEN_CYCLE )
                  .at( name.location, name.length )
                  .arg( "name", std::string{ textOf( name ) } )
                  .arg( "previous", nameOf( start ) ) );
    }
  }
}

void ModuleBuilder::defineLocalLabel( syntax::Token name )
{
  defineLocalNamed( name, bareLocalName( textOf( name ) ), false );
}

void ModuleBuilder::defineLocalNamed( syntax::Token name, std::string_view bare, bool plain )
{
  // The grammar has already refused a local label outside a Proc or a macro
  // body.
  if ( !mOpenScope.has_value() )
  {
    return;
  }
  std::uint32_t const scope = *mOpenScope;

  if ( mOpenMacro.has_value() && !bare.empty() )
  {
    // A plain name is unique in its body and an `@` one may repeat, so the
    // two cannot share a name: a reference would not say which it meant.
    auto const clashes = [&]( LocalDefinition const& definition )
    { return definition.proc == scope && definition.name == bare && ( definition.plain || plain ); };
    if ( auto const earlier = std::ranges::find_if( mLocalDefinitions, clashes ); earlier != mLocalDefinitions.end() )
    {
      report( diag::diagnostic( diag::DiagnosticId::LOCAL_LABEL_REDEFINED )
                  .at( name.location, name.length )
                  .arg( "name", std::string{ bare } )
                  .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_DEFINITION )
                             .at( earlier->position, 0 )
                             .arg( "symbol", std::string{ bare } ) ) );
      return;
    }

    // A parameter stands for what the use wrote, so a label of that name
    // would mean one thing where it is defined and another where it is read;
    // what a `.case` binds is a parameter of its block.
    bool parameter = boundByAnyCase( bare );
    for ( syntax::Token const& declared : mModule->macroAt( *mOpenMacro ).parameters.names )
    {
      parameter = parameter || textOf( declared ) == bare;
    }
    if ( parameter )
    {
      report( diag::diagnostic( diag::DiagnosticId::LABEL_IS_A_PARAMETER )
                  .at( name.location, name.length )
                  .arg( "name", std::string{ bare } ) );
      return;
    }
  }

  // In a macro body the position is the template's, and expansion turns it
  // into a position inside the use's Chunk — see MACRO_BODY_SECTION.
  LabelPosition target{ .section = MACRO_BODY_SECTION,
                        .chunk = currentTarget( name.span() ).nextChunkIndex(),
                        .inner = std::nullopt };
  if ( !mOpenMacro.has_value() )
  {
    target.section = currentSection( name.span() );
  }
  mLocalDefinitions.push_back( LocalDefinition{ .name = bare,
                                                .proc = scope,
                                                .branch = currentBranch(),
                                                .plain = plain,
                                                .position = name.location,
                                                .target = target } );
  mLabelledAt = std::pair{ target.section, target.chunk };
}

void ModuleBuilder::defineLabel( syntax::Token name, diag::SourceSpan span )
{
  if ( isLocal( name ) )
  {
    defineLocalLabel( name );
    return;
  }

  std::string_view const text = textOf( name );
  if ( isMnemonic( text ) )
  {
    // A legal construct warned about rather than a name reserved: this is what
    // a lost indent looks like, and it is suppressible like any other warning.
    report( diag::diagnostic( diag::DiagnosticId::LABEL_NAMED_LIKE_MNEMONIC )
                .at( name.location, name.length )
                .arg( "name", std::string{ text } ) );
  }

  // A macro body is a scope with no name to hang a Symbol on: expanded five
  // times it would define one name five times. A plain label there is a label
  // of the body, as `@name` has always been — see
  // docs/decisions/0046-a-proc-is-a-scope.md.
  if ( mOpenMacro.has_value() )
  {
    defineLocalNamed( name, text, true );
    return;
  }

  SectionIndex const section = currentSection( span );
  if ( mOpenSection.has_value() )
  {
    mOpenSectionLabelled = true;
  }
  ChunkIndex const chunk = mModule->sectionAt( section ).nextChunkIndex();
  defineSymbol( name, SymbolKind::LABEL, LabelPosition{ .section = section, .chunk = chunk, .inner = std::nullopt } );
  mLabelledAt = std::pair{ section, chunk };
}

void ModuleBuilder::defineConstant( syntax::Token name, syntax::ExpressionPtr value, diag::SourceSpan /*span*/ )
{
  collectReferences( value.get() );
  defineSymbol( name, SymbolKind::CONSTANT, std::move( value ) );
}

void ModuleBuilder::emitInstruction( syntax::Token mnemonic,
                                     syntax::OperandShape shape,
                                     syntax::ExpressionPtr operand,
                                     diag::SourceSpan span )
{
  collectReferences( operand.get() );
  markParameters( operand.get() );

  std::vector<syntax::ExpressionPtr> items;
  if ( operand != nullptr )
  {
    items.push_back( std::move( operand ) );
  }

  if ( refusedInTemporary( span ) )
  {
    mPendingWiths.clear();
    return;
  }
  enterWiths( span );
  Section& holder = currentTarget( span );
  applyTaking(
      holder,
      holder.appendChunk( InstructionContent{ .mnemonic = mnemonic, .shape = shape }, span, std::move( items ) ) );
  leaveWiths( span );
}

void ModuleBuilder::with( WithForm form,
                          syntax::ExpressionPtr what,
                          std::optional<syntax::Token> state,
                          diag::SourceSpan span )
{
  collectReferences( what.get() );
  // In a body, what is named may be a parameter, which each use then says.
  markParameters( what.get() );
  // The directive's own token stands as the use's name, since the Chunks it
  // becomes are uses of the driver's macros and a use has a name to report
  // at; the token is the `.with` itself, which is where a finding points.
  syntax::Token directive{ .kind = syntax::TokenKind::DOT,
                           .startsLine = false,
                           .direction = syntax::Direction::NONE,
                           .location = span.begin,
                           .length = 5 };
  mPendingWiths.push_back(
      PendingWith{ .form = form, .directive = directive, .what = std::move( what ), .state = state, .span = span } );
}

void ModuleBuilder::dropWiths()
{
  mPendingWiths.clear();
}

void ModuleBuilder::taking( Taking kind, std::vector<syntax::Token> followers, diag::SourceSpan /*span*/ )
{
  mPendingTaking = kind;
  mPendingFollowers = std::move( followers );
}

void ModuleBuilder::dropTaking()
{
  mPendingTaking.reset();
  mPendingFollowers.clear();
}

void ModuleBuilder::declare( Declaring what,
                             std::optional<DeclaredType> type,
                             std::string place,
                             diag::SourceSpan span )
{
  PendingDeclaration pending{ .what = what, .type = type, .span = span, .place = std::move( place ) };

  // Bytes that are all in registers wait for no reservation: they are the
  // Signature's at once, in the order the declarations stand in — see
  // docs/decisions/0145-an-argument-in-a-register.md.
  if ( !pending.place.empty() && !pending.place.contains( 'm' ) )
  {
    bool argued = false;
    applyDeclaration( pending, std::nullopt, argued );
    return;
  }
  mPendingDeclarations.push_back( std::move( pending ) );
}

void ModuleBuilder::applyDeclaration( PendingDeclaration const& pending,
                                      std::optional<SectionIndex> temporary,
                                      bool& argued )
{
  if ( !mOpenProcSection.has_value() )
  {
    return;
  }
  Section& proc = mModule->sectionAt( *mOpenProcSection );
  Declared const declared{ .temporary = temporary, .type = pending.type, .span = pending.span, .place = pending.place };
  if ( pending.what == Declaring::ARGUMENT && argued )
  {
    report(
        diag::diagnostic( diag::DiagnosticId::DECLARE_TWICE_ARGUMENT ).at( pending.span.begin, pending.span.length ) );
    return;
  }
  if ( pending.what == Declaring::ARGUMENT )
  {
    // A register carries one argument: the result may be in one an argument
    // came in, since the caller has read nothing of it yet.
    for ( Declared const& other : proc.signature().arguments )
    {
      for ( char const letter : pending.place )
      {
        if ( letter != 'm' && other.place.contains( letter ) )
        {
          report( diag::diagnostic( diag::DiagnosticId::DECLARE_REGISTER_TWICE )
                      .at( pending.span.begin, pending.span.length )
                      .arg( "register", std::string( 1, letter ) ) );
          return;
        }
      }
    }
    argued = true;
    proc.declareArgument( declared );
    return;
  }
  if ( proc.signature().result.has_value() )
  {
    report(
        diag::diagnostic( diag::DiagnosticId::DECLARE_SECOND_RESULT ).at( pending.span.begin, pending.span.length ) );
    return;
  }
  proc.declareResult( declared );
}

void ModuleBuilder::dropDeclaration()
{
  mPendingDeclarations.clear();
}

void ModuleBuilder::applyTaking( Section& holder, ChunkIndex chunk )
{
  if ( mPendingTaking.has_value() )
  {
    holder.markTaking( chunk, *mPendingTaking, std::move( mPendingFollowers ) );
    mPendingTaking.reset();
    mPendingFollowers.clear();
  }
}

void ModuleBuilder::enterWiths( diag::SourceSpan /*span*/ )
{
  for ( PendingWith& pending : mPendingWiths )
  {
    // What was named is the entry's one item, so that Expand can type it
    // and the exit needs none: it shows the base of the Window the entry
    // resolved to, which it learns from the entry.
    std::vector<syntax::ExpressionPtr> items;
    items.push_back( std::move( pending.what ) );
    currentTarget( pending.span )
        .appendChunk( MacroUseContent{ .path = {},
                                       .name = pending.directive,
                                       .form = pending.form,
                                       .side = WithSide::ENTER,
                                       .state = pending.state,
                                       .window = std::nullopt,
                                       .pane = std::nullopt,
                                       .shownState = std::nullopt },
                      pending.span,
                      std::move( items ) );
  }
}

void ModuleBuilder::leaveWiths( diag::SourceSpan /*span*/ )
{
  for ( PendingWith const& pending : mPendingWiths | std::views::reverse )
  {
    currentTarget( pending.span )
        .appendChunk( MacroUseContent{ .path = {},
                                       .name = pending.directive,
                                       .form = pending.form,
                                       .side = WithSide::LEAVE,
                                       .state = pending.state,
                                       .window = std::nullopt,
                                       .pane = std::nullopt,
                                       .shownState = std::nullopt },
                      pending.span,
                      {} );
  }
  mPendingWiths.clear();
}

void ModuleBuilder::useMacro( std::vector<syntax::Token> path,
                              syntax::Token name,
                              std::vector<syntax::ExpressionPtr> arguments,
                              diag::SourceSpan span )
{
  for ( syntax::ExpressionPtr const& argument : arguments )
  {
    collectReferences( argument.get() );
    markParameters( argument.get() );
  }

  // The name is kept as written; which macro it is, and whether one exists at
  // all, is settled once every Module is in hand — see Expand.hpp.
  enterWiths( span );
  if ( refusedInTemporary( span ) )
  {
    mPendingWiths.clear();
    return;
  }
  currentTarget( span ).appendChunk( MacroUseContent{ .path = std::move( path ),
                                                      .name = name,
                                                      .form = WithForm::NONE,
                                                      .side = WithSide::NONE,
                                                      .state = std::nullopt,
                                                      .window = std::nullopt,
                                                      .pane = std::nullopt,
                                                      .shownState = std::nullopt },
                                     span,
                                     std::move( arguments ) );
  leaveWiths( span );
}

void ModuleBuilder::beginMacro( syntax::Token name, syntax::Pattern parameters, diag::SourceSpan span )
{
  std::string_view const text = textOf( name );
  if ( isMnemonic( text ) )
  {
    // Refused and not warned about: a use of the macro would be read as the
    // instruction, so the macro could never be reached.
    report( diag::diagnostic( diag::DiagnosticId::MACRO_NAMED_LIKE_MNEMONIC )
                .at( name.location, name.length )
                .arg( "name", std::string{ text } ) );
  }
  for ( std::size_t later = 1; later < parameters.names.size(); ++later )
  {
    for ( std::size_t earlier = 0; earlier < later; ++earlier )
    {
      if ( textOf( parameters.names[earlier] ) == textOf( parameters.names[later] ) )
      {
        report( diag::diagnostic( diag::DiagnosticId::MACRO_PARAMETER_REPEATED )
                    .at( parameters.names[later].location, parameters.names[later].length )
                    .arg( "name", std::string{ textOf( parameters.names[later] ) } ) );
        break;
      }
    }
  }

  // The body is a Section so that it holds Chunks and an arena as one does,
  // and a Section of no Module, so that nothing places, sizes or prunes it: a
  // template, instantiated at each use. Its local labels are scoped to it.
  mOpenScope = mScopeCount;
  ++mScopeCount;
  MacroIndex const index = mModule->addMacro( MacroDefinition{
      .name = name,
      .parameters = std::move( parameters ),
      .body = Section{ PlacementClass::ABSOLUTE, nullptr, nullptr, nullptr, false, false, SectionKind::PLAIN, span },
      .parameterUses = {},
      .caseUses = {},
      .span = span,
      .scope = currentScope() } );
  mOpenMacro = index;
  defineSymbol( name, SymbolKind::MACRO, index );
}

void ModuleBuilder::endMacro( diag::SourceSpan /*span*/ )
{
  mOpenMacro.reset();
  mOpenScope.reset();
}

void ModuleBuilder::markParameters( syntax::Expression* node )
{
  if ( node == nullptr )
  {
    return;
  }
  bool const spread = node->kind == syntax::ExpressionKind::SPREAD;
  if ( node->kind == syntax::ExpressionKind::NAME || spread )
  {
    std::string_view const text = textOf( node->token );
    std::optional<BoundName> const bound = boundName( text );
    if ( spread && ( !bound.has_value() || !bound->pack ) )
    {
      // Nothing to spread: a name that is not a pack, or no body at all.
      // Read on as an error, so that the list is not typed around it.
      report( diag::diagnostic( diag::DiagnosticId::SPREAD_NOT_A_PACK )
                  .at( node->token.location, node->token.length )
                  .arg( "name", std::string{ text } ) );
      node->kind = syntax::ExpressionKind::ERROR;
      return;
    }
    if ( !bound.has_value() || !mOpenMacro.has_value() )
    {
      return;
    }
    MacroDefinition& macro = mModule->macroAt( *mOpenMacro );
    if ( bound->parameter.has_value() )
    {
      macro.parameterUses.emplace( node, *bound->parameter );
    }
    else if ( bound->caseName.has_value() )
    {
      macro.caseUses.emplace( node, *bound->caseName );
    }
    return;
  }
  markParameters( node->left.get() );
  markParameters( node->right.get() );
}

void ModuleBuilder::checkParameters()
{
  // Not a shadow: inside the body the name would mean the argument and
  // outside it the Symbol, and a reader of the body cannot tell which was
  // meant. The same rule a Region's name is held to, see 0038. What a
  // `.case` binds is a parameter of its block and is held to it too.
  auto const check = [this]( syntax::Token const& parameter )
  {
    std::string_view const text = textOf( parameter );
    if ( mModule->symbols().find( text ) == nullptr )
    {
      return;
    }
    report( diag::diagnostic( diag::DiagnosticId::MACRO_PARAMETER_SHADOWS )
                .at( parameter.location, parameter.length )
                .arg( "name", std::string{ text } ) );
  };
  for ( MacroDefinition const& macro : mModule->macros() )
  {
    std::ranges::for_each( macro.parameters.names, check );
    for ( Conditional const& conditional : macro.body.conditionals() )
    {
      if ( !conditional.isMatch() )
      {
        continue;
      }
      for ( Branch const& branch : conditional.branches )
      {
        std::ranges::for_each( branch.pattern.names, check );
      }
    }
  }
}

void ModuleBuilder::emitData( syntax::DataWidth width, std::vector<syntax::ExpressionPtr> items, diag::SourceSpan span )
{
  for ( syntax::ExpressionPtr const& item : items )
  {
    collectReferences( item.get() );
    markParameters( item.get() );
  }

  if ( refusedInTemporary( span ) )
  {
    return;
  }
  Section& holder = currentTarget( span );
  applyTaking( holder, holder.appendChunk( DataContent{ .width = width }, span, std::move( items ) ) );
}

void ModuleBuilder::reserve( syntax::ExpressionPtr size, diag::SourceSpan span )
{
  collectReferences( size.get() );

  // A Proc holds code: a reservation inside one would be state kept in the
  // code, which a write to would make the Proc look patched at run time, and
  // which a Temporary or a `.section` is for — one opened in the Proc
  // included. Refused and not appended, so that the Proc stays what it says
  // it is.
  if ( mOpenProcSection.has_value() && !mOpenSection.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::RESERVE_IN_PROC ).at( span.begin, span.length ) );
    return;
  }

  std::vector<syntax::ExpressionPtr> items;
  items.push_back( std::move( size ) );
  mModule->sectionAt( currentSection( span ) ).appendChunk( ReserveContent{}, span, std::move( items ) );
}

void ModuleBuilder::reserveTemporary( syntax::Token name,
                                      syntax::ExpressionPtr size,
                                      PlacementClass placement,
                                      diag::SourceSpan span )
{
  collectReferences( size.get() );

  // A Section of its own, so that the solver can place it on its own and
  // Prune can drop it when nothing names it, named by the variable's Label
  // as every Section is named by its first. The open Section, if
  // any, is not touched: a Temporary declared inside one is still a Section
  // beside it, exactly as the spelling promises.
  SectionIndex const index = mModule->addSection(
      Section{ placement, nullptr, nullptr, nullptr, false, false, SectionKind::TEMPORARY, span } );
  mModule->sectionAt( index ).setScope( currentScope() );
  std::vector<syntax::ExpressionPtr> items;
  items.push_back( std::move( size ) );
  mModule->sectionAt( index ).appendChunk( ReserveContent{}, span, std::move( items ) );
  defineSymbol(
      name, SymbolKind::LABEL, LabelPosition{ .section = index, .chunk = ChunkIndex{ 0 }, .inner = std::nullopt } );

  if ( mPendingDeclarations.empty() )
  {
    return;
  }

  // The `.declare`s on the lines above say what this variable is to the Proc
  // holding it. The Proc carries the Signature, and the order the
  // declarations arrive in is the order of the arguments — see
  // docs/decisions/0081-a-procs-signature-is-declared.md. Two of them over
  // one reservation make the byte both an argument and the result, which is
  // how a Proc returns through the byte it was given — see
  // docs/decisions/0119-one-temporary-carries-two-roles.md.
  std::vector<PendingDeclaration> const pendings = std::move( mPendingDeclarations );
  mPendingDeclarations.clear();
  bool argued = false;
  for ( PendingDeclaration const& pending : pendings )
  {
    applyDeclaration( pending, index, argued );
  }
}

void ModuleBuilder::transition( syntax::Token phase, diag::SourceSpan span )
{
  // The name is kept as written; which Phase it is, and whether every Phase
  // this code is present in has the edge, is settled once the whole Project
  // and every Module are in hand.
  if ( refusedInTemporary( span ) )
  {
    return;
  }
  mModule->sectionAt( currentSection( span ) )
      .appendChunk( TransitionContent{ .name = phase, .target = std::nullopt }, span, {} );
}

void ModuleBuilder::dispatch( std::vector<syntax::ExpressionPtr> targets, diag::SourceSpan span )
{
  for ( syntax::ExpressionPtr const& target : targets )
  {
    collectReferences( target.get() );
  }

  if ( refusedInTemporary( span ) )
  {
    return;
  }
  Section& holder = currentTarget( span );

  // A row that follows another with no Label between them continues it: one
  // statement written over several lines, and so one Chunk, since the bytes
  // it stands for are one jump and one table. A Label between the rows would
  // name a position inside those bytes, so it ends the statement and the row
  // below it begins another — which the type check refuses as the dispatch
  // whose targets nothing reaches.
  ChunkIndex const next = holder.nextChunkIndex();
  bool const cut = mLabelledAt.has_value() && mLabelledAt->second == next &&
                   ( mOpenMacro.has_value() || mLabelledAt->first == currentSection( span ) );
  if ( !cut && next.value > 0 && std::holds_alternative<DispatchContent>( holder.chunks().back().content ) )
  {
    holder.extendLastChunk( std::move( targets ), span );
    return;
  }
  // No taking is applied: a dispatch takes no address — it names where it
  // goes — so the grammar has already refused a `.own` or a `.root` above
  // one, this being neither an instruction nor data.
  holder.appendChunk( DispatchContent{}, span, std::move( targets ) );
}

void ModuleBuilder::declareCharset( syntax::Token name,
                                    std::optional<CharsetBase> base,
                                    std::vector<CharsetEntry> entries,
                                    diag::SourceSpan span )
{
  // The start bytes are References like any other: a `.charset` may be written
  // in terms of a Constant, and a Constant may be defined further down.
  for ( CharsetEntry const& entry : entries )
  {
    collectReferences( entry.start.get() );
  }
  if ( base.has_value() )
  {
    collectReferences( base->mask.get() );
  }

  CharsetIndex const index = mModule->addCharset(
      CharsetDeclaration{ .name = name, .base = std::move( base ), .entries = std::move( entries ), .span = span } );
  defineSymbol( name, SymbolKind::CHARSET, index );
}

void ModuleBuilder::addAssertion( syntax::ExpressionPtr condition, diag::SourceSpan span )
{
  collectReferences( condition.get() );
  mModule->addAssertion( Assertion{ .condition = std::move( condition ), .span = span } );
}

void ModuleBuilder::declareSlot( syntax::Token name, Binding binding, PlacementClass placement, diag::SourceSpan span )
{
  // A Symbol like any other, whose Cell the end of Assemble supplies.
  (void)span;
  defineSymbol(
      name, SymbolKind::SLOT, SlotDeclaration{ .binding = binding, .placement = placement, .cell = std::nullopt } );
}

void ModuleBuilder::implement( syntax::Token slot, syntax::Token symbol, diag::SourceSpan span )
{
  // Two names and nothing resolved: the Slot may be declared in a Module this
  // one never saw, and the Symbol may be defined further down.
  mModule->addImplementation( Implementation{ .slot = textOf( slot ),
                                              .symbol = textOf( symbol ),
                                              .slotSpan = slot.span(),
                                              .span = span,
                                              .scope = currentScope() } );
}

void ModuleBuilder::silence( syntax::Token code, std::optional<diag::SourceLocation> statement, diag::SourceSpan span )
{
  std::string text{ textOf( code ) };
  std::optional<diag::DiagnosticId> const id = diag::diagnosticIdForCode( text );
  if ( !id.has_value() )
  {
    // Refused for the reason the Project's `diagnostics` block refuses one: a
    // line that quietly silenced nothing would look like it worked.
    report(
        diag::diagnostic( diag::DiagnosticId::OFF_UNKNOWN_CODE ).at( code.location, code.length ).arg( "code", text ) );
    return;
  }
  mModule->addSuppression( Suppression{ .id = *id, .code = std::move( text ), .statement = statement, .span = span } );
}

void ModuleBuilder::markSource( syntax::Token path, syntax::Token line, diag::SourceSpan span )
{
  std::optional<std::int64_t> const value = syntax::numericValueOf( textOf( line ) );
  if ( !value.has_value() || *value < 1 || std::cmp_greater( *value, std::numeric_limits<std::uint32_t>::max() ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::SOURCE_LINE_FROM_ONE )
                .at( line.location, line.length )
                .arg( "line", std::string{ textOf( line ) } ) );
    return;
  }
  mModule->addSourceMark( SourceMark{ .path = std::string{ syntax::quotedOf( textOf( path ) ).body },
                                      .line = static_cast<std::uint32_t>( *value ),
                                      .from = span.begin,
                                      .span = span } );
}

void ModuleBuilder::exportSymbol( syntax::Token name, diag::SourceSpan /*span*/ )
{
  // Applied at the end, because an export may name a Symbol the file defines
  // further down: it speaks about a name, not about what has been built.
  mExports.push_back( Export{ .name = name, .scope = currentScope() } );
}

void ModuleBuilder::applyExports()
{
  for ( Export const& entry : mExports )
  {
    std::string_view const text = textOf( entry.name );
    Symbol const* const found = findScoped( entry.scope, text );
    if ( found == nullptr )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPORT_OF_UNDEFINED_SYMBOL )
                  .at( entry.name.location, entry.name.length )
                  .arg( "symbol", std::string{ text } ) );
      continue;
    }

    // A position inside a Proc is reached from nowhere outside it, so
    // exporting one offers another Module a name it could never use. The
    // Proc's own Label, at its first Chunk, is the entry and exports as
    // anything does; a Temporary declared inside a Proc is a Section beside
    // it and exports too.
    if ( auto const* const label = std::get_if<LabelPosition>( &found->value );
         label != nullptr && label->chunk != ChunkIndex{ 0 } && mModule->sectionAt( label->section ).isProc() )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPORT_FROM_INSIDE_PROC )
                  .at( entry.name.location, entry.name.length )
                  .arg( "symbol", std::string{ found->name } )
                  .arg( "proc", mModule->displayNameOf( label->section, *mSources ) ) );
      continue;
    }
    mModule->symbols().markExported( found->name );
  }

  // A Proc's declared bytes are its interface, and a caller in another Module
  // writes and reads them: an exported Proc exports them — see
  // docs/decisions/0081-a-procs-signature-is-declared.md.
  std::map<std::uint32_t, std::string_view> temporaries;
  std::vector<SectionIndex> exportedProcs;
  for ( Symbol const& symbol : mModule->symbols().symbols() )
  {
    auto const* const label = std::get_if<LabelPosition>( &symbol.value );
    if ( symbol.kind != SymbolKind::LABEL || label == nullptr || label->chunk != ChunkIndex{ 0 } )
    {
      continue;
    }
    temporaries.try_emplace( label->section.value, symbol.name );
    if ( symbol.exported && mModule->sectionAt( label->section ).isProc() )
    {
      exportedProcs.push_back( label->section );
    }
  }
  for ( SectionIndex const proc : exportedProcs )
  {
    Signature const& signature = mModule->sectionAt( proc ).signature();
    std::vector<Declared> declared = signature.arguments;
    if ( signature.result.has_value() )
    {
      declared.push_back( *signature.result );
    }
    for ( Declared const& byte : declared )
    {
      if ( !byte.temporary.has_value() )
      {
        continue;
      }
      if ( auto const found = temporaries.find( byte.temporary->value ); found != temporaries.end() )
      {
        mModule->symbols().markExported( found->second );
      }
    }
  }
}

void ModuleBuilder::collectReferences( syntax::Expression* node, bool dotted )
{
  if ( node == nullptr )
  {
    return;
  }

  // A plain name in a macro body may be a label of the body, which is settled
  // once the body's end is known, since a definition may stand below the use.
  // Not the receiver of a qualified name, which is a Namespace or a Section.
  if ( node->kind == syntax::ExpressionKind::NAME && mOpenMacro.has_value() && mOpenScope.has_value() && !dotted &&
       !node->fromRoot )
  {
    mBodyNames.push_back(
        BodyName{ .node = node, .name = textOf( node->token ), .scope = *mOpenScope, .branch = currentBranch() } );
  }

  // A name written inside a Namespace is looked for there first, and the
  // node remembers where it stood; a literal's prefix is a name too. A
  // leading dot says the top level instead, and binding nothing is what says
  // it: an unbound node is read where a name at the top level is.
  if ( node->kind == syntax::ExpressionKind::NAME || node->kind == syntax::ExpressionKind::STRING ||
       node->kind == syntax::ExpressionKind::CHARACTER )
  {
    if ( std::string_view const scope = currentScope(); !scope.empty() && !node->fromRoot )
    {
      mModule->bindScope( node, scope );
    }
  }

  if ( node->kind == syntax::ExpressionKind::LOCAL_NAME )
  {
    if ( !mOpenScope.has_value() )
    {
      report( diag::diagnostic( diag::DiagnosticId::LOCAL_LABEL_OUTSIDE_PROC )
                  .at( node->token.location, node->token.length ) );
      return;
    }

    mLocalReferences.push_back( LocalReference{ .node = node,
                                                .name = bareLocalName( textOf( node->token ) ),
                                                .proc = *mOpenScope,
                                                .branch = currentBranch(),
                                                .position = node->token.location,
                                                .direction = node->token.direction } );
    return;
  }

  collectReferences( node->left.get(), node->kind == syntax::ExpressionKind::ATTRIBUTE );
  collectReferences( node->right.get() );
}

void ModuleBuilder::resolveLocalLabels()
{
  // A plain name of a body first: where the body defines one, the node is a
  // local label and everything downstream reads it as `@name` is read; where
  // it does not, the name is a Symbol and is left for whoever resolves one.
  for ( BodyName const& reference : mBodyNames )
  {
    auto const matches = [this, &reference]( LocalDefinition const& definition )
    {
      return definition.proc == reference.scope && definition.name == reference.name &&
             branchReaches( definition.branch, reference.branch );
    };
    auto const found = std::ranges::find_if( mLocalDefinitions, matches );
    if ( found == mLocalDefinitions.end() )
    {
      continue;
    }
    reference.node->kind = syntax::ExpressionKind::LOCAL_NAME;
    mModule->bindLocal( reference.node, found->target );
  }

  for ( LocalReference const& reference : mLocalReferences )
  {
    // Definitions are appended as they are read, so this range is in source
    // order and "nearest in a direction" is the first or the last match.
    // A definition in a branch the reference does not stand in may not be
    // there at all, so it is not a definition this reference can choose.
    auto const matches = [this, &reference]( LocalDefinition const& definition )
    {
      return definition.proc == reference.proc && definition.name == reference.name &&
             branchReaches( definition.branch, reference.branch );
    };
    // Whether a definition this reference could otherwise have chosen was
    // kept from it by standing in another branch, which is a different
    // finding from there being none.
    bool blocked = false;
    auto const named = [this, &reference, &blocked]( LocalDefinition const& definition )
    {
      if ( definition.proc != reference.proc || definition.name != reference.name )
      {
        return false;
      }
      blocked = blocked || !branchReaches( definition.branch, reference.branch );
      return true;
    };

    if ( reference.direction == syntax::Direction::NONE )
    {
      if ( reference.name.empty() )
      {
        report( diag::diagnostic( diag::DiagnosticId::ANONYMOUS_LABEL_NEEDS_DIRECTION )
                    .at( reference.position, reference.node->token.length ) );
        continue;
      }

      std::ranges::for_each( mLocalDefinitions, named );
      auto const first = std::ranges::find_if( mLocalDefinitions, matches );
      if ( first == mLocalDefinitions.end() )
      {
        report( diag::diagnostic( blocked ? diag::DiagnosticId::LOCAL_LABEL_ACROSS_BRANCHES
                                          : diag::DiagnosticId::NO_SUCH_LOCAL_LABEL )
                    .at( reference.position, reference.node->token.length )
                    .arg( "name", std::string{ reference.name } ) );
        continue;
      }

      // A bare `@name` must resolve uniquely: repeating is what directions are
      // for, and guessing between two definitions is not a service.
      auto const second = std::find_if( std::next( first ), mLocalDefinitions.end(), matches );
      if ( second != mLocalDefinitions.end() )
      {
        report( diag::diagnostic( diag::DiagnosticId::AMBIGUOUS_LOCAL_LABEL )
                    .at( reference.position, reference.node->token.length )
                    .arg( "name", std::string{ reference.name } )
                    .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_DEFINITION )
                               .at( first->position, 0 )
                               .arg( "symbol", std::string{ reference.name } ) ) );
        continue;
      }

      mModule->bindLocal( reference.node, first->target );
      continue;
    }

    bool const forward = reference.direction == syntax::Direction::FORWARD;
    LocalDefinition const* chosen = nullptr;
    for ( LocalDefinition const& definition : mLocalDefinitions )
    {
      bool const side = forward ? definition.position > reference.position : definition.position < reference.position;
      if ( !side || !named( definition ) || !matches( definition ) )
      {
        continue;
      }
      if ( forward )
      {
        chosen = &definition;
        break;
      }
      chosen = &definition;
    }

    if ( chosen == nullptr )
    {
      report( diag::diagnostic( blocked ? diag::DiagnosticId::LOCAL_LABEL_ACROSS_BRANCHES
                                        : diag::DiagnosticId::NO_SUCH_LOCAL_LABEL )
                  .at( reference.position, reference.node->token.length )
                  .arg( "name", std::string{ reference.name } ) );
      continue;
    }

    mModule->bindLocal( reference.node, chosen->target );
  }
}

void ModuleBuilder::declareTransform( syntax::Token format, syntax::Token label, diag::SourceSpan span )
{
  mModule->addTransform( TransformDeclaration{ .format = textOf( format ),
                                               .label = textOf( label ),
                                               .formatSpan = format.span(),
                                               .labelSpan = label.span(),
                                               .span = span,
                                               .scope = currentScope() } );
}

void ModuleBuilder::applyTransforms()
{
  for ( TransformDeclaration& declaration : mModule->transforms() )
  {
    Symbol const* const found = findScoped( declaration.scope, declaration.label );
    if ( found == nullptr )
    {
      report( diag::diagnostic( diag::DiagnosticId::TRANSFORM_LABEL_NOT_HERE )
                  .at( declaration.labelSpan.begin, declaration.labelSpan.length )
                  .arg( "label", std::string{ declaration.label } ) );
      continue;
    }
    if ( found->kind != SymbolKind::LABEL )
    {
      report( diag::diagnostic( diag::DiagnosticId::TRANSFORM_NOT_A_LABEL )
                  .at( declaration.labelSpan.begin, declaration.labelSpan.length )
                  .arg( "label", std::string{ declaration.label } )
                  .arg( "kind", std::string{ nameOf( found->kind ) } ) );
      continue;
    }
    declaration.label = found->name;
    mModule->symbols().markExported( found->name );
  }
}

void ModuleBuilder::declareDriverRole( syntax::Token role, std::vector<syntax::Token> names, diag::SourceSpan span )
{
  // How many names a role takes is the role's: `open` and `read` a macro,
  // `show` and `showAt` a Window and a macro, `stream` a Window. A role
  // nobody knows is refused once the Module is read, with the rest.
  std::string_view const text = textOf( role );
  bool const windowed = text == "show" || text == "showAt" || text == "stream";
  std::size_t const wanted = text == "show" || text == "showAt" ? 2 : 1;
  bool const known = windowed || text == "open" || text == "read";
  if ( known && names.size() != wanted )
  {
    report( diag::diagnostic( diag::DiagnosticId::DRIVER_ROLE_NAMES )
                .at( role.location, role.length )
                .arg( "role", std::string{ text } )
                .arg( "count", static_cast<std::int64_t>( wanted ) )
                .arg( "given", static_cast<std::int64_t>( names.size() ) ) );
    return;
  }
  syntax::Token const last = names.back();
  bool const hasMacro = text != "stream";
  mModule->addDriverRole( DriverRole{ .role = text,
                                      .window = windowed ? textOf( names.front() ) : std::string_view{},
                                      .windowSpan = windowed ? names.front().span() : role.span(),
                                      .label = hasMacro ? textOf( last ) : std::string_view{},
                                      .roleSpan = role.span(),
                                      .labelSpan = hasMacro ? last.span() : role.span(),
                                      .span = span,
                                      .scope = currentScope() } );
}

void ModuleBuilder::applyDriverRoles()
{
  // A role is declared once — per Window, for the roles that name one.
  std::vector<std::pair<std::string_view, std::string_view>> seen;
  for ( DriverRole& role : mModule->driverRoles() )
  {
    std::pair<std::string_view, std::string_view> const key{ role.role, role.window };
    if ( std::ranges::find( seen, key ) != seen.end() )
    {
      report( diag::diagnostic( diag::DiagnosticId::DRIVER_ROLE_REPEATED )
                  .at( role.roleSpan.begin, role.roleSpan.length )
                  .arg( "role", std::string{ role.role } ) );
      continue;
    }
    seen.push_back( key );

    if ( role.role != "open" && role.role != "read" && role.role != "show" && role.role != "showAt" &&
         role.role != "stream" )
    {
      report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_DRIVER_ROLE )
                  .at( role.roleSpan.begin, role.roleSpan.length )
                  .arg( "role", std::string{ role.role } ) );
      continue;
    }
    if ( role.role == "stream" )
    {
      // Whether the Window exists is the Target's question, asked at the end
      // of Assemble where the driver is resolved.
      continue;
    }
    Symbol const* const found = findScoped( role.scope, role.label );
    if ( found == nullptr )
    {
      report( diag::diagnostic( diag::DiagnosticId::DRIVER_LABEL_NOT_HERE )
                  .at( role.labelSpan.begin, role.labelSpan.length )
                  .arg( "label", std::string{ role.label } ) );
      continue;
    }
    // A role is a macro, and the tool reaches it from the declaration
    // rather than by name, so nothing here is exported: the name stays the
    // driver's own.
    if ( found->kind != SymbolKind::MACRO )
    {
      report( diag::diagnostic( diag::DiagnosticId::DRIVER_NOT_A_MACRO )
                  .at( role.labelSpan.begin, role.labelSpan.length )
                  .arg( "label", std::string{ role.label } )
                  .arg( "kind", std::string{ nameOf( found->kind ) } ) );
      continue;
    }
    role.label = found->name;
  }
}

void ModuleBuilder::finish()
{
  resolveLocalLabels();
  checkParameters();
  applyExports();
  applyTransforms();
  applyDriverRoles();
  resolveThens();
}

} // namespace nga::model
