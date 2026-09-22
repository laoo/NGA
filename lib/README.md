# The library

What the tool ships as source rather than compiles in: a storage driver per
mechanism, a decoder per format over any driver's stream, the machine's own
software where it occupies memory a program has to work around, and a machine
variant that names all three. A variant here describes the machine and not one
program's use of it, so it declares what the hardware has whether or not a
given Project writes it. A Project
reaches a file here by naming it in `modules` or `include` as it names one
of its own; a path that is not beside the naming document is looked for
here.

| File | What it is |
|---|---|
| `atari/130xe.ngp` | An Atari 130XE: the four extended Banks as a unit set and the storage, the Window `ext` that shows them over `$4000-$7FFF`, the Window `os` whose states are the ROM and the RAM beneath it, every Region of the address space, the hardware registers by name, and the three Modules below listed as `resident`. A Project gets the machine with `include "atari/130xe.ngp"` and writes nothing else about it. |
| `atari/os.asm` | The Atari OS as a Module: four Sections that hold no bytes, pinned over its zero page, its variables and its two ROM ranges, each `root`, exporting the names a program reaches it by — `RTCLOK`, `SDMCTL`, `SDLSTL`, `SDLSTH`, `CH`, `SETVBV`, `XITVBV`. Listed in `resident` by the variant; see 0051. |
| `atari/portb.asm` | The driver for extended memory behind `PORTB`: it streams through the variant's Window `ext`, shows a state of `ext` and of `os` on request — the PORTB value per state is its own table, trimmed to the variant's `extension` by a macro — and `copy` reads the window directly. Its roles are macros: `open`, `read`, `show` and `showAt` call a procedure each, the OS Window's two are inline. Needs the variant's `register PORTB`, its Windows `ext` and `os`, a unit set `extension` and storage that is it. |
| `stream/rle.asm` | The `rle` decoder over a driver's stream, read through `nga.read`. |
| `stream/zx0.asm` | The `zx0` decoder over a driver's stream, read through `nga.read`. |

Everything here goes into the program a Project builds, so it is licensed
apart from the tool, under the Zero-Clause BSD licence in
[LICENSE](LICENSE): use it without attribution — see
0207.

Every file here is held to what it claims by the test suite of the repository
it is developed in: the Container built with it is loaded into a machine and
every Transition it takes is run on a 6502.
