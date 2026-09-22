#include "nga/model/PlacementClass.hpp"

namespace nga::model
{

std::string_view nameOf( PlacementClass placement )
{
  switch ( placement )
  {
  case PlacementClass::ZEROPAGE:
    return "zeropage";
  case PlacementClass::ABSOLUTE:
    return "absolute";
  }
  return "absolute";
}

} // namespace nga::model
