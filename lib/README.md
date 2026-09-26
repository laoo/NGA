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
| `atari/130xe.ngp` | An Atari 130XE: the four extended Banks as a unit set and the storage, the Window `ext` that shows them over `$4000-$7FFF`, the Window `os` whose states are the ROM and the RAM beneath it, every Region of the address space, `PORTB`, and the three Modules below listed as `resident`. It reserves what a DOS of the 2.x family holds, since it takes no Container but the `.xex` a DOS loads; a Project gets the machine with `include "atari/130xe.ngp"` and declares for itself only what it wants beyond that — a register by name, or a DOS that takes more. |
| `atari/800xl.ngp` | An Atari 800XL with a double-density disk drive: no banking, so storage is the diskette's sectors, 256 to a unit, and there is no unit set and no Window. Every Region of the address space, `PORTB`, and the three Modules below listed as `resident`. Its Containers are the `.atr` it boots and the `.xex` a DOS loads. A Project gets the machine with `include "atari/800xl.ngp"`. |
| `atari/cart.asm` | The driver for a XEGS cartridge's banks: the units are the Banks of the variant's `banks`, which the byte written to `$D5FF` brings into `$8000-$9FFF` — `switched` in the variant — and the stream is a pointer into that window that walks on to the next unit at its end. A state of `switched` **is** a Bank, so this driver holds no table of values where `portb.asm` holds one, and `show` is two instructions: the window has no named state to number before the Banks, and the board wires the byte written straight to the address lines above the window. The register is written and never read, so which bank is in is this driver's own byte. Needs the variant's Window `switched`, a unit set `banks` and storage that is it; `copy` reads the window directly, as PORTB's does. |
| `atari/charsets.asm` | The Atari's two codes for the same letters, as Charsets: `atascii`, what the character I/O takes, with the graphics characters where an ASCII machine keeps its control codes and `\n` mapping to `$9B`; and `screen`, what the display reads out of screen memory. The graphics are written as the Unicode their glyphs are drawn as, inverse video included — the blocks and triangles have complements and the letters have negative squares — so a picture drawn in the source is the picture the machine draws. Emits nothing; both variants list it, so a Project that includes one has both by name. |
| `atari/xegs128.ngp` | An Atari 800XL holding a 128 KB XEGS cartridge: sixteen banks of eight kilobytes, of which the last is the fixed part the CPU always sees at `$A000-$BFFF` and the other fifteen are the storage the byte written to `$D5FF` brings into `$8000-$9FFF`. The window has **no base** — there is no memory under the banks to come back to, so a `.with` switches and leaves it switched (0217) — and its ranges are in no Region at all, so only a Pane or storage stands there. Its Container is the `.car` and nothing else, and a cartridge needs no DOS, so the whole of RAM below `$8000` is the program's, `$0700` included. A Project gets the machine with `include "atari/xegs128.ngp"`. |
| `atari/disk.asm` | The driver for a double-density diskette: a unit of storage is 256 sectors of 256 bytes, so the unit is the sector number's high byte and the offset is the rest, and unit zero begins at sector 4, the first after the boot record. It reads a sector through the OS's own disk handler at `DSKINV`, so it knows no register and no drive; the stream is that sector in a buffer and a position in it, carrying into the next sector at its end. It names no Window, so `stream`, `show` and `showAt` are not declared. Needs storage by count whose `size` is 65536, and the `.atr` Container — see 0211. |
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
