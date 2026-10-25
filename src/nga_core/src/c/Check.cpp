#include "nga/c/Check.hpp"

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

/// The second pass: what every statement of a file names, and whether the
/// compiler compiles it.
class Checker
{
public:
  Checker( diag::SourceManager const& sources,
           Names const& names,
           Unit const& unit,
           std::size_t index,
           std::vector<CallEdge>& edges,
           std::set<std::uint32_t>& addressed,
           Membership& members )
      : mSources( &sources ), mNames( &names ), mUnit( &unit ), mIndex( index ), mEdges( &edges ),
        mAddressed( &addressed ), mTyping( sources, names, unit, unit.sink, &members )
  {
    mTyping.recordAddresses( &addressed );
  }

  void check()
  {
    for ( ExternalDeclaration const& declaration : mUnit->tree.declarations )
    {
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration );
           function != nullptr && function->isTypedef )
      {
        // A function type declares no function: what it names is the Proc a
        // pointer to it goes through — see docs/decisions/0065-handlers.md.
        attributesKnown( function->attributes, AttributeSite::FUNCTION );
        if ( Attribute const* const noinline = bareAttribute( function->attributes, *mSources, "noinline" ) )
        {
          mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_MISPLACED )
                           .at( noinline->span.begin, noinline->span.length )
                           .arg( "attribute", std::string{ "noinline" } )
                           .arg( "applies", std::string{ "a function" } )
                           .arg( "what", std::string{ "a function type" } ) );
        }
        continue;
      }
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration );
           function != nullptr && function->isExtern )
      {
        externProc( *function );
        continue;
      }
      if ( auto const* function = std::get_if<FunctionDefinition>( &declaration ) )
      {
        functionDefinition( *function );
        continue;
      }
      if ( auto const* object = std::get_if<Declaration>( &declaration ) )
      {
        fileScope( *object );
      }
      if ( auto const* aggregate = std::get_if<StructSpecifier>( &declaration ) )
      {
        structure( *aggregate );
      }
    }
  }

