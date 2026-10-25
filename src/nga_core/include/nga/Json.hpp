#pragma once

#include <string>
#include <string_view>

namespace nga::json
{

/// Appends `text` as a quoted JSON string, escaping what JSON requires and
/// every C0 control as `\uXXXX`, and writing a byte that begins no well-formed
/// UTF-8 sequence as `\ufffd`, since a document of JSON is UTF-8.
///
/// The tool writes JSON in three places and fetches no library to do it: a
/// finding's rendering, the diagnostic catalog, and the facts of a build. One
/// escaper keeps them from disagreeing about a quote.
void appendString( std::string& out, std::string_view text );

} // namespace nga::json
