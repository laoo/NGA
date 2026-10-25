#include "nga/c/Compile.hpp"

#include "nga/c/Check.hpp"
#include "nga/c/Front.hpp"
#include "nga/c/Lower.hpp"
#include "nga/c/Write.hpp"

#include "nga/c/Assigned.hpp"

#include "nga/c/Copies.hpp"
#include "nga/c/Fill.hpp"
#include "nga/c/Fold.hpp"
#include "nga/c/Hoist.hpp"
#include "nga/c/Inline.hpp"
#include "nga/c/Liveness.hpp"
#include "nga/c/Loops.hpp"
#include "nga/c/Narrow.hpp"
#include "nga/c/Promote.hpp"
#include "nga/c/Ranges.hpp"
#include "nga/c/Result.hpp"
#include "nga/c/Reuse.hpp"
#include "nga/c/Select.hpp"
#include "nga/c/Walk.hpp"
#include "nga/c/Wrap.hpp"

#include "nga/c/Lexer.hpp"
#include "nga/c/Lines.hpp"
#include "nga/c/Parser.hpp"
#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nga::c
{

using namespace detail;

namespace
{

/// The first pass: every name a file defines, a name defined twice in it, and
/// each `enum struct`, numbered among the program's.
void gatherSignatures( diag::SourceManager const& sources, Unit& unit, Types& types )
{
  std::vector<EnumerationType>& enumerations = types.enumerations;
  auto const redefined = [&]( Token const& name, Token const& previous )
  {
    // C would take two `u8 x;` as one object; the subset refuses them as it
    // refuses two functions of one name — see
    // docs/decisions/0071-the-subsets-spelling.md.
    std::string const spelling = nameSpelled( sources.textOf( name.span() ) );
    unit.sink->add( diagnostic( diag::DiagnosticId::C_REDEFINITION )
                        .at( name.location, name.length )
                        .arg( "name", spelling )
                        .note( diagnostic( diag::DiagnosticId::C_PREVIOUS_DEFINITION )
                                   .at( previous.location, previous.length )
                                   .arg( "name", spelling ) ) );
  };
  auto const define = [&]( Definition definition )
  {
    std::string const spelling = nameSpelled( sources.textOf( definition.name.span() ) );
    auto const [existing, added] = unit.definitions.try_emplace( spelling, definition );
    if ( !added )
    {
      redefined( definition.name, existing->second.name );
    }
  };

  for ( ExternalDeclaration const& declaration : unit.tree.declarations )
  {
    if ( auto const* function = std::get_if<FunctionDefinition>( &declaration ) )
    {
      std::vector<ParameterType> parameters;
      parameters.reserve( function->parameters.size() );
      for ( Parameter const& parameter : function->parameters )
      {
        parameters.push_back( ParameterType{ .name = parameter.name,
                                             .type = namedType( parameter.type,
                                                                parameter.isPointer,
                                                                parameter.pointeeConst,
                                                                parameter.isPointer && parameter.pointeeVolatile ),
                                             .isNamed = parameter.isNamed } );
      }
      Signature signature{ .result = std::nullopt, .parameters = std::move( parameters ) };
      if ( function->result.has_value() )
      {
        signature.result = namedType( *function->result,
                                      function->resultPointer,
                                      function->resultPointeeConst,
                                      function->resultPointer && function->resultPointeeVolatile );
      }
      std::string const spelling = nameSpelled( sources.textOf( function->name.span() ) );
      if ( function->isTypedef )
      {
        // A function type: the name is a type, and the Proc a pointer to it
        // goes through — see docs/decisions/0065-handlers.md. Its parameters
        // and its result are the Definition's, as a function's are.
        auto const index = static_cast<std::uint32_t>( types.functions.size() );
        types.functions.push_back( FunctionType{ signature, spelling } );
        Definition type;
        type.kind = NameKind::TYPE;
        type.name = function->name;
        giveSignature( type, std::move( signature ) );
        type.function = index;
        define( std::move( type ) );
        continue;
      }
      bool const isSlot = function->isExtern && hasAttribute( function->attributes, sources, "slot" );

      // A Slot that takes or returns **is** the function type its
      // Implementations are members of: they read its Temporaries and declare
      // none, exactly as a member of a `typedef`'s type does, and a call
      // writes them and `jsr`s the Cell — see
      // docs/decisions/0174-a-slot-takes-and-returns.md.
      std::optional<std::uint32_t> slotType;
      if ( isSlot && ( !signature.parameters.empty() || signature.result.has_value() ) )
      {
        slotType = static_cast<std::uint32_t>( types.functions.size() );
        types.functions.push_back( FunctionType{ signature, spelling } );
      }

      Definition declared;
      declared.kind = NameKind::FUNCTION;
      declared.isStatic = function->isStatic;
      declared.name = function->name;
      giveSignature( declared, std::move( signature ) );
      declared.isExtern = function->isExtern;
      declared.pane = function->isExtern ? std::string{} : nameAttribute( function->attributes, sources, "in" );
      declared.under = function->isExtern ? std::string{} : nameAttribute( function->attributes, sources, "under" );
      declared.isSlot = isSlot;
      declared.implements = nameAttribute( function->attributes, sources, "implements" );
      declared.function = slotType;
      define( std::move( declared ) );
      continue;
    }
    if ( auto const* aggregate = std::get_if<StructSpecifier>( &declaration ) )
    {
      AggregateType type{ .name = std::string{ sources.textOf( aggregate->name.span() ) },
                          .isUnion = aggregate->isUnion,
                          .members = {} };
      for ( Declaration const& member : aggregate->members )
      {
        for ( std::size_t index = 0; index < member.declarators.size(); ++index )
        {
          DeclaratorShape const& shape = member.shapes[index];
          type.members.push_back( MemberType{ .name = member.declarators[index],
                                              .type = namedType( member.type,
                                                                 shape.isPointer,
                                                                 member.isConst && shape.isPointer,
                                                                 member.isVolatile && shape.isPointer ),
                                              .isArray = shape.isArray,
                                              .size = shape.size.get(),
                                              .count = 1,
                                              .offset = 0 } );
        }
      }
      auto const index = static_cast<std::uint32_t>( types.aggregates.size() );
      types.aggregates.push_back( std::move( type ) );
      Definition definition;
      definition.kind = NameKind::TYPE;
      definition.name = aggregate->name;
      definition.type = ir::Type::BLOCK;
      definition.aggregate = index;
      define( std::move( definition ) );
      continue;
    }
    if ( auto const* specifier = std::get_if<EnumSpecifier>( &declaration ) )
    {
      std::string const name{ sources.textOf( specifier->name.span() ) };
      if ( specifier->enumerators.size() > MAX_ENUMERATORS )
      {
        unit.sink->add( diagnostic( diag::DiagnosticId::C_TOO_MANY_ENUMERATORS )
                            .at( specifier->name.location, specifier->name.length )
                            .arg( "name", name )
                            .arg( "count", static_cast<std::int64_t>( specifier->enumerators.size() ) ) );
      }
      EnumerationType type{ .name = name, .enumerators = {} };
      std::map<std::string, Token, std::less<>> seen;
      for ( Token const& enumerator : specifier->enumerators )
      {
        std::string spelling{ sources.textOf( enumerator.span() ) };
        if ( auto const [existing, added] = seen.try_emplace( spelling, enumerator ); !added )
        {
          redefined( enumerator, existing->second );
        }
        type.enumerators.push_back( std::move( spelling ) );
      }
      auto const index = static_cast<std::uint32_t>( enumerations.size() );
      enumerations.push_back( std::move( type ) );
      Definition defined;
      defined.kind = NameKind::TYPE;
      defined.name = specifier->name;
      defined.enumeration = index;
      define( std::move( defined ) );

      // An `enum` written without `struct` gives the file its enumerators
      // too, each a `const` of the type — see
      // docs/decisions/0162-an-enum-without-struct.md.
      if ( !specifier->isScoped )
      {
        for ( std::uint32_t at = 0; at < specifier->enumerators.size(); ++at )
        {
          Definition enumerator;
          enumerator.kind = NameKind::OBJECT;
          enumerator.name = specifier->enumerators[at];
          enumerator.enumeration = index;
          enumerator.isConst = true;
          enumerator.enumerator = at;
          define( std::move( enumerator ) );
        }
      }
      continue;
    }
    auto const& object = std::get<Declaration>( declaration );
    bool const named = object.type.kind == TokenKind::IDENTIFIER;
    for ( std::size_t index = 0; index < object.declarators.size(); ++index )
    {
      DeclaratorShape const& shape = object.shapes[index];
      Definition defined;
      defined.kind = NameKind::OBJECT;
      defined.isStatic = object.isStatic;
      defined.name = object.declarators[index];
      defined.type = shape.isPointer ? ir::Type::POINTER : typeNamed( object.type.keyword );
      if ( named )
      {
        defined.typeName = object.type;
      }
      if ( shape.isPointer )
      {
        defined.pointee =
            pointeeOf( ValueType{ .type = typeNamed( object.type.keyword ) }, object.isConst, object.isVolatile );
      }
      defined.isConst = shape.isPointer ? shape.isConstPointer : object.isConst;
      defined.value = object.initialisers[index].get();
      defined.isArray = shape.isArray;
      defined.size = shape.size.get();
      if ( shape.size == nullptr && shape.hasList )
      {
        defined.count = static_cast<std::uint32_t>( shape.list.size() );
      }
      defined.isStriped = isStriped( object.attributes, sources );
      defined.isExtern = object.isExtern;
      defined.pane = object.isExtern ? std::string{} : nameAttribute( object.attributes, sources, "in" );
      defined.isSlot = object.isExtern && hasAttribute( object.attributes, sources, "slot" );
      defined.implements = nameAttribute( object.attributes, sources, "implements" );
      defined.isVolatile = shape.isPointer ? shape.isVolatilePointer : object.isVolatile;
      define( std::move( defined ) );
    }
  }
}

/// Every type written as a name — an object's, a function's result and its
/// parameters', a function type's — given the `enum struct`, the `struct` or
/// the `union` it names: the file's own definition first, and then any
/// file's, as every name is found.
void resolveTypeNames( diag::SourceManager const& sources, std::deque<Unit>& units, Types& registry )
{
  std::map<std::string, Definition const*, std::less<>> types;
  for ( Unit const& unit : units )
  {
    for ( auto const& [spelling, definition] : unit.definitions )
    {
      if ( definition.kind == NameKind::TYPE )
      {
        types.try_emplace( spelling, &definition );
      }
    }
  }

  for ( Unit& unit : units )
  {
    auto const resolved = [&]( Token const& typeName ) -> Definition const*
    {
      std::string_view const name = sources.textOf( typeName.span() );
      Definition const* found = nullptr;
      if ( auto const own = unit.definitions.find( name ); own != unit.definitions.end() )
      {
        found = own->second.kind == NameKind::TYPE ? &own->second : nullptr;
      }
      else if ( auto const other = types.find( name ); other != types.end() )
      {
        found = other->second;
      }
      if ( found == nullptr )
      {
        unit.sink->add( diagnostic( diag::DiagnosticId::C_NOT_A_TYPE )
                            .at( typeName.location, typeName.length )
                            .arg( "name", std::string{ name } ) );
      }
      return found;
    };
    // A named type is an `enum struct`, a value of one byte, or a `struct` or
    // a `union`, a block of the bytes its members take.
    auto const resolve = [&resolved]( NamedType& type )
    {
      std::optional<Token> const typeName = type.typeName;
      if ( !typeName.has_value() )
      {
        return;
      }
      Definition const* const found = resolved( *typeName );
      if ( found == nullptr )
      {
        return;
      }
      if ( type.pointee.has_value() )
      {
        type.pointee->enumeration = found->enumeration;
        type.pointee->aggregate = found->aggregate;
        type.pointee->function = found->function;
        type.pointee->type = found->aggregate.has_value() ? ir::Type::BLOCK : ir::Type::U8;
        return;
      }
      type.enumeration = found->enumeration;
      type.aggregate = found->aggregate;
      type.function = found->function;
      type.type = found->aggregate.has_value() ? ir::Type::BLOCK : ir::Type::U8;
    };

    for ( auto& [spelling, definition] : unit.definitions )
    {
      // A function's signature, and a function type's: a `typedef`'s, or a
      // Slot's that takes or returns, whose type holds the signature too and
      // is given it again once its names are resolved.
      bool const typedefOfFunction = definition.kind == NameKind::TYPE && definition.function.has_value();
      if ( definition.kind == NameKind::FUNCTION || typedefOfFunction )
      {
        if ( definition.result.has_value() )
        {
          resolve( *definition.result );
        }
        for ( ParameterType& parameter : definition.parameters )
        {
          resolve( parameter.type );
        }
        if ( definition.function.has_value() )
        {
          giveSignature( registry.functions.at( *definition.function ), definition );
        }
        continue;
      }
      if ( definition.kind == NameKind::TYPE && definition.aggregate.has_value() )
      {
        for ( MemberType& member : registry.aggregates.at( *definition.aggregate ).members )
        {
          resolve( member.type );
        }
        continue;
      }
      if ( definition.kind != NameKind::OBJECT || !definition.typeName.has_value() )
      {
        continue;
      }
      resolve( definition );
    }
  }
}

/// A qualified name whose first part is no `enum` of C is one name again,
/// the one the assembler exports: the parser kept its parts apart only
/// because it could not tell — see
/// docs/decisions/0230-c-names-a-namespace-with-two-colons.md.
class QualifiedNames
{
public:
  QualifiedNames( diag::SourceManager const& sources, Names const& names, Unit const& unit )
      : mSources( &sources ), mNames( &names ), mUnit( &unit )
  {
  }

  void join( TranslationUnit& tree ) const
  {
    for ( ExternalDeclaration& declaration : tree.declarations )
    {
      if ( auto* const function = std::get_if<FunctionDefinition>( &declaration ) )
      {
        join( function->body );
      }
      else if ( auto* const object = std::get_if<Declaration>( &declaration ) )
      {
        join( *object );
      }
      else if ( auto* const aggregate = std::get_if<StructSpecifier>( &declaration ) )
      {
        for ( Declaration& member : aggregate->members )
        {
          join( member );
        }
      }
    }
  }

private:
  void join( Declaration& declaration ) const
  {
    for ( ExpressionPtr& initialiser : declaration.initialisers )
    {
      join( initialiser );
    }
    for ( DeclaratorShape& shape : declaration.shapes )
    {
      join( shape.size );
      for ( ExpressionPtr& element : shape.list )
      {
        join( element );
      }
    }
  }

  void join( Statement& statement ) const
  {
    join( statement.expression );
    join( statement.initial );
    join( statement.step );
    for ( Statement& item : statement.items )
    {
      join( item );
    }
    if ( statement.declaration != nullptr )
    {
      join( *statement.declaration );
    }
  }

  void join( ExpressionPtr& node ) const
  {
    if ( node == nullptr )
    {
      return;
    }
    if ( node->kind == ExpressionKind::QUALIFIED_NAME )
    {
      std::optional<Meaning> const first =
          mNames->resolve( *mUnit, nameSpelled( mSources->textOf( node->left->token.span() ) ) );
      if ( !first.has_value() || first->kind != NameKind::TYPE || !first->enumeration.has_value() )
      {
        std::uint32_t const begin = node->left->token.location.rawOffset();
        std::uint32_t const end = node->right->token.location.rawOffset() + node->right->token.length;
        node->kind = ExpressionKind::IDENTIFIER;
        node->token = Token{ .kind = TokenKind::IDENTIFIER,
                             .keyword = Keyword::NONE,
                             .location = node->left->token.location,
                             .length = end - begin };
        node->left.reset();
        node->right.reset();
      }
      return;
    }
    join( node->left );
    join( node->right );
    for ( ExpressionPtr& argument : node->arguments )
    {
      join( argument );
    }
  }

  diag::SourceManager const* mSources;
  Names const* mNames;
  Unit const* mUnit;
};

/// The count of an `extern` array written `NAME[]`: the elements its run holds
/// to the end of its Section, which the check holds to leave nothing over.
void settleExterns( std::deque<Unit> const& units, Names& names )
{
  for ( Unit const& unit : units )
  {
    for ( auto const& [spelling, definition] : unit.definitions )
    {
      ExternalName const* const external = names.externalNamed( spelling );
      if ( !definition.isExtern || !definition.isArray || definition.size != nullptr || external == nullptr )
      {
        continue;
      }
      std::uint32_t const element =
          std::max<std::uint32_t>( 1, bytesOf( definition.type, definition.aggregate, names.aggregateOf() ) );
      names.settleCount( definition, runBytes( *external ) / element );
    }
  }
}

/// The value of every `const` at file scope, and the number of elements of
/// every array there whose size is written, each settled after the ones it
/// names, a cycle among them refused once, where the name closing it is read
/// — see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
/// Walked on a stack of its own, so that a long chain of constants costs no
/// depth of the tool's.
void settleConstants( diag::SourceManager const& sources, std::deque<Unit> const& units, Names& names )
{
  enum class State : std::uint8_t
  {
    ACTIVE,
    DONE,
  };
  std::map<Definition const*, State> state;
  std::set<Definition const*> cyclic;

  auto const referenced = [&]( Unit const& unit, Token const& name ) -> Definition const*
  {
    std::optional<Meaning> const meaning = names.resolve( unit, nameSpelled( sources.textOf( name.span() ) ) );
    if ( !meaning.has_value() || meaning->definition == nullptr || !isSettled( *meaning->definition ) )
    {
      return nullptr;
    }
    return meaning->definition;
  };

  auto const settle = [&]( Definition const& definition )
  {
    Unit const& unit = names.unitOf( definition );
    MeaningOf const meaningOf = [&]( Expression const& name )
    { return names.resolve( unit, nameSpelled( sources.textOf( name.token.span() ) ) ); };
    NameValue const valueOf = constantValues( names, unit, meaningOf, sources );
    // A `const` pointer given an address holds it in bytes: the assembler knows
    // the address, and the compiler does not.
    std::vector<Expression const*> reads;
    if ( !definition.isArray && definition.kind == NameKind::OBJECT )
    {
      namesIn( *definition.value, reads );
    }
    bool const address = !definition.isArray && definition.pointee.has_value() &&
                         ( containsAddress( *definition.value ) ||
                           std::ranges::any_of( reads,
                                                [&]( Expression const* read )
                                                {
                                                  std::optional<Meaning> const meaning = meaningOf( *read );
                                                  return meaning.has_value() && meaning->isArray;
                                                } ) );
    if ( address )
    {
      return;
    }
    if ( definition.kind == NameKind::TYPE )
    {
      // Each member's bytes: its type's, times its elements.
      AggregateType layout = names.aggregate( *definition.aggregate );
      std::uint32_t offset = 0;
      std::uint32_t size = 0;
      for ( MemberType& member : layout.members )
      {
        std::uint32_t bytes = ir::sizeOf( member.type.type );
        if ( member.type.aggregate.has_value() && !member.type.pointee.has_value() )
        {
          bytes = cyclic.contains( &definition ) ? 0 : names.aggregate( *member.type.aggregate ).size;
        }
        if ( member.isArray && member.size != nullptr && !cyclic.contains( &definition ) )
        {
          member.count = static_cast<std::uint32_t>(
              std::clamp<std::int64_t>( folded( *member.size, std::nullopt, valueOf ), 0, MAX_ARRAY_BYTES ) );
        }
        member.offset = layout.isUnion ? 0 : offset;
        std::uint32_t const extent = static_cast<std::uint32_t>(
            std::min<std::uint64_t>( std::uint64_t{ bytes } * member.count, MAX_ARRAY_BYTES ) );
        offset =
            static_cast<std::uint32_t>( std::min<std::uint64_t>( std::uint64_t{ offset } + extent, MAX_ARRAY_BYTES ) );
        size = layout.isUnion ? std::max( size, extent ) : offset;
      }
      layout.size = size;
      names.settleLayout( *definition.aggregate, std::move( layout ) );
      return;
    }
    if ( definition.isArray )
    {
      std::int64_t const count = cyclic.contains( &definition ) ? 0 : folded( *definition.size, std::nullopt, valueOf );
      names.settleCount( definition,
                         static_cast<std::uint32_t>( std::clamp<std::int64_t>( count, 0, MAX_ARRAY_BYTES ) ) );
      return;
    }
    std::int64_t value = 0;
    if ( !cyclic.contains( &definition ) )
    {
      value = wrapped( folded( *definition.value, definition.type, valueOf ), definition.type );
    }
    names.settle( definition, value );
  };

  struct Frame
  {
    Definition const* definition = nullptr;
    std::vector<Token> reads;
    std::size_t next = 0;
  };

  // What a definition's value is settled from reads: the names in a
  // constant's value or an array's size, and a type's members' sizes and the
  // types of those that hold one by value.
  auto const readsOf = [&]( Definition const& definition )
  {
    std::vector<Expression const*> expressions;
    std::vector<Token> tokens;
    if ( definition.kind == NameKind::TYPE )
    {
      for ( MemberType const& member : names.aggregate( *definition.aggregate ).members )
      {
        if ( member.size != nullptr )
        {
          namesIn( *member.size, expressions );
        }
        if ( member.type.typeName.has_value() && !member.type.pointee.has_value() )
        {
          tokens.push_back( *member.type.typeName );
        }
      }
    }
    else
    {
      namesIn( definition.isArray ? *definition.size : *definition.value, expressions );
    }
    for ( Expression const* read : expressions )
    {
      tokens.push_back( read->token );
    }
    return tokens;
  };

  for ( Unit const& unit : units )
  {
    for ( auto const& [spelling, root] : unit.definitions )
    {
      if ( !isSettled( root ) || state.contains( &root ) )
      {
        continue;
      }
      std::vector<Frame> frames;
      auto const enter = [&]( Definition const& definition )
      {
        state[&definition] = State::ACTIVE;
        frames.push_back( Frame{ .definition = &definition, .reads = readsOf( definition ), .next = 0 } );
      };
      enter( root );
      while ( !frames.empty() )
      {
        Frame& top = frames.back();
        if ( top.next == top.reads.size() )
        {
          settle( *top.definition );
          state[top.definition] = State::DONE;
          frames.pop_back();
          continue;
        }
        Token const read = top.reads[top.next++];
        Definition const* const target = referenced( names.unitOf( *top.definition ), read );
        if ( target == nullptr )
        {
          continue;
        }
        auto const found = state.find( target );
        if ( found == state.end() )
        {
          enter( *target );
          continue;
        }
        if ( found->second == State::DONE )
        {
          continue;
        }
        if ( target == top.definition && target->kind != NameKind::TYPE )
        {
          // A `const` naming itself is read in its own value, which the check
          // reports as that.
          cyclic.insert( target );
          continue;
        }

        // The frames from the one it names to this one are the cycle.
        std::string cycle;
        bool inCycle = false;
        for ( Frame const& frame : frames )
        {
          inCycle = inCycle || frame.definition == target;
          if ( inCycle )
          {
            cyclic.insert( frame.definition );
            cycle += "`" + std::string{ sources.textOf( frame.definition->name.span() ) } + "` -> ";
          }
        }
        cycle += "`" + std::string{ sources.textOf( target->name.span() ) } + "`";
        // A type holding itself by value has no size; anything else in the
        // cycle is a constant defined through itself.
        names.unitOf( *top.definition )
            .sink->add( diagnostic( target->kind == NameKind::TYPE ? diag::DiagnosticId::C_AGGREGATE_HOLDS_ITSELF
                                                                   : diag::DiagnosticId::C_CONSTANT_CYCLE )
                            .at( read.location, read.length )
                            .arg( "cycle", cycle ) );
      }
    }
  }
}
} // namespace

namespace
{

/// A jump or a branch to a block that holds nothing and only jumps on goes
/// where that block goes, and a block nothing reaches then is gone, the rest
/// keeping their order: `if (...) break;` branches to the loop's exit rather
/// than over a `jmp` to it — see
/// docs/decisions/0152-a-jump-to-a-jump-goes-where-it-goes.md. Not in a
/// function with `[[with]]` blocks, whose regions are numbered by block.
void threadJumps( ir::Unit& unit )
{
  for ( ir::Definition& definition : unit.definitions )
  {
    auto* const function = std::get_if<ir::Function>( &definition );
    if ( function == nullptr || function->blocks.empty() || !function->withs.empty() )
    {
      continue;
    }
    std::vector<ir::Block>& blocks = function->blocks;
    // The entry is never passed through, since the Proc begins there; and a
    // chain of empty blocks ends in at most as many steps as there are blocks,
    // or is a loop that does nothing forever, which keeps its block.
    auto const landing = [&blocks]( std::uint32_t at )
    {
      for ( std::size_t steps = 0;
            steps < blocks.size() && at != 0 && blocks[at].instructions.empty() &&
            blocks[at].terminator.kind == ir::TerminatorKind::JUMP && blocks[at].terminator.target != at;
            ++steps )
      {
        at = blocks[at].terminator.target;
      }
      return at;
    };
    for ( ir::Block& block : blocks )
    {
      ir::eachTarget( block.terminator, [&landing]( std::uint32_t& into ) { into = landing( into ); } );
      ir::Terminator& end = block.terminator;
      if ( end.kind != ir::TerminatorKind::BRANCH || end.target != end.otherwise )
      {
        continue;
      }
      // Both ways land on one block, so nothing is decided: a jump, with the
      // comparison the branch read gone — where nothing else reads it and it
      // reads nothing whose every read is the program's. Otherwise the branch
      // stays, since the comparison would be written whole as a `bool`.
      auto const* const tested = std::get_if<ir::Value>( &end.condition );
      auto const* const compare =
          block.instructions.empty() ? nullptr : std::get_if<ir::Compare>( &block.instructions.back().operation );
      auto const plain = [&unit]( ir::Operand const& operand )
      {
        auto const* const object = std::get_if<ir::Object>( &operand );
        return std::holds_alternative<ir::Constant>( operand ) ||
               ( object != nullptr && !unit.volatiles.contains( ir::baseOf( object->name ) ) );
      };
      if ( tested == nullptr || compare == nullptr || compare->result.index != tested->index ||
           ir::readersOf( *function, *tested ) != 1 || !plain( compare->left ) || !plain( compare->right ) )
      {
        continue;
      }
      block.instructions.pop_back();
      end.kind = ir::TerminatorKind::JUMP;
      end.otherwise = 0;
      end.condition = {};
    }
    std::vector<bool> reached( blocks.size(), false );
    std::vector<std::uint32_t> pending{ 0 };
    while ( !pending.empty() )
    {
      std::uint32_t const at = pending.back();
      pending.pop_back();
      if ( reached[at] )
      {
        continue;
      }
      reached[at] = true;
      std::ranges::copy( ir::successorsOf( blocks[at].terminator ), std::back_inserter( pending ) );
    }
    std::vector<std::uint32_t> renumbered( blocks.size(), 0 );
    std::vector<ir::Block> kept;
    for ( std::uint32_t at = 0; at < blocks.size(); ++at )
    {
      if ( reached[at] )
      {
        renumbered[at] = static_cast<std::uint32_t>( kept.size() );
        kept.push_back( std::move( blocks[at] ) );
      }
    }
    for ( ir::Block& block : kept )
    {
      ir::eachTarget( block.terminator, [&renumbered]( std::uint32_t& into ) { into = renumbered[into]; } );
    }
    blocks = std::move( kept );
  }
}

/// The names of a unit whose bytes are the Target's registers: a constant
/// pointer aimed into one, `const u8* const VCOUNT = (u8*)0xD40B`, which C
/// reads and writes through its name — see docs/decisions/0151-volatile.md.
void markVolatiles( ir::Unit& unit, std::span<AddressRange const> registers )
{
  auto const inRegister = [registers]( std::int64_t address )
  {
    return std::ranges::any_of(
        registers,
        [address]( AddressRange const& range )
        { return std::cmp_greater_equal( address, range.begin ) && std::cmp_less( address, range.end ); } );
  };
  auto const mark = [&]( ir::NamedConstant const& constant )
  {
    if ( constant.value.type == ir::Type::POINTER && constant.value.name.empty() && inRegister( constant.value.value ) )
    {
      unit.volatiles.insert( constant.name );
    }
  };
  // A register the Target names, which C reads and writes by that name —
  // `PORTB`, or an element of `io`.
  for ( AddressRange const& range : registers )
  {
    if ( !range.name.empty() )
    {
      unit.volatiles.insert( range.name );
    }
  }
  for ( ir::Definition const& definition : unit.definitions )
  {
    if ( auto const* const constant = std::get_if<ir::NamedConstant>( &definition ) )
    {
      mark( *constant );
    }
    else if ( auto const* const function = std::get_if<ir::Function>( &definition ) )
    {
      for ( ir::NamedConstant const& local : function->constants )
      {
        mark( local );
      }
    }
  }
}

} // namespace

std::vector<std::optional<ir::Unit>> lower( diag::SourceManager const& sources,
                                            std::span<SourceFile const> files,
                                            std::span<ExternalName const> externals,
                                            diag::DiagnosticSink& sink,
                                            std::span<AddressRange const> registers,
                                            Intent intent )
{
  // Deques, since a Unit points at its sink and a vector would move them. A
  // vector of Units would also copy them as it grows wherever moving one may
  // throw, as it may with MSVC's std::map, and a syntax tree has no copy.
  std::deque<diag::DiagnosticSink> found;
  std::deque<Unit> units;
  for ( SourceFile const& file : files )
  {
    diag::DiagnosticSink& own = found.emplace_back( sink.policy() );
    units.push_back(
        Unit{ .source = &file, .tree = parse( sources, file.file, own ), .sink = &own, .definitions = {} } );
  }

  // Every signature of the program first, a file that did not parse
  // included: what it did parse is what it defines, and leaving it out would
  // report its names as undeclared in every other file.
  Types types;
  for ( Unit& unit : units )
  {
    gatherSignatures( sources, unit, types );
  }
  resolveTypeNames( sources, units, types );
  Names names{ sources, units, externals, std::move( types ) };
  for ( Unit& unit : units )
  {
    QualifiedNames{ sources, names, unit }.join( unit.tree );
  }

  // A cycle among constants is reported with what the check finds, and does
  // not keep the file from being checked.
  std::vector<bool> checked;
  checked.reserve( units.size() );
  for ( Unit const& unit : units )
  {
    checked.push_back( !unit.sink->hasErrors() );
  }
  settleConstants( sources, units, names );
  settleExterns( units, names );

  // A file that did not parse is not checked: what the compiler would say
  // about half a tree is noise beside the error that cut it short.
  std::vector<CallEdge> edges;
  std::vector<std::set<std::uint32_t>> addressed( units.size() );
  Membership members;
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    if ( checked[index] )
    {
      checkUnit( sources, names, units[index], index, edges, addressed[index], members );
    }
  }
  refuseRecursion( sources, edges );
  refuseCallsOfOwnType( edges, members );

  // Every Unit is lowered before any pass runs over one, so that a pass with
  // the whole program in front of it has an IR of every Unit to read. That is
  // what the gap between this loop and the next is for: wrapping a call into
  // its caller is one — see
  // docs/decisions/0172-a-call-with-one-site-is-wrapped.md — and it belongs
  // there and not after the per-Unit passes, since the larger half of what it
  // is worth is that they then see through the body it spliced.
  std::vector<std::optional<ir::Unit>> lowered( units.size() );
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    Unit const& unit = units[index];
    if ( unit.sink->hasErrors() )
    {
      continue;
    }
    ir::Unit one = lowerUnit( sources, names, unit, *unit.sink, addressed[index], members );
    markVolatiles( one, registers );
    if ( !unit.sink->hasErrors() )
    {
      lowered[index] = std::move( one );
    }
  }

  std::vector<diag::DiagnosticSink*> sinks;
  sinks.reserve( units.size() );
  for ( Unit const& unit : units )
  {
    sinks.push_back( unit.sink );
  }
  // Before any pass moves a store or drops one: what the source says about a
  // `static` local read before anything writes it — see
  // docs/decisions/0212-a-static-local-read-before-it-is-written.md.
  for ( std::size_t index = 0; index < lowered.size(); ++index )
  {
    if ( lowered[index].has_value() && sinks[index] != nullptr )
    {
      checkStaticsAreAssigned( *lowered[index], *sinks[index] );
    }
  }

  wrapCalls( lowered, sinks, intent == Intent::SPEED );

  // Before any pass reads a loop: which objects at file scope a loop may hold
  // in a byte of its own, and which functions a loop calls, whose own loops
  // are entered often, both of which take the whole program to know — see
  // docs/decisions/0286-a-loop-holds-a-global-in-a-byte-of-its-own.md and
  // docs/decisions/0303-fit-holds-an-object-where-the-cycles-pay-for-the-bytes.md.
  Promotable const promotable = promotableGlobals( lowered );
  std::set<std::string> const often = enteredOften( lowered );
  Effects const effects = effectsOf( lowered );

  // The passes over the finished IR, one Unit at a time: a copy read a few
  // times read from where it came, then what a loop computes
  // the same at every turn moved before it, a loop that fills memory a byte at
  // a time made one call of the runtime, then what the subset's own
  // widening rules put in two bytes and only one of them needs, the runtime's
  // Procs chosen for what the operands turn out to be and a quotient and a
  // remainder of the same operands made one call of them, then the stores that
  // nothing reads, and last — since what it looks for is a store that
  // survived all four — the local a function returns becoming its result.
  for ( std::optional<ir::Unit>& one : lowered )
  {
    if ( !one.has_value() )
    {
      continue;
    }
    foldConstants( *one );
    propagateCopies( *one );
    promoteGlobals( *one, promotable, often, intent );
    hoistInvariants( *one, effects );
    fillLoops( *one, intent == Intent::SPEED );
    walkPointers( *one, effects );
    countToWrap( *one );
    reuseLoads( *one );
    unshiftHalves( *one );
    narrowRanges( *one );
    narrowIndexes( *one );
    narrowRuntimeCalls( *one );
    shareDivisions( *one );
    dropDeadStores( *one );
    mergeReturnedLocals( *one );
  }

  // Last of all, and over the whole program: which functions leave their
  // result in a parameter's byte, which is part of the Signature every caller
  // reads — see docs/decisions/0120-a-function-returns-through-a-parameter.md.
  returnThroughParameters( lowered );

  for ( diag::DiagnosticSink& own : found )
  {
    sink.merge( std::move( own ) );
  }
  return lowered;
}