private:
  /// A `struct` or a `union`: members, each named once, none `const` nor
  /// given a value, and at least one — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
  void structure( StructSpecifier const& aggregate )
  {
    std::string const typeName = spellingOf( aggregate.name );
    std::size_t members = 0;
    std::map<std::string, Token, std::less<>> seen;
    for ( Declaration const& member : aggregate.members )
    {
      for ( std::size_t index = 0; index < member.declarators.size(); ++index )
      {
        ++members;
        Token const& name = member.declarators[index];
        DeclaratorShape const& shape = member.shapes[index];
        std::string const spelling = spellingOf( name );
        if ( auto const [existing, added] = seen.try_emplace( spelling, name ); !added )
        {
          mTyping.add( diagnostic( diag::DiagnosticId::C_MEMBER_TWICE )
                           .at( name.location, name.length )
                           .arg( "name", spelling )
                           .arg( "type", typeName ) );
        }
        if ( ( member.isConst && !shape.isPointer ) || shape.isConstPointer || member.initialisers[index] != nullptr ||
             shape.hasList )
        {
          mTyping.add( diagnostic( diag::DiagnosticId::C_MEMBER_REFUSED )
                           .at( name.location, name.length )
                           .arg( "name", spelling ) );
        }
        if ( ( member.isVolatile && !shape.isPointer ) || shape.isVolatilePointer )
        {
          mTyping.refuse( name.span(), "a `volatile` member" );
        }
        if ( shape.isArray && shape.isPointer )
        {
          mTyping.refuse( member.span, "an array of pointers" );
        }
        else if ( shape.isArray )
        {
          std::optional<Meaning> const type = mTyping.declaredType( member.type );
          arraySize( name,
                     shape,
                     type.has_value() ? type->type : ir::Type::U8,
                     type.has_value() ? type->aggregate : std::nullopt );
        }
      }
    }
    if ( members == 0 )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EMPTY_AGGREGATE )
                       .at( aggregate.name.location, aggregate.name.length )
                       .arg( "name", typeName ) );
    }
  }

  /// Objects at file scope: a `const` given a value, and any object given one
  /// given a constant, since the loader writes it — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
  void fileScope( Declaration const& object )
  {
    if ( object.type.keyword == Keyword::AUTO )
    {
      // An object at file scope is given a constant, which has no type of its
      // own to deduce from — see
      // docs/decisions/0161-auto-takes-the-type-of-the-value.md.
      mTyping.refuse( object.type.span(), "`auto` at file scope" );
      return;
    }
    attributesKnown( object.attributes, object.isExtern ? AttributeSite::EXTERN : AttributeSite::GLOBAL );
    if ( !object.isExtern )
    {
      placementAsked(
          object, placementAttribute( object.attributes, *mSources ), isStriped( object.attributes, *mSources ), true );
    }
    for ( std::size_t index = 0; index < object.declarators.size(); ++index )
    {
      Token const& declarator = object.declarators[index];
      auto const found = mUnit->definitions.find( nameSpelled( mSources->textOf( declarator.span() ) ) );
      if ( found == mUnit->definitions.end() || found->second.name.location != declarator.location )
      {
        continue;
      }
      Definition const& definition = found->second;
      DeclaratorShape const& array = object.shapes[index];
      if ( !shaped( object, index ) )
      {
        continue;
      }
      if ( definition.function.has_value() && !array.isPointer && definition.typeName.has_value() )
      {
        // A function type has no bytes, so nothing is of it: it is a type as
        // a pointer alone — see
        // docs/decisions/0103-what-building-handlers-needs.md. The name is
        // there by construction: a function type is reached by one.
        mTyping.add( diagnostic( diag::DiagnosticId::C_FUNCTION_TYPE_VALUE )
                         .at( declarator.location, declarator.length )
                         .arg( "name", spellingOf( *definition.typeName ) ) );
        continue;
      }
      if ( definition.isExtern )
      {
        externDeclaration( object, index, definition );
        continue;
      }
      if ( !definition.pane.empty() && definition.type == ir::Type::POINTER && !definition.isArray )
      {
        // A pointer lies on the zero page, which no Window covers.
        mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_MISPLACED )
                         .at( declarator.location, declarator.length )
                         .arg( "attribute", std::string{ "in" } )
                         .arg( "applies", std::string{ "a function or an object that is no pointer" } )
                         .arg( "what", "`" + spellingOf( declarator ) + "`" ) );
      }
      implementsSlot( declarator, definition );
      Meaning type;
      type.kind = NameKind::OBJECT;
      giveType( type, definition );
      if ( array.isArray )
      {
        type.isArray = true;
        type.count = arraySize( declarator, array, type.type, type.aggregate );
      }
      if ( definition.isStriped )
      {
        striped( declarator, array, type );
      }
      given( declarator, object.initialisers[index].get(), array, definition.isConst, true, type );
    }
  }

  /// `extern TYPE NAME;`: a name the assembler exports, read as the type says
  /// and held to the bytes — see
  /// docs/decisions/0093-extern-declares-how-c-reads-the-assembler.md.
  void externDeclaration( Declaration const& object, std::size_t index, Definition const& definition )
  {
    Token const& declarator = object.declarators[index];
    std::string const spelling = spellingOf( declarator );
    auto const refuse = [&]( std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_REFUSED )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "what", std::move( what ) ) );
    };
    if ( object.initialisers[index] != nullptr || object.shapes[index].hasList )
    {
      refuse( "is given a value, and its bytes are the assembler's" );
      return;
    }
    if ( definition.isSlot )
    {
      slotDeclaration( object, index, definition );
      return;
    }
    if ( !object.attributes.empty() && !definition.isStriped )
    {
      refuse( "carries attributes, and where it stands is the assembler's" );
      return;
    }
    if ( definition.isStriped )
    {
      refuse( "is `[[striped]]`, and its layout is the assembler's" );
      return;
    }
    ExternalName const* const external = externalOf( declarator, definition );
    if ( external == nullptr )
    {
      return;
    }
    switch ( external->kind )
    {
    case ExternalKind::PROC:
      refuse( "names a `.proc`, which is declared as a function" );
      return;
    case ExternalKind::UNREAD:
    case ExternalKind::RESERVED:
    case ExternalKind::CHARSET:
    case ExternalKind::PANE:
    case ExternalKind::WINDOW:
      mTyping.unread( declarator, *external );
      return;
    case ExternalKind::CONSTANT:
      externConstant( declarator, definition, *external );
      return;
    case ExternalKind::MEMORY:
    case ExternalKind::SHAPELESS:
      externData( declarator, definition, *external );
      return;
    }
  }

  /// `extern RESULT NAME(PARAMETERS);`: a Proc of the assembler, its count of
  /// parameters and its result held to its `.declare`s, and each parameter's
  /// name and type to its byte — see
  /// docs/decisions/0094-extern-declares-how-c-calls-a-proc.md.
  /// A Slot to data: `[[slot]] extern T* const NAME;`. The Binding is the
  /// type's, so the type must be one a Binding comes off — a pointer, and a
  /// `const` one, since the Cell is the Transition routine's to write. See
  /// docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md.
  void slotDeclaration( Declaration const& object, std::size_t index, Definition const& definition )
  {
    Token const& declarator = object.declarators[index];
    std::string const spelling = spellingOf( declarator );
    auto const refuse = [&]( std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_SLOT_TYPE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "what", std::move( what ) ) );
    };
    if ( !onlySlotAttribute( object.attributes ) )
    {
      return;
    }
    if ( definition.isArray || object.shapes[index].isArray )
    {
      refuse( "this is an array" );
      return;
    }
    if ( definition.type != ir::Type::POINTER )
    {
      refuse( "this is a `" + std::string{ ir::spellingOf( definition.type ) } + "`" );
      return;
    }
    if ( !definition.isConst )
    {
      refuse( "this pointer is not `const`, and the cell is the transition routine's to write" );
      return;
    }
    isFirstExtern( declarator, definition );
  }

  /// Whether `definition` is the first `extern` of its name, which a second
  /// is refused as.
  bool isFirstExtern( Token const& declarator, Definition const& definition )
  {
    std::string const spelling = spellingOf( declarator );
    Definition const* const first = mNames->firstExtern( spelling );
    if ( first == &definition || first == nullptr )
    {
      return true;
    }
    mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_TWICE )
                     .at( declarator.location, declarator.length )
                     .arg( "name", spelling )
                     .note( diagnostic( diag::DiagnosticId::C_PREVIOUS_DEFINITION )
                                .at( first->name.location, first->name.length )
                                .arg( "name", spelling ) ) );
    return false;
  }

  /// What the assembler exports that an `extern` names, where it is the
  /// first of its name, names nothing of C and names something; nothing,
  /// each refused, where it is not.
  [[nodiscard]] ExternalName const* externalOf( Token const& declarator, Definition const& definition )
  {
    if ( !isFirstExtern( declarator, definition ) )
    {
      return nullptr;
    }
    std::string const spelling = spellingOf( declarator );
    if ( mNames->definedInC( spelling ) != nullptr )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_OF_C )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling ) );
      return nullptr;
    }
    ExternalName const* const external = mNames->externalNamed( spelling );
    if ( external == nullptr )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_OF_NOTHING )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling ) );
    }
    return external;
  }

  /// Whether a Slot carries `slot` and nothing else: where it stands is the
  /// assembler's, so any other attribute is refused.
  bool onlySlotAttribute( std::vector<Attribute> const& attributes )
  {
    auto const other = std::ranges::find_if(
        attributes, [this]( Attribute const& attribute ) { return spellingOf( attribute.name ) != "slot"; } );
    if ( other == attributes.end() )
    {
      return true;
    }
    mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_MISPLACED )
                     .at( other->span.begin, other->span.length )
                     .arg( "attribute", spellingOf( other->name ) )
                     .arg( "applies", std::string{ "a declaration the assembler does not own" } )
                     .arg( "what", std::string{ "a slot" } ) );
    return false;
  }

  /// What a definition written `[[implements(NAME)]]` fills: the Cell holds
  /// its address, and only C knows what the Cell is read as, since
  /// `.implements` hands the assembler an address and not a type — see
  /// docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md. A name that
  /// is no Slot of this program passes unread, and Merge answers it.
  void implementsSlot( Token const& where, Definition const& definition )
  {
    if ( definition.implements.empty() )
    {
      return;
    }
    std::optional<Meaning> const slot = mTyping.lookupName( definition.implements );
    if ( !slot.has_value() || slot->definition == nullptr || !slot->definition->isSlot )
    {
      return;
    }
    Definition const& declared = *slot->definition;
    auto const refuse = [&]( std::string wanted, std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_IMPLEMENTS_TYPE )
                       .at( where.location, where.length )
                       .arg( "name", spellingOf( where ) )
                       .arg( "slot", definition.implements )
                       .arg( "wanted", std::move( wanted ) )
                       .arg( "what", std::move( what ) ) );
    };
    if ( declared.kind == NameKind::FUNCTION )
    {
      std::string const wanted =
          declared.function.has_value() ? "a function of the slot's own signature" : "a `void(void)`";
      if ( definition.kind != NameKind::FUNCTION )
      {
        refuse( wanted, "this is an object" );
        return;
      }
      if ( !declared.function.has_value() )
      {
        if ( !definition.parameters.empty() || definition.result.has_value() )
        {
          refuse( wanted, "this one takes or returns something" );
        }
        return;
      }
      // The Slot is the function type, so an Implementation is held to it as
      // a taking holds `&f`, and joins it — see
      // docs/decisions/0174-a-slot-takes-and-returns.md.
      FunctionType const& type = mNames->functionType( *declared.function );
      if ( std::optional<std::string> const wrong = mTyping.signatureDiffers( type, definition ); wrong.has_value() )
      {
        refuse( wanted, *wrong );
        return;
      }
      mTyping.joinType( &definition, *declared.function );
      return;
    }
    if ( definition.kind != NameKind::OBJECT )
    {
      refuse( "what the slot points at", "this is a function" );
      return;
    }
    if ( !declared.pointee.has_value() )
    {
      return;
    }
    Pointee const& wanted = *declared.pointee;
    Pointee const held = pointeeOf( typeOfValue( definition ), definition.isConst );
    if ( definition.type == ir::Type::POINTER )
    {
      if ( !definition.pointee.has_value() || !samePointee( wanted, *definition.pointee ) )
      {
        refuse( describePointee( wanted ), "this points at something else" );
      }
      return;
    }
    if ( !samePointee( wanted, held ) )
    {
      refuse( describePointee( wanted ), "this is " + describePointee( held ) );
    }
  }

  /// A pointee as a finding spells it.
  [[nodiscard]] std::string describePointee( Pointee const& pointee ) const
  {
    return mTyping.describe( typedAs( typeOfValue( pointee ) ) );
  }

  void externProc( FunctionDefinition const& function )
  {
    Token const& declarator = function.name;
    std::string const spelling = spellingOf( declarator );
    Definition const* const definition = definitionOf( *mSources, *mUnit, function );
    if ( definition == nullptr )
    {
      return;
    }
    if ( definition->isSlot )
    {
      // A Slot to a function binds a `vector`, and what the Cell holds is
      // one Implementation per Phase: their bytes are their own, and what
      // would name parameters across them is the handler type of 0065.
      onlySlotAttribute( function.attributes );
      return;
    }
    if ( !function.attributes.empty() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_REFUSED )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "what", std::string{ "carries attributes, and the Proc's are the assembler's" } ) );
      return;
    }
    ExternalName const* const proc = externalOf( declarator, *definition );
    if ( proc == nullptr )
    {
      return;
    }
    auto const mismatch = [&]( Token const& at, std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_SIGNATURE )
                       .at( at.location, at.length )
                       .arg( "name", spelling )
                       .arg( "what", std::move( what ) ) );
    };
    if ( proc->kind != ExternalKind::PROC )
    {
      mismatch( declarator, "names no `.proc`" );
      return;
    }
    if ( definition->parameters.size() != proc->arguments.size() )
    {
      mismatch( declarator,
                "has " + counted( definition->parameters.size(), "parameter" ) + ", and the `.proc` declares " +
                    counted( proc->arguments.size(), "argument" ) );
      return;
    }
    if ( definition->result.has_value() != proc->result.has_value() )
    {
      mismatch( declarator,
                definition->result.has_value() ? "returns a value, and the `.proc` declares no result"
                                               : "returns nothing, and the `.proc` declares a result" );
      return;
    }
    for ( std::size_t index = 0; index < definition->parameters.size(); ++index )
    {
      ParameterType const& parameter = definition->parameters[index];
      ExternalByte const& byte = proc->arguments[index];
      // A byte a register carries has no name to be held to; one in part
      // in memory has its reservation's — see
      // docs/decisions/0145-an-argument-in-a-register.md.
      std::string_view const byteName = byte.name.empty()
                                            ? std::string_view{ byte.place }
                                            : std::string_view{ byte.name }.substr( byte.name.rfind( '.' ) + 1 );
      if ( parameter.isNamed && !byte.name.empty() && spellingOf( parameter.name ) != byteName )
      {
        mismatch( parameter.name,
                  "names its parameter `" + spellingOf( parameter.name ) + "` where the `.proc` declares `" +
                      std::string{ byteName } + "`" );
        return;
      }
      if ( !fitsByte( parameter.type, byte ) )
      {
        mismatch( parameter.name,
                  "reads `" + std::string{ byteName } + "`, " + describeByte( byte ) + ", as " +
                      describeNamed( parameter.type ) );
        return;
      }
    }
    if ( proc->result.has_value() && !fitsByte( *definition->result, *proc->result ) )
    {
      mismatch( declarator,
                "returns " + describeNamed( *definition->result ) + ", and the `.proc` returns " +
                    describeByte( *proc->result ) );
    }
  }

  /// Whether a type of C may be read over a declared byte, by 0094's table.
  [[nodiscard]] bool fitsByte( NamedType const& type, ExternalByte const& byte ) const
  {
    std::uint32_t const bytes = type.pointee.has_value() ? 2U : mNames->bytes( type.type, type.aggregate );
    if ( bytes != byte.bytes )
    {
      return false;
    }
    bool const aggregate = type.aggregate.has_value() && !type.pointee.has_value();
    // A `struct` is copied byte by byte into memory, which a register is not.
    if ( aggregate && !byte.place.empty() )
    {
      return false;
    }
    if ( !byte.isWritten )
    {
      // By its shape: any type of its size, and bytes only as a `struct`.
      return byte.bytes <= 2 || aggregate;
    }
    if ( byte.isBytes )
    {
      return aggregate;
    }
    if ( aggregate )
    {
      return false;
    }
    if ( type.pointee.has_value() )
    {
      return byte.type == ir::Type::U16;
    }
    if ( type.enumeration.has_value() )
    {
      return byte.type == ir::Type::U8;
    }
    return type.type == byte.type;
  }

  [[nodiscard]] static std::string describeByte( ExternalByte const& byte )
  {
    if ( !byte.place.empty() )
    {
      return counted( byte.bytes, "byte" ) + " in `" + byte.place + "`";
    }
    if ( !byte.isWritten )
    {
      return counted( byte.bytes, "byte" ) + " of no type written";
    }
    if ( byte.isBytes )
    {
      return "`u8[" + std::to_string( byte.bytes ) + "]`";
    }
    return "`" + std::string{ ir::spellingOf( byte.type ) } + "`";
  }

  [[nodiscard]] std::string describeNamed( NamedType const& type ) const
  {
    if ( type.pointee.has_value() )
    {
      return "a pointer";
    }
    if ( type.aggregate.has_value() )
    {
      return "`" + mNames->aggregate( *type.aggregate ).name + "`";
    }
    if ( type.enumeration.has_value() )
    {
      return "`" + mNames->enumeration( *type.enumeration ).name + "`";
    }
    return "`" + std::string{ ir::spellingOf( type.type ) } + "`";
  }

  /// A Constant declared: `const`, a scalar, an `enum struct` or a pointer,
  /// and a value it holds where it folds.
  void externConstant( Token const& declarator, Definition const& definition, ExternalName const& external )
  {
    std::string const spelling = spellingOf( declarator );
    if ( !definition.isConst || definition.isArray || definition.aggregate.has_value() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_REFUSED )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "what",
                             std::string{ "names a Constant, which is declared `const`, as a scalar, an `enum struct` "
                                          "or a pointer" } ) );
      return;
    }
    if ( !external.value.has_value() )
    {
      return;
    }
    std::int64_t const value = *external.value;
    if ( definition.enumeration.has_value() || definition.type == ir::Type::BOOL )
    {
      std::size_t const count =
          definition.enumeration.has_value() ? mNames->enumeration( *definition.enumeration ).enumerators.size() : 2;
      if ( value < 0 || std::cmp_greater_equal( value, count ) )
      {
        notOfType( declarator, "`" + spelling + "`", value, definition );
      }
      return;
    }
    if ( value < lowestOf( definition.type ) || value > highestOf( definition.type ) )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_CONSTANT_OUT_OF_RANGE )
                       .at( declarator.location, declarator.length )
                       .arg( "value", value )
                       .arg( "type", std::string{ ir::spellingOf( definition.type ) } ) );
    }
  }

  void notOfType( Token const& declarator, std::string what, std::int64_t value, Definition const& definition )
  {
    std::string const type = definition.enumeration.has_value() ? mNames->enumeration( *definition.enumeration ).name
                                                                : std::string{ ir::spellingOf( definition.type ) };
    mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_VALUE )
                     .at( declarator.location, declarator.length )
                     .arg( "what", std::move( what ) )
                     .arg( "value", value )
                     .arg( "type", type ) );
  }

  /// One value of C a declaration reads: where it lies and what it is.
  struct Leaf
  {
    std::uint32_t offset = 0;
    std::uint32_t bytes = 1;
    ir::Type type = ir::Type::U8;
    std::optional<std::uint32_t> enumeration{};
    std::string path{};
  };

  /// The values of C a type holds, member by member and element by element.
  void leavesOf( ir::Type type,
                 std::optional<std::uint32_t> enumeration,
                 std::optional<std::uint32_t> aggregate,
                 bool pointer,
                 std::uint32_t offset,
                 std::string const& path,
                 std::vector<Leaf>& into ) const
  {
    if ( aggregate.has_value() && !pointer )
    {
      for ( MemberType const& member : mNames->aggregate( *aggregate ).members )
      {
        std::uint32_t const bytes =
            mNames->bytes( member.type.pointee.has_value() ? ir::Type::POINTER : member.type.type,
                           member.type.pointee.has_value() ? std::nullopt : member.type.aggregate );
        std::uint32_t const count = member.isArray ? member.count : 1U;
        std::string const named = path + "." + spellingOf( member.name );
        for ( std::uint32_t element = 0; element < count; ++element )
        {
          leavesOf( member.type.type,
                    member.type.enumeration,
                    member.type.aggregate,
                    member.type.pointee.has_value(),
                    member.offset + offset + ( element * bytes ),
                    member.isArray ? named + "[" + std::to_string( element ) + "]" : named,
                    into );
        }
      }
      return;
    }
    ir::Type const leaf = pointer ? ir::Type::POINTER : type;
    into.push_back( Leaf{ .offset = offset,
                          .bytes = ir::sizeOf( leaf ),
                          .type = leaf,
                          .enumeration = pointer ? std::nullopt : enumeration,
                          .path = path } );
  }

  /// A Label or a Region declared: as many bytes as the type, laid over the
  /// items with no value of C splitting one, and the values that fold held
  /// to a `bool` and an `enum struct`.
  void externData( Token const& declarator, Definition const& definition, ExternalName const& external )
  {
    std::string const spelling = spellingOf( declarator );
    Meaning const meaning = mTyping.lookupName( spelling ).value_or( Meaning{} );
    bool const pointer = definition.pointee.has_value();
    std::uint32_t const element =
        pointer ? 2U : mNames->bytes( definition.type, pointer ? std::nullopt : definition.aggregate );
    std::uint32_t const count = definition.isArray ? meaning.count.value_or( 0 ) : 1U;
    std::uint32_t const run = runBytes( external );
    auto const size = [&]( std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_SIZE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "what", std::move( what ) ) );
    };
    auto const notData = [&]
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_NOT_DATA )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spelling )
                       .arg( "reason", external.stop ) );
    };

    // A type of no bytes was refused where it was declared (NGA7250), and
    // there is no laying it over the items.
    if ( element == 0 )
    {
      return;
    }
    std::uint32_t const total = element * count;
    if ( definition.isArray && definition.size == nullptr )
    {
      if ( !external.stop.empty() )
      {
        notData();
        return;
      }
      if ( run == 0 || run % element != 0 )
      {
        size( "reads elements of " + counted( element, "byte" ) + ", and the " + counted( run, "byte" ) +
              " to the end of its Section are no whole number of them" );
        return;
      }
    }
    else if ( external.isRegion && run != total )
    {
      size( "reads " + counted( total, "byte" ) + ", and the Region is " + std::to_string( run ) + " long" );
      return;
    }
    else if ( run < total )
    {
      if ( !external.stop.empty() )
      {
        notData();
        return;
      }
      size( "reads " + counted( total, "byte" ) + ", and the assembler wrote " + std::to_string( run ) +
            " to the end of its Section" );
      return;
    }

    // Where a value of C may begin and end: between two items, and anywhere
    // inside a reservation.
    std::vector<bool> between( total + 1U, false );
    std::map<std::uint32_t, ExternalCell const*> items;
    std::uint32_t at = 0;
    for ( ExternalCell const& cell : external.cells )
    {
      if ( at > total )
      {
        break;
      }
      between[at] = true;
      if ( !cell.isReserved )
      {
        items.emplace( at, &cell );
      }
      for ( std::uint32_t inside = 1; cell.isReserved && inside < cell.bytes && at + inside <= total; ++inside )
      {
        between[at + inside] = true;
      }
      at += cell.bytes;
      if ( at <= total )
      {
        between[at] = true;
      }
    }

    std::vector<Leaf> leaves;
    for ( std::uint32_t index = 0; index < count; ++index )
    {
      leavesOf( definition.type,
                definition.enumeration,
                definition.aggregate,
                pointer,
                index * element,
                definition.isArray ? spelling + "[" + std::to_string( index ) + "]" : spelling,
                leaves );
    }
    for ( Leaf const& leaf : leaves )
    {
      if ( !between[leaf.offset] || !between[leaf.offset + leaf.bytes] )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_SPLITS )
                         .at( declarator.location, declarator.length )
                         .arg( "path", leaf.path )
                         .arg( "offset", static_cast<std::int64_t>( leaf.offset ) ) );
        return;
      }
    }
    for ( Leaf const& leaf : leaves )
    {
      if ( !leaf.enumeration.has_value() && leaf.type != ir::Type::BOOL )
      {
        continue;
      }
      auto const item = items.find( leaf.offset );
      std::optional<std::int64_t> const itemValue = item == items.end() ? std::nullopt : item->second->value;
      if ( !itemValue.has_value() )
      {
        continue;
      }
      std::int64_t const value = *itemValue;
      std::size_t const values =
          leaf.enumeration.has_value() ? mNames->enumeration( *leaf.enumeration ).enumerators.size() : 2;
      if ( value < 0 || std::cmp_greater_equal( value, values ) )
      {
        std::string const type = leaf.enumeration.has_value() ? mNames->enumeration( *leaf.enumeration ).name
                                                              : std::string{ ir::spellingOf( leaf.type ) };
        mTyping.add( diagnostic( diag::DiagnosticId::C_EXTERN_VALUE )
                         .at( declarator.location, declarator.length )
                         .arg( "what", "`" + leaf.path + "`" )
                         .arg( "value", value )
                         .arg( "type", type ) );
        return;
      }
    }
  }

  /// Where attributes stand: on a function, on a declaration at file scope, on
  /// an `extern`, on a declaration of locals, or before a block.
  enum class AttributeSite : std::uint8_t
  {
    FUNCTION,
    GLOBAL,
    EXTERN,
    LOCAL,
    BLOCK,
    RETURN,
  };

  /// What an attribute stands on, as a finding says it.
  [[nodiscard]] static std::string_view whatStands( AttributeSite site )
  {
    switch ( site )
    {
    case AttributeSite::FUNCTION:
      return "a function";
    case AttributeSite::BLOCK:
      return "a block";
    case AttributeSite::LOCAL:
      return "a local";
    case AttributeSite::RETURN:
      return "a `return`";
    case AttributeSite::GLOBAL:
    case AttributeSite::EXTERN:
      break;
    }
    return "a declaration";
  }

  /// The attributes of a declaration, a function or a block: the subset's,
  /// written without a prefix — see
  /// docs/decisions/0088-an-attribute-has-no-prefix.md; `[[striped]]` on an
  /// array alone, `[[in(PANE)]]` on a function or an object at file scope,
  /// `[[with(...)]]` before a block — see docs/decisions/0096-panes-in-c.md.
  /// `[[transition(PHASE)]]` before a `return` — see
  /// docs/decisions/0064-phase-in-c.md. What `with` is given is read where
  /// the block is checked, and what `transition` is where the `return` is.
  /// What `[[placement]]` is refused over, and where it says nothing — see
  /// docs/decisions/0210-placement-is-declared-in-c-too.md. A pointer is read
  /// through by `(zp),y` and lies in the zero page whatever is asked; a
  /// striped array's place is settled by its layout; and asking for the class
  /// a declaration has anyway is a `.off` that silenced nothing.
  void placementAsked( Declaration const& declared,
                       std::optional<ir::Placement> asked,
                       bool stripes,
                       bool atFileScope,
                       std::optional<std::uint32_t> blockBytes = std::nullopt )
  {
    if ( !asked.has_value() )
    {
      return;
    }
    auto const at = [&]( diag::DiagnosticId id, Token const& name )
    { return diagnostic( id ).at( name.location, name.length ).arg( "name", spellingOf( name ) ); };
    for ( std::size_t index = 0; index < declared.declarators.size(); ++index )
    {
      Token const& declarator = declared.declarators[index];
      if ( stripes )
      {
        mTyping.add( at( diag::DiagnosticId::C_PLACEMENT_AND_STRIPED, declarator ) );
        continue;
      }
      bool const pointer =
          index < declared.shapes.size() && declared.shapes[index].isPointer && !declared.shapes[index].isArray;
      if ( pointer && *asked == ir::Placement::ABSOLUTE )
      {
        mTyping.add( at( diag::DiagnosticId::C_PLACEMENT_ON_POINTER, declarator ) );
        continue;
      }
      // A pointer is `zeropage` wherever it stands, an object at file scope
      // is `absolute` with nothing over it, and a local takes the class of
      // the function or the block around it, unless it keeps its place for
      // the whole program. Whether a local's address is taken is known only
      // once the body is, so a local is judged there.
      if ( pointer || atFileScope )
      {
        saysNothing( declarator, *asked, pointer ? ir::Placement::ZEROPAGE : ir::Placement::ABSOLUTE );
        continue;
      }
      // An array is a Section or a Temporary of its own wherever it stands,
      // never a byte of the Proc, so it takes the class a kept local does; a
      // `struct` past a few bytes lies off the zero page whatever is asked.
      bool const array = index < declared.shapes.size() && declared.shapes[index].isArray;
      bool const large = !array && blockBytes.value_or( 0 ) > SMALL_BLOCK;
      mLocalPlacements.push_back(
          LocalPlacement{ .declarator = declarator,
                          .asked = *asked,
                          .inForce = mPlacement,
                          .isLarge = large,
                          .isKept = array || declared.isStatic || ( declared.isVolatile && !blockBytes.has_value() ),
                          .isData = false,
                          .takesNoByte = false } );
    }
  }

  /// A local array or `struct` written `const` and given constants, which is
  /// kept as read-only data and lies off the zero page whatever is asked.
  void placedAsData( Token const& declarator )
  {
    for ( LocalPlacement& local : mLocalPlacements )
    {
      if ( local.declarator.location.rawOffset() == declarator.location.rawOffset() )
      {
        local.isData = true;
      }
    }
  }

  /// A local the lowering makes a Constant of the assembler, which takes no
  /// byte and so has no class to be asked for: `[[placement]]` on it is told
  /// it places nothing, and is judged no further.
  void placedNowhere( Token const& declarator )
  {
    for ( LocalPlacement& local : mLocalPlacements )
    {
      if ( local.declarator.location.rawOffset() == declarator.location.rawOffset() )
      {
        local.takesNoByte = true;
      }
    }
  }

  /// `NGA7307` where `asked` is the class `declarator` has anyway.
  void saysNothing( Token const& declarator, ir::Placement asked, ir::Placement inForce )
  {
    if ( asked == inForce )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_PLACEMENT_SAYS_NOTHING )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spellingOf( declarator ) )
                       .arg( "class", std::string{ asked == ir::Placement::ZEROPAGE ? "zeropage" : "absolute" } ) );
    }
  }

  /// The locals of the function just checked that asked for a class, against
  /// the class `localPlacement` gives each: an array, a `static` or
  /// `volatile` local, and one whose address the body took, is kept, and
  /// read-only data has its place whatever is asked — asking the zero page for
  /// it is an error rather than a word that says nothing.
  void localPlacementsJudged()
  {
    for ( LocalPlacement const& local : mLocalPlacements )
    {
      // Nothing is placed, which is no fault: the constant costs less than any
      // byte would, wherever that byte were. It is said so that the attribute
      // is not read as having done something.
      if ( local.takesNoByte )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_PLACEMENT_ON_CONSTANT )
                         .at( local.declarator.location, local.declarator.length )
                         .arg( "name", spellingOf( local.declarator ) ) );
        continue;
      }
      if ( local.isData && local.asked == ir::Placement::ZEROPAGE )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_PLACEMENT_ON_DATA )
                         .at( local.declarator.location, local.declarator.length )
                         .arg( "name", spellingOf( local.declarator ) ) );
        continue;
      }
      bool const kept = local.isKept || mAddressed->contains( local.declarator.location.rawOffset() );
      saysNothing( local.declarator,
                   local.asked,
                   localPlacement( local.inForce, mFunctionPlaced, kept, local.isLarge || local.isData ) );
    }
    mLocalPlacements.clear();
  }

  /// The classes in force while a body is checked, the function's at the
  /// bottom, so that an attribute asking for what is already in force can say
  /// so — see docs/decisions/0210-placement-is-declared-in-c-too.md.
  std::vector<ir::Placement> mPlacement{ ir::Placement::ZEROPAGE };
  /// Whether the function being checked carries `[[placement]]` itself.
  bool mFunctionPlaced = false;

  /// A local's `[[placement]]`, held until the body says whether its address
  /// is taken: the class asked for, the classes in force around it, and what
  /// `localPlacement` reads of the local itself.
  struct LocalPlacement
  {
    Token declarator;
    ir::Placement asked;
    std::vector<ir::Placement> inForce;
    bool isLarge;
    bool isKept;
    bool isData;
    bool takesNoByte = false;
  };

  std::vector<LocalPlacement> mLocalPlacements;
  /// Whether every value the declaration being checked was given is one a
  /// `static` local may be given, which is what keeps a `const` one as data.
  bool mListConstant = true;

  void attributesKnown( std::vector<Attribute> const& attributes, AttributeSite site )
  {
    auto const misplaced = [&]( Attribute const& attribute, std::string_view applies, std::string_view what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_MISPLACED )
                       .at( attribute.span.begin, attribute.span.length )
                       .arg( "attribute", spellingOf( attribute.name ) )
                       .arg( "applies", std::string{ applies } )
                       .arg( "what", std::string{ what } ) );
    };
    for ( Attribute const& attribute : attributes )
    {
      std::string const name = spellingOf( attribute.name );
      bool const known = name == "striped" || name == "in" || name == "with" || name == "under" ||
                         name == "trampoline" || name == "transition" || name == "slot" || name == "implements" ||
                         name == "placement" || name == "noinline";
      bool const bare = name == "striped" || name == "trampoline" || name == "slot" || name == "noinline";
      if ( attribute.prefix.has_value() || !known || ( bare && attribute.hasArguments ) )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_UNKNOWN_ATTRIBUTE )
                         .at( attribute.span.begin, attribute.span.length )
                         .arg( "name", std::string{ mSources->textOf( attribute.span ) } ) );
        continue;
      }
      if ( name == "slot" )
      {
        // A Slot is the one thing an `extern` declares rather than describes,
        // and its own check reads the type — see
        // docs/decisions/0101-what-a-phase-in-c-needs-to-be-built.md.
        if ( site != AttributeSite::EXTERN )
        {
          misplaced( attribute, "an `extern`", whatStands( site ) );
        }
        continue;
      }
      if ( name == "implements" )
      {
        if ( site != AttributeSite::FUNCTION && site != AttributeSite::GLOBAL )
        {
          misplaced( attribute, "a function or an object at file scope", whatStands( site ) );
        }
        continue;
      }
      if ( site == AttributeSite::EXTERN )
      {
        // The extern's own check refuses every attribute on it.
        continue;
      }
      if ( name == "noinline" )
      {
        // A function type's is checked where it is declared, since this site
        // is a function's either way.
        if ( site != AttributeSite::FUNCTION )
        {
          misplaced( attribute, "a function", whatStands( site ) );
        }
        continue;
      }
      if ( name == "placement" )
      {
        // The one attribute whose argument is a word of the model rather than
        // the name of something declared — see
        // docs/decisions/0210-placement-is-declared-in-c-too.md.
        if ( site == AttributeSite::RETURN )
        {
          misplaced( attribute, "a declaration, a block or a function", whatStands( site ) );
          continue;
        }
        std::string said;
        if ( attribute.arguments.size() == 1 && attribute.arguments.front().kind == TokenKind::IDENTIFIER )
        {
          said = spellingOf( attribute.arguments.front() );
        }
        if ( said != "zeropage" && said != "absolute" )
        {
          mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_ARGUMENTS )
                           .at( attribute.span.begin, attribute.span.length )
                           .arg( "attribute", name )
                           .arg( "takes", std::string{ "`zeropage` or `absolute`" } ) );
        }
        continue;
      }
      if ( name == "transition" )
      {
        if ( site != AttributeSite::RETURN )
        {
          misplaced( attribute, "a `return`", whatStands( site ) );
        }
        continue;
      }
      if ( site == AttributeSite::RETURN )
      {
        misplaced( attribute, name == "striped" ? "an array" : "a declaration or a block", whatStands( site ) );
        continue;
      }
      if ( name == "striped" )
      {
        if ( site == AttributeSite::FUNCTION || site == AttributeSite::BLOCK )
        {
          misplaced( attribute, "an array", site == AttributeSite::FUNCTION ? "a function" : "a block" );
        }
        continue;
      }
      if ( name == "with" || name == "trampoline" )
      {
        if ( site != AttributeSite::BLOCK )
        {
          misplaced( attribute, "a block", site == AttributeSite::FUNCTION ? "a function" : "a declaration" );
        }
        continue;
      }
      bool const under = name == "under";
      if ( site == AttributeSite::LOCAL || site == AttributeSite::BLOCK ||
           ( under && site != AttributeSite::FUNCTION ) )
      {
        std::string_view what = "an object";
        if ( site == AttributeSite::LOCAL )
        {
          what = "a local";
        }
        else if ( site == AttributeSite::BLOCK )
        {
          what = "a block";
        }
        misplaced( attribute, under ? "a function" : "a function or an object at file scope", what );
        continue;
      }
      if ( attribute.arguments.size() != 1 || attribute.arguments.front().kind != TokenKind::IDENTIFIER )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_ARGUMENTS )
                         .at( attribute.span.begin, attribute.span.length )
                         .arg( "attribute", name )
                         .arg( "takes", std::string{ under ? "one name, a pane" : "one name, a pane or a family" } ) );
        continue;
      }
      std::string const pane = spellingOf( attribute.arguments.front() );
      ExternalName const* const named = paneNamed( pane );
      if ( named == nullptr || ( under && named->paneCount > 1 ) )
      {
        std::optional<Meaning> const meaning = mTyping.lookupName( pane );
        std::string what = meaning.has_value() ? kindOfName( *meaning ) : "a name the program does not know";
        mTyping.add( diagnostic( diag::DiagnosticId::C_PANE_NAME )
                         .at( attribute.arguments.front().location, attribute.arguments.front().length )
                         .arg( "name", pane )
                         .arg( "what", std::move( what ) )
                         .arg( "form", std::string{ under ? "under(NAME)" : "in(NAME)" } )
                         .arg( "takes", std::string{ under ? "a pane" : "a pane or a family" } ) );
      }
    }
  }

  /// The Pane the function being checked runs with shown: the one it is
  /// `in`, or the one it declares it runs `under`; empty in `fixed`.
  [[nodiscard]] std::string const& ownPane() const
  {
    return mFunction->pane.empty() ? mFunction->under : mFunction->pane;
  }

  /// A striped array: an array, of elements that lie in stripes, 256 at most —
  /// see docs/decisions/0089-a-stripe-is-a-section.md.
  void striped( Token const& declarator, DeclaratorShape const& shape, Meaning const& type )
  {
    std::string const name = spellingOf( declarator );
    if ( !shape.isArray || shape.isPointer )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_MISPLACED )
                       .at( declarator.location, declarator.length )
                       .arg( "attribute", std::string{ "striped" } )
                       .arg( "applies", std::string{ "an array" } )
                       .arg( "what", "`" + name + "`" ) );
      return;
    }
    if ( !mTyping.stripable( type.type, type.aggregate ) )
    {
      Typed kind = type.aggregate.has_value() ? blockOf( *type.aggregate ) : typed( type.type );
      if ( type.enumeration.has_value() )
      {
        kind = enumerated( *type.enumeration );
      }
      mTyping.add( diagnostic( diag::DiagnosticId::C_NOT_STRIPABLE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", name )
                       .arg( "what", mTyping.describe( kind ) ) );
      return;
    }
    if ( type.count.value_or( 0 ) > MAX_STRIPED_ELEMENTS )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_STRIPED_TOO_LONG )
                       .at( declarator.location, declarator.length )
                       .arg( "name", name )
                       .arg( "count", static_cast<std::int64_t>( type.count.value_or( 0 ) ) ) );
    }
  }

  /// Whether a declarator's shape is one the subset has: in a declaration of
  /// pointers or of none, and no array of pointers.
  bool shaped( Declaration const& declaration, std::size_t index )
  {
    DeclaratorShape const& shape = declaration.shapes[index];
    Token const& declarator = declaration.declarators[index];
    if ( shape.isPointer != declaration.shapes.front().isPointer )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_MIXED_DECLARATORS )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spellingOf( declarator ) )
                       .arg( "what", std::string{ shape.isPointer ? "a pointer" : "no pointer" } ) );
      return false;
    }
    if ( shape.isPointer && shape.isArray )
    {
      mTyping.refuse( declaration.span, "an array of pointers" );
      return false;
    }
    return true;
  }

  /// Whether an expression reads the name being declared, which is in scope
  /// there, as C has it, and holds nothing yet — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#data--task-3b.
  /// The first read is reported.
  bool readsItself( Token const& declarator, Expression const& value, std::string_view what )
  {
    std::vector<Expression const*> reads;
    namesIn( value, reads );
    std::string const spelling = spellingOf( declarator );
    auto const self =
        std::ranges::find_if( reads, [&]( Expression const* read ) { return spellingOf( read->token ) == spelling; } );
    if ( self == reads.end() )
    {
      return false;
    }
    mTyping.add( diagnostic( diag::DiagnosticId::C_READ_IN_OWN_INITIALISER )
                     .at( ( *self )->token.location, ( *self )->token.length )
                     .arg( "name", spelling )
                     .arg( "what", std::string{ what } ) );
    return true;
  }

  /// The number of elements of an array: its size, a constant of at least one
  /// whose elements fit the address space, or else the length of its list.
  std::optional<std::uint32_t> arraySize( Token const& declarator,
                                          DeclaratorShape const& array,
                                          ir::Type type,
                                          std::optional<std::uint32_t> aggregate = std::nullopt )
  {
    std::int64_t const largest = MAX_ARRAY_BYTES / std::max<std::uint32_t>( 1, mNames->bytes( type, aggregate ) );
    auto const refuse = [&]
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ARRAY_SIZE )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spellingOf( declarator ) )
                       .arg( "largest", largest ) );
      return std::nullopt;
    };
    if ( array.size == nullptr )
    {
      if ( !array.hasList || array.list.empty() )
      {
        return refuse();
      }
      return static_cast<std::uint32_t>( array.list.size() );
    }
    if ( readsItself( declarator, *array.size, "size" ) )
    {
      return std::nullopt;
    }
    Typed const kind = mTyping.expression( *array.size );
    if ( !kind.valid )
    {
      return std::nullopt;
    }
    std::int64_t const count = isKnown( kind ) ? mTyping.fold( *array.size, std::nullopt ) : 0;
    if ( count < 1 || count > largest )
    {
      return refuse();
    }
    return static_cast<std::uint32_t>( count );
  }

  /// What a declarator is given: a `const` is given a value, an array a list
  /// no longer than it, and a value an object keeps from the start is a
  /// constant. The value of a scalar constant, where it is one.
  std::optional<std::int64_t> given( Token const& declarator,
                                     Expression const* value,
                                     DeclaratorShape const& array,
                                     bool isConst,
                                     bool constantRequired,
                                     Meaning const& type )
  {
    bool const aggregate = type.aggregate.has_value() && !array.isArray;
    if ( ( array.isArray && value != nullptr ) || ( !array.isArray && !aggregate && array.hasList ) )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_LIST_MISMATCH )
                       .at( declarator.location, declarator.length )
                       .arg( "name", spellingOf( declarator ) )
                       .arg( "what", std::string{ array.isArray ? "an array" : "no array" } )
                       .arg( "given", std::string{ array.isArray ? "a single value" : "a list" } ) );
      return std::nullopt;
    }
    if ( value == nullptr && !array.hasList )
    {
      if ( isConst )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_CONST_WITHOUT_VALUE )
                         .at( declarator.location, declarator.length )
                         .arg( "name", spellingOf( declarator ) ) );
      }
      return std::nullopt;
    }
    if ( !array.hasList )
    {
      return single( declarator, *value, constantRequired, type );
    }
    if ( aggregate )
    {
      aggregateList( declarator, array.list, *type.aggregate, array.listSpan, constantRequired );
      return std::nullopt;
    }
    // A literal's end is dropped where the array holds its characters exactly.
    std::size_t const given = array.fromLiteral && type.count.has_value() && array.list.size() == *type.count + 1
                                  ? array.list.size() - 1
                                  : array.list.size();
    if ( type.count.has_value() && given > *type.count )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_LIST_TOO_LONG )
                       .at( array.listSpan.begin, array.listSpan.length )
                       .arg( "name", spellingOf( declarator ) )
                       .arg( "count", static_cast<std::int64_t>( *type.count ) )
                       .arg( "given", static_cast<std::int64_t>( array.list.size() ) ) );
    }
    Meaning const element = elementOf( type );
    for ( ExpressionPtr const& listed : array.list )
    {
      listElement( declarator, *listed, element, constantRequired );
    }
    return std::nullopt;
  }

  /// One value a list holds for a place of `slot`'s type: a list of its own
  /// for a `struct` or a `union`, or a value — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
  void listElement( Token const& declarator, Expression const& value, Meaning const& slot, bool constantRequired )
  {
    if ( value.kind != ExpressionKind::LIST )
    {
      single( declarator, value, constantRequired, slot );
      return;
    }
    if ( !slot.aggregate.has_value() )
    {
      listShape( value.span, "a value of `" + spellingOf( declarator ) + "`", "a list" );
      return;
    }
    std::vector<ExpressionPtr> const& elements = value.arguments;
    aggregateList( declarator, elements, *slot.aggregate, value.span, constantRequired );
  }

  /// The list a `struct` or a `union` is given: a value per member in order, a
  /// union's first alone, an array member's in a list of its own.
  void aggregateList( Token const& declarator,
                      std::vector<ExpressionPtr> const& elements,
                      std::uint32_t aggregateIndex,
                      diag::SourceSpan span,
                      bool constantRequired )
  {
    AggregateType const& type = mNames->aggregate( aggregateIndex );
    std::size_t const holds = type.isUnion ? 1 : type.members.size();
    if ( elements.size() > holds )
    {
      listShape( span,
                 "`" + type.name + "`",
                 std::to_string( elements.size() ) + " values for " + counted( holds, "member" ) +
                     ( type.isUnion ? ", its first" : "" ) );
      return;
    }
    for ( std::size_t index = 0; index < elements.size(); ++index )
    {
      MemberType const& member = type.members[index];
      Meaning slot;
      slot.kind = NameKind::OBJECT;
      giveType( slot, member.type );
      Expression const& value = *elements[index];
      if ( !member.isArray )
      {
        listElement( declarator, value, slot, constantRequired );
        continue;
      }
      std::string const memberName = "`" + type.name + "::" + spellingOf( member.name ) + "`";
      if ( value.kind != ExpressionKind::LIST )
      {
        listShape( value.span, memberName + ", an array,", "no list" );
        continue;
      }
      if ( value.arguments.size() > member.count )
      {
        listShape( value.span,
                   memberName + ", of " + counted( member.count, "element" ) + ",",
                   std::to_string( value.arguments.size() ) + " values" );
        continue;
      }
      for ( ExpressionPtr const& element : value.arguments )
      {
        listElement( declarator, *element, slot, constantRequired );
      }
    }
  }

  void listShape( diag::SourceSpan span, std::string const& what, std::string const& given )
  {
    mTyping.add( diagnostic( diag::DiagnosticId::C_LIST_SHAPE )
                     .at( span.begin, span.length )
                     .arg( "what", what )
                     .arg( "given", given ) );
  }

  /// One value given, of the object's type, or of an element of the array's.
  std::optional<std::int64_t>
  single( Token const& declarator, Expression const& value, bool constantRequired, Meaning const& type )
  {
    if ( readsItself( declarator, value, "value" ) )
    {
      mListConstant = false;
      return std::nullopt;
    }
    Typed const kind = mTyping.expression( value );
    mListConstant = mListConstant && kind.valid && !type.aggregate.has_value() && isLaidDown( kind );
    if ( !kind.valid )
    {
      return std::nullopt;
    }
    mTyping.assignable( value, kind, type.type, type.enumeration, type.pointee, type.aggregate );
    if ( isKnown( kind ) )
    {
      return wrapped( mTyping.fold( value, type.type ), type.type );
    }
    // An address the assembler knows, and `nullptr`, are what a pointer at
    // file scope may be given.
    if ( constantRequired && !kind.address && !kind.isNull )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_NOT_CONSTANT )
                       .at( value.span.begin, value.span.length )
                       .arg( "what", "`" + spellingOf( declarator ) + "`" ) );
    }
    return std::nullopt;
  }

  /// A function's parameters are locals of the scope its body's statements
  /// stand in, so that the body declares none of their names again.
  void functionDefinition( FunctionDefinition const& function )
  {
    attributesKnown( function.attributes, AttributeSite::FUNCTION );
    // The class its body is checked under, which a block may narrow and a
    // declaration may narrow again — see
    // docs/decisions/0210-placement-is-declared-in-c-too.md.
    std::optional<ir::Placement> const placed = placementAttribute( function.attributes, *mSources );
    mPlacement.assign( 1, placed.value_or( ir::Placement::ZEROPAGE ) );
    mFunctionPlaced = placed.has_value();
    mLocalPlacements.clear();
    Definition const* const self = definitionOf( *mSources, *mUnit, function );
    if ( self == nullptr )
    {
      return;
    }
    mFunction = self;
    mTyping.within( self );
    implementsSlot( function.name, *self );
    if ( !self->pane.empty() && !self->under.empty() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_UNDER_WITH_IN )
                       .at( function.name.location, function.name.length )
                       .arg( "name", spellingOf( function.name ) ) );
    }
    std::vector<CallSite> calls;
    mTyping.recordCalls( &calls );
    mCalls = &calls;
    mChecked = 0;
    mShown.clear();
    mBlockSwitches.clear();
    mDeclaredIn.clear();
    mCarrying.clear();

    mTyping.openScope();
    std::string const owner = spellingOf( function.name );
    for ( std::size_t index = 0; index < function.parameters.size(); ++index )
    {
      Parameter const& written = function.parameters[index];
      NamedType const& type = self->parameters[index].type;
      Meaning parameter;
      parameter.kind = NameKind::OBJECT;
      giveType( parameter, type );
      parameter.byte = spellingOf( written.name );
      parameter.owner = owner;
      parameter.isParameter = true;
      // A parameter is a byte of its Proc, which only the Proc reaches.
      if ( written.pointeeVolatile && !written.isPointer )
      {
        mTyping.refuse( written.name.span(), "a `volatile` parameter" );
      }
      if ( std::optional<diag::SourceSpan> const size = written.size; size.has_value() )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_ARRAY_PARAMETER_SIZE )
                         .at( size->begin, size->length )
                         .arg( "name", spellingOf( written.name ) ) );
      }
      if ( !mTyping.declareLocal( written.name, parameter ) )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_REDEFINITION )
                         .at( written.name.location, written.name.length )
                         .arg( "name", spellingOf( written.name ) ) );
      }
    }
    for ( Statement const& item : function.body.items )
    {
      statement( item );
    }
    mTyping.closeScope();

    mTyping.recordCalls( nullptr );
    mCalls = nullptr;
    localPlacementsJudged();
    for ( CallSite const& call : calls )
    {
      if ( call.callee != nullptr )
      {
        mEdges->push_back(
            CallEdge{ .caller = self, .callee = call.callee, .unit = mIndex, .span = call.span, .sink = mUnit->sink } );
      }
    }
  }

  /// The calls the statement just checked made, against the Panes: one into a
  /// family is made in a block that shows the member; one into another Pane
  /// of a Window a block shows is refused, since the exit of its `.with`
  /// would put back what the function's own code shows and not the block's;
  /// and a callee in the caller's own Pane, or in what a block shows, needs no
  /// switch — see docs/decisions/0096-panes-in-c.md.
  void checkCalls()
  {
    if ( mCalls == nullptr )
    {
      return;
    }
    for ( ; mChecked < mCalls->size(); ++mChecked )
    {
      CallSite const& call = ( *mCalls )[mChecked];
      std::string const name = call.callee != nullptr ? spellingOf( call.callee->name ) : call.external->name;
      // A Proc that runs under a Pane is called where the Pane is shown —
      // see docs/decisions/0098-a-proc-declares-what-is-shown.md.
      std::string const under = underOfCallee( call.callee, call.external );
      ExternalName const* const runsWith = paneNamed( under );
      if ( runsWith != nullptr && under != ownPane() &&
           !std::ranges::any_of( mShown, [&]( Shown const& block ) { return shows( block, *runsWith ); } ) )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_UNDER_CALLED_ELSEWHERE )
                         .at( call.span.begin, call.span.length )
                         .arg( "name", name )
                         .arg( "pane", under ) );
        continue;
      }
      std::string const pane = paneOfCallee( call.callee, call.external );
      ExternalName const* const shownPane = paneNamed( pane );
      if ( shownPane == nullptr || pane == ownPane() )
      {
        continue;
      }
      if ( std::ranges::any_of( mShown, [&]( Shown const& block ) { return shows( block, *shownPane ); } ) )
      {
        continue;
      }
      if ( shownPane->paneCount > 1 )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_CALL_INTO_FAMILY )
                         .at( call.span.begin, call.span.length )
                         .arg( "name", name )
                         .arg( "family", pane ) );
        continue;
      }
      // Code in a Pane cannot switch its own Window: the call goes through a
      // Trampoline, which the programmer writes.
      ExternalName const* const own = paneNamed( mFunction->pane );
      if ( own != nullptr && own->window == shownPane->window )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_CALL_ACROSS_OWN_WINDOW )
                         .at( call.span.begin, call.span.length )
                         .arg( "name", name )
                         .arg( "pane", pane )
                         .arg( "window", shownPane->window )
                         .arg( "own", mFunction->pane ) );
        continue;
      }
      if ( std::ranges::any_of( mShown, [&]( Shown const& block ) { return block.window == shownPane->window; } ) )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_CALL_ACROSS_BLOCK_WINDOW )
                         .at( call.span.begin, call.span.length )
                         .arg( "name", name )
                         .arg( "pane", pane )
                         .arg( "window", shownPane->window ) );
      }
    }
  }

  /// The Pane or family a name is, or null.
  [[nodiscard]] ExternalName const* paneNamed( std::string_view name ) const
  {
    if ( name.empty() )
    {
      return nullptr;
    }
    std::optional<Meaning> const meaning = mTyping.lookupName( name );
    return meaning.has_value() && meaning->external != nullptr && meaning->external->kind == ExternalKind::PANE
               ? meaning->external
               : nullptr;
  }

  /// A block written `[[with(...)]]`: what it shows, on a Window no block
  /// around it shows; inside it `return`, and `break` or `continue` of a loop
  /// around it, are refused — see docs/decisions/0096-panes-in-c.md.
  void withBlock( Statement const& node )
  {
    attributesKnown( node.attributes, AttributeSite::BLOCK );
    std::optional<Attribute> const attribute = withOf( node.attributes, *mSources );
    std::optional<Shown> shown;
    if ( attribute.has_value() )
    {
      shown = withShown( *attribute );
    }
    else if ( Attribute const* const trampoline = bareAttribute( node.attributes, *mSources, "trampoline" ) )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_ARGUMENTS )
                       .at( trampoline->span.begin, trampoline->span.length )
                       .arg( "attribute", std::string{ "trampoline" } )
                       .arg( "takes", std::string{ "a `with` beside it, whose block it makes a trampoline" } ) );
    }
    if ( attribute.has_value() && shown.has_value() &&
         std::ranges::any_of( mShown, [&]( Shown const& outer ) { return outer.window == shown->window; } ) )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_WITH_NESTED_WINDOW )
                       .at( attribute->span.begin, attribute->span.length )
                       .arg( "window", shown->window ) );
      shown.reset();
    }
    if ( attribute.has_value() && shown.has_value() )
    {
      trampolineForm( node, *attribute, *shown );
    }
    // A block that could not be resolved is checked as a plain one, so that
    // what it holds is still read.
    if ( !shown.has_value() )
    {
      mTyping.openScope();
      for ( Statement const& item : node.items )
      {
        statement( item );
      }
      mTyping.closeScope();
      return;
    }
    mShown.push_back( *shown );
    mBlockSwitches.push_back( mSwitches );
    std::uint32_t const outerLoops = std::exchange( mLoops, 0 );
    mTyping.openScope();
    for ( Statement const& item : node.items )
    {
      statement( item );
    }
    mTyping.closeScope();
    mLoops = outerLoops;
    mBlockSwitches.pop_back();
    mShown.pop_back();
  }

  /// The form of a block held to its place — see
  /// docs/decisions/0097-a-trampoline-is-declared.md. Code in a Pane cannot
  /// switch its own Window, so a block there on another state of the Window
  /// is a Trampoline and says so with `trampoline`; a block on the Pane the
  /// code is in, or on the family, shows what is shown. Anywhere else the
  /// block is a macro under `.with`, and `trampoline` is refused.
  void trampolineForm( Statement const& node, Attribute const& attribute, Shown const& shown )
  {
    bool const written = isTrampoline( node.attributes, *mSources );
    ExternalName const* const own = paneNamed( mFunction->pane );
    if ( own != nullptr && own->window == shown.window && ( shown.pane == mFunction->pane || own->paneCount > 1 ) )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_WITH_OWN_PANE )
                       .at( attribute.span.begin, attribute.span.length )
                       .arg( "pane", mFunction->pane ) );
      return;
    }
    bool const needed = own != nullptr && own->window == shown.window;
    if ( needed && !written )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_TRAMPOLINE_NEEDED )
                       .at( attribute.span.begin, attribute.span.length )
                       .arg( "window", shown.window )
                       .arg( "pane", mFunction->pane ) );
    }
    else if ( !needed && written )
    {
      std::string where = "the code is in fixed";
      if ( !mFunction->under.empty() )
      {
        where = "the code runs under `" + mFunction->under + "`, which it shows again itself";
      }
      else if ( own != nullptr )
      {
        where = "the block is on another window than the code's pane";
      }
      mTyping.add( diagnostic( diag::DiagnosticId::C_TRAMPOLINE_NOT_NEEDED )
                       .at( attribute.span.begin, attribute.span.length )
                       .arg( "where", std::move( where ) ) );
    }
  }

  /// What `[[with(...)]]` names, held to its form: a Pane for `with(PANE)`, a
  /// family or a Window with an index for `with(NAME, i)`, a Window for
  /// `with(WINDOW = STATE)`; the index a `u8` or a constant.
  std::optional<Shown> withShown( Attribute const& attribute )
  {
    std::optional<WithSpec> const spec = withAttribute( attribute );
    if ( !spec.has_value() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_ARGUMENTS )
                       .at( attribute.span.begin, attribute.span.length )
                       .arg( "attribute", std::string{ "with" } )
                       .arg( "takes",
                             std::string{ "`with(PANE)`, `with(FAMILY, i)`, `with(WINDOW, i)` or "
                                          "`with(WINDOW = STATE)`, with `i` a name or a constant" } ) );
      return std::nullopt;
    }
    std::string const name = spellingOf( spec->name );
    std::optional<Meaning> const meaning = mTyping.lookupName( name );
    ExternalName const* const external =
        meaning.has_value() && meaning->definition == nullptr ? meaning->external : nullptr;
    bool const isPane = external != nullptr && external->kind == ExternalKind::PANE && external->paneCount == 1;
    bool const isFamily = external != nullptr && external->kind == ExternalKind::PANE && external->paneCount > 1;
    bool const isWindow = external != nullptr && external->kind == ExternalKind::WINDOW;
    auto const refuse = [&]( std::string form, std::string takes )
    {
      std::string what = meaning.has_value() ? kindOfName( *meaning ) : "a name the program does not know";
      mTyping.add( diagnostic( diag::DiagnosticId::C_PANE_NAME )
                       .at( spec->name.location, spec->name.length )
                       .arg( "name", name )
                       .arg( "what", std::move( what ) )
                       .arg( "form", std::move( form ) )
                       .arg( "takes", std::move( takes ) ) );
    };
    switch ( spec->form )
    {
    case ir::WithForm::PANE:
      if ( !isPane )
      {
        refuse( "with(NAME)", "a pane" );
        return std::nullopt;
      }
      return Shown{ .window = external->window, .pane = name };
    case ir::WithForm::AT:
    {
      if ( !isFamily && !isWindow )
      {
        refuse( "with(NAME, i)", "a family or a window" );
        return std::nullopt;
      }
      if ( !spec->index.has_value() || !withIndex( name, *spec->index ) )
      {
        return std::nullopt;
      }
      return Shown{ .window = isWindow ? name : external->window, .pane = isFamily ? name : std::string{} };
    }
    case ir::WithForm::STATE:
      if ( !isWindow || !spec->state.has_value() )
      {
        refuse( "with(NAME = STATE)", "a window" );
        return std::nullopt;
      }
      return Shown{ .window = name, .state = spellingOf( *spec->state ) };
    }
    return std::nullopt;
  }

  /// The index of `with(NAME, i)`: a `u8` object, or a constant `X` holds.
  bool withIndex( std::string const& name, Token const& index )
  {
    auto const refuse = [&]( std::string what )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_WITH_INDEX )
                       .at( index.location, index.length )
                       .arg( "name", name )
                       .arg( "what", std::move( what ) ) );
      return false;
    };
    if ( index.kind == TokenKind::INTEGER_CONSTANT )
    {
      std::int64_t const value = valueOfIntegerConstant( spellingOf( index ) ).value_or( 0 );
      return value >= 0 && value <= 255 ? true : refuse( "a constant no `u8` holds" );
    }
    std::optional<Meaning> const meaning = mTyping.lookupSpelling( spellingOf( index ) );
    if ( !meaning.has_value() )
    {
      return refuse( "a name nothing defines" );
    }
    if ( meaning->kind != NameKind::OBJECT || meaning->isArray || meaning->type != ir::Type::U8 ||
         meaning->enumeration.has_value() || isAssemblerConstant( *meaning ) )
    {
      return refuse( meaning->kind == NameKind::OBJECT && !meaning->isArray && !isAssemblerConstant( *meaning )
                         ? articled( ir::spellingOf( meaning->type ) )
                         : kindOfName( *meaning ) );
    }
    return true;
  }

  /// The block, innermost first, whose data an expression carries the address
  /// of: `&map[i]` of what a block shows, an array of it read as a pointer, a
  /// local of the block holding such an address, and what is computed from
  /// one. The depth of the block, counted from one; nothing for an address
  /// of nothing shown.
  std::optional<std::size_t> carries( Expression const& node )
  {
    switch ( node.kind )
    {
    case ExpressionKind::IDENTIFIER:
    {
      std::optional<Meaning> const meaning = mTyping.lookup( node );
      if ( !meaning.has_value() )
      {
        return std::nullopt;
      }
      if ( meaning->declaredAt.has_value() )
      {
        auto const found = mCarrying.find( meaning->declaredAt->rawOffset() );
        return found != mCarrying.end() ? std::optional{ found->second } : std::nullopt;
      }
      return meaning->isArray ? shownDepth( *meaning ) : std::nullopt;
    }
    case ExpressionKind::ADDRESS:
      return addressed( *node.left );
    case ExpressionKind::BINARY:
      if ( node.token.kind == TokenKind::PLUS || node.token.kind == TokenKind::MINUS )
      {
        std::optional<std::size_t> const left = carries( *node.left );
        std::optional<std::size_t> const right = carries( *node.right );
        return std::max( left, right );
      }
      return std::nullopt;
    case ExpressionKind::CAST:
      return carries( *node.left );
    case ExpressionKind::MEMBER:
    case ExpressionKind::INDEX:
    {
      // An array member, or an element that is one, read as a pointer.
      std::optional<Shape> const shape = mTyping.shape( node );
      return shape.has_value() && shape->isArray ? addressed( node ) : std::nullopt;
    }
    default:
      return std::nullopt;
    }
  }

  /// Whose address `&PLACE` is: the object at the root of a chain of `.` and
  /// `[]`, where a block shows it, or what a pointer along the chain carries.
  std::optional<std::size_t> addressed( Expression const& place )
  {
    Expression const* node = &place;
    while ( true )
    {
      if ( node->kind == ExpressionKind::IDENTIFIER )
      {
        std::optional<Meaning> const meaning = mTyping.lookup( *node );
        if ( !meaning.has_value() )
        {
          return std::nullopt;
        }
        if ( meaning->pointee.has_value() && !meaning->isArray )
        {
          return carries( *node );
        }
        return shownDepth( *meaning );
      }
      if ( node->kind == ExpressionKind::DEREFERENCE ||
           ( node->kind == ExpressionKind::MEMBER && node->token.kind == TokenKind::ARROW ) )
      {
        return carries( *node->left );
      }
      if ( node->kind == ExpressionKind::MEMBER || node->kind == ExpressionKind::INDEX )
      {
        // Read without typing again, which would report again: `p[i]` through
        // a pointer is what the pointer carries.
        std::optional<Shape> const left = mTyping.shape( *node->left );
        if ( node->kind == ExpressionKind::INDEX && left.has_value() && left->pointee.has_value() && !left->isArray )
        {
          return carries( *node->left );
        }
        node = node->left.get();
        continue;
      }
      return std::nullopt;
    }
  }

  /// The depth of the innermost block showing the Pane an object is in.
  [[nodiscard]] std::optional<std::size_t> shownDepth( Meaning const& meaning ) const
  {
    std::string const pane =
        paneOfCallee( meaning.kind == NameKind::OBJECT ? meaning.definition : nullptr, meaning.external );
    ExternalName const* const shownPane = paneNamed( pane );
    if ( shownPane == nullptr )
    {
      return std::nullopt;
    }
    for ( std::size_t depth = mShown.size(); depth > 0; --depth )
    {
      if ( shows( mShown[depth - 1], *shownPane ) )
      {
        return depth;
      }
    }
    return std::nullopt;
  }

  /// The name at the root of what an address is taken of, for a finding.
  [[nodiscard]] std::string rootName( Expression const& node ) const
  {
    Expression const* root = &node;
    while ( root->left != nullptr && root->kind != ExpressionKind::CALL )
    {
      root = root->left.get();
    }
    return std::string{ mSources->textOf( root->span ) };
  }

  /// An address of what a block shows kept: in a local declared in that block
  /// or one inside it, which then carries it; anywhere else it escapes the
  /// block, and is refused — see docs/decisions/0096-panes-in-c.md.
  void addressKept( Expression const& value, Expression const* target, std::optional<Token> declared )
  {
    std::optional<std::size_t> const depth = carries( value );
    if ( !depth.has_value() )
    {
      return;
    }
    std::optional<std::uint32_t> local;
    std::string escapes = "is stored outside a local";
    if ( declared.has_value() )
    {
      local = declared->location.rawOffset();
    }
    else if ( target != nullptr && target->kind == ExpressionKind::IDENTIFIER )
    {
      std::optional<Meaning> const meaning = mTyping.lookup( *target );
      if ( meaning.has_value() && meaning->declaredAt.has_value() )
      {
        local = meaning->declaredAt->rawOffset();
      }
      escapes = "is stored in `" + std::string{ mSources->textOf( target->span ) } + "`, declared outside the block";
    }
    if ( local.has_value() )
    {
      auto const found = mDeclaredIn.find( *local );
      if ( found != mDeclaredIn.end() && found->second >= *depth )
      {
        mCarrying[*local] = *depth;
        return;
      }
    }
    mTyping.add( diagnostic( diag::DiagnosticId::C_ADDRESS_LEAVES_BLOCK )
                     .at( value.span.begin, value.span.length )
                     .arg( "name", rootName( value ) )
                     .arg( "escapes", escapes ) );
  }

  /// `return;` in a function that returns nothing, and `return x;`, with a
  /// value assignable to the result, in one that returns something.
  void returnStatement( Statement const& statement )
  {
    std::string const name = spellingOf( mFunction->name );
    attributesKnown( statement.attributes, AttributeSite::RETURN );
    bool const transition = hasAttribute( statement.attributes, *mSources, "transition" );
    if ( transition && mShown.empty() )
    {
      // A transition never comes back, so there is no value to hand anyone
      // and no caller to hand it to — see docs/decisions/0064-phase-in-c.md.
      // In a block the `return` itself is refused below, which says more.
      std::string what;
      if ( statement.expression != nullptr )
      {
        what = "this one returns a value";
      }
      else if ( mFunction->result.has_value() )
      {
        what = "`" + name + "` returns " + mTyping.describe( typedAs( *mFunction->result ) );
      }
      if ( !what.empty() )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_TRANSITION_RETURNS )
                         .at( statement.span.begin, statement.span.length )
                         .arg( "what", std::move( what ) ) );
        if ( statement.expression != nullptr )
        {
          mTyping.expression( *statement.expression );
        }
        return;
      }
      if ( nameAttribute( statement.attributes, *mSources, "transition" ).empty() )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_ATTRIBUTE_ARGUMENTS )
                         .at( statement.span.begin, statement.span.length )
                         .arg( "attribute", std::string{ "transition" } )
                         .arg( "takes", std::string{ "one name, a phase of the project" } ) );
      }
      return;
    }
    if ( !mShown.empty() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_LEAVES_WITH_BLOCK )
                       .at( statement.span.begin, statement.span.length )
                       .arg( "keyword", std::string{ "return" } ) );
      if ( statement.expression != nullptr )
      {
        mTyping.expression( *statement.expression );
      }
      return;
    }
    if ( statement.expression == nullptr )
    {
      if ( mFunction->result.has_value() )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_RETURN_WITHOUT_VALUE )
                         .at( statement.span.begin, statement.span.length )
                         .arg( "name", name )
                         .arg( "type", mTyping.describe( typedAs( *mFunction->result ) ) ) );
      }
      return;
    }
    Typed const value = mTyping.expression( *statement.expression );
    if ( !mFunction->result.has_value() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_RETURN_WITH_VALUE )
                       .at( statement.span.begin, statement.span.length )
                       .arg( "name", name ) );
      return;
    }
    if ( value.valid )
    {
      mTyping.assignable( *statement.expression,
                          value,
                          mFunction->result->type,
                          mFunction->result->enumeration,
                          mFunction->result->pointee,
                          mFunction->result->aggregate );
    }
  }

  void statement( Statement const& statement )
  {
    switch ( statement.kind )
    {
    case StatementKind::IF:
      // What the statement was written with before its condition is in scope
      // in the whole of it, its `else` included, and nowhere else — see
      // docs/decisions/0160-a-declaration-where-a-statement-begins-one.md.
      mTyping.openScope();
      initialiser( statement );
      mTyping.condition( *statement.expression );
      checkCalls();
      for ( Statement const& item : statement.items )
      {
        this->statement( item );
      }
      mTyping.closeScope();
      return;
    case StatementKind::COMPOUND:
    {
      Placed const here{ mPlacement, placementAttribute( statement.attributes, *mSources ) };
      if ( !statement.attributes.empty() && !placementOnly( statement.attributes, *mSources ) )
      {
        withBlock( statement );
        return;
      }
      if ( !statement.attributes.empty() )
      {
        attributesKnown( statement.attributes, AttributeSite::BLOCK );
      }
      mTyping.openScope();
      for ( Statement const& item : statement.items )
      {
        this->statement( item );
      }
      mTyping.closeScope();
      return;
    }
    case StatementKind::CASE:
      // A block is a scope, and so is a case, whose Proc is its own.
      mTyping.openScope();
      for ( Statement const& item : statement.items )
      {
        this->statement( item );
      }
      mTyping.closeScope();
      return;
    case StatementKind::DECLARATION:
      declaration( statement );
      checkCalls();
      return;
    case StatementKind::NULL_STATEMENT:
      return;
    case StatementKind::RETURN:
      returnStatement( statement );
      checkCalls();
      return;
    case StatementKind::EXPRESSION:
      expressionStatement( *statement.expression );
      checkCalls();
      return;
    case StatementKind::WHILE:
    case StatementKind::DO:
    case StatementKind::FOR:
      mTyping.openScope();
      if ( statement.initial != nullptr )
      {
        expressionStatement( *statement.initial );
      }
      if ( statement.declaration != nullptr )
      {
        declaration( statement );
      }
      if ( statement.expression != nullptr )
      {
        mTyping.condition( *statement.expression );
      }
      if ( statement.step != nullptr )
      {
        expressionStatement( *statement.step );
      }
      checkCalls();
      ++mLoops;
      for ( Statement const& item : statement.items )
      {
        this->statement( item );
      }
      --mLoops;
      mTyping.closeScope();
      return;
    case StatementKind::SWITCH:
      mTyping.openScope();
      initialiser( statement );
      switchStatement( statement );
      mTyping.closeScope();
      return;
    case StatementKind::BREAK:
    case StatementKind::CONTINUE:
      jump( statement );
      return;
    }
  }

  /// Locals of a block: each declared once in it, hiding what an outer block
  /// or the program calls the same, and given a value of its type — a
  /// constant, where it is `static`.
  /// The type a declaration written `auto` takes from the value it is given,
  /// where it is written as one name given one value — see
  /// docs/decisions/0161-auto-takes-the-type-of-the-value.md.
  std::optional<Meaning> deducedType( Declaration const& declared )
  {
    DeclaratorShape const& shape = declared.shapes.front();
    Expression const* const value = declared.initialisers.front().get();
    if ( declared.declarators.size() != 1 )
    {
      mTyping.refuse( declared.span, "`auto` of more than one name" );
      return std::nullopt;
    }
    if ( shape.isPointer || shape.isArray || shape.hasList )
    {
      mTyping.refuse( declared.span, "`auto` of a pointer or an array" );
      return std::nullopt;
    }
    if ( value == nullptr )
    {
      mTyping.refuse( declared.span, "`auto` given no value" );
      return std::nullopt;
    }
    return mTyping.deducedType( declared.declarators.front(), *value );
  }

  void declaration( Statement const& statement )
  {
    Declaration const& declared = *statement.declaration;
    attributesKnown( declared.attributes, AttributeSite::LOCAL );
    bool const stripes = isStriped( declared.attributes, *mSources );
    std::optional<ir::Placement> const asked = placementAttribute( declared.attributes, *mSources );
    std::optional<Meaning> const type =
        declared.type.keyword == Keyword::AUTO ? deducedType( declared ) : mTyping.declaredType( declared.type );
    placementAsked( declared,
                    asked,
                    stripes,
                    false,
                    type.has_value() && type->aggregate.has_value()
                        ? std::optional{ mNames->bytes( ir::Type::BLOCK, type->aggregate ) }
                        : std::nullopt );
    for ( std::size_t index = 0; index < declared.declarators.size(); ++index )
    {
      Token const& declarator = declared.declarators[index];
      Expression const* const value = declared.initialisers[index].get();
      if ( !type.has_value() )
      {
        if ( value != nullptr )
        {
          mTyping.expression( *value );
        }
        continue;
      }
      DeclaratorShape const& array = declared.shapes[index];
      if ( !shaped( declared, index ) )
      {
        continue;
      }
      if ( type->function.has_value() && !array.isPointer )
      {
        // A function type has no bytes, so no local is of it: it is a type as
        // a pointer alone — see
        // docs/decisions/0103-what-building-handlers-needs.md.
        mTyping.add( diagnostic( diag::DiagnosticId::C_FUNCTION_TYPE_VALUE )
                         .at( declarator.location, declarator.length )
                         .arg( "name", spellingOf( declared.type ) ) );
        continue;
      }
      Meaning local = localMeaning( *type, declared, array, declarator );
      if ( array.isArray )
      {
        local.isArray = true;
        local.count = arraySize( declarator, array, local.type, local.aggregate );
      }
      if ( stripes )
      {
        striped( declarator, array, local );
        local.isStriped = array.isArray && !array.isPointer;
      }
      bool const added = mTyping.declareLocal( declarator, local );
      if ( !added )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_REDEFINITION )
                         .at( declarator.location, declarator.length )
                         .arg( "name", spellingOf( declarator ) ) );
      }
      mDeclaredIn[declarator.location.rawOffset()] = mShown.size();
      mListConstant = true;
      std::optional<std::int64_t> const constant =
          given( declarator, value, array, local.isConst, declared.isStatic, local );
      // What the lowering keeps as data rather than writes each time the
      // declaration runs — see `Lowering::localArray` and `localBlock`.
      if ( declared.isConst && array.hasList && mListConstant && !stripes && !array.isPointer &&
           ( array.isArray || local.aggregate.has_value() ) )
      {
        placedAsData( declarator );
      }
      if ( added && local.isConst && !local.isVolatile && constant.has_value() )
      {
        local.value = constant;
        mTyping.settleLocal( declarator, local );
        if ( !array.isArray && !local.aggregate.has_value() )
        {
          placedNowhere( declarator );
        }
      }
      if ( value != nullptr && !array.isArray )
      {
        addressKept( *value, nullptr, declarator );
      }
    }
  }

  /// `break` leaves the loop or the `switch` it stands in; `continue` the
  /// loop, from inside a case as from anywhere else — a case is a block of
  /// the function, so there is nothing between it and the loop's step — see
  /// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
  void jump( Statement const& statement )
  {
    bool const isBreak = statement.kind == StatementKind::BREAK;
    if ( mLoops == 0 && !mShown.empty() && ( !isBreak || mSwitches <= mBlockSwitches.back() ) )
    {
      // No loop inside the block, and no `switch` for a `break`: whatever it
      // leaves, it leaves the block.
      mTyping.add( diagnostic( diag::DiagnosticId::C_LEAVES_WITH_BLOCK )
                       .at( statement.span.begin, statement.span.length )
                       .arg( "keyword", std::string{ isBreak ? "break" : "continue" } ) );
      return;
    }
    if ( mLoops > 0 || ( isBreak && mSwitches > 0 ) )
    {
      return;
    }
    mTyping.add( diagnostic( diag::DiagnosticId::C_OUTSIDE_LOOP )
                     .at( statement.span.begin, statement.span.length )
                     .arg( "keyword", std::string{ isBreak ? "break" : "continue" } )
                     .arg( "around", std::string{ isBreak ? "loop or `switch`" : "loop" } ) );
  }

  /// The declaration or the expression a statement was written with before
  /// its condition, checked in the scope its names live in.
  void initialiser( Statement const& statement )
  {
    if ( statement.initial != nullptr )
    {
      expressionStatement( *statement.initial );
    }
    if ( statement.declaration != nullptr )
    {
      declaration( statement );
    }
  }

  void switchStatement( Statement const& node )
  {
    Typed const switched = mTyping.expression( *node.expression );
    // A `switch` is the function's own code whatever its shape, so a block
    // that shows a Pane holds one as it holds any other statement: there are
    // no Procs of its own left to have nowhere to stand — see
    // docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
    Typed const& value = switched;
    checkCalls();
    std::optional<std::uint32_t> const enumeration = value.valid ? value.enumeration : std::nullopt;
    // An `enum`, or an ordinary integer, whose labels are then constants —
    // see docs/decisions/0164-a-small-switch-stands-where-it-is.md.
    ir::Type const tested = value.type.value_or( ir::Type::BOOL );
    bool const isInteger =
        !enumeration.has_value() && !value.pointee.has_value() && !value.aggregate.has_value() &&
        ( tested == ir::Type::U8 || tested == ir::Type::I8 || tested == ir::Type::U16 || tested == ir::Type::I16 );
    if ( value.valid && !enumeration.has_value() && !isInteger )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_SWITCH_NOT_ENUMERATION )
                       .at( node.expression->span.begin, node.expression->span.length )
                       .arg( "what", mTyping.describe( value ) ) );
    }
    // The type the labels are held to, which a value refused, or one no
    // `switch` takes, does not give them.
    std::optional<ir::Type> const held = value.valid && isInteger ? value.type : std::nullopt;

    std::vector<bool> named;
    if ( enumeration.has_value() )
    {
      named.assign( mNames->enumeration( *enumeration ).enumerators.size(), false );
    }
    bool hasDefault = false;
    std::set<std::int64_t> values;
    ++mSwitches;
    for ( Statement const& clause : node.items )
    {
      label( clause, enumeration, named, hasDefault, held, values );
      for ( Statement const& item : clause.items )
      {
        statement( item );
      }
    }
    --mSwitches;

    if ( !enumeration.has_value() || hasDefault )
    {
      return;
    }
    std::string missing;
    std::vector<std::string> const& enumerators = mNames->enumeration( *enumeration ).enumerators;
    for ( std::size_t index = 0; index < named.size(); ++index )
    {
      if ( !named[index] )
      {
        missing.append( missing.empty() ? "" : ", " ).append( "`" + enumerators[index] + "`" );
      }
    }
    if ( !missing.empty() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_ENUMERATORS_UNHANDLED )
                       .at( node.span.begin, node.span.length )
                       .arg( "names", missing )
                       .arg( "type", mNames->enumeration( *enumeration ).name ) );
    }
  }

  /// A label of a `switch`: an enumerator of its type once, a constant of
  /// the type it tests once, or `default` once.
  void label( Statement const& clause,
              std::optional<std::uint32_t> enumeration,
              std::vector<bool>& named,
              bool& hasDefault,
              std::optional<ir::Type> tested,
              std::set<std::int64_t>& values )
  {
    auto const repeated = [this, &clause]
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_LABEL_REPEATED )
                       .at( clause.span.begin, clause.span.length )
                       .arg( "label",
                             clause.expression == nullptr
                                 ? std::string{ "default" }
                                 : std::string{ mSources->textOf( clause.expression->span ) } ) );
    };
    if ( clause.expression == nullptr )
    {
      if ( hasDefault )
      {
        repeated();
      }
      hasDefault = true;
      return;
    }
    Typed const kind = mTyping.expression( *clause.expression );
    if ( !kind.valid )
    {
      return;
    }
    if ( !enumeration.has_value() )
    {
      // A label of a `switch` over an integer is a constant of its type — a
      // value the tested type cannot hold matches nothing it could ever hold
      // (0206) — and each value stands once.
      // An enumerator or an address is a constant, of another type than the
      // one tested: that is what is wrong with it, not that it varies.
      if ( isKnown( kind ) && tested.has_value() && ( kind.pointee.has_value() || kind.enumeration.has_value() ) )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_TYPES_DIFFER )
                         .at( clause.expression->span.begin, clause.expression->span.length )
                         .arg( "left", articled( ir::spellingOf( *tested ) ) )
                         .arg( "right", mTyping.describe( kind ) ) );
        return;
      }
      if ( !isKnown( kind ) || kind.pointee.has_value() || kind.enumeration.has_value() )
      {
        mTyping.add( diagnostic( diag::DiagnosticId::C_NOT_CONSTANT )
                         .at( clause.expression->span.begin, clause.expression->span.length )
                         .arg( "what", mTyping.describe( kind ) ) );
        return;
      }
      if ( !tested.has_value() || !mTyping.constantFits( *clause.expression, *tested ) )
      {
        return;
      }
      if ( !values.insert( mTyping.fold( *clause.expression, tested ) ).second )
      {
        repeated();
      }
      return;
    }
    std::optional<std::uint32_t> const index = mNames->enumeratorIndex( *mUnit, *clause.expression );
    if ( kind.enumeration != enumeration || !index.has_value() )
    {
      mTyping.add( diagnostic( diag::DiagnosticId::C_CASE_NOT_ENUMERATOR )
                       .at( clause.expression->span.begin, clause.expression->span.length )
                       .arg( "label", std::string{ mSources->textOf( clause.expression->span ) } )
                       .arg( "type", mNames->enumeration( *enumeration ).name ) );
      return;
    }
    if ( named[*index] )
    {
      repeated();
    }
    named[*index] = true;
  }

  void expressionStatement( Expression const& node )
  {
    switch ( node.kind )
    {
    case ExpressionKind::CALL:
      mTyping.call( node, false );
      return;
    case ExpressionKind::ASSIGNMENT:
      assignment( node );
      return;
    default:
      mTyping.refuse( node.span, "an expression statement that neither assigns nor calls" );
      return;
    }
  }

  /// An assignment statement: what it assigns is held to the same rules
  /// wherever one is written, and only the address a pointer keeps is a
  /// question of where the statement stands.
  void assignment( Expression const& node )
  {
    std::optional<Meaning> const object = mTyping.assignment( node );
    if ( object.has_value() && object->type == ir::Type::POINTER )
    {
      addressKept( *node.right, node.left.get(), std::nullopt );
    }
  }

  [[nodiscard]] std::string spellingOf( Token const& token ) const
  {
    return detail::spellingOf( *mSources, token );
  }

  diag::SourceManager const* mSources;
  Names const* mNames;
  Unit const* mUnit;
  std::size_t mIndex;
  std::vector<CallEdge>* mEdges;
  /// Where each local whose address is taken is declared, which the type
  /// check fills as it reads the program.
  std::set<std::uint32_t> const* mAddressed;
  Typing mTyping;

  /// The function being checked.
  Definition const* mFunction = nullptr;

  /// Its calls as the type check records them, and how many of them the
  /// statements checked so far have been held to the Panes.
  std::vector<CallSite>* mCalls = nullptr;
  std::size_t mChecked = 0;

  /// The `[[with]]` blocks the statement being checked stands in, outermost
  /// first, and the `switch`es each stood in when entered — see
  /// docs/decisions/0096-panes-in-c.md.
  std::vector<Shown> mShown;
  std::vector<std::uint32_t> mBlockSwitches;

  /// How many blocks each local was declared in, by where it is declared, and
  /// the depth of the block whose data a local holds the address of.
  std::map<std::uint32_t, std::size_t> mDeclaredIn;
  std::map<std::uint32_t, std::size_t> mCarrying;

  /// The loops the statement being checked stands in within its case or
  /// function, and in all.
  std::uint32_t mLoops = 0;

  /// The `switch`es it stands in.
  std::uint32_t mSwitches = 0;
};

} // namespace

