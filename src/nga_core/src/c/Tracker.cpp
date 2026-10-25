#include "nga/c/Tracker.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace nga::c::detail
{

namespace
{

/// The number an operand is, where it is one the tracker may step: `#7` is
/// 7, and a name, an address or anything else is nothing.
std::optional<int> numberIn( std::string_view operand )
{
  if ( !operand.starts_with( '#' ) || operand.size() < 2 )
  {
    return std::nullopt;
  }
  std::string_view const digits = operand.substr( 1 );
  int number = 0;
  auto const [end, error] = std::from_chars( digits.data(), digits.data() + digits.size(), number );
  if ( error != std::errc{} || end != digits.data() + digits.size() || number < 0 || number > 255 )
  {
    return std::nullopt;
  }
  return number;
}

/// A load an index register is one step away from, written as that step:
/// `dey` where `Y` holds `#1` and `#0` is wanted, which is one byte where
/// the load is two and the same two cycles, and sets `N` and `Z` from the
/// same number the load would. One step only — two would be two bytes
/// again, and four cycles. `A` has no such step on a 6502, and on a
/// 65SC02 it is
/// docs/decisions/0184-one-more-in-the-accumulator.md's. See
/// docs/decisions/0199-a-byte-another-register-holds-is-transferred.md.
std::string_view steppedTo( char const reg, std::vector<std::string> const& held, std::optional<int> const wanted )
{
  if ( !wanted.has_value() )
  {
    return {};
  }
  for ( std::string const& name : held )
  {
    std::optional<int> const number = numberIn( name );
    if ( !number.has_value() )
    {
      continue;
    }
    if ( ( *number + 1 ) % 256 == *wanted )
    {
      return reg == 'x' ? "inx" : "iny";
    }
    if ( ( *number + 255 ) % 256 == *wanted )
    {
      return reg == 'x' ? "dex" : "dey";
    }
  }
  return {};
}

/// The block whose label the operand is, or nothing for a local label such
/// as `@e0`, which a branch inside a block goes to.
std::optional<std::uint32_t> blockOf( std::string_view operand )
{
  if ( !operand.starts_with( "@l" ) )
  {
    return std::nullopt;
  }
  std::uint32_t index = 0;
  for ( char const digit : operand.substr( 2 ) )
  {
    if ( digit < '0' || digit > '9' )
    {
      return std::nullopt;
    }
    index = ( index * 10 ) + static_cast<std::uint32_t>( digit - '0' );
  }
  return index;
}

} // namespace

/// What `N` and `Z` say once an instruction has run, as far as the tracker
/// asks: what they said before, what `A` holds, what `X` holds, both — after
/// `tax` or `txa` the two hold one byte — or something else.
enum class Tracker::Flags : std::uint8_t
{
  KEPT,
  A,
  X,
  A_AND_X,
  OTHER,
};

/// What an instruction the Writer writes does to what the tracker keeps: the
/// registers it writes, whether it writes the byte its operand names, whether
/// it writes the carry, and what `N` and `Z` then say. A shift, `inc` or `dec`
/// with no operand works on `A` and not on memory, which `accumulator` marks.
/// A branch or a jump writes nothing: one not taken goes on with everything
/// as it was, and one taken lands on a label, which forgets.
struct Tracker::Effect
{
  std::string_view mnemonic;
  bool a = false;
  bool x = false;
  bool y = false;
  bool memory = false;
  bool carry = false;
  Flags flags = Flags::KEPT;
  bool accumulator = false;
};

/// What one line does, its operand deciding between a shift of memory and one
/// of `A`. Anything the table does not hold — `jsr`, a directive, a macro —
/// writes every register, the carry and memory, since what it does cannot be
/// read off its text.
Tracker::Effect Tracker::effectOf( std::string_view mnemonic, std::string_view operand )
{
  // Every instruction the Writer writes, in one place, so that what one does
  // is said once: a mnemonic left out is taken to do anything, which is never
  // wrong and only costs what the tracker would have known.
  static constexpr auto EFFECTS = std::to_array<Effect>( {
      { .mnemonic = "lda", .a = true, .flags = Flags::A },
      { .mnemonic = "ldx", .x = true, .flags = Flags::X },
      { .mnemonic = "ldy", .y = true, .flags = Flags::OTHER },
      { .mnemonic = "sta", .memory = true },
      { .mnemonic = "stx", .memory = true },
      { .mnemonic = "sty", .memory = true },
      { .mnemonic = "stz", .memory = true },
      { .mnemonic = "tax", .x = true, .flags = Flags::A_AND_X },
      { .mnemonic = "txa", .a = true, .flags = Flags::A_AND_X },
      { .mnemonic = "tay", .y = true, .flags = Flags::A },
      { .mnemonic = "tya", .a = true, .flags = Flags::A },
      { .mnemonic = "tsx", .x = true, .flags = Flags::X },
      { .mnemonic = "txs" },
      { .mnemonic = "inx", .x = true, .flags = Flags::X },
      { .mnemonic = "dex", .x = true, .flags = Flags::X },
      { .mnemonic = "iny", .y = true, .flags = Flags::OTHER },
      { .mnemonic = "dey", .y = true, .flags = Flags::OTHER },
      { .mnemonic = "inc", .memory = true, .flags = Flags::OTHER, .accumulator = true },
      { .mnemonic = "dec", .memory = true, .flags = Flags::OTHER, .accumulator = true },
      { .mnemonic = "asl", .memory = true, .carry = true, .flags = Flags::OTHER, .accumulator = true },
      { .mnemonic = "lsr", .memory = true, .carry = true, .flags = Flags::OTHER, .accumulator = true },
      { .mnemonic = "rol", .memory = true, .carry = true, .flags = Flags::OTHER, .accumulator = true },
      { .mnemonic = "ror", .memory = true, .carry = true, .flags = Flags::OTHER, .accumulator = true },
      { .mnemonic = "and", .a = true, .flags = Flags::A },
      { .mnemonic = "ora", .a = true, .flags = Flags::A },
      { .mnemonic = "eor", .a = true, .flags = Flags::A },
      { .mnemonic = "adc", .a = true, .carry = true, .flags = Flags::A },
      { .mnemonic = "sbc", .a = true, .carry = true, .flags = Flags::A },
      { .mnemonic = "cmp", .carry = true, .flags = Flags::OTHER },
      { .mnemonic = "cpx", .carry = true, .flags = Flags::OTHER },
      { .mnemonic = "cpy", .carry = true, .flags = Flags::OTHER },
      { .mnemonic = "bit", .flags = Flags::OTHER },
      { .mnemonic = "clc", .carry = true },
      { .mnemonic = "sec", .carry = true },
      { .mnemonic = "clv" },
      { .mnemonic = "cld" },
      { .mnemonic = "sed" },
      { .mnemonic = "cli" },
      { .mnemonic = "sei" },
      { .mnemonic = "nop" },
      { .mnemonic = "pha" },
      { .mnemonic = "php" },
      { .mnemonic = "pla", .a = true, .flags = Flags::A },
      { .mnemonic = "bcc" },
      { .mnemonic = "bcs" },
      { .mnemonic = "beq" },
      { .mnemonic = "bne" },
      { .mnemonic = "bmi" },
      { .mnemonic = "bpl" },
      { .mnemonic = "bvc" },
      { .mnemonic = "bvs" },
      { .mnemonic = "jcc" },
      { .mnemonic = "jcs" },
      { .mnemonic = "jeq" },
      { .mnemonic = "jne" },
      { .mnemonic = "jmi" },
      { .mnemonic = "jpl" },
      { .mnemonic = "jvc" },
      { .mnemonic = "jvs" },
      { .mnemonic = "jmpcc" },
      { .mnemonic = "jmpcs" },
      { .mnemonic = "jmpeq" },
      { .mnemonic = "jmpne" },
      { .mnemonic = "jra" },
      { .mnemonic = "jmp" },
      { .mnemonic = "rts" },
  } );

  auto const found = std::ranges::find( EFFECTS, mnemonic, &Effect::mnemonic );
  if ( found == EFFECTS.end() )
  {
    return Effect{
      .mnemonic = mnemonic, .a = true, .x = true, .y = true, .memory = true, .carry = true, .flags = Flags::OTHER
    };
  }
  Effect effect = *found;
  if ( effect.accumulator && operand.empty() )
  {
    effect.a = true;
    effect.memory = false;
    effect.flags = Flags::A;
  }
  return effect;
}

bool isOneByte( std::string_view operand )
{
  return operand.find_first_of( ",(" ) == std::string_view::npos;
}

std::optional<std::string_view> indexedBase( std::string_view operand )
{
  std::size_t const comma = operand.find( ',' );
  if ( comma == std::string_view::npos || operand.contains( '(' ) )
  {
    return std::nullopt;
  }
  return byteBase( operand.substr( 0, comma ) );
}

std::string_view byteBase( std::string_view operand )
{
  std::size_t const offset = operand.find_first_of( "+-" );
  return offset == std::string_view::npos ? operand : operand.substr( 0, offset );
}

bool Tracker::isVolatile( std::string_view operand ) const
{
  if ( mVolatiles.empty() )
  {
    return false;
  }
  std::optional<std::string_view> const indexed = indexedBase( operand );
  std::string_view const base = indexed.has_value() ? *indexed : byteBase( operand );
  return mVolatiles.contains( std::string{ base } );
}

bool Tracker::holdsInA( std::string_view name ) const
{
  return std::ranges::find( mAHolds, name ) != mAHolds.end();
}

void Tracker::forget()
{
  mAHolds.clear();
  mXHolds.clear();
  mYHolds.clear();
  mCarryBit7.clear();
  mCarrySaysNot255 = false;
  mCarry = Carry::UNKNOWN;
  mFlagsFromA = false;
  mFlagsFromX = false;
}

std::string_view Tracker::alwaysTaken() const
{
  // Where the processor has `bra` there is no flag to know: every jump is a
  // branch where it reaches, and the tracking below is the 6502's business
  // alone — see docs/decisions/0187-a-jump-that-asks-no-flag.md.
  if ( mCpu == Cpu::WDC65SC02 )
  {
    return "jra";
  }
  if ( mFlagsFromA )
  {
    for ( std::string const& name : mAHolds )
    {
      // A literal alone: `#<label` and the like name a value Place settles,
      // and the flags cannot be read off text the assembler has not folded.
      if ( name.size() < 2 || name.front() != '#' )
      {
        continue;
      }
      std::string_view digits{ name };
      digits.remove_prefix( 1 );
      int base = 10;
      if ( digits.front() == '$' )
      {
        digits.remove_prefix( 1 );
        base = 16;
      }
      if ( digits.empty() ||
           !std::ranges::all_of(
               digits, [base]( char c ) { return base == 16 ? std::isxdigit( c ) != 0 : std::isdigit( c ) != 0; } ) )
      {
        continue;
      }
      unsigned long const value = std::stoul( std::string{ digits }, nullptr, base );
      return ( value & 0xFFU ) == 0 ? "jmpeq" : "jmpne";
    }
  }
  if ( mCarry == Carry::SET )
  {
    return "jmpcs";
  }
  if ( mCarry == Carry::CLEAR )
  {
    return "jmpcc";
  }
  return {};
}

/// Keeps `mFlagsFromA` and `mFlagsFromX` true for one line that is written.
void Tracker::noteFlags( Effect const& effect )
{
  if ( effect.flags != Flags::KEPT )
  {
    mFlagsFromA = effect.flags == Flags::A || effect.flags == Flags::A_AND_X;
    mFlagsFromX = effect.flags == Flags::X || effect.flags == Flags::A_AND_X;
  }
}

/// Forgets what a store to `operand` may have written of what a register
/// was said to hold: that byte, and an element of its array read through an
/// index; for a store through an index, whatever of that array was held,
/// since an element's index stays within its array — see
/// docs/decisions/0149-a-store-to-an-element-stays-in-its-array.md; and for
/// a store through a pointer, every byte but a scalar of the Proc's own,
/// which no pointer is aimed at — see
/// docs/decisions/0150-a-pointer-does-not-reach-the-procs-own-scalars.md.
void Tracker::forgetWritten( std::vector<std::string>& held, std::string_view operand ) const
{
  auto const baseOfHeld = []( std::string const& name ) -> std::string_view
  {
    std::optional<std::string_view> const indexed = indexedBase( name );
    return indexed.has_value() ? *indexed : byteBase( name );
  };
  if ( isOneByte( operand ) )
  {
    std::string_view const base = byteBase( operand );
    std::erase_if( held,
                   [&]( std::string const& name )
                   { return name == operand || ( indexedBase( name ).has_value() && baseOfHeld( name ) == base ); } );
    return;
  }
  if ( std::optional<std::string_view> const base = indexedBase( operand ); base.has_value() )
  {
    std::erase_if( held,
                   [&]( std::string const& name ) { return !name.starts_with( '#' ) && baseOfHeld( name ) == *base; } );
    return;
  }
  std::erase_if( held,
                 [this]( std::string const& name )
                 { return !name.starts_with( '#' ) && !ownScalar( std::string{ byteBase( name ) } ); } );
}

/// Whether a name is a scalar byte, or pair, of the Proc being written: a
/// parameter, the result, a local of a scalar type, or a scratch byte. C
/// takes the address of none of them — a parameter's is refused, and a
/// local whose address is taken is a Section of its own — and the
/// assembler may not count on them, so no pointer is aimed at one.
bool Tracker::ownScalar( std::string const& name ) const
{
  bool const scratch = name.starts_with( "__t" ) && name.size() > 3 &&
                       std::ranges::all_of( name.substr( 3 ), []( char c ) { return c >= '0' && c <= '9'; } );
  return scratch || mOwnScalars.contains( name );
}

/// A load of a byte another register already holds, written as the transfer
/// that puts it there: one byte and two cycles, where the shortest load is
/// two and two. Read from what the registers hold, before the line is noted,
/// so that no path that writes one can be missed — the lesson of
/// docs/decisions/0110-a-value-stays-in-a.md. Only through `A`: the
/// processor has no `txy` or `tyx`, and the way round through `A` costs two
/// instructions and `A` itself. Where no register holds the byte, an index
/// register one step from the number wanted steps to it instead. See
/// docs/decisions/0199-a-byte-another-register-holds-is-transferred.md.
std::string Tracker::transferred( std::string_view text ) const
{
  std::size_t const space = text.find( ' ' );
  if ( space == std::string_view::npos )
  {
    return std::string{ text };
  }
  std::string_view const mnemonic = text.substr( 0, space );
  std::string_view const operand = text.substr( space + 1 );
  // What a register may be said to hold at all: an indexed or indirect
  // operand names a byte only at run time, and a volatile one is nobody's.
  if ( !isOneByte( operand ) || isVolatile( operand ) )
  {
    return std::string{ text };
  }
  auto const holds = []( std::vector<std::string> const& held, std::string_view what )
  { return std::ranges::find( held, what ) != held.end(); };
  // Where the register itself holds the byte the load is left out
  // altogether, which is cheaper still, so the transfer is for the rest.
  if ( mnemonic == "lda" && !holds( mAHolds, operand ) )
  {
    if ( holds( mXHolds, operand ) )
    {
      return "txa";
    }
    if ( holds( mYHolds, operand ) )
    {
      return "tya";
    }
  }
  if ( mnemonic == "ldx" && !holds( mXHolds, operand ) )
  {
    if ( holds( mAHolds, operand ) )
    {
      return "tax";
    }
    if ( std::string_view const step = steppedTo( 'x', mXHolds, numberIn( operand ) ); !step.empty() )
    {
      return std::string{ step };
    }
  }
  if ( mnemonic == "ldy" && !holds( mYHolds, operand ) )
  {
    if ( holds( mAHolds, operand ) )
    {
      return "tay";
    }
    if ( std::string_view const step = steppedTo( 'y', mYHolds, numberIn( operand ) ); !step.empty() )
    {
      return std::string{ step };
    }
  }
  return std::string{ text };
}

bool Tracker::note( std::string_view text )
{
  std::size_t const space = text.find( ' ' );
  std::string_view const mnemonic = text.substr( 0, space );
  std::string_view const operand = space == std::string_view::npos ? std::string_view{} : text.substr( space + 1 );
  Effect const effect = effectOf( mnemonic, operand );
  std::vector<std::string> const before = mAHolds;
  Carry const carryBefore = mCarry;
  if ( !noteIndexes( effect, operand ) || !noteA( effect, operand ) )
  {
    return false;
  }
  noteCarryBit( effect, operand, before );
  noteCarryWrap( effect, operand, carryBefore );
  // An element read through `X` or `Y` is no longer the one the register
  // now picks, once the register moves.
  auto const forgetThrough = [this]( std::string_view suffix )
  {
    std::erase_if( mAHolds, [suffix]( std::string const& name ) { return name.ends_with( suffix ); } );
    std::erase_if( mCarryBit7, [suffix]( std::string const& name ) { return name.ends_with( suffix ); } );
  };
  if ( effect.x )
  {
    forgetThrough( ",x" );
  }
  if ( effect.y )
  {
    forgetThrough( ",y" );
  }
  // A transfer fills the register with what `A` holds, which `A` still holds.
  auto const bytes = [this]
  {
    std::vector<std::string> named = mAHolds;
    std::erase_if( named, []( std::string const& name ) { return name.contains( ',' ); } );
    return named;
  };
  if ( mnemonic == "tax" )
  {
    mXHolds = bytes();
  }
  else if ( mnemonic == "tay" )
  {
    mYHolds = bytes();
  }
  // And the other way: `A` then holds every byte the index register holds,
  // where a `lda` would have left it holding one.
  else if ( mnemonic == "txa" )
  {
    mAHolds = mXHolds;
  }
  else if ( mnemonic == "tya" )
  {
    mAHolds = mYHolds;
  }
  noteFlags( effect );
  return true;
}

/// Keeps `mCarryBit7` true for one line written: an `asl` of `A` pushes the
/// top bit of what `A` held into the carry, which then stands for that bit
/// of each byte `A` held, until something sets the carry otherwise or
/// writes the byte — see docs/decisions/0148-a-bit-the-carry-already-holds.md.
void Tracker::noteCarryBit( Effect const& effect, std::string_view operand, std::vector<std::string> const& before )
{
  if ( effect.mnemonic == "asl" && operand.empty() )
  {
    mCarryBit7 = before;
    return;
  }
  if ( effect.carry )
  {
    mCarryBit7.clear();
    return;
  }
  if ( effect.memory )
  {
    forgetWritten( mCarryBit7, operand );
  }
}

/// Keeps `mCarrySaysNot255` true for one line written: `sbc #1` with the
/// carry set leaves it clear exactly where it borrowed, which is where the
/// byte was zero and `A` is now 255. A store keeps it, and anything that
/// writes `A` or the carry loses it — see
/// docs/decisions/0262-a-counter-stepped-down-counts-in-x.md.
void Tracker::noteCarryWrap( Effect const& effect, std::string_view operand, Carry before )
{
  if ( effect.mnemonic == "sbc" && operand == "#1" && before == Carry::SET )
  {
    mCarrySaysNot255 = true;
    return;
  }
  if ( effect.a || effect.carry )
  {
    mCarrySaysNot255 = false;
  }
}

/// Keeps `mXHolds` and `mYHolds` true for one line about to be written;
/// false where the line is a `ldx` or a `ldy` of what the register already
/// holds, which is then not written.
bool Tracker::noteIndexes( Effect const& effect, std::string_view operand )
{
  bool const x = noteIndex( 'x', mXHolds, effect, operand );
  bool const y = noteIndex( 'y', mYHolds, effect, operand );
  return x && y;
}

/// What one index register holds, kept as `mAHolds` is: the operand of the
/// load that filled it, what `A` held where a transfer filled it, and every
/// byte a store of it has since written it to. A number it holds is still
/// known after the register is stepped, `#0` and `iny` being `#1`, so a
/// `ldy #1` after them is not written either. See
/// docs/decisions/0136-a-byte-x-holds-is-not-loaded-again.md and
/// docs/decisions/0141-what-y-holds.md.
bool Tracker::noteIndex( char const reg,
                         std::vector<std::string>& held,
                         Effect const& effect,
                         std::string_view operand ) const
{
  std::string const own{ reg };
  std::string_view const mnemonic = effect.mnemonic;
  auto const holds = [&held]( std::string_view what ) { return std::ranges::find( held, what ) != held.end(); };

  if ( mnemonic == "ld" + own )
  {
    bool const holdable = isOneByte( operand ) && !isVolatile( operand );
    if ( holdable && holds( operand ) )
    {
      return false;
    }
    held.clear();
    if ( holdable )
    {
      held.emplace_back( operand );
    }
    return true;
  }
  if ( mnemonic == "in" + own || mnemonic == "de" + own )
  {
    // A number stepped is the next number; a name is no longer what it holds.
    int const by = mnemonic.starts_with( "in" ) ? 1 : 255;
    std::vector<std::string> stepped;
    for ( std::string const& name : held )
    {
      if ( std::optional<int> const number = numberIn( name ); number.has_value() )
      {
        stepped.push_back( "#" + std::to_string( ( *number + by ) % 256 ) );
      }
    }
    held = std::move( stepped );
    return true;
  }

  // What writes a byte of memory: the byte the register may hold is then no
  // longer what it holds — any byte, where the store goes through an index
  // or a pointer. Its own store writes what it holds, so it holds that byte
  // too.
  if ( effect.memory && !operand.empty() )
  {
    forgetWritten( held, operand );
    if ( isOneByte( operand ) && !isVolatile( operand ) && mnemonic == "st" + own && !holds( operand ) )
    {
      held.emplace_back( operand );
    }
  }

  // What writes the register otherwise — its own transfer or `tsx`, a `jsr`,
  // a directive or a macro. Its own transfer is taken apart, since it fills
  // the register with what `A` holds, which the caller does not see.
  if ( reg == 'x' ? effect.x : effect.y )
  {
    held.clear();
  }
  return true;
}

/// Keeps `mAHolds` and the carry true for one line about to be written;
/// false where the line is a `lda` of what `A` already holds, or a `clc` or a
/// `sec` the carry already answers.
bool Tracker::noteA( Effect const& effect, std::string_view operand )
{
  std::string_view const mnemonic = effect.mnemonic;

  if ( !noteCarry( effect, operand ) )
  {
    return false;
  }

  // An element read through an index is held too, `t,x`, for as long as
  // the index register stands and nothing writes the array: a `lda` of it
  // again reads the byte `A` holds — see
  // docs/decisions/0149-a-store-to-an-element-stays-in-its-array.md.
  bool const holdable = ( isOneByte( operand ) || indexedBase( operand ).has_value() ) && !isVolatile( operand );
  if ( mnemonic == "lda" )
  {
    if ( holdable && holdsInA( operand ) )
    {
      return false;
    }
    mAHolds.clear();
    if ( holdable )
    {
      mAHolds.emplace_back( operand );
    }
    return true;
  }
  if ( mnemonic == "sta" )
  {
    forgetWritten( mAHolds, operand );
    if ( holdable && !holdsInA( operand ) )
    {
      mAHolds.emplace_back( operand );
    }
    return true;
  }

  if ( effect.memory && !operand.empty() )
  {
    forgetWritten( mAHolds, operand );
  }
  if ( effect.a )
  {
    mAHolds.clear();
  }
  return true;
}

/// Keeps `mCarry` true for one line about to be written; false where the
/// line is a `clc` or a `sec` that would set the carry to what it is.
bool Tracker::noteCarry( Effect const& effect, std::string_view operand )
{
  std::string_view const mnemonic = effect.mnemonic;
  if ( mnemonic == "clc" )
  {
    if ( mCarry == Carry::CLEAR )
    {
      return false;
    }
    mCarry = Carry::CLEAR;
    return true;
  }
  if ( mnemonic == "sec" )
  {
    if ( mCarry == Carry::SET )
    {
      return false;
    }
    mCarry = Carry::SET;
    return true;
  }

  // A branch on the carry says what it is on each way out: taken, it is
  // what the branch tested for; fallen through, it is the other. Every other
  // branch leaves it as it was on both.
  if ( mnemonic == "jcs" || mnemonic == "bcs" )
  {
    mLastBranch = EdgeCarry{ .jumpsTo = blockOf( operand ), .taken = Carry::SET, .fallen = Carry::CLEAR };
    mCarry = Carry::CLEAR;
    return true;
  }
  if ( mnemonic == "jcc" || mnemonic == "bcc" )
  {
    mLastBranch = EdgeCarry{ .jumpsTo = blockOf( operand ), .taken = Carry::CLEAR, .fallen = Carry::SET };
    mCarry = Carry::SET;
    return true;
  }

  if ( effect.carry )
  {
    // An add, a subtract, a compare, a shift, a `jsr`, a directive, a
    // macro: the carry is the machine's.
    mCarry = Carry::UNKNOWN;
  }
  return true;
}

} // namespace nga::c::detail
