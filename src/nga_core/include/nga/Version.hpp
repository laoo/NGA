#pragma once

#include <string_view>

namespace nga
{

/// Version of the tool: the tag the build was made from, or `devel` where there
/// was no tag to describe. See cmake/Version.cmake.
std::string_view versionString();

} // namespace nga