void checkUnit( diag::SourceManager const& sources,
                Names const& names,
                Unit const& unit,
                std::size_t index,
                std::vector<CallEdge>& edges,
                std::set<std::uint32_t>& addressed,
                Membership& members )
{
  Checker{ sources, names, unit, index, edges, addressed, members }.check();
}

/// A member of a function type that calls its own type — through a pointer of
/// it, or by naming another member — writes the Temporaries it is reading
/// itself. The copy 0065 proposed does not save it: the type's jump reaches
/// every member, so the call may come back to this one and write the copy too,
/// which the recursion rule then refuses. The owner gave the shape up rather
/// than keep it — see docs/decisions/0104-handlers-stop-at-one-dispatch.md — so
/// it is refused here, where the finding can name it.
void refuseCallsOfOwnType( std::vector<CallEdge> const& edges, Membership const& members )
{
  auto const typeOf = [&members]( Definition const* function ) -> std::optional<std::uint32_t>
  {
    auto const found = members.ofFunction.find( function );
    return found == members.ofFunction.end() ? std::nullopt : std::optional{ found->second };
  };
  auto const refuse = []( diag::DiagnosticSink* sink, diag::SourceSpan span )
  {
    if ( sink != nullptr )
    {
      sink->add( diagnostic( diag::DiagnosticId::C_CALLS_OWN_TYPE ).at( span.begin, span.length ) );
    }
  };
  for ( Membership::CallOfType const& call : members.callsOfType )
  {
    if ( typeOf( call.caller ) == call.type )
    {
      refuse( call.sink, call.span );
    }
  }
  for ( CallEdge const& edge : edges )
  {
    std::optional<std::uint32_t> const caller = typeOf( edge.caller );
    if ( caller.has_value() && caller == typeOf( edge.callee ) )
    {
      refuse( edge.sink, edge.span );
    }
  }
}

