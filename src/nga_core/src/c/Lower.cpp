#include "nga/c/Lower.hpp"

#include "nga/c/Lexer.hpp"
#include "nga/c/Typing.hpp"

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

namespace nga::c::detail
{

namespace
{

/// A function's blocks as its statements are read. Blocks are made before
/// they are entered, so that a branch can name one it has not reached; the
/// order they are entered in is the order they are written in.
class Blocks
{
public:
  Blocks()
  {
    enter( create() );
  }

  [[nodiscard]] std::uint32_t create()
  {
    mBlocks.emplace_back();
    return static_cast<std::uint32_t>( mBlocks.size() - 1 );
  }

  /// Makes `block` the one instructions go to, the one before having been
  /// ended.
  void enter( std::uint32_t block )
  {
    mCurrent = block;
    mOpen = true;
    mOrder.push_back( block );
  }

  [[nodiscard]] bool isOpen() const
  {
    return mOpen;
  }

  void append( ir::Instruction instruction )
  {
    reopen();
    mBlocks[mCurrent].instructions.push_back( std::move( instruction ) );
  }

  void terminate( ir::Terminator terminator )
  {
    reopen();
    mBlocks[mCurrent].terminator = std::move( terminator );
    mOpen = false;
  }

  /// A `[[with]]` block's blocks, from `begin` up to `end`, which is the one
  /// entered after it: contiguous once ordered, since every block of the
  /// statements inside is entered between the two.
  void region( std::uint32_t begin, std::uint32_t end )
  {
    mRegions.push_back( ir::WithRegion{ .begin = begin, .end = end } );
  }

  /// Gives `function` the blocks in the order they were entered, their
  /// targets renumbered to match, and the regions by those positions.
  void takeInto( ir::Function& function )
  {
    // A block made and never entered would be no position at all, and a
    // target of it would quietly become the entry.
    assert( mOrder.size() == mBlocks.size() && "every block made was entered" );
    std::vector<std::uint32_t> position( mBlocks.size(), 0 );
    for ( std::uint32_t index = 0; index < mOrder.size(); ++index )
    {
      position[mOrder[index]] = index;
    }
    function.withs.clear();
    for ( ir::WithRegion const& region : mRegions )
    {
      function.withs.push_back( ir::WithRegion{ .begin = position[region.begin], .end = position[region.end] } );
    }
    function.blocks.clear();
    function.blocks.reserve( mOrder.size() );
    for ( std::uint32_t const block : mOrder )
    {
      ir::Block moved = std::move( mBlocks[block] );
      moved.terminator.target = position[moved.terminator.target];
      moved.terminator.otherwise = position[moved.terminator.otherwise];
      for ( std::uint32_t& into : moved.terminator.targets )
      {
        into = position[into];
      }
      function.blocks.push_back( std::move( moved ) );
    }
  }

private:
  /// What follows a terminator stands in a block nothing reaches, entered
  /// here.
  void reopen()
  {
    if ( !mOpen )
    {
      enter( create() );
    }
  }

  std::vector<ir::Block> mBlocks;
  std::vector<std::uint32_t> mOrder;
  std::vector<ir::WithRegion> mRegions;
  std::uint32_t mCurrent = 0;
  bool mOpen = false;
};

/// Whether flow from a function's entry reaches `goal`, over the jumps,
/// branches and dispatches between its blocks.
bool reaches( std::vector<ir::Block> const& blocks, std::uint32_t goal )
{
  std::vector<bool> seen( blocks.size(), false );
  std::vector<std::uint32_t> pending{ 0 };
  seen[0] = true;
  while ( !pending.empty() )
  {
    std::uint32_t const block = pending.back();
    pending.pop_back();
    if ( block == goal )
    {
      return true;
    }
    for ( std::uint32_t const target : ir::successorsOf( blocks[block].terminator ) )
    {
      if ( !seen[target] )
      {
        seen[target] = true;
        pending.push_back( target );
      }
    }
  }
  return false;
}

ir::Terminator returning( diag::SourceLocation at )
{
  return ir::Terminator{ .kind = ir::TerminatorKind::RETURN, .at = at, .target = 0, .otherwise = 0, .condition = {} };
}

/// The third pass: a checked file in the compiler's own form, a declaration
/// at a time.
class Lowering
{
public:
  /// `sink` takes the one finding only the blocks can decide: a function
  /// that returns something and whose end is reached.
  /// `addressed` holds where each local whose address is taken is declared.
  Lowering( diag::SourceManager const& sources,
            Names const& names,
            Unit const& unit,
            diag::DiagnosticSink& sink,
            std::set<std::uint32_t> const& addressed,
            Membership const& members )
      : mSources( &sources ), mNames( &names ), mUnit( &unit ), mSink( &sink ),
        mTyping( sources, names, unit, nullptr ), mAddressed( &addressed ), mMembers( &members )
  {
  }

  [[nodiscard]] ir::Unit translationUnit( TranslationUnit const& tree )
  {
    ir::Unit lowered = definitions( tree );
    // An object written `[[in]]` is a Section in its Pane, its stripes each
    // — see docs/decisions/0096-panes-in-c.md.
    for ( ir::Definition& definition : lowered.definitions )
    {
      auto* const object = std::get_if<ir::Global>( &definition );
      if ( object == nullptr )
      {
        continue;
      }
      auto const found = mUnit->definitions.find( object->name );
      if ( found != mUnit->definitions.end() && found->second.kind == NameKind::OBJECT )
      {
        object->pane = found->second.pane;
        object->implements = found->second.implements;
      }
    }
    return lowered;
  }

  [[nodiscard]] ir::Unit definitions( TranslationUnit const& tree )
  {
    ir::Unit lowered;
    for ( ExternalDeclaration const& declaration : tree.declarations )
    {
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration );
           function != nullptr && function->isTypedef )
      {
        lowered.definitions.emplace_back( trampoline( *function ) );
        continue;
      }
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration );
           function != nullptr && function->isExtern )
      {
        // The Proc is the assembler's — unless the declaration is a Slot,
        // which this program declares and the tool gives a Cell; a function
        // binds it as a `vector`, so a call is a `jsr` to the Cell's `jmp`.
        // See docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md.
        if ( hasAttribute( function->attributes, *mSources, "slot" ) )
        {
          ir::Slot slot{ .name = spellingOf( function->name ), .isVector = true, .at = function->name.location };
          Definition const* const self = definitionOf( *mSources, *mUnit, *function );
          for ( std::size_t index = 0; self != nullptr && index < function->parameters.size(); ++index )
          {
            NamedType const& type = self->parameters[index].type;
            slot.parameters.push_back( ir::Local{ .name = spellingOf( function->parameters[index].name ),
                                                  .type = type.type,
                                                  .bytes = mNames->bytes( type.type, type.aggregate ) } );
          }
          if ( self != nullptr && self->result.has_value() )
          {
            slot.result = self->result->type;
            slot.resultBytes = mNames->bytes( self->result->type, self->result->aggregate );
          }
          lowered.definitions.emplace_back( std::move( slot ) );
        }
        continue;
      }
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration ) )
      {
        lowered.definitions.emplace_back( functionDefinition( *function ) );
        // The Procs its `[[with]]` blocks run, after it.
        for ( ir::Function& extra : mExtra )
        {
          lowered.definitions.emplace_back( std::move( extra ) );
        }
        mExtra.clear();
        continue;
      }
      if ( auto const* specifier = std::get_if<StructSpecifier>( &declaration ) )
      {
        std::optional<Meaning> const type = mTyping.lookupName( spellingOf( specifier->name ) );
        ir::Aggregate aggregate{ .name = spellingOf( specifier->name ), .members = {}, .at = specifier->span.begin };
        if ( type.has_value() && type->aggregate.has_value() )
        {
          for ( MemberType const& member : mNames->aggregate( *type->aggregate ).members )
          {
            aggregate.members.emplace_back( spellingOf( member.name ), member.offset );
          }
        }
        lowered.definitions.emplace_back( std::move( aggregate ) );
        continue;
      }
      if ( auto const* specifier = std::get_if<EnumSpecifier>( &declaration ) )
      {
        ir::Enumeration enumeration{ .name = spellingOf( specifier->name ),
                                     .enumerators = {},
                                     .at = specifier->span.begin,
                                     .isScoped = specifier->isScoped };
        for ( Token const& enumerator : specifier->enumerators )
        {
          enumeration.enumerators.push_back( spellingOf( enumerator ) );
        }
        lowered.definitions.emplace_back( std::move( enumeration ) );
        continue;
      }
      auto const& object = std::get<Declaration>( declaration );
      if ( object.isExtern )
      {
        // The bytes are the assembler's, and the text names them — except a
        // Slot, which this program declares and the tool gives a Cell; the
        // Binding is the type's, a function taking a `jmp` and data an
        // address. See docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md.
        if ( hasAttribute( object.attributes, *mSources, "slot" ) )
        {
          for ( Token const& declarator : object.declarators )
          {
            lowered.definitions.emplace_back(
                ir::Slot{ .name = spellingOf( declarator ), .isVector = false, .at = declarator.location } );
          }
        }
        continue;
      }
      Site const fileScope{ .function = nullptr, .blocks = nullptr, .at = object.span.begin };
      // An object at file scope is `absolute` unless it asked otherwise — see
      // docs/decisions/0210-placement-is-declared-in-c-too.md.
      ir::Placement const wanted =
          placementAttribute( object.attributes, *mSources ).value_or( ir::Placement::ABSOLUTE );
      for ( std::size_t index = 0; index < object.declarators.size(); ++index )
      {
        std::string const name = spellingOf( object.declarators[index] );
        if ( mTyping.lookupName( name ).value_or( Meaning{} ).isVolatile )
        {
          mVolatileNames.insert( name );
        }
        lowered.definitions.emplace_back( fileScopeObject( object, index, wanted, fileScope ) );
      }
    }
    for ( ir::Global& literal : mLiterals )
    {
      lowered.definitions.emplace_back( std::move( literal ) );
    }
    lowered.volatiles.insert( mVolatileNames.begin(), mVolatileNames.end() );
    return lowered;
  }

