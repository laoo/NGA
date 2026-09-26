# The NGA tutorial

The chapters, in the order they are meant to be read.

1. [The Project and Modules](01-the-project-and-modules.md) — the one file a
   run is given, and the same program written twice: in the assembler, and in
   the subset of C that compiles to it.
2. [Temporaries and Liveness](02-temporaries-and-liveness.md) — where a
   variable goes, which variables can have the same byte, and who works that
   out.
3. [Targets](03-targets.md) — what the program is going to be, and teaching the
   tool what machine it is for.
4. [Phases](04-phases.md) — a program in parts that take turns in memory: what
   the tool works out from them, what waits on the diskette, and what it
   refuses.
5. [Banks and Windows](05-banks-and-windows.md) — the same program on a machine
   whose storage is part of the address space: what a Window is, what a Bank
   is, and what may not live in one.
6. [Panes](06-panes.md) — a Bank as somewhere for code and data to stay: what
   one switch shows together, who may name it, N Banks of one layout, and the
   other half of the rule.
7. [Slots](07-slots.md) — how resident code reaches what differs from Phase to
   Phase: one name, one live definition at a time, and a cell the Transition
   rewrites.
8. [Generators](08-generators.md) — a Module whose text the tool writes: a file
   as a Section, a script that makes one, and what a script may and may not
   reach.
9. [Macros](09-macros.md) — a body instantiated where it is used: what a
   `.with` can then cover, what a macro is not, and what a body may hold.
10. [Conditionals and packs](10-conditionals.md) — which statements are in the
    program, which value a number has, however many arguments there are, and
    why the first two are different questions.
11. [Character sets](11-character-sets.md) — what a literal means: a picture
    drawn in its own source, the same letters at other numbers, and why an
    unprefixed literal must be ASCII.
12. [Instructions](12-instructions.md) — statements the processor does not
    have: a branch that is two bytes or five, a jump into one of a Proc's own
    positions, a jump whose condition the writer vouches for, what the
    Target's processor adds, and the one rule that decides whether a
    parenthesis indirects.
13. [Roots](13-roots.md) — where the tool stops being able to see: an address
    taken and nobody behind it, who follows one, what saying so by hand costs,
    and the memory only the hardware reaches.
14. [Cartridges](14-cartridges.md) — a Container nothing loads: the same
    program as an image the CPU reads where it lies, what a board is, the ROM a
    Region can be, a bank left switched, and the cold start that brings the
    entry Phase its own bytes.

The last four are reference. Read them when you need a detail, not in order.

15. [The Project file](15-project-file.md) — every block and every option of a
    `.ngp`, and how the tool reads the file.
16. [The assembler](16-assembler.md) — every directive, attribute and operator
    an `.asm` Module may hold. The 6502 instruction set is not here.
17. [The C dialect](17-c-dialect.md) — what a `.ngc` Module is, written as the
    differences from C: what is gone, what changed, and what was added.
18. [Diagnostics](18-diagnostics.md) — every finding the tool can report, by
    the part of the tool that raises it: what the identifier means, what the
    severity costs, and a note where the message cannot say the whole of it.