/// Recursion refused on the program's call graph: each cycle once, at the
/// call closing it that stands last in the order the files were given, with
/// the cycle named — see docs/decisions/0082-a-call-writes-the-callees-bytes.md.
/// The graph's components are found by Tarjan's algorithm, run on a stack of
/// its own so that a long chain of calls costs no depth of the tool's.
void refuseRecursion( diag::SourceManager const& sources, std::vector<CallEdge> const& edges )
{
  std::map<Definition const*, std::uint32_t> numbers;
  std::vector<Definition const*> functions;
  auto const number = [&]( Definition const* function )
  {
    auto const [found, added] = numbers.try_emplace( function, static_cast<std::uint32_t>( functions.size() ) );
    if ( added )
    {
      functions.push_back( function );
    }
    return found->second;
  };
  std::vector<std::vector<std::size_t>> outgoing;
  for ( std::size_t index = 0; index < edges.size(); ++index )
  {
    std::uint32_t const from = number( edges[index].caller );
    number( edges[index].callee );
    outgoing.resize( functions.size() );
    outgoing[from].push_back( index );
  }
  outgoing.resize( functions.size() );

  constexpr std::uint32_t unvisited = UINT32_MAX;
  std::vector<std::uint32_t> order( functions.size(), unvisited );
  std::vector<std::uint32_t> lowest( functions.size(), 0 );
  std::vector<std::uint32_t> component( functions.size(), unvisited );
  std::vector<bool> stacked( functions.size(), false );
  std::vector<std::uint32_t> stack;
  std::uint32_t visited = 0;
  std::uint32_t components = 0;

  struct Frame
  {
    std::uint32_t function = 0;
    std::size_t next = 0;
  };

  for ( std::uint32_t root = 0; root < functions.size(); ++root )
  {
    if ( order[root] != unvisited )
    {
      continue;
    }
    std::vector<Frame> frames{ Frame{ .function = root, .next = 0 } };
    order[root] = lowest[root] = visited++;
    stack.push_back( root );
    stacked[root] = true;
    while ( !frames.empty() )
    {
      std::uint32_t const at = frames.back().function;
      if ( frames.back().next < outgoing[at].size() )
      {
        std::uint32_t const to = numbers.at( edges[outgoing[at][frames.back().next++]].callee );
        if ( order[to] == unvisited )
        {
          order[to] = lowest[to] = visited++;
          stack.push_back( to );
          stacked[to] = true;
          frames.push_back( Frame{ .function = to, .next = 0 } );
        }
        else if ( stacked[to] )
        {
          lowest[at] = std::min( lowest[at], order[to] );
        }
        continue;
      }
      if ( lowest[at] == order[at] )
      {
        std::uint32_t member = unvisited;
        do
        {
          member = stack.back();
          stack.pop_back();
          stacked[member] = false;
          component[member] = components;
        } while ( member != at );
        ++components;
      }
      frames.pop_back();
      if ( !frames.empty() )
      {
        lowest[frames.back().function] = std::min( lowest[frames.back().function], lowest[at] );
      }
    }
  }

  // The call that closes each cycle: of the calls inside a component, the one
  // written last.
  std::vector<std::optional<std::size_t>> closing( components );
  for ( std::size_t index = 0; index < edges.size(); ++index )
  {
    std::uint32_t const from = component[numbers.at( edges[index].caller )];
    if ( from != component[numbers.at( edges[index].callee )] )
    {
      continue;
    }
    std::optional<std::size_t>& chosen = closing[from];
    auto const place = []( CallEdge const& edge ) { return std::pair{ edge.unit, edge.span.begin.rawOffset() }; };
    if ( !chosen.has_value() || place( edges[*chosen] ) < place( edges[index] ) )
    {
      chosen = index;
    }
  }

  for ( std::optional<std::size_t> const& chosen : closing )
  {
    if ( !chosen.has_value() )
    {
      continue;
    }
    CallEdge const& edge = edges[*chosen];
    std::uint32_t const from = numbers.at( edge.caller );
    std::uint32_t const to = numbers.at( edge.callee );

    // The way back from the callee to the caller, inside the component, by
    // the fewest calls.
    std::vector<std::optional<std::uint32_t>> previous( functions.size() );
    std::vector<bool> reached( functions.size(), false );
    std::vector<std::uint32_t> frontier{ to };
    reached[to] = true;
    for ( std::size_t head = 0; head < frontier.size() && !reached[from]; ++head )
    {
      for ( std::size_t const index : outgoing[frontier[head]] )
      {
        std::uint32_t const next = numbers.at( edges[index].callee );
        if ( !reached[next] && component[next] == component[from] )
        {
          reached[next] = true;
          previous[next] = frontier[head];
          frontier.push_back( next );
        }
      }
    }
    std::vector<std::uint32_t> path{ from };
    for ( std::optional<std::uint32_t> step = previous[from]; step.has_value() && from != to; step = previous[*step] )
    {
      path.push_back( *step );
      if ( *step == to )
      {
        break;
      }
    }
    std::ranges::reverse( path );
    std::string cycle = "`" + std::string{ sources.textOf( edge.caller->name.span() ) } + "`";
    for ( std::uint32_t const function : path )
    {
      cycle += " -> `" + std::string{ sources.textOf( functions[function]->name.span() ) } + "`";
    }
    edge.sink->add(
        diagnostic( diag::DiagnosticId::C_RECURSION ).at( edge.span.begin, edge.span.length ).arg( "cycle", cycle ) );
  }
}

} // namespace nga::c::detail
