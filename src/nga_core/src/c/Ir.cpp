#include "nga/c/Ir.hpp"

namespace nga::c::ir
{

namespace
{

std::string textOf( Operand const& operand )
{
  if ( auto const* constant = std::get_if<Constant>( &operand ) )
  {
    return std::string{ spellingOf( constant->type ) } + " " + ( constant->name.empty() ? "" : constant->name + "=" ) +
           std::to_string( constant->value );
  }
  if ( auto const* object = std::get_if<Object>( &operand ) )
  {
    return "@" + object->name;
  }
  return "%" + std::to_string( std::get<Value>( operand ).index );
}

std::string_view nameOf( UnaryOperator op )
{
  switch ( op )
  {
  case UnaryOperator::NEGATE:
    return "neg";
  case UnaryOperator::COMPLEMENT:
    return "not";
  case UnaryOperator::LOGICAL_NOT:
    return "lnot";
  }
  return "";
}

std::string_view nameOf( BinaryOperator op )
{
  switch ( op )
  {
  case BinaryOperator::ADD:
    return "add";
  case BinaryOperator::SUBTRACT:
    return "sub";
  case BinaryOperator::AND:
    return "and";
  case BinaryOperator::OR:
    return "or";
  case BinaryOperator::XOR:
    return "xor";
  case BinaryOperator::SHIFT_LEFT:
    return "shl";
  case BinaryOperator::SHIFT_RIGHT:
    return "shr";
  }
  return "";
}

std::string_view nameOf( Comparison op )
{
  switch ( op )
  {
  case Comparison::EQUAL:
    return "eq";
  case Comparison::NOT_EQUAL:
    return "ne";
  case Comparison::LESS:
    return "lt";
  case Comparison::GREATER_OR_EQUAL:
    return "ge";
  }
  return "";
}

/// `%N = NAME TYPE OPERANDS`.
/// An object's type, and an array's number of elements, and what it is given.
std::string shapeOf( Global const& object )
{
  std::string text{ spellingOf( object.type ) };
  if ( object.count.has_value() )
  {
    text += "[" + std::to_string( *object.count ) + "]";
  }
  text += object.isStatic ? " static" : "";
  text += object.pane.empty() ? "" : " in " + object.pane;
  if ( object.value.has_value() )
  {
    text += ", " + textOf( *object.value );
  }
  if ( !object.elements.empty() )
  {
    text += ", {";
    for ( std::size_t index = 0; index < object.elements.size(); ++index )
    {
      text += ( index == 0 ? " " : ", " ) + textOf( object.elements[index] );
    }
    text += " }";
  }
  for ( auto const& [path, elements] : object.stripes )
  {
    std::string name;
    for ( std::string const& part : path )
    {
      name += ( name.empty() ? "" : "." ) + part;
    }
    text += ", stripe " + name;
  }
  return text + ( object.isTemporary ? " temporary" : "" );
}

std::string defining( Value result, std::string_view name, Type type, std::string const& operands )
{
  return "  %" + std::to_string( result.index ) + " = " + std::string{ name } + " " +
         std::string{ spellingOf( type ) } + " " + operands + "\n";
}

std::string textOf( Instruction const& instruction, Function const& function )
{
  if ( auto const* call = std::get_if<Call>( &instruction.operation ) )
  {
    std::string arguments;
    for ( Argument const& argument : call->arguments )
    {
      arguments.append( arguments.empty() ? "" : ", " ).append( textOf( argument.value ) );
      if ( !argument.place.empty() )
      {
        arguments.append( " in " ).append( argument.place );
      }
    }
    std::string const called = call->name + ( call->arguments.empty() ? "" : "(" + arguments + ")" ) +
                               ( call->resultPlace.empty() ? "" : " out " + call->resultPlace ) +
                               ( call->with.empty() ? "" : " with " + call->with );
    if ( call->result.has_value() )
    {
      return defining( *call->result, "call", function.values.at( call->result->index ), called );
    }
    return "  call " + called + "\n";
  }
  if ( auto const* with = std::get_if<EnterWith>( &instruction.operation ) )
  {
    std::string text = "  with " + with->name;
    if ( with->form == WithForm::AT && with->index.has_value() )
    {
      text += ", " + textOf( *with->index );
    }
    else if ( with->form == WithForm::STATE )
    {
      text += " = " + with->state;
    }
    return text + "\n";
  }
  if ( auto const* store = std::get_if<Store>( &instruction.operation ) )
  {
    return "  store " + store->name + ", " + textOf( store->value ) + "\n";
  }
  if ( auto const* unary = std::get_if<Unary>( &instruction.operation ) )
  {
    return defining( unary->result, nameOf( unary->op ), unary->type, textOf( unary->operand ) );
  }
  if ( auto const* binary = std::get_if<Binary>( &instruction.operation ) )
  {
    return defining(
        binary->result, nameOf( binary->op ), binary->type, textOf( binary->left ) + ", " + textOf( binary->right ) );
  }
  if ( auto const* choice = std::get_if<Switch>( &instruction.operation ) )
  {
    return "  switch " + textOf( choice->value ) + ", " + std::to_string( choice->first ) + ", " +
           std::to_string( choice->count ) + "\n";
  }
  if ( auto const* convert = std::get_if<Convert>( &instruction.operation ) )
  {
    Type const source = typeOf( convert->operand, function );
    std::string_view name = "bits";
    if ( sizeOf( convert->type ) < sizeOf( source ) )
    {
      name = "trunc";
    }
    else if ( sizeOf( convert->type ) > sizeOf( source ) )
    {
      name = isSigned( source ) ? "sext" : "zext";
    }
    return defining( convert->result, name, convert->type, textOf( convert->operand ) );
  }
  if ( auto const* load = std::get_if<Load>( &instruction.operation ) )
  {
    std::string const name = load->high.empty() ? load->name : load->name + "/" + load->high;
    return defining( load->result, "load", load->type, name + "[" + textOf( load->index ) + "]" );
  }
  if ( auto const* indirect = std::get_if<LoadIndirect>( &instruction.operation ) )
  {
    return defining( indirect->result,
                     indirect->isVolatile ? "load volatile" : "load",
                     indirect->type,
                     "(" + textOf( indirect->pointer ) + ")," + textOf( indirect->index ) );
  }
  if ( auto const* indirect = std::get_if<StoreIndirect>( &instruction.operation ) )
  {
    return std::string{ indirect->isVolatile ? "  store volatile (" : "  store (" } + textOf( indirect->pointer ) +
           ")," + textOf( indirect->index ) + ", " + textOf( indirect->value ) + "\n";
  }
  if ( auto const* copy = std::get_if<Copy>( &instruction.operation ) )
  {
    auto const place = []( Place const& at )
    {
      return at.pointer.has_value() ? "(" + textOf( *at.pointer ) + ")+" + std::to_string( at.offset ) : "@" + at.name;
    };
    return "  copy " + std::to_string( copy->bytes ) + ", " + place( copy->from ) + " -> " + place( copy->to ) + "\n";
  }
  if ( auto const* element = std::get_if<StoreElement>( &instruction.operation ) )
  {
    return "  store " + element->name + "[" + textOf( element->index ) + "], " + textOf( element->value ) + "\n";
  }
  auto const& compare = std::get<Compare>( instruction.operation );
  return defining(
      compare.result, nameOf( compare.op ), compare.type, textOf( compare.left ) + ", " + textOf( compare.right ) );
}

} // namespace

std::uint32_t sizeOf( Type type )
{
  switch ( type )
  {
  case Type::U8:
  case Type::I8:
  case Type::BOOL:
    return 1;
  case Type::U16:
  case Type::I16:
  case Type::POINTER:
    return 2;
  case Type::BLOCK:
    return 0;
  }
  return 0;
}

bool isSigned( Type type )
{
  return type == Type::I8 || type == Type::I16;
}

std::int64_t wrapped( std::int64_t value, Type type )
{
  std::uint32_t const bits = 8 * sizeOf( type );
  std::uint64_t const span = std::uint64_t{ 1 } << bits;
  std::uint64_t const low = static_cast<std::uint64_t>( value ) & ( span - 1 );
  if ( isSigned( type ) && low >= span / 2 )
  {
    return static_cast<std::int64_t>( low ) - static_cast<std::int64_t>( span );
  }
  return static_cast<std::int64_t>( low );
}

std::string_view spellingOf( Type type )
{
  switch ( type )
  {
  case Type::U8:
    return "u8";
  case Type::I8:
    return "i8";
  case Type::U16:
    return "u16";
  case Type::I16:
    return "i16";
  case Type::BOOL:
    return "bool";
  case Type::POINTER:
    return "ptr";
  case Type::BLOCK:
    return "block";
  }
  return "";
}

Type typeOf( Operand const& operand, Function const& function )
{
  if ( auto const* constant = std::get_if<Constant>( &operand ) )
  {
    return constant->type;
  }
  if ( auto const* object = std::get_if<Object>( &operand ) )
  {
    return object->type;
  }
  return function.values.at( std::get<Value>( operand ).index );
}

std::optional<Value> resultOf( Instruction const& instruction )
{
  if ( auto const* call = std::get_if<Call>( &instruction.operation ) )
  {
    return call->result;
  }
  if ( auto const* unary = std::get_if<Unary>( &instruction.operation ) )
  {
    return unary->result;
  }
  if ( auto const* binary = std::get_if<Binary>( &instruction.operation ) )
  {
    return binary->result;
  }
  if ( auto const* compare = std::get_if<Compare>( &instruction.operation ) )
  {
    return compare->result;
  }
  if ( auto const* convert = std::get_if<Convert>( &instruction.operation ) )
  {
    return convert->result;
  }
  if ( auto const* load = std::get_if<Load>( &instruction.operation ) )
  {
    return load->result;
  }
  if ( auto const* indirect = std::get_if<LoadIndirect>( &instruction.operation ) )
  {
    return indirect->result;
  }
  return std::nullopt;
}

std::string dump( Unit const& unit )
{
  std::string text;
  bool afterFunction = false;
  for ( Definition const& definition : unit.definitions )
  {
    // A function is set off by a blank line from what stands on either side
    // of it; globals stand together.
    bool const isFunction = std::holds_alternative<Function>( definition );
    if ( !text.empty() && ( isFunction || afterFunction ) )
    {
      text += '\n';
    }
    afterFunction = isFunction;

    if ( auto const* global = std::get_if<Global>( &definition ) )
    {
      text.append( "global " ).append( global->name ).append( " " ).append( shapeOf( *global ) ).append( "\n" );
      continue;
    }

    if ( auto const* constant = std::get_if<NamedConstant>( &definition ) )
    {
      text.append( "constant " ).append( constant->name );
      text.append( constant->isStatic ? " static, " : ", " ).append( textOf( constant->value ) ).append( "\n" );
      continue;
    }

    if ( auto const* aggregate = std::get_if<Aggregate>( &definition ) )
    {
      text.append( "struct " ).append( aggregate->name ).append( " {" );
      for ( std::size_t index = 0; index < aggregate->members.size(); ++index )
      {
        text.append( index == 0 ? " " : ", " )
            .append( aggregate->members[index].first )
            .append( "+" )
            .append( std::to_string( aggregate->members[index].second ) );
      }
      text.append( " }\n" );
      continue;
    }

    if ( auto const* enumeration = std::get_if<Enumeration>( &definition ) )
    {
      text.append( "enum " ).append( enumeration->name ).append( " {" );
      for ( std::size_t index = 0; index < enumeration->enumerators.size(); ++index )
      {
        text.append( index == 0 ? " " : ", " ).append( enumeration->enumerators[index] );
      }
      text.append( " }\n" );
      continue;
    }

    auto const& function = std::get<Function>( definition );
    text.append( "function " )
        .append( function.name )
        .append( function.isStatic ? " static" : "" )
        .append( function.pane.empty() ? "" : " in " + function.pane )
        .append( function.under.empty() ? "" : " under " + function.under );
    text.append( function.then.empty() ? "\n" : " then " + function.then + "\n" );
    for ( Local const& parameter : function.parameters )
    {
      text.append( "  parameter " )
          .append( parameter.name )
          .append( " " )
          .append( spellingOf( parameter.type ) )
          .append( "\n" );
    }
    if ( function.result.has_value() )
    {
      text.append( "  result " ).append( spellingOf( *function.result ) );
      if ( !function.resultByte.empty() )
      {
        text.append( " in " ).append( function.resultByte );
      }
      text.append( "\n" );
    }
    for ( Local const& local : function.locals )
    {
      text.append( "  local " ).append( local.name ).append( " " ).append( spellingOf( local.type ) ).append( "\n" );
    }
    for ( NamedConstant const& constant : function.constants )
    {
      text.append( "  constant " )
          .append( constant.name )
          .append( ", " )
          .append( textOf( constant.value ) )
          .append( "\n" );
    }
    for ( Global const& object : function.sections )
    {
      text.append( "  own " ).append( object.name ).append( " " ).append( shapeOf( object ) ).append( "\n" );
    }
    for ( std::size_t index = 0; index < function.blocks.size(); ++index )
    {
      Block const& block = function.blocks[index];
      text.append( "block" ).append( std::to_string( index ) ).append( ":\n" );
      for ( Instruction const& instruction : block.instructions )
      {
        text.append( textOf( instruction, function ) );
      }
      Terminator const& terminator = block.terminator;
      switch ( terminator.kind )
      {
      case TerminatorKind::RETURN:
        text.append( "  ret\n" );
        break;
      case TerminatorKind::TRANSITION:
        text.append( "  enter " ).append( terminator.phase ).append( "\n" );
        break;
      case TerminatorKind::FALL:
        text.append( "  fall\n" );
        break;
      case TerminatorKind::JUMP:
        text.append( "  jump block" ).append( std::to_string( terminator.target ) ).append( "\n" );
        break;
      case TerminatorKind::BRANCH:
        text.append( "  branch " )
            .append( textOf( terminator.condition ) )
            .append( ", block" )
            .append( std::to_string( terminator.target ) )
            .append( ", block" )
            .append( std::to_string( terminator.otherwise ) )
            .append( "\n" );
        break;
      case TerminatorKind::DISPATCH:
        text.append( "  dispatch" );
        for ( std::size_t at = 0; at < terminator.targets.size(); ++at )
        {
          text.append( at == 0 ? " block" : ", block" ).append( std::to_string( terminator.targets[at] ) );
        }
        text.append( "\n" );
        break;
      }
    }
  }
  return text;
}

} // namespace nga::c::ir
