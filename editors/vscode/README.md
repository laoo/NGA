# NGA for Visual Studio Code

The NGA 6502 assembler, its C subset and its Project files, in the editor.
Everything the extension shows it learns by running `nga` on the Project a
file belongs to, so what it shows is what the tool knows.

## What it does

- `.ngp` and `.ngc` files open as NGA. An `.asm` file opens as NGA when a
  Project names it, so the extension never takes over a file another
  assembler's extension is meant to read.
- The status bar says which Project builds the file in front of you. That is
  the nearest `.ngp` above the file that names it. A `.ngp` another one
  includes is a part of that Project, not a Project of its own. Where one
  directory holds several Projects that name the file, the status bar says how
  many there are, and clicking it chooses one. The choice is remembered for
  the workspace.
- When you open a file, the extension builds its Project. When you save a
  file, it builds the Project again and shows every finding the build reported
  in the file it is about. When you pause in typing, it builds what your
  buffers hold, saved or not, and the findings follow the text on the screen.
  A newer build cancels an older one that has not finished. A finding's identifier links to its entry in the
  [catalogue](https://laoo.github.io/NGA/diagnostics/). A note that points
  into a file is a link to that place. A finding with no place in any file,
  such as a Phase that does not fit, is shown on the first line of the Project
  file.
- A finding in what a `.ngc` compiled to is shown in the `.ngc`, on the line
  of C it came from. A finding in other text the tool wrote, such as the C
  runtime, is shown in that text.
- Go to Definition and Find All References follow a name to where the last
  build says it is defined and to every place it is written, across files and
  into the text the tool wrote: a `.export` list, a branch of `.if` the build
  left out, and the Project file count as much as an instruction. A local label such as `@loop`,
  `@-loop` or `@+` is followed within its own file, by the rules the tool
  resolves it by. The outline of a file lists what it defines, each Proc
  holding the labels written inside it, and Go to Symbol in Workspace
  searches every name of every Project built so far.
- Hovering over a name says where the last build put it: its address, the
  range and size of its Section, the Phases that hold it, the Pane and state
  that show it, what it waits in and as what, and, for a Temporary, the
  Temporaries that share its bytes. A constant shows its value and a Region
  its range. The same line stands above every Section as a CodeLens, and a
  Section that Prune dropped says so.
- Code that Prune dropped is faded, as an editor fades code nothing
  reaches. A Proc nobody calls fades from its `.proc` to its `.endp`, and a
  block inside it that is still reached keeps its colours. Hovering over
  the faded text says why it was dropped. Nothing of it reaches the list of
  problems.
- The listing shows where each statement landed and the bytes it wrote,
  such as `$2100: A9 01`, read off the Container the build wrote. By default
  it appears when the pointer rests on a statement: faded, in a column of its
  own past the longest line, so the bytes do not run into the code. It goes
  when the cursor moves. Set `nga.listing` to `always` to have it beside
  every statement, or to `off`. A `.xex`, an `.atr` and a `.car` are read
  back that way. Another Container gives no listing rather than a guessed
  one.
- Completion offers what may stand where you type. In an `.asm` that means:
  a directive after its dot; a mnemonic of the Project's processor or a
  macro where a statement starts; a name the operand can reach, from its own
  Module or exported by another; and a local label after `@`. In a `.ngp` it
  means: a file of the shipped library after `include`; a source file in
  `modules`; the Modules and groups a Phase needs; a Phase after `then` or
  `entry`; a Proc after a Phase's `entry`; and a word of the grammar where a
  statement starts. The words come from the tool, so they are the ones it
  knows.
- Rename Symbol changes a name everywhere it is written. Renaming a Proc
  changes the qualified names of the labels inside it too, `helper.inner`
  becoming `assist.inner` wherever it is written that way. A local label is
  renamed with the references that reach it. The extension builds the
  Project first, so the rename works on the text on the screen. It refuses a
  name written outside the workspace, such as one the shipped library
  defines, or in text the tool wrote, and a new name that is not a name or
  is taken already.
- **NGA: Show the memory map of this Project**, also a button above any of
  its files, opens the map beside the code. It shows the graph of the
  Phases, the zero page and memory with the Phases down and the addresses
  across, the pools shaded, and what waits in storage. Clicking zooms in
  until the band is 16 bytes wide. A ⌘ or Ctrl click on a Section opens
  where it is written. The map is drawn again after every build.
- The **NGA Container** view in the Explorer shows the Container the last
  build wrote as a tree: its header and its parts, the segments of a `.xex`,
  the banks and fixed part of a cartridge, the parts of a diskette. Under
  each part are its runs, each with where it lands, its size, what wrote it
  and its first bytes. Clicking a run opens the statement that wrote it.
  With the cursor in a statement, the view selects that statement's run.
- **NGA: Show what this C compiled to**, also a button above any `.ngc`,
  opens the assembler the C became beside it. With the cursor on a line of
  C, the assembler it became is highlighted, in every place it became code.
  With the cursor in the assembler, the line of C it came from is
  highlighted. Both follow every build.
- **NGA: Open the text the tool wrote for this Project** lists what the last
  build wrote rather than read: what each `.ngc` compiled to, the runtime, a
  Generator's Module, the Transition routine. Each opens read-only, coloured
  like any other assembler, and is read again when the Project is built.
- The colours come from the tool, which classifies every token with the same
  parser that builds the file, so a directive, a mnemonic, a macro and a label
  each look like what they are, and a colour follows your typing at once. A
  name is coloured by what the last build says it means: the name of a Proc as
  a function, a Temporary as a variable, a constant as a read-only variable, a
  Region as a static one, and a label inside a Proc as a label. Directives are
  a token type of their own, `directive`, which a theme may colour as it likes
  and which otherwise looks like a macro.

## Settings

`nga.path` is the `nga` to run. When it is empty, the extension runs the `nga`
it carries, and where it carries none, the one on `PATH`.

## Building it

```sh
cd editors/vscode
npm ci
npm run check    # types, style, names, and the server's tests
```

The server's tests run against `build/debug` of this repository, or against
the `nga` that `NGA_BINARY` names.

A release builds a VSIX for each platform it builds a binary for: Linux x64,
macOS arm64 and Windows x64. Each carries that platform's `nga` and its
library in `bin/`, which is where the extension looks first, so the extension
and the tool it runs always come from the same release.
