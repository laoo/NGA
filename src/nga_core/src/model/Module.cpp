#include "nga/model/Module.hpp"

#include <variant>

namespace nga::model
{

SectionIndex Module::addSection( Section section )
{
  SectionIndex const index{ static_cast<std::uint32_t>( mSections.size() ) };
  mSections.push_back( std::move( section ) );
  return index;
}

void Module::addImplementation( Implementation implementation )
{
  mImplementations.push_back( implementation );
}

void Module::addAssertion( Assertion assertion )
{
  mAssertions.push_back( std::move( assertion ) );
}

void Module::addSuppression( Suppression suppression )
{
  mSuppressions.push_back( std::move( suppression ) );
}

void Module::addSourceMark( SourceMark mark )
{
  mSourceMarks.push_back( std::move( mark ) );
}

void Module::addTransform( TransformDeclaration declaration )
{
  mTransforms.push_back( declaration );
}

void Module::addDriverRole( DriverRole role )
{
  mDriverRoles.push_back( role );
}

CharsetIndex Module::addCharset( CharsetDeclaration declaration )
{
  mCharsets.push_back( std::move( declaration ) );
  return CharsetIndex{ static_cast<std::uint32_t>( mCharsets.size() - 1 ) };
}

MacroIndex Module::addMacro( MacroDefinition macro )
{
  mMacros.push_back( std::move( macro ) );
  return MacroIndex{ static_cast<std::uint32_t>( mMacros.size() - 1 ) };
}

std::optional<ModuleIndex> Module::homeOf( syntax::Expression const* node ) const
{
  auto const entry = mExpressionHomes.find( node );
  if ( entry == mExpressionHomes.end() )
  {
    return std::nullopt;
  }
  return entry->second;
}

void Module::bindHome( syntax::Expression const* node, ModuleIndex home )
{
  mExpressionHomes.insert_or_assign( node, home );
}

std::string_view Module::intern( std::string text )
{
  mInterned.push_back( std::move( text ) );
  return mInterned.back();
}

std::optional<std::string_view> Module::scopeOf( syntax::Expression const* node ) const
{
  auto const entry = mExpressionScopes.find( node );
  if ( entry == mExpressionScopes.end() )
  {
    return std::nullopt;
  }
  return entry->second;
}

void Module::bindScope( syntax::Expression const* node, std::string_view scope )
{
  mExpressionScopes.insert_or_assign( node, scope );
}

std::optional<LabelPosition> Module::localTarget( syntax::Expression const* reference ) const
{
  auto const entry = mLocalTargets.find( reference );
  if ( entry == mLocalTargets.end() )
  {
    return std::nullopt;
  }
  return entry->second;
}

void Module::bindLocal( syntax::Expression const* reference, LabelPosition target )
{
  mLocalTargets.insert_or_assign( reference, target );
}

std::string Module::displayNameOf( SectionIndex index, diag::SourceManager const& /*sources*/ ) const
{
  // A Section is named by its first Label: the lowest position it holds a
  // Label at, and at one position the Label defined first — a Proc's, a
  // Temporary's, the Cell's `ngaCurrentPhase`, or whichever a `.section`
  // holds first.
  std::optional<std::string_view> name;
  std::uint32_t best = 0;
  for ( Symbol const& symbol : mSymbols.symbols() )
  {
    auto const* const label = std::get_if<LabelPosition>( &symbol.value );
    if ( label != nullptr && label->section == index && ( !name.has_value() || label->chunk.value < best ) )
    {
      name = symbol.name;
      best = label->chunk.value;
    }
  }
  return name.has_value() ? std::string{ *name } : "(anonymous section of " + mName + ")";
}

} // namespace nga::model
