#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nga::syntax
{

/// The encoding one of the three payload directives names — see
/// docs/spec/syntax.md#data-and-assertions and
/// docs/decisions/0069-generators.md.
enum class Payload : std::uint8_t
{
  HEX,
  BINARY,
  BASE64,
};

/// How the encoding is written, for a diagnostic that has to name it.
std::string_view nameOf( Payload encoding );

/// How many characters one group takes, and how many bytes it yields: 2 to 1,
/// 8 to 1, 4 to 3. What "whole bytes" is measured against.
std::uint32_t charactersPerGroup( Payload encoding );

/// A character a payload may not hold, positioned in the literal's body.
struct PayloadFault
{
  /// Bytes from the start of the body, which is where the parser adds the
  /// literal's own position.
  std::uint32_t offset = 0;

  /// What a diagnostic underlines: one character, or two for an escape.
  std::uint32_t length = 1;

  /// The character as a message prints it.
  std::string character;

  /// `=` where padding may not stand, which is a different thing to say than
  /// a character the alphabet does not have.
  bool padding = false;
};

/// A payload decoded, with **every** fault rather than the first: one run
/// reports every mistake, as the grammar above does.
struct DecodedPayload
{
  std::vector<std::uint8_t> bytes;
  std::vector<PayloadFault> faults;

  /// Characters the alphabet accepted. Whitespace never counts, which is why
  /// whole bytes is asked of this and not of the text.
  std::uint32_t characters = 0;

  bool wholeBytes = true;
};

/// Decodes the body of a payload literal, escapes left unresolved as
/// `quotedOf` leaves them: space and horizontal tab are skipped, an accepted
/// character contributes, and anything else is a fault carrying its position.
///
/// Total: it reports nothing and cannot fail. Ill-formed UTF-8 never reaches
/// here, since the lexer refuses it in a literal.
DecodedPayload decodePayload( Payload encoding, std::string_view body );

} // namespace nga::syntax
