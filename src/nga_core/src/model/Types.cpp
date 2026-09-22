#include "nga/model/Types.hpp"

namespace nga::model
{

std::string_view nameOf( Space space )
{
  return space == Space::RUNTIME ? "runtime" : "storage";
}

} // namespace nga::model
