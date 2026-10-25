#include "nga/model/Types.hpp"

namespace nga::model
{

std::string_view nameOf( Space space )
{
  switch ( space )
  {
  case Space::RUNTIME:
    return "runtime";
  case Space::STORAGE:
    return "storage";
  case Space::LINEAR:
    return "linear";
  }
  return "runtime";
}

} // namespace nga::model
