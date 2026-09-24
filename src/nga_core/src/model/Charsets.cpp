#include "nga/model/Evaluate.hpp"
#include "nga/model/Merge.hpp"
#include "nga/syntax/Literal.hpp"

#include <string>
#include <utility>

namespace nga::model
{

namespace
{

/// Resolves every declaration of every Module, once.
///
/// Derivation is why this is a walk with a stack rather than a loop: a base may
/// stand below the set that derives from it, or in another Module entirely, so
/// a declaration is resolved when it is reached and remembered afterwards.
class Resolver
{
public:
  Resolver( diag::SourceManager const& sources, GlobalSymbols const& symbols, diag::DiagnosticSink& sink )
      : mSources( &sources ), mSymbols( &symbols ), mSink( &sink ), mResult( symbols.modules().size() )
  {
  }

  Charsets run();

private:
  /// The resolved table, resolving it first when it has not been reached yet.
  /// Null only when the declaration is part of a cycle, which has been
  /// reported: a caller then has nothing to derive from and says nothing more.
  Charset const* tableOf( CharsetRef where );

  void resolve( CharsetRef where, Charset& into );

  /// Reports every name in an expression that resolves to nothing, and says
  /// whether it reported. The declarations are the one place expressions live
  /// outside a Chunk or an Assertion, so the type check never walks them; a
  /// misspelt Constant would otherwise be reported as "not a declared value",
  /// which names the consequence rather than the mistake.
  bool reportUnknownNames( ModuleIndex home, syntax::Expression const& node );
  void applyBase( CharsetRef where, CharsetBase const& base, Charset& into );
  void applyEntry( ModuleIndex home, CharsetEntry const& entry, Charset& into, bool derived );

  [[nodiscard]] std::string_view textOf( diag::SourceSpan span ) const
  {
    return mSources->textOf( span );
  }

  void report( diag::Diagnostic value ) const
  {
    mSink->add( std::move( value ) );
  }

  diag::SourceManager const* mSources;
  GlobalSymbols const* mSymbols;
  diag::DiagnosticSink* mSink;
  Charsets mResult;

  /// Whether each declaration has been resolved, and which are on the stack.
  /// Indexed exactly as the declarations are.
  std::vector<std::vector<bool>> mDone;
  std::vector<CharsetRef> mVisiting;
};

Charsets Resolver::run()
{
  std::span<Module const> const modules = mSymbols->modules();
  mDone.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    std::size_t const count = modules[module].charsets().size();
    mDone[module].assign( count, false );
    mResult.forModule( ModuleIndex{ static_cast<std::uint32_t>( module ) } ).resize( count );
  }

  // In Project order and declaration order, because every diagnostic raised
  // below is part of the output contract.
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    for ( std::size_t charset = 0; charset < modules[module].charsets().size(); ++charset )
    {
      tableOf( CharsetRef{ .module = ModuleIndex{ static_cast<std::uint32_t>( module ) },
                           .charset = CharsetIndex{ static_cast<std::uint32_t>( charset ) } } );
    }
  }
  return std::move( mResult );
}

Charset const* Resolver::tableOf( CharsetRef where )
{
  if ( mDone[where.module.value][where.charset.value] )
  {
    return &mResult.at( where );
  }

  for ( CharsetRef const& open : mVisiting )
  {
    if ( open == where )
    {
      CharsetDeclaration const& declaration = mSymbols->moduleAt( where.module ).charsetAt( where.charset );
      report( diag::diagnostic( diag::DiagnosticId::CHARSET_CYCLE )
                  .at( declaration.name.location, declaration.name.length )
                  .arg( "charset", std::string{ textOf( declaration.name.span() ) } ) );
      mDone[where.module.value][where.charset.value] = true;
      return nullptr;
    }
  }

  mVisiting.push_back( where );
  resolve( where, mResult.forModule( where.module )[where.charset.value] );
  mVisiting.pop_back();

  mDone[where.module.value][where.charset.value] = true;
  return &mResult.at( where );
}

bool Resolver::reportUnknownNames( ModuleIndex home, syntax::Expression const& node )
{
  bool reported = false;
  if ( node.kind == syntax::ExpressionKind::NAME )
  {
    std::string_view const name = textOf( node.token.span() );
    if ( !mSymbols->lookup( home, name ).has_value() )
    {
      report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_SYMBOL )
                  .at( node.span.begin, node.span.length )
                  .arg( "symbol", std::string{ name } ) );
      reported = true;
    }
  }
  if ( node.left != nullptr )
  {
    reported = reportUnknownNames( home, *node.left ) || reported;
  }
  if ( node.right != nullptr )
  {
    reported = reportUnknownNames( home, *node.right ) || reported;
  }
  return reported;
}

void Resolver::resolve( CharsetRef where, Charset& into )
{
  CharsetDeclaration const& declaration = mSymbols->moduleAt( where.module ).charsetAt( where.charset );

  if ( declaration.base.has_value() )
  {
    applyBase( where, *declaration.base, into );
  }
  for ( CharsetEntry const& entry : declaration.entries )
  {
    applyEntry( where.module, entry, into, declaration.base.has_value() );
  }
}

