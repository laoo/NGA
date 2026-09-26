Notes for the seventeenth chapter, one per identifier, joined into it by the
`catalogue` marker. A diagnostic without a note shows its message template
alone, which is what keeps the chapter from waiting on prose nobody has
written. Add a note when the message cannot say the whole of it.

The heading is the identifier and nothing else. Everything under it until the
next heading is the note.

## NGA0113

A label in column one that is spelled like an instruction is almost always an
instruction that lost its indentation. The tool cannot tell the two apart,
because a label is marked by position and nothing else.

Turn the warning off where a program means it.

## NGA0138

A directive that produces bytes needs somewhere to put them. `.section` and
`.proc` are the two places that have an address.

## NGA0141

The processor has `(zp,x)` and `(zp),y` and no other indirect mode. So
`(table),x` is not indirect, and the parentheses group an expression instead.
The operand that comes out is an ordinary address, and the instruction is three
bytes where the indirect form is two.

The warning is there because the two spellings are one character apart.

## NGA0158

A macro body holds instructions and data. It does not hold declarations. A
macro is instantiated where it is used, so a declaration inside one would
declare the same name again at every use.

## NGA0169

A conditional branch holds instructions and data, for the same reason a macro
body does.

## NGA1212

The text after the colon comes from the generator, not from the tool. A script
that is given something it cannot use says so itself, and this is how it
reaches the reader.

## NGA2237

A literal without a prefix must be ASCII. Nothing says what a byte above 127
would mean, because the machine's own character set is not Unicode and not
ASCII either. Name a Charset and the question has an answer.

## NGA2252

A Slot has one live implementation in each Phase. Two Modules that implement it
in the same Phase leave the tool nothing to write into the Cell.

## NGA2412

Code that reaches a Symbol in a Pane must run while that Pane is shown. Put the
statement under a `.with`, or put the code in a Proc declared `under` that
Pane.

## NGA2414

A Section is in memory in the Phases it is declared in and in no others. A
reference across that boundary would read whatever the other Phase left there.

This is the refusal that makes Phases safe. The answer is usually a Slot.

## NGA3003

A Window shows one of the states it declares. A name it does not declare names
nothing, and so does the name of the unit set behind it.

## NGA3007

A statement under `.with` runs while the Window shows what the `.with` asked
for. An instruction that changes the Window would end that, and the rest of the
statement would run against a different memory.

## NGA3008

The `.with` shows one Pane. A statement under it that reaches into another one
is reaching into memory that is not there.

## NGA4501

A Phase nothing enters is built, placed and carried in the Container, and never
runs. It is usually a `then` that was renamed or removed.

Raise it to an error in a Project where that must not happen.

## NGA4606

Taking the address of a Section tells the tool nothing about who will follow
it. Say who does. `.own` means this Section does, `.own NAME` means another
one does, and `.root` means the hardware does.

Without that, Trace cannot tell what the program still reaches, and a Section
nothing reaches is dropped.

## NGA4607

A jump through a pointer goes wherever the pointer holds. The tool follows what
the program owns, so a Section that jumps that way has to own the code it can
reach.

## NGA5107

The Target says which processor the program is for. An instruction the
processor does not have cannot be assembled, whatever the machine in front of
you happens to be.

## NGA5205

A Window is where storage arrives. A Section that waits in storage cannot also
live at the address it arrives through, because the arrival would overwrite it
while it is being read.

## NGA5218

A reserved Region belongs to something else, usually the machine or its DOS. A
pin puts a Section there anyway, which the tool allows and reports. Read it as
a question: is this Region really reserved, or is this pin really wanted.

## NGA6102

A branch reaches 128 bytes back and 127 forward. The tool says how far the
target actually is, and names the conditional jump that reaches anywhere. It is
five bytes where a branch is two.
