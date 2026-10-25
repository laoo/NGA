#pragma once

#include "nga/c/Compile.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nga::c::detail
{

/// What the carry is, as far as the lines written say: `clc` and `sec` set
/// it, an add, a subtract, a compare or a shift leave it as the machine
/// decides, and a branch on it says what it is on each way out. A `clc`
/// where it is known clear, or a `sec` where it is known set, is not
/// written. See docs/decisions/0113-a-carry-known-is-not-set-again.md.
enum class Carry : std::uint8_t
{
  UNKNOWN,
  CLEAR,
  SET,
};

/// What the carry is on each way out of a block: where its branch is taken,
/// and where it falls through or jumps. One and the same but for a branch
/// on the carry itself, which is what makes them two — and then `jumpsTo`
/// says which block the branch went to, since the text branches to
/// whichever of the two successors is not written next, and that is not
/// always the one the IR calls the target.
struct EdgeCarry
{
  std::optional<std::uint32_t> jumpsTo{};
  Carry taken = Carry::UNKNOWN;
  Carry fallen = Carry::UNKNOWN;
};

/// An operand that names one byte, or an immediate: something a register can
/// be said to hold. An indexed or indirect operand names a byte only at run
/// time.
[[nodiscard]] bool isOneByte( std::string_view operand );

/// The array a store or a load through an index names — `t` of `t+1,x` or
/// `t-4,y` — where the operand is one: nothing for a byte, and nothing for
/// a pointer, `(p),y`, which may be aimed anywhere.
[[nodiscard]] std::optional<std::string_view> indexedBase( std::string_view operand );

/// The object a byte of it names: `t` of `t+2`.
[[nodiscard]] std::string_view byteBase( std::string_view operand );

/// What the registers, the carry and the flags hold, as far as the lines the
/// Writer has written say, kept by reading each line before it is written,
/// so that no path that writes a register or a byte can be missed — the
/// lesson of docs/decisions/0109-an-induction-variable-in-x.md. A line it
/// finds saying nothing new is left out, and a load of what another register
/// holds is written as the transfer.
class Tracker
{
public:
  explicit Tracker( Cpu cpu ) : mCpu( cpu ) {}

  /// The unit's names that change or act behind the program's back, which no
  /// register is said to hold and no access to which is moved, merged or
  /// dropped — see docs/decisions/0151-volatile.md.
  void setVolatiles( std::set<std::string> volatiles )
  {
    mVolatiles = std::move( volatiles );
  }

  [[nodiscard]] std::set<std::string> const& volatiles() const
  {
    return mVolatiles;
  }

  /// Whether an operand, or an object's name, is of a volatile object: by the
  /// name of what it is a byte or an element of.
  [[nodiscard]] bool isVolatile( std::string_view operand ) const;

  /// The scalar bytes of the Proc being written, by the names its text gives
  /// them, which a store through a pointer does not reach.
  void setOwnScalars( std::set<std::string> ownScalars )
  {
    mOwnScalars = std::move( ownScalars );
  }

  /// The line as it is to be written: a load of a byte another register
  /// already holds as the transfer that puts it there, or a load of a number
  /// an index register is one step from as that step.
  [[nodiscard]] std::string transferred( std::string_view text ) const;

  /// Reads one line about to be written and keeps what is known true; false
  /// where the line loads what the register already holds, or sets the carry
  /// to what it is, and is then not written.
  bool note( std::string_view text );

  /// Forgets everything, as a label does: a way in from elsewhere may have
  /// left anything.
  void forget();

  /// What `A` holds, by every name the text has for it: the operand of the
  /// `lda` that filled it, an immediate included, and every byte a `sta` has
  /// since written it to. See docs/decisions/0110-a-value-stays-in-a.md.
  [[nodiscard]] std::vector<std::string> const& inA() const
  {
    return mAHolds;
  }

  [[nodiscard]] bool holdsInA( std::string_view name ) const;

  /// A block entered holding what every way into it agrees on.
  void enter( std::vector<std::string> inA, Carry carry )
  {
    mAHolds = std::move( inA );
    mCarry = carry;
    mCarrySaysNot255 = false;
  }

  /// A parameter the caller leaves in `A` or in `X`.
  void holdInA( std::string name )
  {
    mAHolds.push_back( std::move( name ) );
  }

  void holdInX( std::string name )
  {
    mXHolds.push_back( std::move( name ) );
  }

  /// What `X` holds, by every name the text has for it — see
  /// docs/decisions/0289-a-pair-only-compared-is-held-in-a-and-x.md.
  [[nodiscard]] std::vector<std::string> const& inX() const
  {
    return mXHolds;
  }

  [[nodiscard]] bool holdsInX( std::string_view name ) const
  {
    return std::ranges::find( mXHolds, name ) != mXHolds.end();
  }

  /// The carry is clear, as the writer knows where the machine does not say:
  /// the high byte of an element's address carries nothing out, the element
  /// lying in its array and the array in memory — see
  /// docs/decisions/0284-an-elements-address-carries-nothing-out.md.
  void carryClear()
  {
    mCarry = Carry::CLEAR;
    mCarrySaysNot255 = false;
  }

  [[nodiscard]] Carry carry() const
  {
    return mCarry;
  }

  /// Whether `N` and `Z` say what `A` holds, as far as the lines written say:
  /// set by what loads or computes `A`, left by a store, a flag's own
  /// instruction or a branch, and lost to anything that sets them from
  /// something else — `ldx`, `inx`, `cmp`, `bit`, a shift of memory, a `jsr`.
  /// Kept apart from what `A` holds, since a `lda` left out because `A`
  /// already held the byte leaves the flags as whatever set them last. See
  /// docs/decisions/0130-a-test-for-zero-the-flags-already-answer.md.
  [[nodiscard]] bool flagsFromA() const
  {
    return mFlagsFromA;
  }

  /// The same for `X`: set by what loads or steps it, so that `inx` and a
  /// test of the counter against zero is `inx / jne` — see
  /// docs/decisions/0135-a-loop-run-up-to-its-wrap.md.
  [[nodiscard]] bool flagsFromX() const
  {
    return mFlagsFromX;
  }

  /// Whether the carry holds bit 7 of the byte, an `asl` having pushed it out
  /// of `A` since the byte was last written — see
  /// docs/decisions/0148-a-bit-the-carry-already-holds.md.
  [[nodiscard]] bool bit7InCarry( std::string_view name ) const
  {
    return std::ranges::find( mCarryBit7, name ) != mCarryBit7.end();
  }

  /// Whether the carry says the byte `A` holds is not 255: an `sbc #1` with
  /// the carry set left it so, and nothing has written `A` or the carry since
  /// — see docs/decisions/0262-a-counter-stepped-down-counts-in-x.md.
  [[nodiscard]] bool carrySaysNot255() const
  {
    return mCarrySaysNot255;
  }

  /// The Jcc that is taken wherever it stands, or empty where nothing the
  /// lines written say makes one certain.
  ///
  /// A jump whose condition is known to hold **is** a jump, and a branch is a
  /// byte shorter for the same three cycles — one more only where it crosses
  /// a page, which is the trade docs/decisions/0077-a-jcc-is-two-bytes-or-five.md
  /// already makes for every conditional and nobody calls a cost. See
  /// docs/decisions/0181-a-jump-whose-flag-is-known.md.
  [[nodiscard]] std::string_view alwaysTaken() const;

  /// What the last branch written said of the carry on each way out of the
  /// block, where it was a branch on the carry; nothing otherwise, and nothing
  /// once the block's own instructions are written.
  [[nodiscard]] std::optional<EdgeCarry> const& lastBranch() const
  {
    return mLastBranch;
  }

  void forgetLastBranch()
  {
    mLastBranch.reset();
  }

private:
  enum class Flags : std::uint8_t;
  struct Effect;

  [[nodiscard]] static Effect effectOf( std::string_view mnemonic, std::string_view operand );

  void noteFlags( Effect const& effect );
  void noteCarryBit( Effect const& effect, std::string_view operand, std::vector<std::string> const& before );
  void noteCarryWrap( Effect const& effect, std::string_view operand, Carry before );
  bool noteIndexes( Effect const& effect, std::string_view operand );
  bool noteIndex( char reg, std::vector<std::string>& held, Effect const& effect, std::string_view operand ) const;
  bool noteA( Effect const& effect, std::string_view operand );
  bool noteCarry( Effect const& effect, std::string_view operand );
  void forgetWritten( std::vector<std::string>& held, std::string_view operand ) const;
  [[nodiscard]] bool ownScalar( std::string const& name ) const;

  Cpu mCpu;
  std::set<std::string> mVolatiles;
  std::set<std::string> mOwnScalars;

  std::vector<std::string> mAHolds;

  /// What `X` holds, by every name the text has for it, kept the same way and
  /// forgotten at every label: a `ldx` of one of those names is written as
  /// nothing. See docs/decisions/0136-a-byte-x-holds-is-not-loaded-again.md.
  std::vector<std::string> mXHolds;

  /// And what `Y` holds, the same way: above all the `#0` of `ldy #0` before
  /// `lda (p),y`. See docs/decisions/0141-what-y-holds.md.
  std::vector<std::string> mYHolds;

  /// The bytes whose bit 7 the carry holds, as the `asl` that pushed it out of
  /// `A` left it.
  std::vector<std::string> mCarryBit7;

  bool mCarrySaysNot255 = false;

  Carry mCarry = Carry::UNKNOWN;
  bool mFlagsFromA = false;
  bool mFlagsFromX = false;
  std::optional<EdgeCarry> mLastBranch;
};

} // namespace nga::c::detail