namespace
{

/// Whether a function may carry a parameter or its result in a register: one
/// every call of which is a `jsr` from C that loads what it declares — not a
/// member of a function type, whose bytes are the type's, not a Slot's, not
/// one a `[[with]]` reaches the bytes of, not one called through
/// a `.with`, which may write `A` on the way, and not one entered by falling
/// into it.
bool mayPlace( ir::Function const& function, std::set<std::string> const& fallenInto )
{
  if ( function.isTrampoline || !function.memberOf.empty() || !function.implements.empty() || !function.pane.empty() ||
       !function.under.empty() || !function.then.empty() || !function.withs.empty() ||
       fallenInto.contains( function.name ) || function.blocks.empty() )
  {
    return false;
  }
  for ( ir::Block const& block : function.blocks )
  {
    for ( ir::Instruction const& instruction : block.instructions )
    {
      if ( std::holds_alternative<ir::EnterWith>( instruction.operation ) )
      {
        return false;
      }
    }
  }
  return true;
}

/// The parameter a byte of which `A` would be loaded with first: the first
/// object the first block reads into `A`, where that is a parameter of one
/// byte.
ir::Local* firstReadIntoA( ir::Function& function )
{
  auto const parameterNamed = [&function]( std::string const& name ) -> ir::Local*
  {
    auto const found = std::ranges::find_if( function.parameters,
                                             [&name]( ir::Local const& parameter ) { return parameter.name == name; } );
    return found == function.parameters.end() ? nullptr : &*found;
  };
  for ( ir::Instruction const& instruction : function.blocks.front().instructions )
  {
    std::vector<ir::Operand const*> read;
    if ( auto const* const binary = std::get_if<ir::Binary>( &instruction.operation ) )
    {
      read = { &binary->left, &binary->right };
    }
    else if ( auto const* const compare = std::get_if<ir::Compare>( &instruction.operation ) )
    {
      read = { &compare->left, &compare->right };
    }
    else if ( auto const* const unary = std::get_if<ir::Unary>( &instruction.operation ) )
    {
      read = { &unary->operand };
    }
    else if ( auto const* const convert = std::get_if<ir::Convert>( &instruction.operation ) )
    {
      read = { &convert->operand };
    }
    else if ( auto const* const store = std::get_if<ir::Store>( &instruction.operation ) )
    {
      read = { &store->value };
    }
    for ( ir::Operand const* operand : read )
    {
      if ( auto const* const object = std::get_if<ir::Object>( operand ) )
      {
        ir::Local* const parameter = parameterNamed( object->name );
        bool const byte =
            parameter != nullptr && parameter->type != ir::Type::BLOCK && ir::sizeOf( parameter->type ) == 1;
        return byte ? parameter : nullptr;
      }
    }
    if ( !read.empty() )
    {
      return nullptr;
    }
  }
  return nullptr;
}

/// The parameter of one byte the function first indexes an element by.
ir::Local* firstIndex( ir::Function& function )
{
  for ( ir::Block const& block : function.blocks )
  {
    for ( ir::Instruction const& instruction : block.instructions )
    {
      ir::Operand const* index = nullptr;
      if ( auto const* const load = std::get_if<ir::Load>( &instruction.operation ); load != nullptr && !load->scaled )
      {
        index = &load->index;
      }
      else if ( auto const* const store = std::get_if<ir::StoreElement>( &instruction.operation );
                store != nullptr && !store->scaled )
      {
        index = &store->index;
      }
      auto const* const object = index != nullptr ? std::get_if<ir::Object>( index ) : nullptr;
      if ( object == nullptr )
      {
        continue;
      }
      for ( ir::Local& parameter : function.parameters )
      {
        if ( parameter.name == object->name && parameter.type != ir::Type::BLOCK && ir::sizeOf( parameter.type ) == 1 )
        {
          return &parameter;
        }
      }
    }
  }
  return nullptr;
}

/// Hands `visit` every call in every function of every unit, in order, as
/// `( unit, caller, block, at, call )`, for as long as it answers true.
template <typename Units, typename Visit>
void forEachCall( Units& units, Visit const& visit )
{
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    if ( !units[index].has_value() )
    {
      continue;
    }
    for ( auto& definition : units[index]->definitions )
    {
      auto* const caller = std::get_if<ir::Function>( &definition );
      if ( caller == nullptr )
      {
        continue;
      }
      for ( auto& block : caller->blocks )
      {
        for ( std::size_t at = 0; at < block.instructions.size(); ++at )
        {
          if ( auto* const call = std::get_if<ir::Call>( &block.instructions[at].operation );
               call != nullptr && !visit( index, *caller, block, at, *call ) )
          {
            return;
          }
        }
      }
    }
  }
}

