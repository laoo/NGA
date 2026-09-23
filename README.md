# NGA

A resource-aware 6502 assembler with a compiler
for a subset of C that builds on the same model.


## The Problem

Writing for an 8-bit machine with a 6502 is easy. The OS services are mapped
and documented, the hardware registers are there to be written to directly, and
you know which range of memory is yours and how much of the zero page you may
use.

It gets hard quickly once the program comes near the limits of the machine.
Placing data in memory by hand becomes a problem of its own, and so does
finding room on the zero page for one more variable.

Banking is the only way to get more memory, and it brings its own trouble.
Every block has to be given a place, and you have to know which blocks are
visible at every moment the program runs. The more blocks there are, the more
of that you carry in your head.

The work is tedious and easy to get wrong, and the mistakes do not show up when
you build. They show up on the machine.

Memory management in a heavily constrained system is not something to leave to
the programmer to do by hand. It is an allocation problem with fixed rules,
which a tool can solve and then check. NGA is that tool.

## The Solution

NGA is a static verifier of memory layout that happens to assemble 6502. It
takes a project describing your program as a graph of execution and works out
which code and data must be present at the same time, and which never are and
can therefore stand at one address. A constraint solver then assigns every
address, bank and overlay at once. Errors that normally surface as a crash on
real hardware — a phase that does not fit in memory, a reference to data that
is not there when the code runs, a call into a bank that is not switched in —
are reported at build time instead.

## Building

Requires CMake 3.25+, Ninja and a C++23 compiler (Apple clang 17+, GCC 13+, or
MSVC 19.38+).

```sh
cmake --preset debug
cmake --build --preset debug
```

Presets: `debug`, `release`, `asan` (AddressSanitizer + UBSan, not on Windows),
and `windows` (Visual Studio, Windows only).

The first `cmake --preset` downloads about 50 MB: OR-Tools, which NGA links for
its layout solver, prebuilt as one static archive per platform by
[nga-deps](https://github.com/laoo/nga-deps) and pinned by hash in
`cmake/OrTools.cmake`. It is downloaded once per build directory and nothing is
compiled from it. Without a network, unpack the archive yourself and configure
with `-DNGA_ORTOOLS_PREFIX=/path/to/or-tools-<version>-<platform>`.

The archives exist for Linux x86_64 (libstdc++), macOS arm64 and Windows x64
(MSVC); on anything else the configure step says so and stops.

## Licence

The tool is under the MIT licence, [LICENSE](LICENSE). What it puts into a
program it builds — the files under [`lib/`](lib/), and the code and text it
writes itself — is yours without attribution, under the Zero-Clause BSD licence
of [`lib/LICENSE`](lib/LICENSE); the licence file says so in its last
paragraph. What a binary is built from beyond NGA's own source, and the
notices a binary release carries, are in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Documentation

[The tutorial](tutorial/README.md) is the documentation: what this tool does
that another 6502 assembler does not, a chapter at a time, from the first
program to the whole memory model, every one of them with programs this
repository builds.
