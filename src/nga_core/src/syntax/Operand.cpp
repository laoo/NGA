#include "nga/syntax/Operand.hpp"

namespace nga::syntax
{

std::string_view nameOf( OperandShape shape )
{
  switch ( shape )
  {
  case OperandShape::NONE:
    return "none";
  case OperandShape::IMMEDIATE:
    return "immediate";
  case OperandShape::DIRECT:
    return "direct";
  case OperandShape::DIRECT_X:
    return "direct,x";
  case OperandShape::DIRECT_Y:
    return "direct,y";
  case OperandShape::INDIRECT:
    return "indirect";
  case OperandShape::INDIRECT_Y:
    return "indirect,y";
  case OperandShape::INDEXED_INDIRECT:
    return "indexed indirect";
  }
  return "none";
}

std::string_view nameOf( DataWidth width )
{
  return width == DataWidth::BYTE ? "byte" : "word";
}

} // namespace nga::syntax