/// Whether every call of the function that reads its result stores it whole
/// into an object right after, which a pair whose high byte is in `A` does in
/// as many instructions as before, less the callee's store: a caller that
/// reads the pair as an operand where it lies would first have to set it
/// down — see docs/decisions/0145-an-argument-in-a-register.md.
template <typename Resolve>
bool storedWhole( std::vector<std::optional<ir::Unit>> const& units,
                  Resolve const& resolve,
                  ir::Function const& callee )
{
  bool whole = true;
  forEachCall(
      units,
      [&]( std::size_t index, ir::Function const& caller, ir::Block const& block, std::size_t at, ir::Call const& call )
      {
        auto const where = resolve( index, call.name );
        if ( !where.has_value() || where->function != &callee || !call.result.has_value() )
        {
          return true;
        }
        auto const* const store = at + 1 < block.instructions.size()
                                      ? std::get_if<ir::Store>( &block.instructions[at + 1].operation )
                                      : nullptr;
        whole = store != nullptr && isValue( store->value, *call.result ) && ir::sizeOf( store->type ) == 2 &&
                ir::readersOf( caller, *call.result ) == 1;
        return whole;
      } );
  return whole;
}

} // namespace

/// Where each function of C carries its parameters and its result, chosen
/// from its own text, callees before their callers so that the calls in a
/// function's text are written as they will be — and then every call of it
/// told. A parameter goes in `A` where the first block reads it into `A`
/// first, or in `X` where an element is indexed by it, and only where its
/// byte is then named by nothing; the result stays in `A` where `A` holds it
/// at every `rts` — see docs/decisions/0145-an-argument-in-a-register.md.
void choosePlaces( diag::SourceManager const& sources,
                   std::span<SourceFile const> files,
                   std::vector<std::optional<ir::Unit>>& units,
                   Cpu cpu,
                   Intent intent )
{
  struct Where
  {
    std::size_t unit = 0;
    ir::Function* function = nullptr;
  };

  std::map<std::string, Where> exported;
  std::vector<std::map<std::string, ir::Function*>> own( units.size() );
  std::set<std::string> fallenInto;
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    if ( !units[index].has_value() )
    {
      continue;
    }
    for ( ir::Definition& definition : units[index]->definitions )
    {
      if ( auto* const function = std::get_if<ir::Function>( &definition ) )
      {
        own[index][function->name] = function;
        if ( !function->isStatic )
        {
          exported[function->name] = Where{ .unit = index, .function = function };
        }
        if ( !function->then.empty() )
        {
          fallenInto.insert( function->then );
        }
      }
    }
  }
  auto const resolve = [&]( std::size_t unit, std::string const& name ) -> std::optional<Where>
  {
    if ( auto const found = own[unit].find( name ); found != own[unit].end() )
    {
      return Where{ .unit = unit, .function = found->second };
    }
    if ( auto const found = exported.find( name ); found != exported.end() )
    {
      return found->second;
    }
    return std::nullopt;
  };

  // Callees first: the call graph has no cycle, since recursion is refused.
  // A function no call of C reaches is called from the assembler or not at
  // all, and keeps its bytes: nothing of C would gain, and an assembler
  // caller would be refused for what it writes.
  std::vector<Where> order;
  std::set<ir::Function const*> seen;
  std::set<ir::Function const*> calledFromC;
  std::function<void( Where )> visit = [&]( Where where )
  {
    if ( !seen.insert( where.function ).second )
    {
      return;
    }
    for ( ir::Block const& block : where.function->blocks )
    {
      for ( ir::Instruction const& instruction : block.instructions )
      {
        if ( auto const* const call = std::get_if<ir::Call>( &instruction.operation ) )
        {
          if ( std::optional<Where> const callee = resolve( where.unit, call->name ); callee.has_value() )
          {
            calledFromC.insert( callee->function );
            visit( *callee );
          }
        }
      }
    }
    order.push_back( where );
  };
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    for ( auto const& [name, function] : own[index] )
    {
      visit( Where{ .unit = index, .function = function } );
    }
  }

  for ( Where const& where : order )
  {
    ir::Function& function = *where.function;
    if ( !calledFromC.contains( where.function ) || !mayPlace( function, fallenInto ) )
    {
      continue;
    }
    // Not the parameter whose byte is also the result's: the callers read
    // the result there after the call — see
    // docs/decisions/0120-a-function-returns-through-a-parameter.md.
    ir::Local* inX = firstIndex( function );
    if ( inX != nullptr && inX->name == function.resultByte )
    {
      inX = nullptr;
    }
    if ( inX != nullptr )
    {
      inX->place = "x";
    }

    // For `A`, the parameter the first block reads into it first, and then
    // every other byte in turn: one the body reads only as the operand of an
    // operator that commutes is found in `A` there too, `adc a` in place of
    // `lda a / adc b` — see
    // docs/decisions/0147-the-operand-a-holds-goes-first.md.
    std::vector<ir::Local*> candidates;
    if ( ir::Local* const first = firstReadIntoA( function ); first != nullptr && first != inX )
    {
      candidates.push_back( first );
    }
    for ( ir::Local& parameter : function.parameters )
    {
      bool const byte = parameter.type != ir::Type::BLOCK && ir::sizeOf( parameter.type ) == 1;
      if ( byte && &parameter != inX && std::ranges::find( candidates, &parameter ) == candidates.end() )
      {
        candidates.push_back( &parameter );
      }
    }
    std::erase_if( candidates,
                   [&function]( ir::Local const* parameter ) { return parameter->name == function.resultByte; } );
    auto const facts = [&]
    { return detail::placeFacts( sources, files[where.unit].path, cpu, intent, function, *units[where.unit] ); };
    std::optional<PlaceFacts> found;
    for ( ir::Local* const candidate : candidates )
    {
      candidate->place = "a";
      found = facts();
      if ( !namedIn( found->body, candidate->name ) )
      {
        break;
      }
      candidate->place.clear();
      found.reset();
    }
    if ( !found.has_value() )
    {
      found = facts();
    }
    if ( inX != nullptr && namedIn( found->body, inX->name ) )
    {
      inX->place.clear();
      found = facts();
    }

    // The result, where `A` holds it, or a pair's high byte, at every `rts`.
    PlaceFacts const& written = found.value();
    std::string const result{ RESULT };
    // Not a result another function's is, or that is another's: its byte is
    // the one the other's callers read — see
    // docs/decisions/0278-a-result-may-be-another-procs-result.md.
    bool const scalar = function.result.has_value() && *function.result != ir::Type::BLOCK &&
                        function.resultByte.empty() && function.resultElsewhere.empty() && !function.resultShared &&
                        ir::sizeOf( *function.result ) <= 2;
    auto const heldEverywhere = [&written]( std::string const& name )
    {
      return !written.returns.empty() &&
             std::ranges::all_of( written.returns,
                                  [&name]( std::vector<std::string> const& held )
                                  { return std::ranges::find( held, name ) != held.end(); } );
    };
    if ( scalar && onlyStoredIn( written.body, result ) )
    {
      if ( ir::sizeOf( *function.result ) == 1 && heldEverywhere( result ) )
      {
        function.resultPlace = "a";
      }
      else if ( ir::sizeOf( *function.result ) == 2 && heldEverywhere( result + "+1" ) &&
                storedWhole( units, resolve, function ) )
      {
        function.resultPlace = "ma";
      }
    }

    // Every call of it, told.
    forEachCall(
        units,
        [&]( std::size_t index, ir::Function& /*caller*/, ir::Block& /*block*/, std::size_t /*at*/, ir::Call& call )
        {
          std::optional<Where> const callee = resolve( index, call.name );
          if ( !callee.has_value() || callee->function != &function )
          {
            return true;
          }
          for ( ir::Argument& argument : call.arguments )
          {
            for ( ir::Local const& parameter : function.parameters )
            {
              if ( argument.name == function.name + "." + parameter.name )
              {
                argument.place = parameter.place;
              }
            }
          }
          call.resultPlace = function.resultPlace;
          return true;
        } );
  }
}

