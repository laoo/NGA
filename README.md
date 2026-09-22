# NGA

A resource-aware 6502 assembler with a compiler
for a subset of C that builds on the same model.


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
