#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// The Writer's text read back a line at a time, and the passes that rewrite
// it from what stands on its lines alone: they know nothing of the IR it was
// written from, so that what one of them leans on is in the text and nowhere
// else.

namespace nga::c::detail
{

/// One line of the text: where it stands, from its first byte to one past its
/// newline, and what is on it with the indent and the newline taken off. Past
/// the end of the text it is empty and stands at the end.
struct Line
{
  std::size_t begin = 0;
  std::size_t end = 0;
  std::string_view bare;

  /// Whether the line begins with a space: an instruction or a mark, where a
  /// label and a declaration begin at the margin.
  bool indented = false;

  /// Whether the line is a `.source` mark, which stands between two
  /// statements and is no instruction.
  [[nodiscard]] bool isMark() const;

  /// The instruction's mnemonic, and its operand, empty where it has none.
  [[nodiscard]] std::string_view mnemonic() const;
  [[nodiscard]] std::string_view operand() const;
};

/// The line that begins at `from`.
[[nodiscard]] Line lineAt( std::string_view text, std::size_t from );

/// The first line from `from` on that is not a mark: a mark stands between a
/// statement and the next as often as not, and the next is what a rewrite
/// asks about.
[[nodiscard]] Line pastMarks( std::string_view text, std::size_t from );

/// Where the declaration of a byte a Proc need not have stands in the text,
/// and the name it declares.
struct Declared
{
  std::string name;
  std::size_t begin = 0;
  std::size_t end = 0;

  /// The Proc it is a byte of, and where that Proc's text begins among the
  /// regions: what the Proc writes up to the next function, the macros of its
  /// `[[with]]` blocks included, names it by itself, and anything else only
  /// as `PROC.NAME`.
  std::string owner{};
  std::size_t region = 0;
};

/// Whether a line, bare, is a store of a register to the byte, or to a byte
/// of it: `sta name`, `stx name+1`.
[[nodiscard]] bool isStoreTo( std::string_view line, std::string_view name );

/// Whether some line names the byte: the name with no letter, digit or `_`
/// beside it, so that `__1i` is not found inside `__1if`, and `g.__0seen`
/// counts, since a case of a `switch` names its function's byte through it. A
/// word in a comment or a path counts too, which keeps a declaration rather
/// than dropping one, and that is the direction to err in.
[[nodiscard]] bool namesByte( std::string_view lines, std::string_view name );

/// Whether every line that names the byte is a store to it.
[[nodiscard]] bool onlyStoredTo( std::string_view lines, std::string_view name );

/// The body with every name that is `name` qualified by another Proc's
/// blanked out, so that only its own byte is found.
[[nodiscard]] std::string ownLines( std::string body, std::string const& name );

/// Whether every line of the body that names its own byte stores to it: a
/// name another Proc's, `bits8.__ret`, is that Proc's byte and not this one.
[[nodiscard]] bool onlyStoredIn( std::string const& body, std::string const& name );

/// Whether some line of the body names its own byte.
[[nodiscard]] bool namedIn( std::string const& body, std::string const& name );

/// What the lines of text come to, roughly: an instruction's opcode and its
/// operand, a byte for an operand of the Proc's own, which lies on the zero
/// page, and two for any other name, which may not; a Jcc at two. Enough to
/// tell a loop that fits a branch from one that does not.
[[nodiscard]] std::size_t estimatedBytes( std::string_view text );

/// Takes out of the text from `begin` on every store to one of the bytes.
void dropStoresTo( std::string& text, std::size_t begin, std::vector<std::string> const& names );

/// Puts back the load a `stz` cost. A zero stored with the accumulator
/// leaves the byte in `A`, and the writer leans on that: where the next
/// instruction reads the byte again, `stz` has bought a byte and paid for a
/// load. The pair goes back to what it was, which is the same two
/// instructions and the same flags — see
/// docs/decisions/0183-a-zero-stored-without-the-accumulator.md.
void loadBackWhatZeroStored( std::string& text, std::size_t begin );

/// Takes out a jump, certain or not, to a label that stands before the next
/// instruction anyway, with nothing but labels and marks between: the way it
/// jumps over has come to write nothing. And a conditional jump over a
/// certain one, to a label right after it, is the other condition to where
/// the certain one went: `jcs @a / jmp @b / @a` is `jcc @b / @a`. See
/// docs/decisions/0280-a-shift-right-both-ways-share-is-the-test-of-bit-0.md.
void dropJumpsToWhatFollows( std::string& text, std::size_t begin );

/// `ora X / sta X` is `tsb X` on a 65SC02: the bits of `A` set in the byte,
/// in one instruction instead of two. What it costs is that `A` is left
/// holding the mask rather than the result, so this fires only where the
/// **next** instruction fills `A` without reading it — which is the shape
/// the writer produces anyway, a run of `x |= …` loading its next operand.
///
/// `trb` has no counterpart here: `X &= ~A` would want the mask
/// complemented, and no program of the set masks a byte in place at all.
/// See docs/decisions/0188-bits-set-in-a-byte.md.
void setBitsInPlace( std::string& text, std::size_t begin );

/// A byte of a Proc's own that no line of the Module names is not
/// declared: the emitter found somewhere else for it to be, or for nothing
/// to be there at all, and what is left is a Temporary for Trace to walk
/// and Place to give an address to. Run once the whole Module stands,
/// since a byte may be named from a Proc written after the one that
/// declares it. `regions` is where each function's text begins, in the
/// order written, and `owners` its name.
void dropUnnamedBytes( std::string& text,
                       std::vector<Declared> const& declared,
                       std::vector<std::size_t> const& regions,
                       std::vector<std::string> const& owners );

} // namespace nga::c::detail