/// Every call of a Proc of the assembler told where its bytes are, the ones
/// the compiler writes itself included — a call of the runtime, and one
/// `Select` sent to another Proc of it — so that where the runtime's
/// `.declare`s keep a byte in a register, the call puts it there: see
/// docs/decisions/0146-the-runtime-in-registers.md.
void placeAssemblerCalls( std::vector<std::optional<ir::Unit>>& units, std::span<ExternalName const> externals )
{
  std::map<std::string, ExternalName const*> procs;
  for ( ExternalName const& name : externals )
  {
    if ( name.kind == ExternalKind::PROC )
    {
      procs[name.name] = &name;
    }
  }
  forEachCall(
      units,
      [&procs](
          std::size_t /*unit*/, ir::Function& /*caller*/, ir::Block& /*block*/, std::size_t /*at*/, ir::Call& call )
      {
        auto const found = procs.find( call.name );
        if ( found == procs.end() )
        {
          return true;
        }
        ExternalName const& proc = *found->second;
        for ( std::size_t index = 0; index < call.arguments.size() && index < proc.arguments.size(); ++index )
        {
          call.arguments[index].place = proc.arguments[index].place;
        }
        if ( std::optional<ExternalByte> const& result = proc.result; result.has_value() )
        {
          call.resultPlace = result.value().place;
        }
        return true;
      } );
}

std::vector<std::optional<std::string>> compile( diag::SourceManager const& sources,
                                                 std::span<SourceFile const> files,
                                                 std::span<ExternalName const> externals,
                                                 diag::DiagnosticSink& sink,
                                                 std::span<AddressRange const> registers,
                                                 Intent intent,
                                                 Cpu cpu )
{
  std::vector<std::optional<ir::Unit>> lowered = lower( sources, files, externals, sink, registers, intent );
  // The blocks as the text will have them, a jump to a jump gone: after every
  // pass over the IR, which the blocks as the source wrote them serve.
  for ( std::optional<ir::Unit>& unit : lowered )
  {
    if ( unit.has_value() )
    {
      threadJumps( *unit );
    }
  }
  placeAssemblerCalls( lowered, externals );
  choosePlaces( sources, files, lowered, cpu, intent );
  std::vector<std::optional<std::string>> texts( lowered.size() );
  std::size_t index = 0;
  for ( std::optional<ir::Unit> const& one : lowered )
  {
    if ( one.has_value() )
    {
      texts[index] = writeUnit( sources, files[index].path, cpu, intent, *one );
    }
    ++index;
  }
  return texts;
}

} // namespace nga::c