void Resolver::applyBase( CharsetRef where, CharsetBase const& base, Charset& into )
{
  std::string_view const name = textOf( base.name.span() );
  std::optional<SymbolRef> const found = mSymbols->lookup( where.module, name );
  if ( !found.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_SYMBOL )
                .at( base.name.location, base.name.length )
                .arg( "symbol", std::string{ name } ) );
    return;
  }

  std::optional<CharsetRef> const from = charsetOf( *mSymbols, *found );
  if ( !from.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::NOT_A_CHARSET )
                .at( base.name.location, base.name.length )
                .arg( "symbol", std::string{ name } ) );
    return;
  }

  // The cycle check has to see this declaration, not the Symbol that named it,
  // which is why the stack holds CharsetRefs and the lookup happens first.
  Charset const* const table = tableOf( *from );
  if ( table == nullptr )
  {
    return;
  }

  std::int64_t mask = 0;
  if ( base.mask != nullptr )
  {
    if ( reportUnknownNames( where.module, *base.mask ) )
    {
      return;
    }
    std::optional<std::int64_t> const value =
        declaredValueOf( *mSources, *mSymbols, nullptr, where.module, *base.mask );
    if ( !value.has_value() )
    {
      report( diag::diagnostic( diag::DiagnosticId::CHARSET_VALUE_NOT_DECLARED )
                  .at( base.mask->span.begin, base.mask->span.length ) );
      return;
    }
    if ( *value < 0 || *value > 0xFF )
    {
      // Said with the run diagnostic because it is the same fact: a mask wider
      // than a byte would take every mapping out of one.
      report( diag::diagnostic( diag::DiagnosticId::CHARSET_RUN_PAST_BYTE )
                  .at( base.mask->span.begin, base.mask->span.length )
                  .arg( "start", std::int64_t{ 0 } )
                  .arg( "end", *value ) );
      return;
    }
    mask = *value;
  }

  // `^` and not `|`: over a base holding bytes above $7F, setting a bit would
  // collapse two code points onto one byte and say nothing. See 0013.
  for ( auto const& [codePoint, byte] : table->mappings() )
  {
    into.remap( codePoint, static_cast<std::uint8_t>( byte ^ static_cast<std::uint8_t>( mask ) ) );
  }
}

void Resolver::applyEntry( ModuleIndex home, CharsetEntry const& entry, Charset& into, bool derived )
{
  if ( containsError( *entry.start ) )
  {
    return;
  }

  if ( reportUnknownNames( home, *entry.start ) )
  {
    return;
  }

  std::optional<std::int64_t> const start = declaredValueOf( *mSources, *mSymbols, nullptr, home, *entry.start );
  if ( !start.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::CHARSET_VALUE_NOT_DECLARED )
                .at( entry.start->span.begin, entry.start->span.length ) );
    return;
  }

  std::vector<char32_t> const points =
      syntax::codePointsOf( syntax::quotedOf( textOf( entry.characters.span() ) ).body );
  std::int64_t const end = *start + static_cast<std::int64_t>( points.size() ) - 1;
  if ( *start < 0 || end > 0xFF )
  {
    report( diag::diagnostic( diag::DiagnosticId::CHARSET_RUN_PAST_BYTE )
                .at( entry.characters.location, entry.characters.length )
                .arg( "start", *start )
                .arg( "end", end ) );
    return;
  }

  std::int64_t byte = *start;
  for ( char32_t const point : points )
  {
    // A derived block's entries override the base, so only a second entry in
    // this block is a contradiction. Two code points sharing one byte is not:
    // a font with only uppercase glyphs wants exactly that.
    bool const fresh = derived ? ( into.remap( point, static_cast<std::uint8_t>( byte ) ), true )
                               : into.map( point, static_cast<std::uint8_t>( byte ) );
    if ( !fresh )
    {
      report( diag::diagnostic( diag::DiagnosticId::DUPLICATE_CHARSET_ENTRY )
                  .at( entry.characters.location, entry.characters.length )
                  .arg( "character", syntax::displayOf( point ) ) );
    }
    ++byte;
  }
}

} // namespace

std::optional<std::uint8_t> Charset::byteFor( char32_t codePoint ) const
{
  auto const found = mBytes.find( codePoint );
  return found == mBytes.end() ? std::nullopt : std::optional{ found->second };
}

bool Charset::map( char32_t codePoint, std::uint8_t byte )
{
  return mBytes.emplace( codePoint, byte ).second;
}

void Charset::remap( char32_t codePoint, std::uint8_t byte )
{
  mBytes[codePoint] = byte;
}

std::optional<CharsetRef> charsetOf( GlobalSymbols const& symbols, SymbolRef where )
{
  Symbol const& symbol = symbols.at( where );
  if ( symbol.kind != SymbolKind::CHARSET || !std::holds_alternative<CharsetIndex>( symbol.value ) )
  {
    return std::nullopt;
  }
  return CharsetRef{ .module = where.module, .charset = std::get<CharsetIndex>( symbol.value ) };
}

Charsets resolveCharsets( diag::SourceManager const& sources, GlobalSymbols const& symbols, diag::DiagnosticSink& sink )
{
  Resolver resolver{ sources, symbols, sink };
  return resolver.run();
}

} // namespace nga::model