private:
  /// What is being lowered: the function, its blocks, and the statement the
  /// instructions come from.
  struct Site
  {
    ir::Function* function = nullptr;
    Blocks* blocks = nullptr;
    diag::SourceLocation at;
  };

  /// Where `break` and `continue` go in the loop being lowered.
  struct Loop
  {
    std::uint32_t exit = 0;
    std::uint32_t next = 0;
  };

  /// What one declarator of a declaration at file scope becomes: bytes, of a
  /// striped array, of a `struct` or a `union` or an array of them, of an
  /// array, or of a scalar or a pointer; or a Constant, where it is a `const`
  /// the assembler folds.
  [[nodiscard]] ir::Definition
  fileScopeObject( Declaration const& object, std::size_t index, ir::Placement wanted, Site const& fileScope )
  {
    Token const& declarator = object.declarators[index];
    std::string const name = spellingOf( declarator );
    Expression const* const value = object.initialisers[index].get();
    ir::Type const type = typeNamed( object.type.keyword );
    DeclaratorShape const& array = object.shapes[index];
    std::optional<Meaning> const named = mTyping.lookupName( name );
    if ( named.has_value() && named->isStriped && array.isArray )
    {
      ir::Global striped =
          stripedObject( name, object.isStatic, declarator.location, *named, array, array.hasList, fileScope );
      striped.isConst = isReadOnly( object );
      return striped;
    }
    // A `struct` or a `union`, or an array of them, is bytes: each member a
    // value given, or zero.
    if ( named.has_value() && named->aggregate.has_value() && !array.isPointer )
    {
      std::uint32_t const count = array.isArray ? named->count.value_or( 0 ) : 1;
      std::uint32_t const bytes = count * mNames->bytes( ir::Type::BLOCK, named->aggregate );
      return ir::Global{ .name = name,
                         .type = ir::Type::U8,
                         .isStatic = object.isStatic,
                         .at = declarator.location,
                         .value = std::nullopt,
                         .count = bytes,
                         .elements = array.hasList ? blockConstants( array, elementOf( *named ), count, fileScope )
                                                   : std::vector<ir::Constant>{},
                         .isTemporary = false,
                         .isConst = isReadOnly( object ),
                         .placement = wanted };
    }
    if ( array.isArray )
    {
      std::uint32_t const count = named.value_or( Meaning{} ).count.value_or( 0 );
      return ir::Global{ .name = name,
                         .type = type,
                         .isStatic = object.isStatic,
                         .at = declarator.location,
                         .value = std::nullopt,
                         .count = count,
                         .elements = elementsOf( array, type, count, fileScope ),
                         .isTemporary = false,
                         .isConst = isReadOnly( object ),
                         .placement = wanted };
    }
    // A `const` whose value the assembler folds is a Constant; one given an
    // address holds it in bytes.
    if ( isReadOnly( object ) && value != nullptr && !array.isPointer )
    {
      return ir::NamedConstant{ .name = name,
                                .value = constantOf( *value, type, fileScope ),
                                .isStatic = object.isStatic,
                                .at = declarator.location };
    }
    if ( array.isPointer && array.isConstPointer && named.has_value() && named->value.has_value() && value != nullptr )
    {
      return ir::NamedConstant{ .name = name,
                                .value = constantOf( *value, ir::Type::POINTER, fileScope ),
                                .isStatic = object.isStatic,
                                .at = declarator.location };
    }
    ir::Type const objectType = array.isPointer ? ir::Type::POINTER : type;
    return ir::Global{ .name = name,
                       .type = objectType,
                       .isStatic = object.isStatic,
                       .at = declarator.location,
                       .value = value != nullptr ? std::optional{ constantOf( *value, objectType, fileScope ) }
                                                 : std::nullopt,
                       .isConst = isReadOnly( object ),
                       .placement = wanted };
  }

  static ir::Terminator jumpTo( std::uint32_t block, diag::SourceLocation at )
  {
    return ir::Terminator{
      .kind = ir::TerminatorKind::JUMP, .at = at, .target = block, .otherwise = 0, .condition = {}
    };
  }

  /// Flow that runs off the end of the open block into `block`.
  static void fall( Site const& site, std::uint32_t block )
  {
    if ( site.blocks->isOpen() )
    {
      site.blocks->terminate( jumpTo( block, site.at ) );
    }
  }

  static ir::Function function( std::string name, bool isStatic, diag::SourceLocation at )
  {
    return ir::Function{
      .name = std::move( name ), .isStatic = isStatic, .at = at, .values = {}, .blocks = {}, .then = {}
    };
  }

  /// A function type's Proc: the Temporaries a call through a pointer of it
  /// writes, and the jump that reaches the member — see
  /// docs/decisions/0065-handlers.md. Prune drops it where the program points
  /// through no pointer of the type.
  [[nodiscard]] ir::Function trampoline( FunctionDefinition const& definition )
  {
    ir::Function lowered = function( spellingOf( definition.name ), false, definition.span.begin );
    lowered.isTrampoline = true;
    Definition const* const self = definitionOf( *mSources, *mUnit, definition );
    for ( std::size_t index = 0; self != nullptr && index < definition.parameters.size(); ++index )
    {
      NamedType const& type = self->parameters[index].type;
      lowered.parameters.push_back( ir::Local{ .name = spellingOf( definition.parameters[index].name ),
                                               .type = type.type,
                                               .bytes = mNames->bytes( type.type, type.aggregate ) } );
    }
    if ( self != nullptr && self->result.has_value() )
    {
      lowered.result = self->result->type;
      lowered.resultBytes = mNames->bytes( self->result->type, self->result->aggregate );
    }
    return lowered;
  }

  [[nodiscard]] ir::Function functionDefinition( FunctionDefinition const& definition )
  {
    ir::Function lowered = function( spellingOf( definition.name ), definition.isStatic, definition.span.begin );
    lowered.isInline = definition.isInline;
    lowered.inlineSpan = definition.inlineSpan;
    lowered.isNoinline = hasAttribute( definition.attributes, *mSources, "noinline" );
    std::optional<ir::Placement> const asked = placementAttribute( definition.attributes, *mSources );
    lowered.placement = asked.value_or( ir::Placement::ZEROPAGE );
    mPlacement.assign( 1, lowered.placement );
    mFunctionPlaced = asked.has_value();
    Definition const* const self = definitionOf( *mSources, *mUnit, definition );
    mOwner = lowered.name;
    mPane = self != nullptr ? self->pane : std::string{};
    mUnder = self != nullptr ? self->under : std::string{};
    lowered.implements = self != nullptr ? self->implements : std::string{};
    mShown.clear();
    lowered.pane = mPane;
    lowered.under = mUnder;
    mResult = self != nullptr ? self->result : std::nullopt;
    lowered.result = mResult.has_value() ? std::optional{ mResult->type } : std::nullopt;
    lowered.resultBytes = mResult.has_value() ? mNames->bytes( mResult->type, mResult->aggregate ) : 0;

    Blocks blocks;
    mLoops.clear();
    mCounted.emplace_back();
    // The parameters, and the statements of the body in the same scope, as
    // the check has them.
    mTyping.openScope();

    // A member of a function type reads its arguments from the type's own
    // Temporaries and declares none: one convention per function, whoever
    // calls it — see docs/decisions/0065-handlers.md.
    std::string const member = self != nullptr ? typeOfFunction( self ) : std::string{};
    lowered.memberOf = member;
    mMemberOf = member;
    for ( std::size_t index = 0; self != nullptr && index < definition.parameters.size(); ++index )
    {
      Token const& name = definition.parameters[index].name;
      NamedType const& type = self->parameters[index].type;
      if ( member.empty() )
      {
        lowered.parameters.push_back( ir::Local{
            .name = spellingOf( name ), .type = type.type, .bytes = mNames->bytes( type.type, type.aggregate ) } );
      }
      std::string const byte = member.empty() ? spellingOf( name ) : member + "." + spellingOf( name );
      std::string const owner = member.empty() ? mOwner : std::string{};
      Meaning parameter;
      parameter.kind = NameKind::OBJECT;
      giveType( parameter, type );
      parameter.byte = byte;
      parameter.owner = owner;
      parameter.isParameter = true;
      mTyping.declareLocal( name, parameter );
    }
    for ( Statement const& item : definition.body.items )
    {
      statement( item, lowered, blocks );
    }
    mTyping.closeScope();
    mCounted.pop_back();
    lowered.sections = std::exchange( mSections, {} );
    // Every Section of the function is in its Pane, so that its code runs in
    // one state of the Window — see docs/decisions/0096-panes-in-c.md.
    for ( ir::Global& object : lowered.sections )
    {
      object.pane = mPane;
    }

    // Falling off the end of a function returns from it as `return;` does,
    // under the line of the closing brace; where the last statement ended its
    // block, nothing runs off the end to need one.
    diag::SourceSpan const body = definition.body.span;
    diag::SourceLocation const closing =
        diag::SourceLocation::fromRawOffset( body.begin.rawOffset() + body.length - 1 );
    bool const fallsOff = blocks.isOpen();
    if ( fallsOff )
    {
      blocks.terminate( returning( closing ) );
    }
    blocks.takeInto( lowered );

    // The block entered last is the one that fell off, and a function with a
    // result may not reach it: the caller would read whatever `__ret` held.
    if ( fallsOff && mResult.has_value() &&
         reaches( lowered.blocks, static_cast<std::uint32_t>( lowered.blocks.size() - 1 ) ) )
    {
      mSink->add( diagnostic( diag::DiagnosticId::C_END_REACHED )
                      .at( closing, 1 )
                      .arg( "name", lowered.name )
                      .arg( "type", mTyping.describe( typedAs( *mResult ) ) ) );
    }
    return lowered;
  }

  void statement( Statement const& statement, ir::Function& function, Blocks& blocks )
  {
    Site const site{ .function = &function, .blocks = &blocks, .at = statement.span.begin };
    switch ( statement.kind )
    {
    case StatementKind::COMPOUND:
    {
      // A block carrying only `[[placement]]` is an ordinary block whose
      // declarations lie where it says — see
      // docs/decisions/0210-placement-is-declared-in-c-too.md.
      std::optional<ir::Placement> const asked = placementAttribute( statement.attributes, *mSources );
      Placed const here{ mPlacement, asked };
      if ( !statement.attributes.empty() && !placementOnly( statement.attributes, *mSources ) )
      {
        withBlock( statement, site );
        return;
      }
      mTyping.openScope();
      for ( Statement const& item : statement.items )
      {
        this->statement( item, function, blocks );
      }
      mTyping.closeScope();
      return;
    }
    case StatementKind::CASE:
      mTyping.openScope();
      for ( Statement const& item : statement.items )
      {
        this->statement( item, function, blocks );
      }
      mTyping.closeScope();
      return;
    case StatementKind::DECLARATION:
      declaration( statement, site );
      return;
    case StatementKind::NULL_STATEMENT:
      return;
    case StatementKind::RETURN:
      if ( std::optional<NamedType> const result = mResult; statement.expression != nullptr && result.has_value() )
      {
        // Stored before the addresses of the calls it leaves are taken off. A
        // member leaves its result in its type's byte, whoever called it — see
        // docs/decisions/0065-handlers.md.
        std::string returned = mMemberOf + "." + std::string{ RESULT };
        if ( mMemberOf.empty() )
        {
          returned = function.name == mOwner ? std::string{ RESULT } : mOwner + "." + std::string{ RESULT };
        }
        if ( result->aggregate.has_value() )
        {
          copyInto( ir::Place{ .name = returned },
                    *statement.expression,
                    mNames->bytes( result->type, result->aggregate ),
                    site );
        }
        else
        {
          ir::Operand value = operand( *statement.expression, result->type, site );
          blocks.append( ir::Instruction{
              .operation = ir::Store{ .name = returned, .type = result->type, .value = std::move( value ) },
              .at = site.at } );
        }
      }
      if ( std::string const phase = nameAttribute( statement.attributes, *mSources, "transition" ); !phase.empty() )
      {
        // The Phase is entered here and nothing comes back: the name goes to
        // the text unread, and the assembler resolves it — see
        // docs/decisions/0064-phase-in-c.md.
        blocks.terminate( ir::Terminator{ .kind = ir::TerminatorKind::TRANSITION,
                                          .at = site.at,
                                          .target = 0,
                                          .otherwise = 0,
                                          .condition = {},

                                          .phase = phase } );
        return;
      }
      blocks.terminate( returning( site.at ) );
      return;
    case StatementKind::EXPRESSION:
      expressionStatement( *statement.expression, site );
      return;
    case StatementKind::IF:
      // What the statement declares before its condition lives as long as the
      // statement does — see
      // docs/decisions/0160-a-declaration-where-a-statement-begins-one.md.
      mTyping.openScope();
      initialiser( statement, site );
      ifStatement( statement, site );
      mTyping.closeScope();
      return;
    case StatementKind::WHILE:
    case StatementKind::DO:
    case StatementKind::FOR:
      mTyping.openScope();
      loop( statement, site );
      mTyping.closeScope();
      return;
    case StatementKind::SWITCH:
      mTyping.openScope();
      initialiser( statement, site );
      switchInPlace( statement, site );
      mTyping.closeScope();
      return;
    case StatementKind::BREAK:
    case StatementKind::CONTINUE:
    {
      // The check refuses one no loop and no `switch` stands around, so one
      // stands here: `at` says so loudly were that ever not so, where a
      // fallback would quietly make the `break` something else.
      Loop const& around = mLoops.at( mLoops.size() - 1 );
      blocks.terminate( jumpTo( statement.kind == StatementKind::BREAK ? around.exit : around.next, site.at ) );
      return;
    }
    }
  }

  /// A local is a byte of the Proc being lowered, named after itself — see
  /// docs/decisions/0080-a-local-is-a-byte-of-its-proc.md. A `const` given a
  /// constant is a Constant of the Proc, named as a local is, and a `static`
  /// one is a Section in a Namespace of its function's name, numbered among
  /// the function's own locals wherever it stands, since that is the scope its
  /// name is in — see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
  void declaration( Statement const& node, Site const& site )
  {
    Declaration const& declared = *node.declaration;
    // The innermost `[[placement]]` over a byte the program names is its own —
    // see docs/decisions/0210-placement-is-declared-in-c-too.md.
    Placed const here{ mPlacement, placementAttribute( declared.attributes, *mSources ) };
    // What `auto` took from the value, read again as the check read it; what
    // the check refused is not lowered at all.
    Meaning const type =
        declared.type.keyword == Keyword::AUTO
            ? mTyping.deducedType( declared.declarators.front(), *declared.initialisers.front() ).value_or( Meaning{} )
            : mTyping.declaredType( declared.type ).value_or( Meaning{} );
    for ( std::size_t index = 0; index < declared.declarators.size(); ++index )
    {
      Token const& declarator = declared.declarators[index];
      Expression const* const value = declared.initialisers[index].get();
      DeclaratorShape const& shape = declared.shapes[index];
      Meaning local = localMeaning( type, declared, shape, declarator );
      local.isStriped = shape.isArray && !shape.isPointer && isStriped( declared.attributes, *mSources );
      mTyping.declareLocal( declarator, local );

      if ( shape.isArray )
      {
        localArray( declarator, shape, declared, local, site );
        markVolatile( declarator, local );
        continue;
      }
      if ( local.aggregate.has_value() )
      {
        localBlock( declarator, shape, value, declared, local, site );
        markVolatile( declarator, local );
        continue;
      }

      bool const constant = value != nullptr && isKnown( mTyping.expression( *value ) );
      if ( local.isConst && !local.isVolatile && constant )
      {
        local.byte = "__" + std::to_string( mCounted.back().locals++ ) + spellingOf( declarator );
        local.owner = site.function->name;
        ir::Constant const given = constantOf( *value, local.type, site );
        local.value = given.value;
        site.function->constants.push_back(
            ir::NamedConstant{ .name = local.byte, .value = given, .isStatic = true, .at = declarator.location } );
        mTyping.settleLocal( declarator, local );
        continue;
      }
      // A local whose address is taken is no Temporary: a callee reading
      // through the pointer holds no Reference to it — see
      // docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#pointers--task-3d.
      // Nor is a `volatile` one, which is read and written where the source
      // says, and which no Trace shares — see docs/decisions/0151-volatile.md.
      bool const addressed = mAddressed->contains( declarator.location.rawOffset() );
      if ( declared.isStatic || addressed || local.isVolatile )
      {
        local.byte = "__" + std::to_string( mCounted.front().locals++ ) + spellingOf( declarator );
        local.owner = mOwner;
        mSections.push_back( ir::Global{ .name = local.byte,
                                         .type = local.type,
                                         .isStatic = true,
                                         .at = declarator.location,
                                         .value = declared.isStatic && value != nullptr
                                                      ? std::optional{ constantOf( *value, local.type, site ) }
                                                      : std::nullopt,
                                         .written = spellingOf( declarator ),
                                         .placement = localPlacement( mPlacement, mFunctionPlaced, true, false ) } );
        mTyping.settleLocal( declarator, local );
        markVolatile( declarator, local );
        if ( !declared.isStatic && value != nullptr )
        {
          site.blocks->append( ir::Instruction{ .operation = ir::Store{ .name = nameOf( local, declarator, site ),
                                                                        .type = local.type,
                                                                        .value = operand( *value, local.type, site ) },
                                                .at = site.at } );
        }
        continue;
      }

      local.byte = "__" + std::to_string( mCounted.back().locals++ ) + spellingOf( declarator );
      local.owner = site.function->name;
      site.function->locals.push_back(
          ir::Local{ .name = local.byte,
                     .type = local.type,
                     .placement = localPlacement( mPlacement, mFunctionPlaced, false, false ) } );
      mTyping.settleLocal( declarator, local );

      if ( value != nullptr )
      {
        site.blocks->append( ir::Instruction{
            .operation =
                ir::Store{ .name = local.byte, .type = local.type, .value = operand( *value, local.type, site ) },
            .at = site.at } );
      }
    }
  }

  /// A striped array as an object of stripes: a byte of each element to the
  /// stripe of its offset, and the list it is given where `given` — see
  /// docs/decisions/0089-a-stripe-is-a-section.md.
  [[nodiscard]] ir::Global stripedObject( std::string name,
                                          bool isStatic,
                                          diag::SourceLocation at,
                                          Meaning const& array,
                                          DeclaratorShape const& shape,
                                          bool given,
                                          Site const& site )
  {
    Meaning const element = elementOf( array );
    std::uint32_t const count = array.count.value_or( 0 );
    ir::Type const elementType = array.aggregate.has_value() ? ir::Type::BLOCK : array.type;
    std::uint32_t const size = mNames->bytes( elementType, array.aggregate );
    std::vector<std::vector<std::string>> const paths = stripePaths( elementType, array.aggregate );
    std::vector<ir::Constant> bytes;
    if ( given )
    {
      bytes = bytesOf( array.aggregate.has_value() ? blockConstants( shape, element, count, site )
                                                   : elementsOf( shape, array.type, count, site ) );
    }
    ir::Global striped{ .name = std::move( name ),
                        .type = ir::Type::U8,
                        .isStatic = isStatic,
                        .at = at,
                        .value = std::nullopt,
                        .count = count,
                        .elements = {},
                        .isTemporary = false };
    for ( std::uint32_t stripe = 0; stripe < size && stripe < paths.size(); ++stripe )
    {
      std::vector<ir::Constant> elements;
      for ( std::uint32_t index = 0; !bytes.empty() && index < count; ++index )
      {
        elements.push_back( bytes.at( ( index * size ) + stripe ) );
      }
      striped.stripes.emplace_back( paths[stripe], std::move( elements ) );
    }
    return striped;
  }

  /// The stripe each byte of an element of a striped array lies in, by the
  /// byte's offset: a member's path, under `__hi` for the high byte of one of
  /// two, and `lo` and `hi` for a `u16` or an `i16` — see
  /// docs/decisions/0089-a-stripe-is-a-section.md.
  [[nodiscard]] std::vector<std::vector<std::string>> stripePaths( ir::Type type,
                                                                   std::optional<std::uint32_t> aggregate ) const
  {
    if ( !aggregate.has_value() )
    {
      return { { "lo" }, { "hi" } };
    }
    std::vector<std::vector<std::string>> paths( mNames->bytes( type, aggregate ) );
    std::function<void( std::uint32_t, std::vector<std::string> const&, std::uint32_t )> walk;
    walk = [&]( std::uint32_t index, std::vector<std::string> const& prefix, std::uint32_t base )
    {
      for ( MemberType const& member : mNames->aggregate( index ).members )
      {
        std::vector<std::string> path = prefix;
        path.push_back( spellingOf( member.name ) );
        std::uint32_t const at = base + member.offset;
        if ( member.type.aggregate.has_value() && !member.type.pointee.has_value() )
        {
          walk( *member.type.aggregate, path, at );
          continue;
        }
        if ( at < paths.size() )
        {
          paths[at] = path;
        }
        if ( ir::sizeOf( member.type.type ) == 2 && at + 1 < paths.size() )
        {
          path.insert( path.begin(), "__hi" );
          paths[at + 1] = path;
        }
      }
    };
    walk( *aggregate, {}, 0 );
    return paths;
  }

  /// Constants of one or two bytes as bytes: a name as its low and high byte.
  static std::vector<ir::Constant> bytesOf( std::vector<ir::Constant> const& constants )
  {
    std::vector<ir::Constant> bytes;
    for ( ir::Constant const& constant : constants )
    {
      if ( ir::sizeOf( constant.type ) == 1 )
      {
        bytes.push_back( ir::Constant{ .type = ir::Type::U8, .value = constant.value, .name = constant.name } );
        continue;
      }
      auto const bits = static_cast<std::uint64_t>( constant.value );
      bytes.push_back( ir::Constant{ .type = ir::Type::U8,
                                     .value = static_cast<std::int64_t>( bits & 0xFFU ),
                                     .name = constant.name.empty() ? std::string{} : "<(" + constant.name + ")" } );
      bytes.push_back( ir::Constant{ .type = ir::Type::U8,
                                     .value = static_cast<std::int64_t>( ( bits >> 8U ) & 0xFFU ),
                                     .name = constant.name.empty() ? std::string{} : ">(" + constant.name + ")" } );
    }
    return bytes;
  }

  /// The list an array is given, as many constants as it has elements, those
  /// the list leaves out zero as C has them; nothing for an array given none.
  [[nodiscard]] std::vector<ir::Constant>
  elementsOf( DeclaratorShape const& array, ir::Type type, std::uint32_t count, Site const& site )
  {
    std::vector<ir::Constant> elements;
    if ( !array.hasList )
    {
      return elements;
    }
    elements.reserve( count );
    for ( ExpressionPtr const& element : array.list )
    {
      elements.push_back( constantOf( *element, type, site ) );
    }
    elements.resize( count, ir::Constant{ .type = type, .value = 0, .name = {} } );
    return elements;
  }

  /// A local array: a Section in the Namespace of its function, numbered among
  /// the function's own locals as a `static` one is. A `static` array, and a
  /// `const` one given constants, hold their bytes between calls; any other is
  /// a Temporary its declaration writes the list into, each time it runs — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#arrays--task-3c.
  /// It is no byte of the Proc, so it lies where the innermost `[[placement]]`
  /// over it says and is `absolute` with nothing over it, as a `static` local
  /// is; the `const` one given constants is read-only data, which lies off the
  /// zero page whatever is asked.
  void localArray( Token const& declarator,
                   DeclaratorShape const& array,
                   Declaration const& declared,
                   Meaning local,
                   Site const& site )
  {
    auto count = static_cast<std::uint32_t>( array.list.size() );
    if ( array.size != nullptr )
    {
      count = static_cast<std::uint32_t>( mTyping.fold( *array.size, std::nullopt ) );
    }
    local.isArray = true;
    local.count = count;
    local.byte = "__" + std::to_string( mCounted.front().locals++ ) + spellingOf( declarator );
    local.owner = mOwner;
    mTyping.settleLocal( declarator, local );

    if ( local.isStriped )
    {
      localStriped( declarator, local, array, declared, site );
      return;
    }
    if ( local.aggregate.has_value() )
    {
      Meaning const element = elementOf( local );
      bool const data = declared.isConst && array.hasList && constantList( array, element, count );
      bool const kept = declared.isStatic || data;
      bool const addressedBlock = mAddressed->contains( declarator.location.rawOffset() );
      mSections.push_back( ir::Global{ .name = local.byte,
                                       .type = ir::Type::U8,
                                       .isStatic = true,
                                       .at = declarator.location,
                                       .value = std::nullopt,
                                       .count = count * mNames->bytes( ir::Type::BLOCK, local.aggregate ),
                                       .elements = kept && array.hasList ? blockConstants( array, element, count, site )
                                                                         : std::vector<ir::Constant>{},
                                       .isTemporary = !kept && !addressedBlock,
                                       .isConst = isReadOnly( declared ) && kept,
                                       .placement = localPlacement( mPlacement, mFunctionPlaced, true, data ) } );
      if ( !kept && array.hasList )
      {
        storeList( nameOf( local, declarator, site ), array, element, count, site );
      }
      return;
    }

    bool const constants = std::ranges::all_of(
        array.list, [this]( ExpressionPtr const& element ) { return isLaidDown( mTyping.expression( *element ) ); } );
    bool const data = declared.isConst && constants;
    bool const kept = declared.isStatic || data;
    bool const addressed = mAddressed->contains( declarator.location.rawOffset() );
    mSections.push_back(
        ir::Global{ .name = local.byte,
                    .type = local.type,
                    .isStatic = true,
                    .at = declarator.location,
                    .value = std::nullopt,
                    .count = count,
                    .elements = kept ? elementsOf( array, local.type, count, site ) : std::vector<ir::Constant>{},
                    .isTemporary = !kept && !addressed,
                    .isConst = isReadOnly( declared ) && kept,
                    .placement = localPlacement( mPlacement, mFunctionPlaced, true, data ) } );
    if ( kept || !array.hasList )
    {
      return;
    }
    ir::Constant const base{ .type = ir::Type::POINTER, .value = 0, .name = nameOf( local, declarator, site ) };
    for ( std::uint32_t element = 0; element < count; ++element )
    {
      ir::Operand value = ir::Constant{ .type = local.type, .value = 0, .name = {} };
      if ( element < array.list.size() )
      {
        value = converted( operand( *array.list[element], local.type, site ), local.type, site );
      }
      site.blocks->append( ir::Instruction{
          .operation = ir::Store{ .name = offsetName( base, std::int64_t{ element } * ir::sizeOf( local.type ) ),
                                  .type = local.type,
                                  .value = std::move( value ) },
          .at = site.at } );
    }
  }

  /// A local striped array: its stripes in Namespaces of its name in the
  /// function's Proc, kept as a `static` array or a `const` one given
  /// constants is, and otherwise each a Temporary its declaration writes the
  /// list into — see docs/decisions/0089-a-stripe-is-a-section.md and
  /// docs/decisions/0090-a-namespace-may-stand-in-a-proc.md.
  void localStriped( Token const& declarator,
                     Meaning const& local,
                     DeclaratorShape const& array,
                     Declaration const& declared,
                     Site const& site )
  {
    Meaning const element = elementOf( local );
    bool const constants = local.aggregate.has_value()
                               ? constantList( array, element, local.count.value_or( 0 ) )
                               : std::ranges::all_of( array.list,
                                                      [this]( ExpressionPtr const& listed )
                                                      { return isLaidDown( mTyping.expression( *listed ) ); } );
    bool const kept = declared.isStatic || ( declared.isConst && array.hasList && constants );
    ir::Global striped =
        stripedObject( local.byte, true, declarator.location, local, array, kept && array.hasList, site );
    striped.isTemporary = !kept;
    striped.isConst = isReadOnly( declared ) && kept;
    mSections.push_back( std::move( striped ) );
    if ( kept || !array.hasList )
    {
      return;
    }

    ir::Type const elementType = local.aggregate.has_value() ? ir::Type::BLOCK : local.type;
    std::uint32_t const bytes = mNames->bytes( elementType, local.aggregate );
    Reach const base{ .kind = Reach::Kind::STRIPED,
                      .type = ir::Type::U8,
                      .name = {},
                      .pointer = {},
                      .index = {},
                      .scaled = true,
                      .isArray = false,
                      .array = nameOf( local, declarator, site ),
                      .paths = stripePaths( elementType, local.aggregate ),
                      .offset = 0 };
    Visit const store = [&]( Expression const* value, Meaning const& slot, std::uint32_t offset )
    {
      Reach place = base;
      place.index = ir::Constant{ .type = ir::Type::U8, .value = offset / bytes, .name = {} };
      place.offset = offset % bytes;
      if ( slot.aggregate.has_value() )
      {
        transfer( place, reach( *value, site ), mNames->bytes( ir::Type::BLOCK, slot.aggregate ), site );
        return;
      }
      place.type = slot.pointee.has_value() ? ir::Type::POINTER : slot.type;
      ir::Operand operand = ir::Constant{ .type = place.type, .value = 0, .name = {} };
      if ( value != nullptr )
      {
        operand = converted( this->operand( *value, place.type, site ), place.type, site );
      }
      write( place, std::move( operand ), site );
    };
    std::uint32_t const count = local.count.value_or( 0 );
    for ( std::uint32_t index = 0; index < count; ++index )
    {
      visitValue( index < array.list.size() ? array.list[index].get() : nullptr, element, index * bytes, store );
    }
  }

  /// Whether a list gives a `struct` or a `union`, or each element of an
  /// array of them, only what a `static` one may be given: constants, known
  /// addresses and `nullptr`, and no whole value, which is copied.
  bool constantList( DeclaratorShape const& shape, Meaning const& element, std::uint32_t count )
  {
    bool constant = true;
    Visit const check = [&]( Expression const* value, Meaning const& slot, std::uint32_t /*offset*/ )
    {
      if ( value == nullptr )
      {
        return;
      }
      constant = constant && !slot.aggregate.has_value() && isLaidDown( mTyping.expression( *value ) );
    };
    if ( !shape.isArray )
    {
      visitAggregate( shape.list, element.aggregate.value_or( 0 ), 0, check );
      return constant;
    }
    std::uint32_t const bytes = mNames->bytes( ir::Type::BLOCK, element.aggregate );
    for ( std::uint32_t index = 0; index < count && index < shape.list.size(); ++index )
    {
      visitValue( shape.list[index].get(), element, index * bytes, check );
    }
    return constant;
  }

  /// What a list sets, at the offset it sets it, from `base`: every scalar a
  /// value is given for, and every one it leaves out, which is zero; a value
  /// of a `struct` or a `union` itself, which is copied.
  using Visit = std::function<void( Expression const* value, Meaning const& slot, std::uint32_t offset )>;

  void visitValue( Expression const* value, Meaning const& slot, std::uint32_t offset, Visit const& visit )
  {
    if ( !slot.aggregate.has_value() || ( value != nullptr && value->kind != ExpressionKind::LIST ) )
    {
      visit( value, slot, offset );
      return;
    }
    static std::vector<ExpressionPtr> const NO_ELEMENTS;
    visitAggregate( value != nullptr ? value->arguments : NO_ELEMENTS, slot.aggregate.value_or( 0 ), offset, visit );
  }

  void visitAggregate( std::vector<ExpressionPtr> const& elements,
                       std::uint32_t aggregateIndex,
                       std::uint32_t base,
                       Visit const& visit )
  {
    AggregateType const& type = mNames->aggregate( aggregateIndex );
    std::size_t const members = type.isUnion ? std::min<std::size_t>( 1, type.members.size() ) : type.members.size();
    for ( std::size_t index = 0; index < members; ++index )
    {
      MemberType const& member = type.members[index];
      Meaning slot;
      slot.kind = NameKind::OBJECT;
      giveType( slot, member.type );
      Expression const* const value = index < elements.size() ? elements[index].get() : nullptr;
      std::uint32_t const at = base + member.offset;
      if ( !member.isArray )
      {
        visitValue( value, slot, at, visit );
        continue;
      }
      std::uint32_t const bytes = mNames->bytes( slot.type, slot.aggregate );
      for ( std::uint32_t element = 0; element < member.count; ++element )
      {
        Expression const* const given =
            value != nullptr && element < value->arguments.size() ? value->arguments[element].get() : nullptr;
        visitValue( given, slot, at + ( element * bytes ), visit );
      }
    }
  }

  /// The bytes a `struct`, a `union` or an array of them is given, as
  /// constants of the widths of their members, zero where nothing is given.
  [[nodiscard]] std::vector<ir::Constant>
  blockConstants( DeclaratorShape const& shape, Meaning const& element, std::uint32_t count, Site const& site )
  {
    std::map<std::uint32_t, ir::Constant> given;
    Visit const collect = [&]( Expression const* value, Meaning const& slot, std::uint32_t offset )
    {
      ir::Type const type = slot.pointee.has_value() ? ir::Type::POINTER : slot.type;
      given.insert_or_assign( offset,
                              value != nullptr ? constantOf( *value, type, site )
                                               : ir::Constant{ .type = type, .value = 0, .name = {} } );
    };
    std::uint32_t const bytes = mNames->bytes( ir::Type::BLOCK, element.aggregate );
    for ( std::uint32_t index = 0; index < count; ++index )
    {
      Expression const* value = nullptr;
      if ( shape.isArray && index < shape.list.size() )
      {
        value = shape.list[index].get();
      }
      if ( shape.isArray )
      {
        visitValue( value, element, index * bytes, collect );
      }
      else
      {
        visitAggregate( shape.list, element.aggregate.value_or( 0 ), 0, collect );
      }
    }
    std::vector<ir::Constant> elements;
    for ( std::uint32_t offset = 0; offset < count * bytes; )
    {
      if ( auto const found = given.find( offset ); found != given.end() )
      {
        elements.push_back( found->second );
        offset += ir::sizeOf( found->second.type );
        continue;
      }
      elements.push_back( ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} } );
      ++offset;
    }
    return elements;
  }

  /// A list written into bytes where a local is declared, each time it runs:
  /// a store per scalar, zero where nothing is given, and a copy per value of
  /// a `struct` or a `union`.
  void storeList( std::string const& name,
                  DeclaratorShape const& shape,
                  Meaning const& element,
                  std::uint32_t count,
                  Site const& site )
  {
    Visit const store = [&]( Expression const* value, Meaning const& slot, std::uint32_t offset )
    {
      ir::Constant const base{ .type = ir::Type::POINTER, .value = 0, .name = name };
      if ( slot.aggregate.has_value() )
      {
        copyInto( ir::Place{ .name = offsetName( base, offset ) },
                  *value,
                  mNames->bytes( ir::Type::BLOCK, slot.aggregate ),
                  site );
        return;
      }
      ir::Type const type = slot.pointee.has_value() ? ir::Type::POINTER : slot.type;
      ir::Operand operand = ir::Constant{ .type = type, .value = 0, .name = {} };
      if ( value != nullptr )
      {
        operand = converted( this->operand( *value, type, site ), type, site );
      }
      site.blocks->append( ir::Instruction{
          .operation = ir::Store{ .name = offsetName( base, offset ), .type = type, .value = std::move( operand ) },
          .at = site.at } );
    };
    std::uint32_t const bytes = mNames->bytes( ir::Type::BLOCK, element.aggregate );
    if ( !shape.isArray )
    {
      visitAggregate( shape.list, element.aggregate.value_or( 0 ), 0, store );
      return;
    }
    for ( std::uint32_t index = 0; index < count; ++index )
    {
      visitValue( index < shape.list.size() ? shape.list[index].get() : nullptr, element, index * bytes, store );
    }
  }

  /// A local `struct` or `union`: a `.ztemp` of four bytes or fewer and a
  /// `.temp` past that, or a Section of its own where it is `static` or its
  /// address is taken — see docs/decisions/0086-a-struct-by-value.md. A list
  /// is written into it, and a value copied, where it is declared.
  void localBlock( Token const& declarator,
                   DeclaratorShape const& shape,
                   Expression const* value,
                   Declaration const& declared,
                   Meaning local,
                   Site const& site )
  {
    std::uint32_t const bytes = mNames->bytes( ir::Type::BLOCK, local.aggregate );
    bool const addressed = mAddressed->contains( declarator.location.rawOffset() );
    // A `const` one given constants is kept as a `static` one is: nothing
    // can tell them apart, a local's bytes being its Proc's in every call.
    // It is read-only data, which lies off the zero page whatever is asked.
    bool const data = declared.isConst && shape.hasList && constantList( shape, local, 1 );
    bool const fixed = declared.isStatic || data;
    bool const kept = fixed || addressed;
    if ( kept || bytes > SMALL_BLOCK )
    {
      local.byte = "__" + std::to_string( mCounted.front().locals++ ) + spellingOf( declarator );
      local.owner = mOwner;
      mSections.push_back( ir::Global{
          .name = local.byte,
          .type = ir::Type::U8,
          .isStatic = true,
          .at = declarator.location,
          .value = std::nullopt,
          .count = bytes,
          .elements = fixed && shape.hasList ? blockConstants( shape, local, 1, site ) : std::vector<ir::Constant>{},
          .isTemporary = !kept,
          .written = spellingOf( declarator ),
          // Past a few bytes it lies off the zero
          // page whatever is asked, as a Temporary
          // block does.
          .placement = localPlacement(
              mPlacement, mFunctionPlaced, declared.isStatic || addressed, data || bytes > SMALL_BLOCK ) } );
    }
    else
    {
      local.byte = "__" + std::to_string( mCounted.back().locals++ ) + spellingOf( declarator );
      local.owner = site.function->name;
      site.function->locals.push_back(
          ir::Local{ .name = local.byte,
                     .type = ir::Type::BLOCK,
                     .bytes = bytes,
                     .placement = localPlacement( mPlacement, mFunctionPlaced, false, false ) } );
    }
    mTyping.settleLocal( declarator, local );
    if ( fixed )
    {
      return;
    }
    std::string const name = nameOf( local, declarator, site );
    if ( shape.hasList )
    {
      storeList( name, shape, local, 1, site );
    }
    else if ( value != nullptr )
    {
      copyInto( ir::Place{ .name = name }, *value, bytes, site );
    }
  }

  /// The name an object is written by from where the lowering stands: a local
  /// of another Proc is named through it, `f.__0i`.
  /// A translated character as the assembler writes it: its own token, or
  /// how a string literal holding it spells it.
  [[nodiscard]] std::string spellingOfCharacter( Expression const& node ) const
  {
    return node.spelling.empty() ? spellingOf( node.token ) : node.spelling;
  }

  /// The Section a string literal's bytes and its end lie in, at file scope:
  /// one for equal literals of a file — see
  /// docs/decisions/0095-literals-and-the-runtime.md.
  std::string literalSection( Expression const& node )
  {
    std::string key;
    for ( ExpressionPtr const& character : node.arguments )
    {
      key += character->spelling;
    }
    auto const [found, added] = mLiteralNames.try_emplace( key, "__s" + std::to_string( mLiteralNames.size() ) );
    if ( !added )
    {
      return found->second;
    }
    ir::Global literal{ .name = found->second,
                        .type = ir::Type::U8,
                        .isStatic = true,
                        .at = node.token.location,
                        .value = std::nullopt,
                        .count = static_cast<std::uint32_t>( node.arguments.size() ),
                        .elements = {},
                        .isTemporary = false };
    for ( ExpressionPtr const& character : node.arguments )
    {
      literal.elements.push_back(
          mTyping.isTranslated( *character )
              ? ir::Constant{ .type = ir::Type::U8, .value = 0, .name = spellingOfCharacter( *character ) }
              : ir::Constant{ .type = ir::Type::U8, .value = character->value, .name = {} } );
    }
    mLiterals.push_back( std::move( literal ) );
    return found->second;
  }

  /// A local written `volatile`, by the name its lowered bytes have.
  void markVolatile( Token const& declarator, Meaning const& local )
  {
    if ( !local.isVolatile )
    {
      return;
    }
    std::string const spelling = spellingOf( declarator );
    Meaning const settled = mTyping.lookupSpelling( spelling ).value_or( Meaning{} );
    mVolatileNames.insert( settled.byte.empty() ? spelling : settled.byte );
  }

  [[nodiscard]] std::string nameOf( Meaning const& meaning, Token const& token, Site const& site ) const
  {
    if ( meaning.byte.empty() )
    {
      return spellingOf( token );
    }
    if ( meaning.owner.empty() || meaning.owner == site.function->name )
    {
      // A byte of no owner is named whole: a member's arguments are the
      // function type's — see docs/decisions/0065-handlers.md.
      return meaning.byte;
    }
    return meaning.owner + "." + meaning.byte;
  }

  /// A `[[with]]` block: its entry, then its statements as a region of
  /// blocks of their own, which the text writes as a macro used once under
  /// `.with` — see docs/decisions/0096-panes-in-c.md. The check held the
  /// attribute to one of the four forms and the index to a `u8` or a
  /// constant; a constant index of a family names the member, `FAMILY + n`.
  void withBlock( Statement const& node, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    std::optional<Attribute> const attribute = withOf( node.attributes, *mSources );
    std::optional<WithSpec> const spec = attribute.has_value() ? withAttribute( *attribute ) : std::nullopt;
    if ( !spec.has_value() )
    {
      return;
    }
    std::string const name = spellingOf( spec->name );
    std::optional<Meaning> const meaning = mTyping.lookupName( name );
    ExternalName const* const external =
        meaning.has_value() && meaning->definition == nullptr ? meaning->external : nullptr;
    if ( external == nullptr )
    {
      return;
    }
    bool const isWindow = external->kind == ExternalKind::WINDOW;
    std::string const label = "__w" + std::to_string( mWiths++ );
    if ( !isTrampoline( node.attributes, *mSources ) )
    {
      region( node, *spec, name, isWindow ? name : external->window, label, site );
      return;
    }

    // A Trampoline: a Proc of its own in `fixed`, running `under` the
    // function's Pane, holding the `.with` over the block's body and called
    // by `jsr` — see docs/decisions/0097-a-trampoline-is-declared.md and
    // docs/decisions/0098-a-proc-declares-what-is-shown.md. The block's
    // locals are the Proc's; what it names of the function's is qualified.
    ir::Function proc = function( label, true, site.at );
    proc.under = mPane;
    Blocks inner;
    std::vector<Loop> const outer = std::exchange( mLoops, {} );
    mCounted.emplace_back();
    Site const in{ .function = &proc, .blocks = &inner, .at = site.at };
    region( node, *spec, name, isWindow ? name : external->window, label + "b", in );
    inner.terminate( returning( site.at ) );
    inner.takeInto( proc );
    mCounted.pop_back();
    mLoops = outer;
    mExtra.push_back( std::move( proc ) );
    blocks.append( ir::Instruction{ .operation = ir::Call{ .name = label, .arguments = {}, .result = std::nullopt },
                                    .at = site.at } );
  }

  /// The entry of a block and its statements as a region of `site`'s blocks:
  /// the macro `body` used once under `.with`, showing `window`.
  void region( Statement const& node,
               WithSpec const& spec,
               std::string const& name,
               std::string window,
               std::string body,
               Site const& site )
  {
    Blocks& blocks = *site.blocks;
    bool const isWindow = window == name;
    ir::EnterWith enter{ .form = spec.form, .name = name, .body = std::move( body ) };
    if ( std::optional<Meaning> const meaning = mTyping.lookupName( name );
         meaning.has_value() && meaning->external != nullptr )
    {
      enter.stateBytes = meaning->external->stateBytes;
    }
    Shown shown{ .window = std::move( window ) };
    switch ( spec.form )
    {
    case ir::WithForm::PANE:
      shown.pane = name;
      break;
    case ir::WithForm::AT:
    {
      if ( !spec.index.has_value() )
      {
        return;
      }
      Token const& index = *spec.index;
      if ( !isWindow )
      {
        shown.pane = name;
      }
      if ( index.kind == TokenKind::INTEGER_CONSTANT )
      {
        std::int64_t const value = valueOfIntegerConstant( spellingOf( index ) ).value_or( 0 );
        if ( isWindow )
        {
          enter.index = ir::Constant{ .type = ir::Type::U8, .value = value, .name = {} };
        }
        else
        {
          enter.form = ir::WithForm::PANE;
          enter.name = name + " + " + std::to_string( value );
        }
        break;
      }
      Meaning const held = mTyping.lookupSpelling( spellingOf( index ) ).value_or( Meaning{} );
      enter.index = ir::Object{ .name = nameOf( held, index, site ), .type = ir::Type::U8 };
      enter.member = !isWindow;
      break;
    }
    case ir::WithForm::STATE:
      if ( !spec.state.has_value() )
      {
        return;
      }
      enter.state = spellingOf( *spec.state );
      shown.state = enter.state;
      break;
    }
    blocks.append( ir::Instruction{ .operation = std::move( enter ), .at = site.at } );
    std::uint32_t const first = blocks.create();
    fall( site, first );
    blocks.enter( first );
    mShown.push_back( shown );
    mTyping.openScope();
    for ( Statement const& item : node.items )
    {
      statement( item, *site.function, blocks );
    }
    mTyping.closeScope();
    mShown.pop_back();
    std::uint32_t const after = blocks.create();
    fall( site, after );
    blocks.enter( after );
    blocks.region( first, after );
  }

  /// The Pane a call into `pane` is wrapped in `.with` for: none where the
  /// callee is in the caller's own Pane or in what a block around the call
  /// shows — see docs/decisions/0096-panes-in-c.md.
  [[nodiscard]] std::string withFor( std::string const& pane ) const
  {
    if ( pane.empty() || pane == mPane || pane == mUnder )
    {
      return {};
    }
    std::optional<Meaning> const meaning = mTyping.lookupName( pane );
    ExternalName const* const external =
        meaning.has_value() && meaning->definition == nullptr ? meaning->external : nullptr;
    if ( external == nullptr || external->kind != ExternalKind::PANE )
    {
      return {};
    }
    if ( std::ranges::any_of( mShown, [&]( Shown const& block ) { return shows( block, *external ); } ) )
    {
      return {};
    }
    return pane;
  }

  void ifStatement( Statement const& node, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    bool const hasElse = node.items.size() > 1;
    std::uint32_t const then = blocks.create();
    std::uint32_t const otherwise = hasElse ? blocks.create() : 0;
    std::uint32_t const join = blocks.create();

    condition( *node.expression, then, hasElse ? otherwise : join, site );
    blocks.enter( then );
    statement( node.items.front(), *site.function, blocks );
    fall( site, join );
    if ( hasElse )
    {
      blocks.enter( otherwise );
      statement( node.items.back(), *site.function, blocks );
      fall( site, join );
    }
    blocks.enter( join );
  }

  /// What a statement was written with before its condition: a declaration
  /// whose stores stand where it was written, or an expression statement.
  void initialiser( Statement const& node, Site const& site )
  {
    if ( node.initial != nullptr )
    {
      expressionStatement( *node.initial, site );
    }
    if ( node.declaration != nullptr )
    {
      declaration( node, site );
    }
  }

  /// A loop's blocks in the order they are written: the test at the head for
  /// `while` and `for`, and at the foot for `do`.
  void loop( Statement const& node, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    initialiser( node, site );

    if ( node.kind == StatementKind::DO )
    {
      std::uint32_t const body = blocks.create();
      std::uint32_t const test = blocks.create();
      std::uint32_t const exit = blocks.create();
      fall( site, body );
      blocks.enter( body );
      governed( node, Loop{ .exit = exit, .next = test }, site );
      fall( site, test );
      blocks.enter( test );
      condition( *node.expression, body, exit, site );
      blocks.enter( exit );
      return;
    }

    std::uint32_t const head = blocks.create();
    std::uint32_t const body = blocks.create();
    std::uint32_t const step = node.step != nullptr ? blocks.create() : head;
    std::uint32_t const exit = blocks.create();
    fall( site, head );
    blocks.enter( head );
    if ( node.expression != nullptr )
    {
      condition( *node.expression, body, exit, site );
    }
    else
    {
      blocks.terminate( jumpTo( body, site.at ) );
    }
    blocks.enter( body );
    governed( node, Loop{ .exit = exit, .next = step }, site );
    if ( node.step != nullptr )
    {
      fall( site, step );
      blocks.enter( step );
      expressionStatement( *node.step, site );
    }
    fall( site, head );
    blocks.enter( exit );
  }

  /// The statement a loop governs, with `break` and `continue` going where
  /// `loop` says.
  void governed( Statement const& node, Loop where, Site const& site )
  {
    mLoops.push_back( where );
    statement( node.items.front(), *site.function, *site.blocks );
    mLoops.pop_back();
  }

  /// What each label of a `switch` stands for: the value, and the body it
  /// runs by its place among the bodies.
  struct Labelled
  {
    std::int64_t value = 0;
    std::size_t body = 0;

    /// The clause the label is written in, by its place among the
    /// `switch`'s, `default` counted: where its compare is marked as from.
    std::size_t clause = 0;
  };

  /// A `switch`, written where it stands: the bodies are blocks of the
  /// function's own Proc, `break` a jump past them all and a body that does
  /// not break falling into the next. Few labels are a chain of compares and
  /// branches ([0164](docs/decisions/0164-a-small-switch-stands-where-it-is.md));
  /// more are a `.dispatch` over a table spanning them, which goes to
  /// positions of this Proc and so costs the Proc nothing — see
  /// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
  void switchInPlace( Statement const& node, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    Typed const kind = mTyping.expression( *node.expression );
    ir::Type const type = kind.type.value_or( ir::Type::U8 );

    // The value is read once: where it is an object the compares read it
    // there, and anything else — a call, an operator, a `volatile` byte —
    // goes into a byte of the Proc first.
    ir::Operand control = operand( *node.expression, type, site );
    auto const* const object = std::get_if<ir::Object>( &control );
    if ( object == nullptr || mVolatileNames.contains( ir::baseOf( object->name ) ) )
    {
      std::string const name = "__v" + std::to_string( mCounted.back().tested++ );
      site.function->locals.push_back(
          ir::Local{ .name = name, .type = type, .bytes = 0, .placement = mPlacement.front() } );
      site.blocks->append( ir::Instruction{
          .operation = ir::Store{ .name = name, .type = type, .value = std::move( control ) }, .at = site.at } );
      control = ir::Object{ .name = name, .type = type };
    }

    // Which body each label runs, and which the labels with no body of their
    // own run: the one written next, as a case with no `break` falls into it.
    std::vector<Labelled> labels;
    std::vector<std::size_t> bodies;
    std::optional<std::size_t> defaultBody;
    std::vector<std::size_t> pending;
    bool pendingDefault = false;
    for ( std::size_t index = 0; index < node.items.size(); ++index )
    {
      Statement const& clause = node.items[index];
      if ( clause.expression == nullptr )
      {
        pendingDefault = true;
      }
      else
      {
        pending.push_back( labels.size() );
        labels.push_back( Labelled{ .value = labelValue( *clause.expression, type ), .body = 0, .clause = index } );
      }
      if ( clause.items.empty() )
      {
        continue;
      }
      for ( std::size_t const at : pending )
      {
        labels[at].body = bodies.size();
      }
      if ( pendingDefault )
      {
        defaultBody = bodies.size();
      }
      pending.clear();
      pendingDefault = false;
      bodies.push_back( index );
    }
    // Labels after the last body run nothing, and so does `default` there.
    for ( std::size_t const at : pending )
    {
      labels[at].body = bodies.size();
    }

    SwitchForm const form = formOf( mTyping.labelValues( node, type ) );

    std::vector<std::uint32_t> blockOf;
    blockOf.reserve( bodies.size() + 1 );
    for ( std::size_t body = 0; body < bodies.size(); ++body )
    {
      blockOf.push_back( blocks.create() );
    }
    std::uint32_t const join = blocks.create();
    blockOf.push_back( join );
    auto const bodyBlock = [&blockOf]( std::size_t body ) { return blockOf[body]; };

    // The table: one entry per value the labels span, holding the body that
    // value runs — a label's, or `default`'s where no label names it, or the
    // way past the whole of it. The clamp puts everything outside the span
    // into one entry more at the end, which holds the same; a byte spanning
    // every value it has is inside whatever it holds, and is clamped to
    // nothing — see docs/decisions/0166-a-tables-span-is-its-labels.md.
    if ( !form.inPlace )
    {
      bool const clamps = ir::sizeOf( type ) == 2 || form.span < MAX_TABLE;
      auto const entries = static_cast<std::size_t>( form.span ) + ( clamps ? 1 : 0 );
      std::vector<std::uint32_t> targets( entries, bodyBlock( defaultBody.value_or( bodies.size() ) ) );
      for ( Labelled const& one : labels )
      {
        targets[static_cast<std::size_t>( one.value - form.first )] = bodyBlock( one.body );
      }
      site.blocks->append( ir::Instruction{ .operation = ir::Switch{ .value = control,
                                                                     .count = static_cast<std::uint32_t>( form.span ),
                                                                     .first = form.first,
                                                                     .type = type },
                                            .at = site.at } );
      blocks.terminate( ir::Terminator{ .kind = ir::TerminatorKind::DISPATCH,
                                        .at = site.at,
                                        .target = 0,
                                        .otherwise = 0,
                                        .condition = {},
                                        .phase = {},
                                        .targets = std::move( targets ) } );
    }

    // The chain: each label a compare and a branch, the last falling through
    // to `default`'s body or past the whole of it.
    for ( std::size_t at = 0; form.inPlace && at < labels.size(); ++at )
    {
      Statement const& clause = node.items[labels[at].clause];
      std::uint32_t const otherwise =
          at + 1 < labels.size() ? blocks.create() : bodyBlock( defaultBody.value_or( bodies.size() ) );
      ir::Value const decided = define( site, ir::Type::BOOL );
      site.blocks->append( ir::Instruction{
          .operation = ir::Compare{ .result = decided,
                                    .op = ir::Comparison::EQUAL,
                                    .type = type,
                                    .left = control,
                                    .right = ir::Constant{ .type = type, .value = labels[at].value, .name = {} } },
          .at = clause.span.begin } );
      blocks.terminate( ir::Terminator{
          .kind = ir::TerminatorKind::BRANCH,
          .at = clause.span.begin,
          .target = bodyBlock( labels[at].body ),
          .otherwise = otherwise,
          .condition = decided,
      } );
      if ( at + 1 < labels.size() )
      {
        blocks.enter( otherwise );
      }
    }
    if ( form.inPlace && labels.empty() )
    {
      fall( site, bodyBlock( defaultBody.value_or( bodies.size() ) ) );
    }

    // The bodies, in the order written: `break` leaves them all, and what
    // runs off the end of one runs into the next.
    // `break` leaves the `switch`; `continue` goes where the loop around it
    // sends one, there being no Proc between a case and that loop.
    mLoops.push_back( Loop{ .exit = join, .next = mLoops.empty() ? join : mLoops.back().next } );
    for ( std::size_t body = 0; body < bodies.size(); ++body )
    {
      blocks.enter( bodyBlock( body ) );
      statement( node.items[bodies[body]], *site.function, blocks );
      fall( site, bodyBlock( body + 1 ) );
    }
    mLoops.pop_back();
    blocks.enter( join );
  }

  /// What a label of a `switch` stands for: an enumerator's place in its
  /// type, or the constant it is written as.
  [[nodiscard]] std::int64_t labelValue( Expression const& label, ir::Type type )
  {
    if ( std::optional<std::uint32_t> const enumerator = mNames->enumeratorIndex( *mUnit, label ) )
    {
      return *enumerator;
    }
    return wrapped( mTyping.fold( label, type ), type );
  }

  /// A condition as branches: to `whenTrue` where it holds and `whenFalse`
  /// where it does not, `&&` and `||` taking their right side only where the
  /// left has not decided, as C has them.
  void condition( Expression const& node, std::uint32_t whenTrue, std::uint32_t whenFalse, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    if ( node.kind == ExpressionKind::BINARY && isLogical( node.token.kind ) )
    {
      std::uint32_t const middle = blocks.create();
      if ( node.token.kind == TokenKind::AMPERSAND_AMPERSAND )
      {
        condition( *node.left, middle, whenFalse, site );
      }
      else
      {
        condition( *node.left, whenTrue, middle, site );
      }
      blocks.enter( middle );
      condition( *node.right, whenTrue, whenFalse, site );
      return;
    }
    if ( node.kind == ExpressionKind::UNARY && node.token.kind == TokenKind::BANG )
    {
      condition( *node.left, whenFalse, whenTrue, site );
      return;
    }

    Typed const kind = mTyping.expression( node );
    bool const typedKnown = kind.known && kind.type.has_value();
    if ( typedKnown || kind.decided.has_value() )
    {
      bool const holds = typedKnown ? mTyping.fold( node, ir::Type::BOOL ) != 0 : kind.decided.value_or( false );
      blocks.terminate( jumpTo( holds ? whenTrue : whenFalse, site.at ) );
      return;
    }
    ir::Operand value = operand( node, ir::Type::BOOL, site );
    blocks.terminate( ir::Terminator{
        .kind = ir::TerminatorKind::BRANCH,
        .at = site.at,
        .target = whenTrue,
        .otherwise = whenFalse,
        .condition = std::move( value ),
    } );
  }

  /// A call through a pointer of a function type: a call of the type's own
  /// Proc, whose Temporaries hold the arguments and whose `ptr` holds where
  /// to go — see docs/decisions/0065-handlers.md. The pointer is written last,
  /// as any argument is, so a call made while computing one finds it whole.
  std::optional<ir::Value> throughPointer( Expression const& node, std::uint32_t type, Site const& site, bool read )
  {
    FunctionType const& named = mNames->functionType( type );
    std::vector<ir::Argument> arguments;
    for ( std::size_t index = 0; index < node.arguments.size() && index < named.parameters.size(); ++index )
    {
      ParameterType const& parameter = named.parameters[index];
      std::string const argumentName = named.name + "." + spellingOf( parameter.name );
      if ( parameter.type.aggregate.has_value() )
      {
        arguments.push_back( ir::Argument{ .name = argumentName,
                                           .type = ir::Type::BLOCK,
                                           .value = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} },
                                           .from = placeOf( *node.arguments[index], site ),
                                           .bytes = mNames->bytes( ir::Type::BLOCK, parameter.type.aggregate ) } );
        continue;
      }
      arguments.push_back( ir::Argument{ .name = argumentName,
                                         .type = parameter.type.type,
                                         .value = operand( *node.arguments[index], parameter.type.type, site ) } );
    }
    arguments.push_back( ir::Argument{ .name = named.name + "." + std::string{ TRAMPOLINE_POINTER },
                                       .type = ir::Type::POINTER,
                                       .value = operand( *node.left, ir::Type::POINTER, site ) } );
    std::optional<ir::Value> result;
    if ( read && named.result.has_value() && !named.result->aggregate.has_value() )
    {
      result = define( site, named.result->type );
    }
    site.blocks->append( ir::Instruction{
        .operation = ir::Call{ .name = named.name, .arguments = std::move( arguments ), .result = result, .with = {} },
        .at = site.at } );
    return result;
  }

  /// A call: every argument computed first, in the order written, and each
  /// then written to its parameter's byte by the call itself, so that no call
  /// made while computing one meets a byte already written — see
  /// docs/decisions/0082-a-call-writes-the-callees-bytes.md. Its value, where
  /// `read` asks for one, is the callee's `__ret`, read right after it.
  std::optional<ir::Value> call( Expression const& node, Site const& site, bool read )
  {
    std::string const name = spellingOf( node.left->token );
    Meaning const meaning = mTyping.resolve( *node.left ).value_or( Meaning{} );
    Definition const* const callee = meaning.kind == NameKind::FUNCTION ? meaning.definition : nullptr;
    if ( meaning.kind == NameKind::ASSEMBLER_PROC )
    {
      return assemblerCall( node, meaning, read, site );
    }
    if ( meaning.kind == NameKind::OBJECT && meaning.pointee.has_value() && meaning.pointee->function.has_value() )
    {
      return throughPointer( node, *meaning.pointee->function, site, read );
    }

    // A member reads its arguments from its type's Temporaries, so a direct
    // call writes them too — see docs/decisions/0065-handlers.md.
    std::string const bytesOwner = typeOfFunction( callee );
    std::vector<ir::Argument> arguments;
    for ( std::size_t index = 0; callee != nullptr && index < node.arguments.size(); ++index )
    {
      ParameterType const& parameter = callee->parameters.at( index );
      std::string const argumentName = ( bytesOwner.empty() ? name : bytesOwner ) + "." + spellingOf( parameter.name );
      if ( parameter.type.aggregate.has_value() )
      {
        arguments.push_back( ir::Argument{ .name = argumentName,
                                           .type = ir::Type::BLOCK,
                                           .value = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} },
                                           .from = placeOf( *node.arguments[index], site ),
                                           .bytes = mNames->bytes( ir::Type::BLOCK, parameter.type.aggregate ) } );
        continue;
      }
      ir::Operand value = operand( *node.arguments[index], parameter.type.type, site );
      arguments.push_back(
          ir::Argument{ .name = argumentName, .type = parameter.type.type, .value = std::move( value ) } );
    }
    std::optional<ir::Value> result;
    if ( read && callee != nullptr && callee->result.has_value() && !callee->result->aggregate.has_value() )
    {
      result = define( site, callee->result->type );
    }
    site.blocks->append( ir::Instruction{
        .operation =
            ir::Call{ .name = name,
                      .arguments = std::move( arguments ),
                      .result = result,
                      .returned = bytesOwner.empty() ? std::string{} : bytesOwner + "." + std::string{ RESULT },
                      .with = withFor( callee != nullptr ? callee->pane : std::string{} ) },
        .at = site.at } );
    return result;
  }

  /// A statement the second pass let through: a call of a name, or a value
  /// assigned to one.
  /// A call of an assembler's `.proc`: each argument written to the byte its
  /// `.declare arg` names, and the result read from its `.declare ret`'s.
  std::optional<ir::Value> assemblerCall( Expression const& node, Meaning const& meaning, bool read, Site const& site )
  {
    ExternalName const& proc = *meaning.external;
    Definition const* const declared = meaning.definition;
    std::vector<ir::Argument> arguments;
    for ( std::size_t index = 0; index < node.arguments.size(); ++index )
    {
      ExternalByte const& byte = proc.arguments.at( index );
      if ( declared != nullptr )
      {
        // As a call of C passes its function's parameters: a `struct` copied.
        NamedType const& parameter = declared->parameters.at( index ).type;
        if ( parameter.aggregate.has_value() && !parameter.pointee.has_value() )
        {
          arguments.push_back( ir::Argument{ .name = byte.name,
                                             .type = ir::Type::BLOCK,
                                             .value = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} },
                                             .from = placeOf( *node.arguments[index], site ),
                                             .bytes = mNames->bytes( ir::Type::BLOCK, parameter.aggregate ) } );
          continue;
        }
        arguments.push_back( ir::Argument{ .name = byte.name,
                                           .type = parameter.type,
                                           .value = operand( *node.arguments[index], parameter.type, site ),
                                           .from = std::nullopt,
                                           .bytes = 0,
                                           .place = byte.place } );
        continue;
      }
      arguments.push_back( ir::Argument{ .name = byte.name,
                                         .type = byte.type,
                                         .value = operand( *node.arguments[index], byte.type, site ),
                                         .from = std::nullopt,
                                         .bytes = 0,
                                         .place = byte.place } );
    }
    std::optional<ir::Value> result;
    bool const blockResult =
        declared != nullptr && declared->result.has_value() && declared->result->aggregate.has_value();
    if ( read && proc.result.has_value() && !blockResult )
    {
      result = define(
          site, declared != nullptr && declared->result.has_value() ? declared->result->type : proc.result->type );
    }
    site.blocks->append( ir::Instruction{
        .operation = ir::Call{ .name = spellingOf( node.left->token ),
                               .arguments = std::move( arguments ),
                               .result = result,
                               .returned = proc.result.has_value() ? proc.result->name : std::string{},
                               .with = withFor( proc.pane ),
                               .resultPlace = proc.result.has_value() ? proc.result->place : std::string{} },
        .at = site.at } );
    return result;
  }

  void expressionStatement( Expression const& node, Site const& site )
  {
    if ( node.kind == ExpressionKind::CALL )
    {
      call( node, site, false );
      return;
    }
    if ( Typed const target = mTyping.expression( *node.left ); target.aggregate.has_value() )
    {
      // Where the bytes go first, then where they come from, as written.
      std::uint32_t const bytes = mNames->bytes( ir::Type::BLOCK, target.aggregate );
      Reach const destination = reach( *node.left, site );
      if ( destination.kind == Reach::Kind::STRIPED )
      {
        transfer( destination, reach( *node.right, site ), bytes, site );
        return;
      }
      copyInto( placeFrom( destination, site ), *node.right, bytes, site );
      return;
    }
    assigned( node, site, false );
  }

  /// A value read now, which a step written after its target keeps before the
  /// store: a place is a name the text would read again after it, and a value
  /// is already the byte a load left — see
  /// docs/decisions/0198-a-compound-assignment-is-a-value.md.
  static ir::Operand kept( ir::Operand value, ir::Type type, Site const& site )
  {
    if ( !std::holds_alternative<ir::Object>( value ) )
    {
      return value;
    }
    return materialized( std::move( value ), type, site );
  }

  /// An assignment, whose value is read where `wanted` says: what the target
  /// holds after it, or what it held before where the source wrote the step
  /// after the target.
  std::optional<ir::Operand> assigned( Expression const& node, Site const& site, bool wanted )
  {
    bool const before = wanted && node.writtenAfter;
    if ( node.left->kind == ExpressionKind::INDEX || node.left->kind == ExpressionKind::DEREFERENCE ||
         node.left->kind == ExpressionKind::MEMBER )
    {
      // Where the value goes first, then the value, as they are written.
      Reach const place = reach( *node.left, site );
      std::optional<ir::Operand> const held =
          before ? std::optional{ kept( read( place, site ), place.type, site ) } : std::nullopt;
      ir::Operand const value = converted( operand( *node.right, place.type, site ), place.type, site );
      write( place, value, site );
      if ( !wanted )
      {
        return std::nullopt;
      }
      return before ? *held : kept( value, place.type, site );
    }
    Meaning const target = mTyping.resolve( *node.left ).value_or( Meaning{} );
    std::string name = nameOf( target, node.left->token, site );
    std::optional<ir::Operand> const held =
        before ? std::optional{ materialized( ir::Object{ .name = name, .type = target.type }, target.type, site ) }
               : std::nullopt;
    ir::Operand const value = operand( *node.right, target.type, site );
    site.blocks->append( ir::Instruction{
        .operation = ir::Store{ .name = std::move( name ), .type = target.type, .value = value }, .at = site.at } );
    if ( !wanted )
    {
      return std::nullopt;
    }
    return before ? *held : kept( value, target.type, site );
  }

  /// `&&` or `||` read as a value: a byte of its own that either path writes,
  /// since what a condition decides is not one value but two
  /// ([0079](../decisions/0079-a-condition-is-a-bool.md)).
  ir::Operand decided( Expression const& node, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    std::string const byte = "__b" + std::to_string( mCounted.back().conditions++ );
    site.function->locals.push_back(
        ir::Local{ .name = byte, .type = ir::Type::BOOL, .placement = mPlacement.front() } );

    std::uint32_t const whenTrue = blocks.create();
    std::uint32_t const whenFalse = blocks.create();
    std::uint32_t const join = blocks.create();
    condition( node, whenTrue, whenFalse, site );
    for ( std::uint32_t const path : { whenTrue, whenFalse } )
    {
      blocks.enter( path );
      blocks.append( ir::Instruction{
          .operation = ir::Store{ .name = byte,
                                  .type = ir::Type::BOOL,
                                  .value = ir::Constant{ .type = ir::Type::BOOL, .value = path == whenTrue ? 1 : 0 } },
          .at = site.at } );
      blocks.terminate( jumpTo( join, site.at ) );
    }
    blocks.enter( join );
    return ir::Object{ .name = byte, .type = ir::Type::BOOL };
  }

  /// `c ? a : b` read as a value: a byte of its own that either way writes,
  /// as `&&` and `||` take one — see
  /// docs/decisions/0169-a-conditional-operator.md.
  ir::Operand chosen( Expression const& node, ir::Type context, Site const& site )
  {
    Blocks& blocks = *site.blocks;
    ir::Type const type = mTyping.expression( node ).type.value_or( context );
    std::string const byte = "__b" + std::to_string( mCounted.back().conditions++ );
    site.function->locals.push_back(
        ir::Local{ .name = byte, .type = type, .bytes = 0, .placement = mPlacement.front() } );

    std::uint32_t const whenTrue = blocks.create();
    std::uint32_t const whenFalse = blocks.create();
    std::uint32_t const join = blocks.create();
    condition( *node.left, whenTrue, whenFalse, site );
    for ( std::uint32_t const path : { whenTrue, whenFalse } )
    {
      Expression const& way = path == whenTrue ? *node.arguments.front() : *node.arguments.back();
      blocks.enter( path );
      blocks.append( ir::Instruction{
          .operation =
              ir::Store{ .name = byte, .type = type, .value = converted( operand( way, type, site ), type, site ) },
          .at = site.at } );
      blocks.terminate( jumpTo( join, site.at ) );
    }
    blocks.enter( join );
    return ir::Object{ .name = byte, .type = type };
  }

  static ir::Value define( Site const& site, ir::Type type )
  {
    site.function->values.push_back( type );
    return ir::Value{ .index = static_cast<std::uint32_t>( site.function->values.size() - 1 ) };
  }

  /// An expression the check let through, as an operand, the instructions it
  /// needs appended first. A constant takes `context`, the type of where it is
  /// used.
  ir::Operand operand( Expression const& node, ir::Type context, Site const& site )
  {
    if ( node.kind == ExpressionKind::CONDITIONAL )
    {
      return chosen( node, context, site );
    }
    if ( node.kind == ExpressionKind::CALL )
    {
      return call( node, site, true ).value_or( ir::Value{} );
    }
    if ( node.kind == ExpressionKind::ASSIGNMENT )
    {
      return assigned( node, site, true ).value_or( ir::Value{} );
    }
    if ( node.kind == ExpressionKind::QUALIFIED_NAME )
    {
      return ir::Constant{ .type = ir::Type::U8, .value = mNames->enumeratorIndex( *mUnit, node ).value_or( 0 ) };
    }
    if ( node.kind == ExpressionKind::NULL_POINTER )
    {
      return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = {} };
    }
    if ( node.kind == ExpressionKind::STRING_LITERAL )
    {
      return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = literalSection( node ) };
    }
    if ( node.kind == ExpressionKind::CHARACTER_CONSTANT && mTyping.isTranslated( node ) )
    {
      return ir::Constant{ .type = ir::Type::U8, .value = 0, .name = spellingOfCharacter( node ) };
    }
    if ( node.kind == ExpressionKind::IDENTIFIER )
    {
      std::optional<Meaning> const named = mTyping.lookup( node );
      // An array's name alone is the address of its first element.
      if ( named.has_value() && named->isArray )
      {
        return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = nameOf( *named, node.token, site ) };
      }
      if ( named.has_value() && isOpaqueConstant( *named ) )
      {
        return ir::Constant{ .type = named->type, .value = 0, .name = nameOf( *named, node.token, site ) };
      }
    }
    if ( node.kind == ExpressionKind::BINARY && isLogical( node.token.kind ) )
    {
      return decided( node, site );
    }
    Typed const kind = mTyping.expression( node );
    if ( node.kind == ExpressionKind::PREDEFINED_CONSTANT )
    {
      return ir::Constant{ .type = ir::Type::BOOL, .value = node.value };
    }
    if ( !kind.type.has_value() && kind.known )
    {
      return ir::Constant{ .type = context, .value = wrapped( mTyping.fold( node, context ), context ) };
    }
    if ( kind.known && kind.type.has_value() )
    {
      return constantOf( node, *kind.type, site );
    }
    if ( kind.decided.has_value() )
    {
      return ir::Constant{ .type = ir::Type::BOOL, .value = *kind.decided ? 1 : 0 };
    }

    // Constants with a `?:` of two among them are computed where the program
    // runs, in the type of where they are used, as a constant would be.
    ir::Type const type = kind.type.value_or( context );
    if ( node.kind == ExpressionKind::CAST )
    {
      // What is cast is lowered in its own type: an `enum struct`'s is `u8`.
      ir::Type const source = mTyping.expression( *node.left ).type.value_or( type );
      return converted( operand( *node.left, source, site ), type, site );
    }
    if ( node.kind == ExpressionKind::INDEX || node.kind == ExpressionKind::DEREFERENCE )
    {
      return read( reach( node, site ), site );
    }
    if ( node.kind == ExpressionKind::MEMBER )
    {
      Reach const place = reach( node, site );
      // An array member is the address of its first element.
      if ( place.isArray )
      {
        return addressOfReach( place, site );
      }
      ir::Operand value = read( place, site );
      Expression const* root = &node;
      while ( root->kind == ExpressionKind::MEMBER || root->kind == ExpressionKind::INDEX )
      {
        root = root->left.get();
      }
      if ( root->kind == ExpressionKind::CALL && std::holds_alternative<ir::Object>( value ) )
      {
        return materialized( std::move( value ), place.type, site );
      }
      return value;
    }
    if ( node.kind == ExpressionKind::ADDRESS )
    {
      return addressOf( *node.left, site );
    }
    if ( node.kind == ExpressionKind::BINARY )
    {
      Typed const left = mTyping.expression( *node.left );
      Typed const right = mTyping.expression( *node.right );
      if ( left.pointee.has_value() || right.pointee.has_value() || left.isNull || right.isNull )
      {
        if ( isComparison( node.token.kind ) )
        {
          return comparison( node, site );
        }
        return pointerArithmetic( node, left, right, site );
      }
    }
    if ( node.kind == ExpressionKind::IDENTIFIER )
    {
      Meaning const meaning = mTyping.resolve( node ).value_or( Meaning{} );
      if ( meaning.kind == NameKind::FUNCTION || meaning.kind == NameKind::ASSEMBLER_PROC )
      {
        // A function's name alone is its address, and carries the Proc of its
        // type as `&f` does — see docs/decisions/0065-handlers.md.
        return ir::Constant{ .type = ir::Type::POINTER,
                             .value = 0,
                             .name = nameOf( meaning, node.token, site ),
                             .follower = typeOfMember( meaning, node ) };
      }
      return ir::Object{ .name = nameOf( meaning, node.token, site ), .type = type };
    }
    if ( node.kind == ExpressionKind::UNARY )
    {
      ir::Operand inner = operand( *node.left, type, site );
      ir::UnaryOperator op = ir::UnaryOperator::LOGICAL_NOT;
      if ( node.token.kind == TokenKind::MINUS )
      {
        op = ir::UnaryOperator::NEGATE;
      }
      else if ( node.token.kind == TokenKind::TILDE )
      {
        op = ir::UnaryOperator::COMPLEMENT;
      }
      ir::Value const result = define( site, type );
      site.blocks->append( ir::Instruction{
          .operation = ir::Unary{ .result = result, .op = op, .type = type, .operand = std::move( inner ) },
          .at = site.at } );
      return result;
    }
    if ( isComparison( node.token.kind ) )
    {
      return comparison( node, site );
    }

    TokenKind const op = node.token.kind;
    if ( op == TokenKind::STAR || op == TokenKind::SLASH || op == TokenKind::PERCENT )
    {
      return product( node, type, site );
    }
    ir::Operand left = converted( operand( *node.left, type, site ), type, site );
    ir::Operand right;
    if ( !isShift( node.token.kind ) )
    {
      right = converted( operand( *node.right, type, site ), type, site );
    }
    else if ( isKnown( mTyping.expression( *node.right ) ) )
    {
      right = ir::Constant{ .type = ir::Type::U8, .value = mTyping.fold( *node.right, std::nullopt ) };
    }
    else
    {
      // A count is a `u8` whatever it shifts.
      right = operand( *node.right, ir::Type::U8, site );
    }
    ir::Value const result = define( site, type );
    site.blocks->append( ir::Instruction{ .operation = ir::Binary{ .result = result,
                                                                   .op = binaryOperator( node.token.kind ),
                                                                   .type = type,
                                                                   .left = std::move( left ),
                                                                   .right = std::move( right ) },
                                          .at = site.at } );
    return result;
  }

  /// `*`, `/` or `%` of a value, of `type`: shifts and adds by a constant,
  /// a shift and a mask by a power of two, and a call of the runtime
  /// otherwise — see docs/decisions/0095-literals-and-the-runtime.md.
  ir::Operand product( Expression const& node, ir::Type type, Site const& site )
  {
    TokenKind const op = node.token.kind;
    bool const leftKnown = isKnown( mTyping.expression( *node.left ) );
    bool const rightKnown = isKnown( mTyping.expression( *node.right ) );
    std::uint32_t const bits = 8 * ir::sizeOf( type );
    // A constant as the bits of the type, which is what a product is taken in.
    auto const bitsOf = [&]( Expression const& constant )
    {
      auto const value = static_cast<std::uint64_t>( mTyping.fold( constant, type ) );
      return static_cast<std::uint32_t>( value & ( ( std::uint64_t{ 1 } << bits ) - 1U ) );
    };
    if ( op == TokenKind::STAR && ( leftKnown || rightKnown ) )
    {
      Expression const& value = rightKnown ? *node.left : *node.right;
      Expression const& factor = rightKnown ? *node.right : *node.left;
      return multiplied( converted( operand( value, type, site ), type, site ), bitsOf( factor ), type, site );
    }
    std::int64_t const divisor = rightKnown ? wrapped( mTyping.fold( *node.right, type ), type ) : 0;
    bool const powerOfTwo = rightKnown && divisor > 0 && ( divisor & ( divisor - 1 ) ) == 0;
    // A signed remainder is the runtime's even by a power of two, so its left
    // side is lowered once, for the call.
    if ( powerOfTwo && op != TokenKind::STAR && ( op == TokenKind::SLASH || !ir::isSigned( type ) ) )
    {
      auto const shift = static_cast<std::int64_t>( std::countr_zero( static_cast<std::uint64_t>( divisor ) ) );
      ir::Operand value = converted( operand( *node.left, type, site ), type, site );
      if ( !ir::isSigned( type ) )
      {
        return op == TokenKind::SLASH
                   ? ir::Operand{ binaryValue( ir::BinaryOperator::SHIFT_RIGHT,
                                               type,
                                               std::move( value ),
                                               ir::Constant{ .type = ir::Type::U8, .value = shift, .name = {} },
                                               site ) }
                   : ir::Operand{ binaryValue( ir::BinaryOperator::AND,
                                               type,
                                               std::move( value ),
                                               ir::Constant{ .type = type, .value = divisor - 1, .name = {} },
                                               site ) };
      }
      if ( shift == 0 )
      {
        return value;
      }
      // C truncates towards zero, so a negative value is biased by the
      // divisor less one before the shift, which rounds towards minus
      // infinity: the sign spread over the value, masked to the bias.
      ir::Value const sign = binaryValue( ir::BinaryOperator::SHIFT_RIGHT,
                                          type,
                                          value,
                                          ir::Constant{ .type = ir::Type::U8, .value = bits - 1, .name = {} },
                                          site );
      ir::Value const bias = binaryValue(
          ir::BinaryOperator::AND, type, sign, ir::Constant{ .type = type, .value = divisor - 1, .name = {} }, site );
      ir::Value const biased = binaryValue( ir::BinaryOperator::ADD, type, std::move( value ), bias, site );
      return binaryValue( ir::BinaryOperator::SHIFT_RIGHT,
                          type,
                          biased,
                          ir::Constant{ .type = ir::Type::U8, .value = shift, .name = {} },
                          site );
    }

    // The runtime: its Proc per operator, width and sign, called as any Proc
    // of the assembler is.
    std::string const width = std::to_string( bits );
    std::string name;
    std::string left = "dividend";
    std::string right = "divisor";
    std::string result;
    if ( op == TokenKind::STAR )
    {
      name = "__mul" + width;
      left = "left";
      right = "right";
      result = "product";
    }
    else
    {
      bool const signedOperands = ir::isSigned( type );
      name = std::string{ signedOperands ? "__s" : "__u" } + ( op == TokenKind::SLASH ? "div" : "mod" ) + width;
      result = op == TokenKind::SLASH ? "quotient" : "remainder";
      if ( op == TokenKind::SLASH && !signedOperands )
      {
        // An unsigned division builds the quotient where the dividend was,
        // so one byte of it carries both roles — see
        // docs/decisions/0119-one-temporary-carries-two-roles.md.
        left = "dividendAndQuotient";
        result = left;
      }
    }
    std::vector<ir::Argument> arguments;
    arguments.push_back( ir::Argument{ .name = name + "." + left,
                                       .type = type,
                                       .value = converted( operand( *node.left, type, site ), type, site ) } );
    arguments.push_back( ir::Argument{ .name = name + "." + right,
                                       .type = type,
                                       .value = converted( operand( *node.right, type, site ), type, site ) } );
    ir::Value const value = define( site, type );
    site.blocks->append( ir::Instruction{ .operation = ir::Call{ .name = name,
                                                                 .arguments = std::move( arguments ),
                                                                 .result = value,
                                                                 .returned = name + "." + result },
                                          .at = site.at } );
    return value;
  }

  static ir::BinaryOperator binaryOperator( TokenKind kind )
  {
    switch ( kind )
    {
    case TokenKind::MINUS:
      return ir::BinaryOperator::SUBTRACT;
    case TokenKind::AMPERSAND:
      return ir::BinaryOperator::AND;
    case TokenKind::PIPE:
      return ir::BinaryOperator::OR;
    case TokenKind::CARET:
      return ir::BinaryOperator::XOR;
    case TokenKind::LESS_LESS:
      return ir::BinaryOperator::SHIFT_LEFT;
    case TokenKind::GREATER_GREATER:
      return ir::BinaryOperator::SHIFT_RIGHT;
    default:
      return ir::BinaryOperator::ADD;
    }
  }

  /// An expression of constants the check let through, in `type`: named by the
  /// `const` it is, where it is one read in its own type, or by the object and
  /// offset it is the address of.
  [[nodiscard]] ir::Constant constantOf( Expression const& node, ir::Type type, Site const& site )
  {
    ir::Constant constant{ .type = type, .value = wrapped( mTyping.fold( node, type ), type ), .name = {} };
    if ( node.kind == ExpressionKind::STRING_LITERAL )
    {
      return ir::Constant{ .type = type, .value = 0, .name = literalSection( node ) };
    }
    if ( node.kind == ExpressionKind::CHARACTER_CONSTANT && mTyping.isTranslated( node ) )
    {
      return ir::Constant{ .type = type, .value = 0, .name = spellingOfCharacter( node ) };
    }
    if ( std::optional<std::string> const address = addressName( node, site ); address.has_value() )
    {
      constant.value = 0;
      constant.name = *address;
      return constant;
    }
    if ( node.kind != ExpressionKind::IDENTIFIER )
    {
      return constant;
    }
    std::optional<Meaning> const meaning = mTyping.lookup( node );
    if ( meaning.has_value() && isOpaqueConstant( *meaning ) )
    {
      constant.value = 0;
      constant.name = nameOf( *meaning, node.token, site );
      return constant;
    }
    if ( meaning.has_value() && meaning->isConst && meaning->value.has_value() && meaning->type == type &&
         ( meaning->external == nullptr || meaning->definition != nullptr ) )
    {
      constant.name = nameOf( *meaning, node.token, site );
      constant.valued = true;
    }
    return constant;
  }

  /// Where an element, or what a pointer points at, is reached: an object at an
  /// address the text names, an element of one through `X`, or `Y` bytes on
  /// from a pointer on the zero page.
  struct Reach
  {
    enum class Kind : std::uint8_t
    {
      OBJECT,
      INDEXED,
      INDIRECT,

      /// In a striped array's stripes, `index` in `X`.
      STRIPED,
    };

    Kind kind = Kind::OBJECT;
    ir::Type type = ir::Type::U8;
    std::string name{};
    ir::Operand pointer{};
    ir::Operand index{};

    /// An INDEXED index already counted in bytes.
    bool scaled = false;

    /// A member that is an array, whose value is its first element's address.
    bool isArray = false;

    /// An INDIRECT place reached through a pointer to `volatile`.
    bool isVolatile = false;

    /// A STRIPED place: the array's name, the stripe of each byte of an
    /// element, and the byte of the element it is at.
    std::string array{};
    std::vector<std::vector<std::string>> paths{};
    std::uint32_t offset = 0;
  };

  /// The Label of the stripe a byte of a striped element lies in.
  static std::string stripeOf( Reach const& place, std::uint32_t byte )
  {
    std::string name = place.array;
    if ( byte < place.paths.size() )
    {
      for ( std::string const& part : place.paths[byte] )
      {
        name += "." + part;
      }
    }
    return name;
  }

  /// A pointer an operand is, on the zero page where `(zp),y` reads through
  /// it: an object of C declared a pointer lies there, and any other object —
  /// a member, a `u16` cast to a pointer, a name of the assembler — is copied
  /// to a pair of scratch bytes first.
  ir::Operand pointerOperand( Expression const& node, Site const& site )
  {
    ir::Operand pointer = operand( node, ir::Type::POINTER, site );
    if ( std::holds_alternative<ir::Object>( pointer ) && !onZeroPage( node ) )
    {
      return materialized( std::move( pointer ), ir::Type::POINTER, site );
    }
    return pointer;
  }

  /// Whether an object read as a pointer is one of C declared a pointer, cast
  /// or not, which is a pair of bytes on the zero page.
  [[nodiscard]] bool onZeroPage( Expression const& node ) const
  {
    Expression const* named = &node;
    while ( named->kind == ExpressionKind::CAST && named->left != nullptr )
    {
      named = named->left.get();
    }
    if ( named->kind != ExpressionKind::IDENTIFIER )
    {
      return false;
    }
    std::optional<Meaning> const meaning = mTyping.lookup( *named );
    return meaning.has_value() && meaning->kind == NameKind::OBJECT && meaning->external == nullptr &&
           meaning->pointee.has_value() && !meaning->isArray;
  }

  /// An operand as a value of its own, read now: a member of a call's result is
  /// read before another call writes the result again.
  static ir::Operand materialized( ir::Operand value, ir::Type type, Site const& site )
  {
    ir::Value const result = define( site, type );
    site.blocks->append( ir::Instruction{
        .operation = ir::Convert{ .result = result, .type = type, .operand = std::move( value ) }, .at = site.at } );
    return result;
  }

  /// `value` times a constant, as shifts and adds from the factor's top bit
  /// down — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
  /// A value is read once, by its own instruction, and so is a `volatile`
  /// object, whose every read is one of the program's: either is kept in a
  /// byte of the Proc first, which the adds read as often as they need.
  ir::Operand multiplied( ir::Operand value, std::uint32_t factor, ir::Type type, Site const& site )
  {
    if ( factor == 0 )
    {
      return ir::Constant{ .type = type, .value = 0, .name = {} };
    }
    auto const shifted = [&]( ir::Operand operand, int count ) -> ir::Operand
    {
      if ( count == 0 )
      {
        return operand;
      }
      return binaryValue( ir::BinaryOperator::SHIFT_LEFT,
                          type,
                          std::move( operand ),
                          ir::Constant{ .type = ir::Type::U8, .value = count, .name = {} },
                          site );
    };
    int const top = std::bit_width( factor ) - 1;
    if ( std::has_single_bit( factor ) )
    {
      return shifted( std::move( value ), top );
    }
    auto const* const object = std::get_if<ir::Object>( &value );
    if ( std::holds_alternative<ir::Value>( value ) ||
         ( object != nullptr && mVolatileNames.contains( ir::baseOf( object->name ) ) ) )
    {
      std::string const byte = "__m" + std::to_string( mCounted.back().conditions++ );
      site.function->locals.push_back( ir::Local{ .name = byte, .type = type, .placement = mPlacement.front() } );
      site.blocks->append( ir::Instruction{
          .operation = ir::Store{ .name = byte, .type = type, .value = std::move( value ) }, .at = site.at } );
      value = ir::Object{ .name = byte, .type = type };
    }
    ir::Operand product = value;
    int pending = 0;
    for ( int bit = top - 1; bit >= 0; --bit )
    {
      ++pending;
      if ( ( ( factor >> static_cast<unsigned>( bit ) ) & 1U ) != 0 )
      {
        product = binaryValue( ir::BinaryOperator::ADD, type, shifted( std::move( product ), pending ), value, site );
        pending = 0;
      }
    }
    return shifted( std::move( product ), pending );
  }

  /// `s.m` and `p->m`: where the member is, `offset` bytes on from where what
  /// holds it is.
  Reach member( Expression const& node, Site const& site )
  {
    Typed const kind = mTyping.expression( node );
    ir::Type const type = kind.type.value_or( ir::Type::U8 );
    Typed const outer = mTyping.expression( *node.left );
    bool const arrow = node.token.kind == TokenKind::ARROW;
    std::uint32_t const aggregateIndex =
        arrow ? outer.pointee.value_or( Pointee{} ).aggregate.value_or( 0 ) : outer.aggregate.value_or( 0 );
    MemberType const* const found =
        memberNamed( mNames->aggregate( aggregateIndex ), spellingOf( node.right->token ), *mSources );
    std::uint32_t const offset = found != nullptr ? found->offset : 0;
    ir::Type memberType = found != nullptr && found->type.pointee.has_value() ? ir::Type::POINTER : type;
    bool const isArray = found != nullptr && found->isArray;
    if ( isArray )
    {
      memberType = found->type.type;
    }
    Reach outerPlace;
    if ( arrow )
    {
      ir::Operand pointer = pointerOperand( *node.left, site );
      // A pointer that is a constant, a number or a name the assembler knows,
      // points at an address the text writes, as an element's does.
      if ( auto const* constant = std::get_if<ir::Constant>( &pointer ) )
      {
        outerPlace = Reach{ .kind = Reach::Kind::OBJECT,
                            .type = memberType,
                            .name = offsetName( *constant, 0 ),
                            .pointer = {},
                            .index = {} };
      }
      else
      {
        outerPlace = Reach{ .kind = Reach::Kind::INDIRECT,
                            .type = memberType,
                            .name = {},
                            .pointer = std::move( pointer ),
                            .index = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} } };
      }
    }
    else
    {
      outerPlace = reach( *node.left, site );
    }
    Reach place = offsetReach( std::move( outerPlace ), offset, memberType, site );
    place.isArray = isArray;
    return place;
  }

  /// A place `offset` bytes on from another.
  static Reach offsetReach( Reach outer, std::uint32_t offset, ir::Type type, Site const& site )
  {
    outer.type = type;
    if ( outer.kind == Reach::Kind::STRIPED )
    {
      outer.offset += offset;
      return outer;
    }
    if ( outer.kind != Reach::Kind::INDIRECT )
    {
      outer.name = offsetName( ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = outer.name }, offset );
      return outer;
    }
    std::int64_t const at = std::get<ir::Constant>( outer.index ).value + offset;
    if ( at + ir::sizeOf( type ) <= 256 )
    {
      outer.index = ir::Constant{ .type = ir::Type::U8, .value = at, .name = {} };
      return outer;
    }
    outer.pointer =
        binaryValue( ir::BinaryOperator::ADD,
                     ir::Type::POINTER,
                     outer.pointer,
                     ir::Constant{ .type = ir::Type::POINTER, .value = wrapped( at, ir::Type::POINTER ), .name = {} },
                     site );
    outer.index = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} };
    return outer;
  }

  /// The address a place is at.
  static ir::Operand addressOfReach( Reach const& place, Site const& site )
  {
    switch ( place.kind )
    {
    case Reach::Kind::STRIPED:
    {
      // A byte of a stripe, at its Label and the index.
      ir::Constant const stripe{ .type = ir::Type::POINTER, .value = 0, .name = stripeOf( place, place.offset ) };
      if ( auto const* constant = std::get_if<ir::Constant>( &place.index ) )
      {
        return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = offsetName( stripe, constant->value ) };
      }
      return binaryValue(
          ir::BinaryOperator::ADD, ir::Type::POINTER, stripe, converted( place.index, ir::Type::POINTER, site ), site );
    }
    case Reach::Kind::OBJECT:
      return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = place.name };
    case Reach::Kind::INDEXED:
    {
      ir::Operand offset = converted( place.index, ir::Type::POINTER, site );
      if ( !place.scaled && ir::sizeOf( place.type ) == 2 )
      {
        offset = binaryValue( ir::BinaryOperator::SHIFT_LEFT,
                              ir::Type::POINTER,
                              std::move( offset ),
                              ir::Constant{ .type = ir::Type::U8, .value = 1, .name = {} },
                              site );
      }
      return binaryValue( ir::BinaryOperator::ADD,
                          ir::Type::POINTER,
                          ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = place.name },
                          std::move( offset ),
                          site );
    }
    case Reach::Kind::INDIRECT:
      if ( auto const* constant = std::get_if<ir::Constant>( &place.index );
           constant != nullptr && constant->value == 0 )
      {
        return place.pointer;
      }
      return binaryValue( ir::BinaryOperator::ADD,
                          ir::Type::POINTER,
                          place.pointer,
                          converted( place.index, ir::Type::POINTER, site ),
                          site );
    }
    return place.pointer;
  }

  /// Where the bytes of a `struct` or a `union` an expression is are.
  ir::Place placeOf( Expression const& node, Site const& site )
  {
    return placeFrom( reach( node, site ), site );
  }

  /// Where a place's bytes are, as a copy reads them: a name, or a pointer on
  /// the zero page and an offset. A striped place has no such bytes, and is
  /// copied a byte at a time instead.
  static ir::Place placeFrom( Reach const& place, Site const& site )
  {
    if ( place.kind == Reach::Kind::OBJECT )
    {
      return ir::Place{ .name = place.name };
    }
    if ( place.kind == Reach::Kind::INDIRECT )
    {
      if ( auto const* constant = std::get_if<ir::Constant>( &place.index ) )
      {
        return ir::Place{ .name = {},
                          .pointer = place.pointer,
                          .offset = static_cast<std::uint32_t>( constant->value ) };
      }
    }
    return ir::Place{ .name = {}, .pointer = addressOfReach( place, site ), .offset = 0 };
  }

  /// The bytes of a `struct` copied where either side lies in stripes: a byte
  /// at a time, each read and then written, an index several instructions read
  /// kept in a byte of its own — see docs/decisions/0089-a-stripe-is-a-section.md.
  void transfer( Reach to, Reach from, std::uint32_t bytes, Site const& site )
  {
    auto const kept = [&]( Reach& place )
    {
      if ( place.kind == Reach::Kind::STRIPED && std::holds_alternative<ir::Value>( place.index ) )
      {
        std::string const name = "__i" + std::to_string( mCounted.back().indexes++ );
        site.function->locals.push_back(
            ir::Local{ .name = name, .type = ir::Type::U8, .bytes = 0, .placement = mPlacement.front() } );
        site.blocks->append( ir::Instruction{
            .operation = ir::Store{ .name = name, .type = ir::Type::U8, .value = place.index }, .at = site.at } );
        place.index = ir::Object{ .name = name, .type = ir::Type::U8 };
      }
      if ( place.kind == Reach::Kind::INDEXED )
      {
        place = Reach{ .kind = Reach::Kind::INDIRECT,
                       .type = place.type,
                       .name = {},
                       .pointer = addressOfReach( place, site ),
                       .index = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} } };
      }
    };
    kept( to );
    kept( from );
    auto const byteOf = []( Reach place, std::uint32_t byte )
    {
      place.type = ir::Type::U8;
      switch ( place.kind )
      {
      case Reach::Kind::STRIPED:
        place.offset += byte;
        break;
      case Reach::Kind::OBJECT:
        place.name = offsetName( ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = place.name }, byte );
        break;
      default:
        place.index = ir::Constant{ .type = ir::Type::U8,
                                    .value = std::get<ir::Constant>( place.index ).value + byte,
                                    .name = {} };
        break;
      }
      return place;
    };
    for ( std::uint32_t byte = 0; byte < bytes; ++byte )
    {
      write( byteOf( to, byte ), read( byteOf( from, byte ), site ), site );
    }
  }

  /// The bytes of a `struct` or a `union` copied into a place: from where the
  /// expression is, a pointer on either side taking no offset in a loop.
  void copyInto( ir::Place to, Expression const& from, std::uint32_t bytes, Site const& site )
  {
    Reach const origin = reach( from, site );
    if ( origin.kind == Reach::Kind::STRIPED )
    {
      Reach const target =
          to.pointer.has_value()
              ? Reach{ .kind = Reach::Kind::INDIRECT,
                       .type = ir::Type::U8,
                       .name = {},
                       .pointer = *to.pointer,
                       .index = ir::Constant{ .type = ir::Type::U8, .value = to.offset, .name = {} } }
              : Reach{ .kind = Reach::Kind::OBJECT, .type = ir::Type::U8, .name = to.name, .pointer = {}, .index = {} };
      transfer( target, origin, bytes, site );
      return;
    }
    ir::Place source = placeFrom( origin, site );
    auto const unshifted = [&]( ir::Place& place )
    {
      if ( place.pointer.has_value() && place.offset != 0 && bytes > SMALL_BLOCK )
      {
        place.pointer = binaryValue( ir::BinaryOperator::ADD,
                                     ir::Type::POINTER,
                                     *place.pointer,
                                     ir::Constant{ .type = ir::Type::POINTER, .value = place.offset, .name = {} },
                                     site );
        place.offset = 0;
      }
    };
    unshifted( to );
    unshifted( source );
    site.blocks->append( ir::Instruction{
        .operation = ir::Copy{ .from = std::move( source ), .to = std::move( to ), .bytes = bytes }, .at = site.at } );
  }

  /// The name an address constant is written by: the object and the offset,
  /// `NAME+N` or `NAME-N`, or the address itself.
  static std::string offsetName( ir::Constant const& base, std::int64_t offset )
  {
    if ( base.name.empty() )
    {
      return std::to_string( wrapped( base.value + offset, ir::Type::POINTER ) );
    }
    if ( offset == 0 )
    {
      return base.name;
    }
    return base.name + ( offset < 0 ? "-" : "+" ) + std::to_string( offset < 0 ? -offset : offset );
  }

  /// What the address of an object at a name the assembler knows is written
  /// by: an array's name alone, `&x`, `&t[3]`; nothing for any other.
  [[nodiscard]] std::optional<std::string> addressName( Expression const& node, Site const& site )
  {
    Expression const* place = nullptr;
    if ( node.kind == ExpressionKind::ADDRESS )
    {
      place = node.left.get();
    }
    else if ( node.kind == ExpressionKind::IDENTIFIER || node.kind == ExpressionKind::MEMBER )
    {
      // An array alone, a member one among them, is its first element's.
      if ( std::optional<Shape> const shape = mTyping.shape( node ); shape.has_value() && shape->isArray )
      {
        place = &node;
      }
    }
    if ( place == nullptr )
    {
      return std::nullopt;
    }
    // A byte of a stripe at a constant index is at the stripe's Label.
    if ( mTyping.throughStripes( *place ) )
    {
      Reach const stripe = reach( *place, site );
      if ( stripe.kind != Reach::Kind::STRIPED || !std::holds_alternative<ir::Constant>( stripe.index ) )
      {
        return std::nullopt;
      }
      return std::get<ir::Constant>( addressOfReach( stripe, site ) ).name;
    }
    std::optional<std::pair<std::string, std::int64_t>> const fixed = fixedPlace( *place, site );
    if ( !fixed.has_value() )
    {
      return std::nullopt;
    }
    return offsetName( ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = fixed->first }, fixed->second );
  }

  /// The name and offset of a place at an address the assembler knows: an
  /// object, a member of one by `.`, an element by a constant index.
  std::optional<std::pair<std::string, std::int64_t>> fixedPlace( Expression const& node, Site const& site )
  {
    switch ( node.kind )
    {
    case ExpressionKind::IDENTIFIER:
    {
      std::optional<Meaning> const meaning = mTyping.lookup( node );
      if ( !meaning.has_value() || meaning->kind != NameKind::OBJECT || meaning->pointee.has_value() )
      {
        return std::nullopt;
      }
      return std::pair{ nameOf( *meaning, node.token, site ), std::int64_t{ 0 } };
    }
    case ExpressionKind::MEMBER:
    {
      std::optional<Shape> const outer = mTyping.shape( *node.left );
      if ( node.token.kind != TokenKind::DOT || !outer.has_value() || !outer->aggregate.has_value() || outer->isArray )
      {
        return std::nullopt;
      }
      MemberType const* const member =
          memberNamed( mNames->aggregate( *outer->aggregate ), spellingOf( node.right->token ), *mSources );
      std::optional<std::pair<std::string, std::int64_t>> base = fixedPlace( *node.left, site );
      if ( member == nullptr || !base.has_value() )
      {
        return std::nullopt;
      }
      base->second += member->offset;
      return base;
    }
    case ExpressionKind::INDEX:
    {
      std::optional<Shape> const outer = mTyping.shape( *node.left );
      std::optional<Shape> const element = mTyping.shape( node );
      if ( !outer.has_value() || !outer->isArray || !element.has_value() ||
           !isKnown( mTyping.expression( *node.right ) ) )
      {
        return std::nullopt;
      }
      std::optional<std::pair<std::string, std::int64_t>> base = fixedPlace( *node.left, site );
      if ( !base.has_value() )
      {
        return std::nullopt;
      }
      base->second += mTyping.fold( *node.right, std::nullopt ) * mNames->bytes( element->type, element->aggregate ) *
                      ( element->isArray ? element->count : 1 );
      return base;
    }
    default:
      return std::nullopt;
    }
  }

  /// An integer operand in `ptr`, times `size`: an offset of that many
  /// elements.
  ir::Operand scaled( Expression const& node, std::uint32_t size, Site const& site )
  {
    Typed const kind = mTyping.expression( node );
    if ( isKnown( kind ) )
    {
      std::int64_t const value = mTyping.fold( node, std::nullopt ) * size;
      return ir::Constant{ .type = ir::Type::POINTER, .value = wrapped( value, ir::Type::POINTER ), .name = {} };
    }
    ir::Operand offset =
        converted( operand( node, kind.type.value_or( ir::Type::U8 ), site ), ir::Type::POINTER, site );
    return multiplied( std::move( offset ), size, ir::Type::POINTER, site );
  }

  static ir::Value
  binaryValue( ir::BinaryOperator op, ir::Type type, ir::Operand left, ir::Operand right, Site const& site )
  {
    ir::Value const result = define( site, type );
    site.blocks->append( ir::Instruction{
        .operation =
            ir::Binary{
                .result = result, .op = op, .type = type, .left = std::move( left ), .right = std::move( right ) },
        .at = site.at } );
    return result;
  }

  /// `p + n`, `n + p`, `p - n`, and `p - q` counted in elements.
  ir::Operand pointerArithmetic( Expression const& node, Typed const& left, Typed const& right, Site const& site )
  {
    if ( left.pointee.has_value() && right.pointee.has_value() )
    {
      ir::Operand bytes = converted( binaryValue( ir::BinaryOperator::SUBTRACT,
                                                  ir::Type::POINTER,
                                                  operand( *node.left, ir::Type::POINTER, site ),
                                                  operand( *node.right, ir::Type::POINTER, site ),
                                                  site ),
                                     ir::Type::I16,
                                     site );
      // The bytes between them are a whole number of elements, so the count
      // is exact however it is divided: by a shift where an element is a
      // power of two bytes, and by the runtime a `/` of two `i16`s calls
      // where it is not.
      std::uint32_t const size = mNames->bytes( left.pointee->type, left.pointee->aggregate );
      if ( size <= 1 )
      {
        return bytes;
      }
      if ( std::has_single_bit( size ) )
      {
        return binaryValue( ir::BinaryOperator::SHIFT_RIGHT,
                            ir::Type::I16,
                            std::move( bytes ),
                            ir::Constant{ .type = ir::Type::U8, .value = std::countr_zero( size ), .name = {} },
                            site );
      }
      std::vector<ir::Argument> arguments;
      arguments.push_back( ir::Argument{ .name = "__sdiv16.dividend", .type = ir::Type::I16, .value = bytes } );
      arguments.push_back( ir::Argument{ .name = "__sdiv16.divisor",
                                         .type = ir::Type::I16,
                                         .value = ir::Constant{ .type = ir::Type::I16, .value = size, .name = {} } } );
      ir::Value const count = define( site, ir::Type::I16 );
      site.blocks->append( ir::Instruction{ .operation = ir::Call{ .name = "__sdiv16",
                                                                   .arguments = std::move( arguments ),
                                                                   .result = count,
                                                                   .returned = "__sdiv16.quotient" },
                                            .at = site.at } );
      return count;
    }
    bool const pointerLeft = left.pointee.has_value();
    Expression const& pointer = pointerLeft ? *node.left : *node.right;
    Expression const& count = pointerLeft ? *node.right : *node.left;
    std::optional<Pointee> const pointee = pointerLeft ? left.pointee : right.pointee;
    std::uint32_t const size =
        mNames->bytes( pointee.value_or( Pointee{} ).type, pointee.value_or( Pointee{} ).aggregate );
    ir::Operand base = operand( pointer, ir::Type::POINTER, site );
    ir::Operand offset = scaled( count, size, site );
    return binaryValue( node.token.kind == TokenKind::MINUS ? ir::BinaryOperator::SUBTRACT : ir::BinaryOperator::ADD,
                        ir::Type::POINTER,
                        std::move( base ),
                        std::move( offset ),
                        site );
  }

  /// `&x`, `&t[i]` and `&*p`.
  /// The function type a function belongs to, or empty.
  [[nodiscard]] std::string typeOfFunction( Definition const* function ) const
  {
    if ( mMembers == nullptr || function == nullptr )
    {
      return {};
    }
    auto const found = mMembers->ofFunction.find( function );
    return found == mMembers->ofFunction.end() ? std::string{} : mNames->functionType( found->second ).name;
  }

  /// The function type a name is a member of, or empty: what the taking of
  /// its address is `.own`ed by — see docs/decisions/0065-handlers.md.
  [[nodiscard]] std::string typeOfMember( Meaning const& meaning, Expression const& named ) const
  {
    if ( mMembers == nullptr )
    {
      return {};
    }
    if ( meaning.kind == NameKind::FUNCTION && meaning.definition != nullptr )
    {
      auto const found = mMembers->ofFunction.find( meaning.definition );
      return found == mMembers->ofFunction.end() ? std::string{} : mNames->functionType( found->second ).name;
    }
    if ( meaning.kind == NameKind::ASSEMBLER_PROC )
    {
      auto const found = mMembers->ofProc.find( spellingOf( named.token ) );
      return found == mMembers->ofProc.end() ? std::string{} : mNames->functionType( found->second ).name;
    }
    return {};
  }

  ir::Operand addressOf( Expression const& operand, Site const& site )
  {
    if ( operand.kind == ExpressionKind::DEREFERENCE )
    {
      return this->operand( *operand.left, ir::Type::POINTER, site );
    }
    if ( operand.kind == ExpressionKind::MEMBER ||
         ( operand.kind == ExpressionKind::INDEX && operand.left->kind != ExpressionKind::IDENTIFIER ) )
    {
      return addressOfReach( reach( operand, site ), site );
    }
    Expression const& named = operand.kind == ExpressionKind::INDEX ? *operand.left : operand;
    Meaning const meaning = mTyping.lookup( named ).value_or( Meaning{} );
    if ( operand.kind == ExpressionKind::IDENTIFIER )
    {
      // The address of a function is followed by the Proc of its type, which
      // the taking says — see docs/decisions/0065-handlers.md.
      return ir::Constant{ .type = ir::Type::POINTER,
                           .value = 0,
                           .name = nameOf( meaning, named.token, site ),
                           .follower = typeOfMember( meaning, named ) };
    }
    ir::Operand base =
        meaning.pointee.has_value()
            ? this->operand( named, ir::Type::POINTER, site )
            : ir::Operand{ ir::Constant{
                  .type = ir::Type::POINTER, .value = 0, .name = nameOf( meaning, named.token, site ) } };
    std::uint32_t const size = meaning.pointee.has_value()
                                   ? mNames->bytes( meaning.pointee->type, meaning.pointee->aggregate )
                                   : mNames->bytes( meaning.type, meaning.aggregate );
    if ( isKnown( mTyping.expression( *operand.right ) ) )
    {
      std::int64_t const offset = mTyping.fold( *operand.right, std::nullopt ) * size;
      if ( auto const* constant = std::get_if<ir::Constant>( &base ) )
      {
        return ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = offsetName( *constant, offset ) };
      }
      if ( offset == 0 )
      {
        return base;
      }
    }
    ir::Operand offset = scaled( *operand.right, size, site );
    return binaryValue( ir::BinaryOperator::ADD, ir::Type::POINTER, std::move( base ), std::move( offset ), site );
  }

  /// Where `t[i]` or `*p` is: through `X` into an array at most a byte of
  /// index long, at a name and offset where both are constants, through `Y`
  /// from a pointer where the offset fits a byte — and otherwise at an address
  /// computed into a pair of scratch bytes, which are on the zero page.
  Reach reach( Expression const& node, Site const& site )
  {
    Reach place = reachOf( node, site );
    // What a pointer to `volatile` reaches: through the pointer, by the
    // instruction; at an address the text writes, by its name — see
    // docs/decisions/0151-volatile.md.
    if ( mTyping.expression( node ).isVolatile )
    {
      if ( place.kind == Reach::Kind::INDIRECT )
      {
        place.isVolatile = true;
      }
      else if ( !place.name.empty() )
      {
        mVolatileNames.insert( std::string{ baseName( place.name ) } );
      }
    }
    return place;
  }

  /// The object a name of the text is a byte of: `t` of `t+2`.
  static std::string_view baseName( std::string_view name )
  {
    std::size_t const offset = name.find_first_of( "+-" );
    return offset == std::string_view::npos ? name : name.substr( 0, offset );
  }

  Reach reachOf( Expression const& node, Site const& site )
  {
    Typed const kind = mTyping.expression( node );
    ir::Type const type = kind.type.value_or( ir::Type::U8 );
    std::uint32_t const size = mNames->bytes( type, kind.aggregate );
    if ( node.kind == ExpressionKind::IDENTIFIER )
    {
      Meaning const meaning = mTyping.lookup( node ).value_or( Meaning{} );
      return Reach{ .kind = Reach::Kind::OBJECT,
                    .type = type,
                    .name = nameOf( meaning, node.token, site ),
                    .pointer = {},
                    .index = {} };
    }
    if ( node.kind == ExpressionKind::CALL )
    {
      call( node, site, false );
      // A Proc of the assembler returns in the byte its `.declare ret` names.
      Meaning const callee = mTyping.lookup( *node.left ).value_or( Meaning{} );
      std::string const returned =
          callee.kind == NameKind::ASSEMBLER_PROC && callee.external != nullptr && callee.external->result.has_value()
              ? callee.external->result->name
              : spellingOf( node.left->token ) + "." + std::string{ RESULT };
      return Reach{ .kind = Reach::Kind::OBJECT, .type = type, .name = returned, .pointer = {}, .index = {} };
    }
    if ( node.kind == ExpressionKind::MEMBER )
    {
      return member( node, site );
    }
    Expression const* const index = node.kind == ExpressionKind::INDEX ? node.right.get() : nullptr;
    std::optional<Meaning> const array =
        node.kind == ExpressionKind::INDEX && node.left->kind == ExpressionKind::IDENTIFIER
            ? mTyping.lookup( *node.left )
            : std::optional<Meaning>{};

    if ( array.has_value() && array->isStriped )
    {
      Typed const indexKind = mTyping.expression( *index );
      ir::Operand stripedIndex =
          isKnown( indexKind ) ? ir::Operand{ ir::Constant{
                                     .type = ir::Type::U8, .value = mTyping.fold( *index, std::nullopt ), .name = {} } }
                               : operand( *index, ir::Type::U8, site );
      ir::Type const elementType = array->aggregate.has_value() ? ir::Type::BLOCK : array->type;
      return Reach{ .kind = Reach::Kind::STRIPED,
                    .type = type,
                    .name = {},
                    .pointer = {},
                    .index = std::move( stripedIndex ),
                    .scaled = true,
                    .isArray = false,
                    .array = nameOf( *array, node.left->token, site ),
                    .paths = stripePaths( elementType, array->aggregate ),
                    .offset = 0 };
    }

    ir::Operand base;
    if ( array.has_value() && array->isArray )
    {
      std::string const name = nameOf( *array, node.left->token, site );
      Typed const indexKind = mTyping.expression( *index );
      bool const reachedByX = indexKind.type == ir::Type::U8 && !isKnown( indexKind ) &&
                              ( size == 1 || ( array->count.has_value() && *array->count * size <= 256 ) );
      if ( reachedByX && size <= 2 && !kind.aggregate.has_value() )
      {
        return Reach{ .kind = Reach::Kind::INDEXED,
                      .type = type,
                      .name = name,
                      .pointer = {},
                      .index = operand( *index, ir::Type::U8, site ) };
      }
      if ( reachedByX )
      {
        // `X` holds the index times the size, which the array's bytes keep
        // below 256.
        return Reach{ .kind = Reach::Kind::INDEXED,
                      .type = type,
                      .name = name,
                      .pointer = {},
                      .index = multiplied( operand( *index, ir::Type::U8, site ), size, ir::Type::U8, site ),
                      .scaled = true };
      }
      base = ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = name };
    }
    else
    {
      base = pointerOperand( *node.left, site );
    }

    std::optional<std::int64_t> known;
    Typed indexKind = constant();
    if ( index == nullptr )
    {
      known = 0;
    }
    else if ( indexKind = mTyping.expression( *index ); isKnown( indexKind ) )
    {
      known = mTyping.fold( *index, std::nullopt ) * size;
    }

    if ( auto const* constant = std::get_if<ir::Constant>( &base ) )
    {
      if ( known.has_value() )
      {
        return Reach{
          .kind = Reach::Kind::OBJECT, .type = type, .name = offsetName( *constant, *known ), .pointer = {}, .index = {}
        };
      }
      if ( indexKind.type == ir::Type::U8 && size == 1 && !kind.aggregate.has_value() )
      {
        return Reach{ .kind = Reach::Kind::INDEXED,
                      .type = type,
                      .name = offsetName( *constant, 0 ),
                      .pointer = {},
                      .index = operand( *index, ir::Type::U8, site ) };
      }
    }
    else if ( known.has_value() && *known >= 0 && *known + size <= 256 )
    {
      return Reach{ .kind = Reach::Kind::INDIRECT,
                    .type = type,
                    .name = {},
                    .pointer = std::move( base ),
                    .index = ir::Constant{ .type = ir::Type::U8, .value = *known, .name = {} } };
    }
    else if ( !known.has_value() && indexKind.type == ir::Type::U8 && size == 1 && !kind.aggregate.has_value() )
    {
      // A byte the index reaches by `Y` — but not a `struct` of one byte,
      // whose member is reached at a constant offset from its address.
      return Reach{ .kind = Reach::Kind::INDIRECT,
                    .type = type,
                    .name = {},
                    .pointer = std::move( base ),
                    .index = operand( *index, ir::Type::U8, site ) };
    }

    ir::Operand const offset =
        known.has_value() ? ir::Operand{ ir::Constant{
                                .type = ir::Type::POINTER, .value = wrapped( *known, ir::Type::POINTER ), .name = {} } }
                          : scaled( *index, size, site );
    ir::Value const address =
        binaryValue( ir::BinaryOperator::ADD, ir::Type::POINTER, std::move( base ), offset, site );
    return Reach{ .kind = Reach::Kind::INDIRECT,
                  .type = type,
                  .name = {},
                  .pointer = address,
                  .index = ir::Constant{ .type = ir::Type::U8, .value = 0, .name = {} } };
  }

  static ir::Operand read( Reach const& place, Site const& site )
  {
    if ( place.kind == Reach::Kind::OBJECT )
    {
      return ir::Object{ .name = place.name, .type = place.type };
    }
    if ( place.kind == Reach::Kind::STRIPED )
    {
      std::string const stripe = stripeOf( place, place.offset );
      auto const* constant = std::get_if<ir::Constant>( &place.index );
      if ( constant != nullptr && ir::sizeOf( place.type ) == 1 )
      {
        return ir::Object{ .name = offsetName( ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = stripe },
                                               constant->value ),
                           .type = place.type };
      }
      ir::Value const result = define( site, place.type );
      site.blocks->append( ir::Instruction{
          .operation = ir::Load{ .result = result,
                                 .type = place.type,
                                 .name = stripe,
                                 .index = place.index,
                                 .scaled = true,
                                 .high = ir::sizeOf( place.type ) == 2 ? stripeOf( place, place.offset + 1 ) : "" },
          .at = site.at } );
      return result;
    }
    ir::Value const result = define( site, place.type );
    if ( place.kind == Reach::Kind::INDEXED )
    {
      site.blocks->append( ir::Instruction{ .operation = ir::Load{ .result = result,
                                                                   .type = place.type,
                                                                   .name = place.name,
                                                                   .index = place.index,
                                                                   .scaled = place.scaled },
                                            .at = site.at } );
      return result;
    }
    site.blocks->append( ir::Instruction{ .operation = ir::LoadIndirect{ .result = result,
                                                                         .type = place.type,
                                                                         .pointer = place.pointer,
                                                                         .index = place.index,
                                                                         .isVolatile = place.isVolatile },
                                          .at = site.at } );
    return result;
  }

  static void write( Reach const& place, ir::Operand value, Site const& site )
  {
    switch ( place.kind )
    {
    case Reach::Kind::STRIPED:
    {
      std::string const stripe = stripeOf( place, place.offset );
      auto const* constant = std::get_if<ir::Constant>( &place.index );
      if ( constant != nullptr && ir::sizeOf( place.type ) == 1 )
      {
        site.blocks->append( ir::Instruction{
            .operation =
                ir::Store{ .name = offsetName( ir::Constant{ .type = ir::Type::POINTER, .value = 0, .name = stripe },
                                               constant->value ),
                           .type = place.type,
                           .value = std::move( value ) },
            .at = site.at } );
        return;
      }
      site.blocks->append( ir::Instruction{
          .operation =
              ir::StoreElement{ .name = stripe,
                                .type = place.type,
                                .index = place.index,
                                .value = std::move( value ),
                                .scaled = true,
                                .high = ir::sizeOf( place.type ) == 2 ? stripeOf( place, place.offset + 1 ) : "" },
          .at = site.at } );
      return;
    }
    case Reach::Kind::OBJECT:
      site.blocks->append( ir::Instruction{
          .operation = ir::Store{ .name = place.name, .type = place.type, .value = std::move( value ) },
          .at = site.at } );
      return;
    case Reach::Kind::INDEXED:
      site.blocks->append( ir::Instruction{ .operation = ir::StoreElement{ .name = place.name,
                                                                           .type = place.type,
                                                                           .index = place.index,
                                                                           .value = std::move( value ),
                                                                           .scaled = place.scaled },
                                            .at = site.at } );
      return;
    case Reach::Kind::INDIRECT:
      site.blocks->append( ir::Instruction{ .operation = ir::StoreIndirect{ .type = place.type,
                                                                            .pointer = place.pointer,
                                                                            .index = place.index,
                                                                            .value = std::move( value ),
                                                                            .isVolatile = place.isVolatile },
                                            .at = site.at } );
      return;
    }
  }

  /// An operand in `type`: a constant held in it, an object read as its low
  /// bytes where it narrows, and anything else converted by an instruction —
  /// see docs/decisions/0084-widths-data-arrays-pointers-aggregates.md.
  static ir::Operand converted( ir::Operand value, ir::Type type, Site const& site )
  {
    ir::Type const source = ir::typeOf( value, *site.function );
    if ( source == type )
    {
      return value;
    }
    if ( auto const* constant = std::get_if<ir::Constant>( &value ) )
    {
      // A constant only the assembler knows keeps its name, narrowed to its
      // low byte where the type is one byte.
      std::string name = constant->name;
      if ( !name.empty() && ir::sizeOf( type ) < ir::sizeOf( source ) )
      {
        name = "<(" + name + ")";
      }
      return ir::Constant{ .type = type, .value = wrapped( constant->value, type ), .name = std::move( name ) };
    }
    if ( auto const* object = std::get_if<ir::Object>( &value );
         object != nullptr && ir::sizeOf( type ) <= ir::sizeOf( source ) )
    {
      return ir::Object{ .name = object->name, .type = type };
    }
    ir::Value const result = define( site, type );
    site.blocks->append( ir::Instruction{
        .operation = ir::Convert{ .result = result, .type = type, .operand = std::move( value ) }, .at = site.at } );
    return result;
  }

  ir::Operand comparison( Expression const& node, Site const& site )
  {
    // Compared in the wider of the two types, a constant taking the other's.
    std::optional<ir::Type> const leftType = mTyping.expression( *node.left ).type;
    std::optional<ir::Type> const rightType = mTyping.expression( *node.right ).type;
    ir::Type const type = leftType.has_value() && rightType.has_value()
                              ? widerOf( *leftType, *rightType )
                              : leftType.value_or( rightType.value_or( ir::Type::U8 ) );
    ir::Operand left = converted( operand( *node.left, type, site ), type, site );
    ir::Operand right = converted( operand( *node.right, type, site ), type, site );

    ir::Comparison op = ir::Comparison::EQUAL;
    bool swapped = false;
    switch ( node.token.kind )
    {
    case TokenKind::BANG_EQUAL:
      op = ir::Comparison::NOT_EQUAL;
      break;
    case TokenKind::LESS:
      op = ir::Comparison::LESS;
      break;
    case TokenKind::GREATER:
      op = ir::Comparison::LESS;
      swapped = true;
      break;
    case TokenKind::LESS_EQUAL:
      op = ir::Comparison::GREATER_OR_EQUAL;
      swapped = true;
      break;
    case TokenKind::GREATER_EQUAL:
      op = ir::Comparison::GREATER_OR_EQUAL;
      break;
    default:
      break;
    }
    if ( swapped )
    {
      std::swap( left, right );
    }

    ir::Value const result = define( site, ir::Type::BOOL );
    site.blocks->append( ir::Instruction{
        .operation =
            ir::Compare{
                .result = result, .op = op, .type = type, .left = std::move( left ), .right = std::move( right ) },
        .at = site.at } );
    return result;
  }

  [[nodiscard]] std::string spellingOf( Token const& token ) const
  {
    return detail::spellingOf( *mSources, token );
  }

  diag::SourceManager const* mSources;
  Names const* mNames;
  Unit const* mUnit;
  diag::DiagnosticSink* mSink;
  Typing mTyping;
  std::vector<Loop> mLoops;

  /// The Pane the function being lowered is in, or empty, and the `[[with]]`
  /// blocks the statement being lowered stands in, outermost first — see
  /// docs/decisions/0096-panes-in-c.md.
  std::string mPane;
  std::string mUnder;

  /// The function type whose Temporaries the function being lowered reads,
  /// or empty — see docs/decisions/0065-handlers.md.
  std::string mMemberOf;
  std::vector<Shown> mShown;

  /// The `[[with]]` blocks of the file so far, which number their macros
  /// and their Trampolines.
  std::uint32_t mWiths = 0;

  /// The function being lowered, which a case of a `switch` in it names its
  /// parameters and its result through, and what it returns.
  std::string mOwner;
  std::optional<NamedType> mResult;

  /// Per Proc being lowered: how many locals it has taken a byte for, which
  /// numbers the next, and how many bytes a `&&` or an `||` has needed.
  /// The classes in force, the function's at the bottom and a block's above
  /// it: what the program names takes the innermost, and what the compiler
  /// takes for itself the function's — see
  /// docs/decisions/0210-placement-is-declared-in-c-too.md.
  std::vector<ir::Placement> mPlacement{ ir::Placement::ZEROPAGE };
  /// Whether the function being lowered carries `[[placement]]`, which is
  /// what tells the bottom of `mPlacement` asked for from the default.
  bool mFunctionPlaced = false;

  /// What a Proc being lowered has numbered so far, so that each byte it
  /// names is a name of its own.
  struct Counted
  {
    /// The locals given bytes; a `static` one is numbered among the
    /// function's own, the first entry, from a `[[with]]` block's Proc too.
    std::uint32_t locals = 0;
    /// The bytes that have kept a condition, or a value a multiplication
    /// reads more than once.
    std::uint32_t conditions = 0;
    /// The bytes that have kept an index into a striped array that more than
    /// one instruction reads.
    std::uint32_t indexes = 0;
    /// The bytes that have kept the value a `switch` written where it stands
    /// tests — see docs/decisions/0164-a-small-switch-stands-where-it-is.md.
    std::uint32_t tested = 0;
  };

  /// One per Proc being lowered: the function's, then a `[[with]]` block's.
  std::vector<Counted> mCounted;

  /// The Procs the `[[with]]` blocks of the function being lowered run, each
  /// a block that is a Proc of its own — see docs/decisions/0096-panes-in-c.md.
  std::vector<ir::Function> mExtra;

  /// The `static` locals of the function being lowered, its cases' included.
  std::vector<ir::Global> mSections;

  /// The Sections of the file's string literals, by their characters.
  std::vector<ir::Global> mLiterals;
  std::map<std::string, std::string> mLiteralNames;

  /// Where each local whose address is taken is declared.
  std::set<std::uint32_t> const* mAddressed;

  /// The names of what is `volatile`, as the lowered unit names them — see
  /// docs/decisions/0151-volatile.md.
  std::set<std::string> mVolatileNames;

  /// Which function type each member belongs to, gathered while the program
  /// was checked — see docs/decisions/0065-handlers.md.
  Membership const* mMembers = nullptr;
};

} // namespace

ir::Unit lowerUnit( diag::SourceManager const& sources,
                    Names const& names,
                    Unit const& unit,
                    diag::DiagnosticSink& sink,
                    std::set<std::uint32_t> const& addressed,
                    Membership const& members )
{
  return Lowering{ sources, names, unit, sink, addressed, members }.translationUnit( unit.tree );
}

} // namespace nga::c::detail
