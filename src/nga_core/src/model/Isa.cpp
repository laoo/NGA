#include "nga/model/Isa.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace nga::model
{

namespace
{

// The legal 6502 instruction set, and nothing else: the undocumented opcodes
// are not instructions this assembler will emit by accident.
//
// Ordered by mnemonic, which is how it reads, not how it is searched — the
// invariants that matter are asserted by the tests rather than by the layout:
// every opcode appears once, every mnemonic and mode pair appears once.
constexpr std::array<Instruction, 151> TABLE{ {
    Instruction{ .mnemonic = "adc", .mode = AddressingMode::IMMEDIATE, .opcode = 0x69 },
    Instruction{ .mnemonic = "adc", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x65 },
    Instruction{ .mnemonic = "adc", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x75 },
    Instruction{ .mnemonic = "adc", .mode = AddressingMode::ABSOLUTE, .opcode = 0x6D },
    Instruction{ .mnemonic = "adc", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x7D },
    Instruction{ .mnemonic = "adc", .mode = AddressingMode::ABSOLUTE_Y, .opcode = 0x79 },
    Instruction{ .mnemonic = "adc", .mode = AddressingMode::INDEXED_INDIRECT, .opcode = 0x61 },
    Instruction{ .mnemonic = "adc", .mode = AddressingMode::INDIRECT_INDEXED, .opcode = 0x71 },
    Instruction{ .mnemonic = "and", .mode = AddressingMode::IMMEDIATE, .opcode = 0x29 },
    Instruction{ .mnemonic = "and", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x25 },
    Instruction{ .mnemonic = "and", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x35 },
    Instruction{ .mnemonic = "and", .mode = AddressingMode::ABSOLUTE, .opcode = 0x2D },
    Instruction{ .mnemonic = "and", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x3D },
    Instruction{ .mnemonic = "and", .mode = AddressingMode::ABSOLUTE_Y, .opcode = 0x39 },
    Instruction{ .mnemonic = "and", .mode = AddressingMode::INDEXED_INDIRECT, .opcode = 0x21 },
    Instruction{ .mnemonic = "and", .mode = AddressingMode::INDIRECT_INDEXED, .opcode = 0x31 },
    Instruction{ .mnemonic = "asl", .mode = AddressingMode::IMPLIED, .opcode = 0x0A },
    Instruction{ .mnemonic = "asl", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x06 },
    Instruction{ .mnemonic = "asl", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x16 },
    Instruction{ .mnemonic = "asl", .mode = AddressingMode::ABSOLUTE, .opcode = 0x0E },
    Instruction{ .mnemonic = "asl", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x1E },
    Instruction{ .mnemonic = "bcc", .mode = AddressingMode::RELATIVE, .opcode = 0x90 },
    Instruction{ .mnemonic = "bcs", .mode = AddressingMode::RELATIVE, .opcode = 0xB0 },
    Instruction{ .mnemonic = "beq", .mode = AddressingMode::RELATIVE, .opcode = 0xF0 },
    Instruction{ .mnemonic = "bit", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x24 },
    Instruction{ .mnemonic = "bit", .mode = AddressingMode::ABSOLUTE, .opcode = 0x2C },
    Instruction{ .mnemonic = "bmi", .mode = AddressingMode::RELATIVE, .opcode = 0x30 },
    Instruction{ .mnemonic = "bne", .mode = AddressingMode::RELATIVE, .opcode = 0xD0 },
    Instruction{ .mnemonic = "bpl", .mode = AddressingMode::RELATIVE, .opcode = 0x10 },
    Instruction{ .mnemonic = "brk", .mode = AddressingMode::IMPLIED, .opcode = 0x00 },
    Instruction{ .mnemonic = "bvc", .mode = AddressingMode::RELATIVE, .opcode = 0x50 },
    Instruction{ .mnemonic = "bvs", .mode = AddressingMode::RELATIVE, .opcode = 0x70 },
    Instruction{ .mnemonic = "clc", .mode = AddressingMode::IMPLIED, .opcode = 0x18 },
    Instruction{ .mnemonic = "cld", .mode = AddressingMode::IMPLIED, .opcode = 0xD8 },
    Instruction{ .mnemonic = "cli", .mode = AddressingMode::IMPLIED, .opcode = 0x58 },
    Instruction{ .mnemonic = "clv", .mode = AddressingMode::IMPLIED, .opcode = 0xB8 },
    Instruction{ .mnemonic = "cmp", .mode = AddressingMode::IMMEDIATE, .opcode = 0xC9 },
    Instruction{ .mnemonic = "cmp", .mode = AddressingMode::ZERO_PAGE, .opcode = 0xC5 },
    Instruction{ .mnemonic = "cmp", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0xD5 },
    Instruction{ .mnemonic = "cmp", .mode = AddressingMode::ABSOLUTE, .opcode = 0xCD },
    Instruction{ .mnemonic = "cmp", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0xDD },
    Instruction{ .mnemonic = "cmp", .mode = AddressingMode::ABSOLUTE_Y, .opcode = 0xD9 },
    Instruction{ .mnemonic = "cmp", .mode = AddressingMode::INDEXED_INDIRECT, .opcode = 0xC1 },
    Instruction{ .mnemonic = "cmp", .mode = AddressingMode::INDIRECT_INDEXED, .opcode = 0xD1 },
    Instruction{ .mnemonic = "cpx", .mode = AddressingMode::IMMEDIATE, .opcode = 0xE0 },
    Instruction{ .mnemonic = "cpx", .mode = AddressingMode::ZERO_PAGE, .opcode = 0xE4 },
    Instruction{ .mnemonic = "cpx", .mode = AddressingMode::ABSOLUTE, .opcode = 0xEC },
    Instruction{ .mnemonic = "cpy", .mode = AddressingMode::IMMEDIATE, .opcode = 0xC0 },
    Instruction{ .mnemonic = "cpy", .mode = AddressingMode::ZERO_PAGE, .opcode = 0xC4 },
    Instruction{ .mnemonic = "cpy", .mode = AddressingMode::ABSOLUTE, .opcode = 0xCC },
    Instruction{ .mnemonic = "dec", .mode = AddressingMode::ZERO_PAGE, .opcode = 0xC6 },
    Instruction{ .mnemonic = "dec", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0xD6 },
    Instruction{ .mnemonic = "dec", .mode = AddressingMode::ABSOLUTE, .opcode = 0xCE },
    Instruction{ .mnemonic = "dec", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0xDE },
    Instruction{ .mnemonic = "dex", .mode = AddressingMode::IMPLIED, .opcode = 0xCA },
    Instruction{ .mnemonic = "dey", .mode = AddressingMode::IMPLIED, .opcode = 0x88 },
    Instruction{ .mnemonic = "eor", .mode = AddressingMode::IMMEDIATE, .opcode = 0x49 },
    Instruction{ .mnemonic = "eor", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x45 },
    Instruction{ .mnemonic = "eor", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x55 },
    Instruction{ .mnemonic = "eor", .mode = AddressingMode::ABSOLUTE, .opcode = 0x4D },
    Instruction{ .mnemonic = "eor", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x5D },
    Instruction{ .mnemonic = "eor", .mode = AddressingMode::ABSOLUTE_Y, .opcode = 0x59 },
    Instruction{ .mnemonic = "eor", .mode = AddressingMode::INDEXED_INDIRECT, .opcode = 0x41 },
    Instruction{ .mnemonic = "eor", .mode = AddressingMode::INDIRECT_INDEXED, .opcode = 0x51 },
    Instruction{ .mnemonic = "inc", .mode = AddressingMode::ZERO_PAGE, .opcode = 0xE6 },
    Instruction{ .mnemonic = "inc", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0xF6 },
    Instruction{ .mnemonic = "inc", .mode = AddressingMode::ABSOLUTE, .opcode = 0xEE },
    Instruction{ .mnemonic = "inc", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0xFE },
    Instruction{ .mnemonic = "inx", .mode = AddressingMode::IMPLIED, .opcode = 0xE8 },
    Instruction{ .mnemonic = "iny", .mode = AddressingMode::IMPLIED, .opcode = 0xC8 },
    Instruction{ .mnemonic = "jmp", .mode = AddressingMode::ABSOLUTE, .opcode = 0x4C },
    Instruction{ .mnemonic = "jmp", .mode = AddressingMode::INDIRECT, .opcode = 0x6C },
    Instruction{ .mnemonic = "jsr", .mode = AddressingMode::ABSOLUTE, .opcode = 0x20 },
    Instruction{ .mnemonic = "lda", .mode = AddressingMode::IMMEDIATE, .opcode = 0xA9 },
    Instruction{ .mnemonic = "lda", .mode = AddressingMode::ZERO_PAGE, .opcode = 0xA5 },
    Instruction{ .mnemonic = "lda", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0xB5 },
    Instruction{ .mnemonic = "lda", .mode = AddressingMode::ABSOLUTE, .opcode = 0xAD },
    Instruction{ .mnemonic = "lda", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0xBD },
    Instruction{ .mnemonic = "lda", .mode = AddressingMode::ABSOLUTE_Y, .opcode = 0xB9 },
    Instruction{ .mnemonic = "lda", .mode = AddressingMode::INDEXED_INDIRECT, .opcode = 0xA1 },
    Instruction{ .mnemonic = "lda", .mode = AddressingMode::INDIRECT_INDEXED, .opcode = 0xB1 },
    Instruction{ .mnemonic = "ldx", .mode = AddressingMode::IMMEDIATE, .opcode = 0xA2 },
    Instruction{ .mnemonic = "ldx", .mode = AddressingMode::ZERO_PAGE, .opcode = 0xA6 },
    Instruction{ .mnemonic = "ldx", .mode = AddressingMode::ZERO_PAGE_Y, .opcode = 0xB6 },
    Instruction{ .mnemonic = "ldx", .mode = AddressingMode::ABSOLUTE, .opcode = 0xAE },
    Instruction{ .mnemonic = "ldx", .mode = AddressingMode::ABSOLUTE_Y, .opcode = 0xBE },
    Instruction{ .mnemonic = "ldy", .mode = AddressingMode::IMMEDIATE, .opcode = 0xA0 },
    Instruction{ .mnemonic = "ldy", .mode = AddressingMode::ZERO_PAGE, .opcode = 0xA4 },
    Instruction{ .mnemonic = "ldy", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0xB4 },
    Instruction{ .mnemonic = "ldy", .mode = AddressingMode::ABSOLUTE, .opcode = 0xAC },
    Instruction{ .mnemonic = "ldy", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0xBC },
    Instruction{ .mnemonic = "lsr", .mode = AddressingMode::IMPLIED, .opcode = 0x4A },
    Instruction{ .mnemonic = "lsr", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x46 },
    Instruction{ .mnemonic = "lsr", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x56 },
    Instruction{ .mnemonic = "lsr", .mode = AddressingMode::ABSOLUTE, .opcode = 0x4E },
    Instruction{ .mnemonic = "lsr", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x5E },
    Instruction{ .mnemonic = "nop", .mode = AddressingMode::IMPLIED, .opcode = 0xEA },
    Instruction{ .mnemonic = "ora", .mode = AddressingMode::IMMEDIATE, .opcode = 0x09 },
    Instruction{ .mnemonic = "ora", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x05 },
    Instruction{ .mnemonic = "ora", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x15 },
    Instruction{ .mnemonic = "ora", .mode = AddressingMode::ABSOLUTE, .opcode = 0x0D },
    Instruction{ .mnemonic = "ora", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x1D },
    Instruction{ .mnemonic = "ora", .mode = AddressingMode::ABSOLUTE_Y, .opcode = 0x19 },
    Instruction{ .mnemonic = "ora", .mode = AddressingMode::INDEXED_INDIRECT, .opcode = 0x01 },
    Instruction{ .mnemonic = "ora", .mode = AddressingMode::INDIRECT_INDEXED, .opcode = 0x11 },
    Instruction{ .mnemonic = "pha", .mode = AddressingMode::IMPLIED, .opcode = 0x48 },
    Instruction{ .mnemonic = "php", .mode = AddressingMode::IMPLIED, .opcode = 0x08 },
    Instruction{ .mnemonic = "pla", .mode = AddressingMode::IMPLIED, .opcode = 0x68 },
    Instruction{ .mnemonic = "plp", .mode = AddressingMode::IMPLIED, .opcode = 0x28 },
    Instruction{ .mnemonic = "rol", .mode = AddressingMode::IMPLIED, .opcode = 0x2A },
    Instruction{ .mnemonic = "rol", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x26 },
    Instruction{ .mnemonic = "rol", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x36 },
    Instruction{ .mnemonic = "rol", .mode = AddressingMode::ABSOLUTE, .opcode = 0x2E },
    Instruction{ .mnemonic = "rol", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x3E },
    Instruction{ .mnemonic = "ror", .mode = AddressingMode::IMPLIED, .opcode = 0x6A },
    Instruction{ .mnemonic = "ror", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x66 },
    Instruction{ .mnemonic = "ror", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x76 },
    Instruction{ .mnemonic = "ror", .mode = AddressingMode::ABSOLUTE, .opcode = 0x6E },
    Instruction{ .mnemonic = "ror", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x7E },
    Instruction{ .mnemonic = "rti", .mode = AddressingMode::IMPLIED, .opcode = 0x40 },
    Instruction{ .mnemonic = "rts", .mode = AddressingMode::IMPLIED, .opcode = 0x60 },
    Instruction{ .mnemonic = "sbc", .mode = AddressingMode::IMMEDIATE, .opcode = 0xE9 },
    Instruction{ .mnemonic = "sbc", .mode = AddressingMode::ZERO_PAGE, .opcode = 0xE5 },
    Instruction{ .mnemonic = "sbc", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0xF5 },
    Instruction{ .mnemonic = "sbc", .mode = AddressingMode::ABSOLUTE, .opcode = 0xED },
    Instruction{ .mnemonic = "sbc", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0xFD },
    Instruction{ .mnemonic = "sbc", .mode = AddressingMode::ABSOLUTE_Y, .opcode = 0xF9 },
    Instruction{ .mnemonic = "sbc", .mode = AddressingMode::INDEXED_INDIRECT, .opcode = 0xE1 },
    Instruction{ .mnemonic = "sbc", .mode = AddressingMode::INDIRECT_INDEXED, .opcode = 0xF1 },
    Instruction{ .mnemonic = "sec", .mode = AddressingMode::IMPLIED, .opcode = 0x38 },
    Instruction{ .mnemonic = "sed", .mode = AddressingMode::IMPLIED, .opcode = 0xF8 },
    Instruction{ .mnemonic = "sei", .mode = AddressingMode::IMPLIED, .opcode = 0x78 },
    Instruction{ .mnemonic = "sta", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x85 },
    Instruction{ .mnemonic = "sta", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x95 },
    Instruction{ .mnemonic = "sta", .mode = AddressingMode::ABSOLUTE, .opcode = 0x8D },
    Instruction{ .mnemonic = "sta", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x9D },
    Instruction{ .mnemonic = "sta", .mode = AddressingMode::ABSOLUTE_Y, .opcode = 0x99 },
    Instruction{ .mnemonic = "sta", .mode = AddressingMode::INDEXED_INDIRECT, .opcode = 0x81 },
    Instruction{ .mnemonic = "sta", .mode = AddressingMode::INDIRECT_INDEXED, .opcode = 0x91 },
    Instruction{ .mnemonic = "stx", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x86 },
    Instruction{ .mnemonic = "stx", .mode = AddressingMode::ZERO_PAGE_Y, .opcode = 0x96 },
    Instruction{ .mnemonic = "stx", .mode = AddressingMode::ABSOLUTE, .opcode = 0x8E },
    Instruction{ .mnemonic = "sty", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x84 },
    Instruction{ .mnemonic = "sty", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x94 },
    Instruction{ .mnemonic = "sty", .mode = AddressingMode::ABSOLUTE, .opcode = 0x8C },
    Instruction{ .mnemonic = "tax", .mode = AddressingMode::IMPLIED, .opcode = 0xAA },
    Instruction{ .mnemonic = "tay", .mode = AddressingMode::IMPLIED, .opcode = 0xA8 },
    Instruction{ .mnemonic = "tsx", .mode = AddressingMode::IMPLIED, .opcode = 0xBA },
    Instruction{ .mnemonic = "txa", .mode = AddressingMode::IMPLIED, .opcode = 0x8A },
    Instruction{ .mnemonic = "txs", .mode = AddressingMode::IMPLIED, .opcode = 0x9A },
    Instruction{ .mnemonic = "tya", .mode = AddressingMode::IMPLIED, .opcode = 0x98 },
} };

/// A Jcc, by the Bcc it is written as where the distance fits one and the Bcc
/// it jumps over a `jmp` with where it does not. Named by mnemonic, so that
/// the table above stays the one place an opcode is written.

/// What the 65SC02 adds to the 6502: the `(zp)` modes, the stack's index
/// registers, `stz`, `trb`/`tsb`, `inc`/`dec` of `A`, `bit` in three more
/// modes, `bra`, and an indexed indirect jump. Not the Rockwell bit
/// instructions, which the 65SC02 does not have either — see
/// docs/decisions/0182-the-target-names-its-processor.md.
constexpr std::array<Instruction, 27> EXTRA65SC02{ {
    Instruction{ .mnemonic = "ora", .mode = AddressingMode::INDIRECT_ZERO_PAGE, .opcode = 0x12 },
    Instruction{ .mnemonic = "and", .mode = AddressingMode::INDIRECT_ZERO_PAGE, .opcode = 0x32 },
    Instruction{ .mnemonic = "eor", .mode = AddressingMode::INDIRECT_ZERO_PAGE, .opcode = 0x52 },
    Instruction{ .mnemonic = "adc", .mode = AddressingMode::INDIRECT_ZERO_PAGE, .opcode = 0x72 },
    Instruction{ .mnemonic = "sta", .mode = AddressingMode::INDIRECT_ZERO_PAGE, .opcode = 0x92 },
    Instruction{ .mnemonic = "lda", .mode = AddressingMode::INDIRECT_ZERO_PAGE, .opcode = 0xB2 },
    Instruction{ .mnemonic = "cmp", .mode = AddressingMode::INDIRECT_ZERO_PAGE, .opcode = 0xD2 },
    Instruction{ .mnemonic = "sbc", .mode = AddressingMode::INDIRECT_ZERO_PAGE, .opcode = 0xF2 },
    Instruction{ .mnemonic = "bra", .mode = AddressingMode::RELATIVE, .opcode = 0x80 },
    Instruction{ .mnemonic = "phx", .mode = AddressingMode::IMPLIED, .opcode = 0xDA },
    Instruction{ .mnemonic = "plx", .mode = AddressingMode::IMPLIED, .opcode = 0xFA },
    Instruction{ .mnemonic = "phy", .mode = AddressingMode::IMPLIED, .opcode = 0x5A },
    Instruction{ .mnemonic = "ply", .mode = AddressingMode::IMPLIED, .opcode = 0x7A },
    Instruction{ .mnemonic = "stz", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x64 },
    Instruction{ .mnemonic = "stz", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x74 },
    Instruction{ .mnemonic = "stz", .mode = AddressingMode::ABSOLUTE, .opcode = 0x9C },
    Instruction{ .mnemonic = "stz", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x9E },
    Instruction{ .mnemonic = "trb", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x14 },
    Instruction{ .mnemonic = "trb", .mode = AddressingMode::ABSOLUTE, .opcode = 0x1C },
    Instruction{ .mnemonic = "tsb", .mode = AddressingMode::ZERO_PAGE, .opcode = 0x04 },
    Instruction{ .mnemonic = "tsb", .mode = AddressingMode::ABSOLUTE, .opcode = 0x0C },
    Instruction{ .mnemonic = "inc", .mode = AddressingMode::IMPLIED, .opcode = 0x1A },
    Instruction{ .mnemonic = "dec", .mode = AddressingMode::IMPLIED, .opcode = 0x3A },
    Instruction{ .mnemonic = "bit", .mode = AddressingMode::IMMEDIATE, .opcode = 0x89 },
    Instruction{ .mnemonic = "bit", .mode = AddressingMode::ZERO_PAGE_X, .opcode = 0x34 },
    Instruction{ .mnemonic = "bit", .mode = AddressingMode::ABSOLUTE_X, .opcode = 0x3C },
    Instruction{ .mnemonic = "jmp", .mode = AddressingMode::ABSOLUTE_INDEXED_INDIRECT, .opcode = 0x7C },
} };

struct Jcc
{
  std::string_view mnemonic;
  std::string_view branch;
  std::string_view opposite;
};

constexpr std::array<Jcc, 8> JCCS{ {
    Jcc{ .mnemonic = "jcc", .branch = "bcc", .opposite = "bcs" },
    Jcc{ .mnemonic = "jcs", .branch = "bcs", .opposite = "bcc" },
    Jcc{ .mnemonic = "jeq", .branch = "beq", .opposite = "bne" },
    Jcc{ .mnemonic = "jmi", .branch = "bmi", .opposite = "bpl" },
    Jcc{ .mnemonic = "jne", .branch = "bne", .opposite = "beq" },
    Jcc{ .mnemonic = "jpl", .branch = "bpl", .opposite = "bmi" },
    Jcc{ .mnemonic = "jvc", .branch = "bvc", .opposite = "bvs" },
    Jcc{ .mnemonic = "jvs", .branch = "bvs", .opposite = "bvc" },
} };

/// The certain jumps: a jump the writer knows is taken, written as the branch
/// of the same letters where one reaches. Only the four the compiler can
/// know — what `A` holds with the flags from it, and the carry — because a
/// mnemonic nothing writes is surface nothing tests.
constexpr std::array<std::pair<std::string_view, std::string_view>, 5> CERTAIN{ {
    { "jmpcc", "bcc" },
    { "jmpcs", "bcs" },
    { "jmpeq", "beq" },
    { "jmpne", "bne" },
    // The 65SC02's own, which asks no flag at all: spelled as the Jccs are,
    // the `b` of its branch turned to a `j` — see
    // docs/decisions/0187-a-jump-that-asks-no-flag.md.
    { "jra", "bra" },
} };

std::string_view branchOfCertain( std::string_view mnemonic )
{
  for ( auto const& [name, branch] : CERTAIN )
  {
    if ( name == mnemonic )
    {
      return branch;
    }
  }
  return {};
}

Jcc const* jccOf( std::string_view mnemonic )
{
  for ( Jcc const& entry : JCCS )
  {
    if ( entry.mnemonic == mnemonic )
    {
      return &entry;
    }
  }
  return nullptr;
}

} // namespace

std::string_view nameOf( AddressingMode mode )
{
  switch ( mode )
  {
  case AddressingMode::IMPLIED:
    return "implied";
  case AddressingMode::IMMEDIATE:
    return "immediate";
  case AddressingMode::ZERO_PAGE:
    return "zero page";
  case AddressingMode::ZERO_PAGE_X:
    return "zero page,x";
  case AddressingMode::ZERO_PAGE_Y:
    return "zero page,y";
  case AddressingMode::ABSOLUTE:
    return "absolute";
  case AddressingMode::ABSOLUTE_X:
    return "absolute,x";
  case AddressingMode::ABSOLUTE_Y:
    return "absolute,y";
  case AddressingMode::INDIRECT:
    return "indirect";
  case AddressingMode::INDEXED_INDIRECT:
    return "indexed indirect";
  case AddressingMode::INDIRECT_INDEXED:
    return "indirect,y";
  case AddressingMode::RELATIVE:
    return "relative";
  case AddressingMode::INDIRECT_ZERO_PAGE:
    return "indirect";
  case AddressingMode::ABSOLUTE_INDEXED_INDIRECT:
    return "indexed indirect";
  case AddressingMode::BRANCH_OVER_JUMP:
    return "branch over jump";
  }
  return "implied";
}

std::uint32_t sizeOf( AddressingMode mode )
{
  switch ( mode )
  {
  case AddressingMode::IMPLIED:
    return 1;
  case AddressingMode::IMMEDIATE:
  case AddressingMode::ZERO_PAGE:
  case AddressingMode::ZERO_PAGE_X:
  case AddressingMode::ZERO_PAGE_Y:
  case AddressingMode::INDEXED_INDIRECT:
  case AddressingMode::INDIRECT_INDEXED:
  case AddressingMode::RELATIVE:
  case AddressingMode::INDIRECT_ZERO_PAGE:
    return 2;
  case AddressingMode::ABSOLUTE:
  case AddressingMode::ABSOLUTE_X:
  case AddressingMode::ABSOLUTE_Y:
  case AddressingMode::INDIRECT:
  case AddressingMode::ABSOLUTE_INDEXED_INDIRECT:
    return 3;
  case AddressingMode::BRANCH_OVER_JUMP:
    return 5;
  }
  return 1;
}

std::span<Instruction const> instructionTable()
{
  return TABLE;
}

bool isMnemonic( std::string_view mnemonic )
{
  auto const named = [mnemonic]( Instruction const& entry ) { return entry.mnemonic == mnemonic; };
  return isJcc( mnemonic ) || isCertainJump( mnemonic ) || std::ranges::any_of( TABLE, named ) ||
         std::ranges::any_of( EXTRA65SC02, named );
}

bool needs65sc02( std::string_view mnemonic, AddressingMode mode )
{
  // `jra` in its short form is `bra`, which the 6502 has not. Its long form
  // is a `jmp`, which every processor has, so only the branch is refused —
  // and Size starts every certain jump in the branch.
  if ( mnemonic == "jra" )
  {
    return mode == AddressingMode::RELATIVE;
  }
  return std::ranges::any_of( EXTRA65SC02,
                              [mnemonic, mode]( Instruction const& entry )
                              { return entry.mnemonic == mnemonic && entry.mode == mode; } );
}

bool isCertainJump( std::string_view mnemonic )
{
  return !branchOfCertain( mnemonic ).empty();
}

AddressingMode lengthenedModeOf( std::string_view mnemonic )
{
  return isCertainJump( mnemonic ) ? AddressingMode::ABSOLUTE : AddressingMode::BRANCH_OVER_JUMP;
}

bool isJcc( std::string_view mnemonic )
{
  return jccOf( mnemonic ) != nullptr;
}

std::optional<std::string_view> jccFor( std::string_view branch )
{
  for ( Jcc const& entry : JCCS )
  {
    if ( entry.branch == branch )
    {
      return entry.mnemonic;
    }
  }
  return std::nullopt;
}

std::optional<std::uint8_t> opcodeOf( std::string_view mnemonic, AddressingMode mode )
{
  for ( Instruction const& entry : TABLE )
  {
    if ( entry.mnemonic == mnemonic && entry.mode == mode )
    {
      return entry.opcode;
    }
  }
  for ( Instruction const& entry : EXTRA65SC02 )
  {
    if ( entry.mnemonic == mnemonic && entry.mode == mode )
    {
      return entry.opcode;
    }
  }
  if ( Jcc const* const jcc = jccOf( mnemonic ); jcc != nullptr )
  {
    if ( mode == AddressingMode::RELATIVE )
    {
      return opcodeOf( jcc->branch, AddressingMode::RELATIVE );
    }
    if ( mode == AddressingMode::BRANCH_OVER_JUMP )
    {
      return opcodeOf( jcc->opposite, AddressingMode::RELATIVE );
    }
  }
  if ( std::string_view const branch = branchOfCertain( mnemonic ); !branch.empty() )
  {
    if ( mode == AddressingMode::RELATIVE )
    {
      return opcodeOf( branch, AddressingMode::RELATIVE );
    }
    if ( mode == AddressingMode::ABSOLUTE )
    {
      // Both forms go where the operand is, and the long one is a `jmp`
      // rather than a branch over one: the condition is known to hold.
      return opcodeOf( "jmp", AddressingMode::ABSOLUTE );
    }
  }
  return std::nullopt;
}

MemoryAccess accessOf( std::string_view mnemonic )
{
  // A Jcc goes where its operand is, in either form, and so does a certain
  // jump.
  if ( isJcc( mnemonic ) || isCertainJump( mnemonic ) )
  {
    return MemoryAccess::JUMP;
  }

  // Every mnemonic of the table that takes an operand, by what it does with
  // it. `bit` only reads, `cmp` only reads, and the read-modify-write forms
  // are the shifts and the increments on memory.
  constexpr std::array<std::pair<std::string_view, MemoryAccess>, 35> accesses{ {
      { "adc", MemoryAccess::READ },       { "and", MemoryAccess::READ },  { "asl", MemoryAccess::READ_WRITE },
      { "bcc", MemoryAccess::JUMP },       { "bcs", MemoryAccess::JUMP },  { "beq", MemoryAccess::JUMP },
      { "bit", MemoryAccess::READ },       { "bmi", MemoryAccess::JUMP },  { "bne", MemoryAccess::JUMP },
      { "bpl", MemoryAccess::JUMP },       { "bvc", MemoryAccess::JUMP },  { "bvs", MemoryAccess::JUMP },
      { "cmp", MemoryAccess::READ },       { "cpx", MemoryAccess::READ },  { "cpy", MemoryAccess::READ },
      { "dec", MemoryAccess::READ_WRITE }, { "eor", MemoryAccess::READ },  { "inc", MemoryAccess::READ_WRITE },
      { "bra", MemoryAccess::JUMP },       { "stz", MemoryAccess::WRITE }, { "trb", MemoryAccess::READ_WRITE },
      { "tsb", MemoryAccess::READ_WRITE }, { "jmp", MemoryAccess::JUMP },  { "jsr", MemoryAccess::CALL },
      { "lda", MemoryAccess::READ },       { "ldx", MemoryAccess::READ },  { "ldy", MemoryAccess::READ },
      { "lsr", MemoryAccess::READ_WRITE }, { "ora", MemoryAccess::READ },  { "rol", MemoryAccess::READ_WRITE },
      { "ror", MemoryAccess::READ_WRITE }, { "sbc", MemoryAccess::READ },  { "sta", MemoryAccess::WRITE },
      { "stx", MemoryAccess::WRITE },      { "sty", MemoryAccess::WRITE },
  } };
  for ( auto const& [name, access] : accesses )
  {
    if ( name == mnemonic )
    {
      return access;
    }
  }
  return MemoryAccess::NONE;
}

ControlFlow flowOf( std::string_view mnemonic )
{
  // A Jcc may go to its operand or on, whichever form it takes. A certain
  // jump only ever goes to its operand, whichever form it takes.
  if ( isJcc( mnemonic ) )
  {
    return ControlFlow::BRANCH;
  }
  if ( isCertainJump( mnemonic ) )
  {
    return ControlFlow::JUMP;
  }

  // The eight branches, the two transfers and the two returns; everything
  // else in the table goes on to the next instruction.
  constexpr std::array<std::pair<std::string_view, ControlFlow>, 13> flows{ {
      { "bcc", ControlFlow::BRANCH },
      { "bcs", ControlFlow::BRANCH },
      { "beq", ControlFlow::BRANCH },
      { "bmi", ControlFlow::BRANCH },
      { "bne", ControlFlow::BRANCH },
      { "bpl", ControlFlow::BRANCH },
      { "bvc", ControlFlow::BRANCH },
      { "bvs", ControlFlow::BRANCH },
      // `bra` never falls through, so it is a jump and not a branch, however
      // its operand is written.
      { "bra", ControlFlow::JUMP },
      { "jmp", ControlFlow::JUMP },
      { "jsr", ControlFlow::CALL },
      { "rti", ControlFlow::RETURN },
      { "rts", ControlFlow::RETURN },
  } };
  for ( auto const& [name, flow] : flows )
  {
    if ( name == mnemonic )
    {
      return flow;
    }
  }
  return ControlFlow::NEXT;
}

std::optional<AddressingMode> modeFor( std::string_view mnemonic, syntax::OperandShape shape, PlacementClass placement )
{
  auto const has = [mnemonic]( AddressingMode mode ) { return opcodeOf( mnemonic, mode ).has_value(); };

  // Zero page where it was declared zero page and the instruction has that
  // form; absolute otherwise. Widening is always correct — an absolute
  // instruction reaches a zero-page address — and narrowing never is.
  auto const direct = [&has, placement]( AddressingMode zeroPage, AddressingMode absolute )
  { return placement == PlacementClass::ZEROPAGE && has( zeroPage ) ? zeroPage : absolute; };

  AddressingMode mode = AddressingMode::IMPLIED;
  switch ( shape )
  {
  case syntax::OperandShape::NONE:
    mode = AddressingMode::IMPLIED;
    break;
  case syntax::OperandShape::IMMEDIATE:
    mode = AddressingMode::IMMEDIATE;
    break;
  case syntax::OperandShape::DIRECT:
    // A branch is the one instruction whose operand is a distance rather than
    // an address. A Jcc and a certain jump have a relative form and a longer
    // one, and both start in the relative: Size lengthens what does not
    // reach, and never the other way about.
    mode = has( AddressingMode::RELATIVE ) ? AddressingMode::RELATIVE
                                           : direct( AddressingMode::ZERO_PAGE, AddressingMode::ABSOLUTE );
    break;
  case syntax::OperandShape::DIRECT_X:
    mode = direct( AddressingMode::ZERO_PAGE_X, AddressingMode::ABSOLUTE_X );
    break;
  case syntax::OperandShape::DIRECT_Y:
    mode = direct( AddressingMode::ZERO_PAGE_Y, AddressingMode::ABSOLUTE_Y );
    break;
  case syntax::OperandShape::INDIRECT:
    // `(abs)` is `jmp`'s alone and `(zp)` is every other instruction's, so
    // the mnemonic settles it and no placement is consulted: the 6502 has
    // neither of the latter, which is what the processor check is for.
    mode = has( AddressingMode::INDIRECT ) ? AddressingMode::INDIRECT : AddressingMode::INDIRECT_ZERO_PAGE;
    break;
  case syntax::OperandShape::INDIRECT_Y:
    mode = AddressingMode::INDIRECT_INDEXED;
    break;
  case syntax::OperandShape::INDEXED_INDIRECT:
    // `(zp,x)` for what reads through a pointer, `(abs,x)` for `jmp`, which
    // has no zero-page form of it.
    mode = has( AddressingMode::INDEXED_INDIRECT ) ? AddressingMode::INDEXED_INDIRECT
                                                   : AddressingMode::ABSOLUTE_INDEXED_INDIRECT;
    break;
  }

  return has( mode ) ? std::optional{ mode } : std::nullopt;
}

} // namespace nga::model
