# Reference: the C dialect

A `.ngc` Module is C. This chapter lists where it differs from C. It assumes you
know the language and does not repeat what is unchanged.

Name a `.ngc` file in the Project's `modules` block and the tool compiles it
before it assembles it. The text it produces is an ordinary `.asm` Module.

## What is not there

**No preprocessor.** There is no `#include`, no `#define` and no conditional
compilation. `#` and `##` are tokens that no grammar accepts. A Project constant
reaches C through `extern const`, and a program built two ways is two Project
files over a common one.

**No declarations before use.** Every name is visible to the whole program. The
compiler reads what every `.ngc` defines and what every other Module exports
before it checks any body. A prototype is therefore a parse error: write `void
f(void) { ... }` and call `f` from any file.

**No `int`, `char`, `long`, `short`, `signed`, `unsigned`.** The integer types
are `u8`, `i8`, `u16` and `i16`. Writing `int` or `char` is an error that names
the spelling to use instead.

**No floating point.** There are no `float` and `double` types and no floating
constants.

**No `goto` and no labels.** `break`, `continue` and `return` are the only
jumps.

**No `typedef`** except the one form that names a function type. See [Function
types](#function-types).

Also gone: variadic functions, an array of pointers, a pointer to a pointer, `&`
of a whole array, and the comma operator.

Plain assignment is a statement and not a value, so `a = b = 0` is an error. A
compound assignment is a value.

C reserves every keyword of C23 whether or not the subset has the construct, so
a later construct takes no name from your program. A name C reserves for the
implementation, with a leading `__`, or a leading `_` and a capital, is an
error.

## Lexical differences

| Form | Status |
|---|---|
| `42` | decimal |
| `0x2A` | hexadecimal |
| `0b101010` | binary |
| `010` | error. C reads a leading zero as octal. |
| `10u`, `1ull` | error. There are no suffixes. |
| `1.5`, `1e5` | error. There is no floating point. |

`'` separates digits and must stand between two of them: `0xFF'FF` and `1'000`.

A string literal or a character constant may carry a character set name glued to
the opening quote: `screen"HELLO"`, `screen'a'`. See
[Character sets](11-character-sets.md). A literal with no prefix must be ASCII.

An array given a string literal is given its characters **and its terminating
`\0`**. So the Charset must map `\0` as well, or the tool reports that it does
not map a character of the literal.

The escapes are the assembler's six: `\\`, `\"`, `\'`, `\n`, `\t` and `\0`.
There is no `\xNN`.

Adjacent string literals join only where they carry one prefix, and no prefix
counts as one.

Comments are C's. `/*` does not nest. A `//` comment ending in a backslash is an
error, because C would carry it onto the next line.

Source is UTF-8. A comment may hold any character, and invalid UTF-8 there is an
error. Outside a comment, a character beyond ASCII stands only in a literal that
names a Charset.

## Types

| Type | Bytes | Holds |
|---|---|---|
| `u8` | 1 | 0 to 255 |
| `i8` | 1 | -128 to 127 |
| `u16` | 2, low first | 0 to 65535 |
| `i16` | 2, low first | -32768 to 32767 |
| `bool` | 1 | 0 or 1 |
| an `enum` or an `enum struct` | 1 | its enumerators, numbered from 0 |

An enumeration is written `enum struct NAME { A, B, C };` or
`enum NAME { A, B, C };`. The two types behave alike in every way but the
spelling of their enumerators.

| Written | Enumerator reached as |
|---|---|
| `enum struct NAME { A };` | `NAME::A` only |
| `enum NAME { A };` | `A` or `NAME::A` |

An enumerator of a plain `enum` is a name of the file, so one that meets a
global, a function, a type or an enumerator of another `enum` is an error. An
`.asm` Module reads such an enumerator as a Constant of the Module. An
enumerator of an `enum struct` stands in a Namespace of the type's name.

A value of either type is assigned and compared only with a value of the same
type, by its enumerator's order, and is never computed. A pointer may point at
one, and a `switch` may test one. A `switch` over a plain `enum` takes its
labels either way.

A value on an enumerator is a parse error, and so is a type written after the
name.

Nothing but a `bool` casts to a `bool`, and nothing casts to an `enum struct`.

A `struct` or a `union` at file scope is a type of its own. Members lie one
after another with no padding, and a union's all start at offset zero.

`sizeof` takes a type, an object, an element, or what a pointer points at.

### `auto`

A local written `auto` takes the type of the value it is given. The value must
carry its own type. `auto a = 5;` is an error, and `auto a = (u8)5;` says it.
`auto` declares one name with one value, and is no pointer and no array.

## What `static`, `inline` and `volatile` mean here

**`static` at file scope is Module privacy.** A name that is not `static` is
exported and every file sees it. A `static` name is visible in its own file and
nowhere else.

**`static` storage is not zeroed.** `u8 x;` at file scope and a `static` local
are a reservation, and nothing writes it. Such an object holds whatever was in
that byte. After a Transition that is what the Section that lived there left.
Write `u8 x = 0;` for a zero.

C zeroes static storage. This dialect does not, because on this machine the
zeros cost either bytes or time. Zeros in the image cost one byte of file per
byte of object, so a 4 KB buffer costs 4 KB.

Zeros from a loop before `entry` cost more than the loop. The solver places each
Section on its own, so the objects are not one range. A loop therefore needs a
table of their extents. That table travels in the image, and the loop runs again
after every Transition that loads a Section over another Phase's bytes.

**The tool checks a `static` local.** It reports a read that some path from the
function's entry reaches without writing it first:

```c
static u8 count;
count = count + 1;      // error: read before anything writes it
```

The first call reads a byte nobody wrote. Nothing outside the function writes
that byte, so one function's control flow answers the question. Write
`static u8 count = 0;`.

Three cases go unchecked, because the tool cannot see the write: an array, an
object whose address you take, and an object you gave a value.

**`volatile` is exact.** Every read in the source is one read in the output and
every store one store, in order. None is held in a register, none leaves its
loop, a store is not forwarded to a later read, and no byte is changed in place
with `inc` or `asl`.

A byte is `volatile` where you wrote it so, where you reach it through a pointer
to `volatile`, or where it is one of the Target's registers, whatever C
declared.

A parameter is never `volatile`, because only its Proc reaches its byte. A
member is not, because the subset keeps `volatile` to whole objects and to
pointees.

## Reading the assembler

`extern` does not declare a name for the linker. It says **how C reads a name
the assembler exports**. Write it once in the program and every file sees it.

```c
extern const Sprite sprites[];
extern State state;
extern const i8 OFFSET;
extern void drawSprite(Sprite sprite, u8 frame);
```

Without an `extern`, what an assembler Module exports is read by its shape:

| The assembler's | In C |
|---|---|
| the entry of a `.proc` | a function taking what its `.declare arg`s name and returning what its `.declare ret` names |
| a Label over `.res` or `.ztemp`, of 1 byte, of 2, of 3 or more | `u8`, `u16`, `u8[N]` |
| a Label over `.byte` lines, or `.word` lines | `u8[]`, `u16[]` |
| a Constant of literals | a constant, folded |
| any other Constant, such as `label + 2` | a `u16` with a value, no size and no address |
| a `register` or a `region`, of 1 byte, of 2, of more | `u8`, `u16`, `u8[N]` at its address |

A Label with no clear shape is refused: code, `.byte` and `.word` under one
Label, or data and a reservation together. So is a character set, a Window, a
Pane, a macro or a Slot the assembler declared.

`extern RESULT NAME(PARAMETERS);` says how C calls a `.proc`. It must have as
many parameters as the Proc has `.declare arg` lines, and a result exactly where
the Proc has a `.declare ret`. A parameter's name, where you write one, is the
name of the byte its `.declare` names.

`const` on an `extern` keeps C from writing the name.

## Attributes

An attribute is written before a declaration, a function, a block or a `return`,
with no prefix. The dialect has nine.

| Attribute | Stands on | Means |
|---|---|---|
| `[[in(PANE)]]` | a function or a file-scope object | the Proc or Section carries `in PANE` |
| `[[under(PANE)]]` | a function | the Proc is a Trampoline: `.proc NAME, under PANE` |
| `[[with(...)]]` | a block | the block is a macro used once under `.with` |
| `trampoline` | beside `with` | the compiler emits a Trampoline Proc for the block |
| `[[transition(PHASE)]]` | a bare `return;` | writes `.transition PHASE` in place of the `rts` |
| `[[slot]]` | an `extern` | declares a Slot, with the Binding its type gives |
| `[[implements(SLOT)]]` | a function or a file-scope object | writes `.implements SLOT, NAME` |
| `[[striped]]` | an array | a Section per byte of an element |
| `[[placement(...)]]` | a declaration, a block or a function | `zeropage` or `absolute` |

An attribute with a prefix, one given arguments where it takes none, and any of
C23's own are none of the dialect's.

### Panes

See [Panes](06-panes.md).

`[[with(...)]]` has the four forms `.with` has: `with(PANE)`, `with(FAMILY, i)`,
`with(WINDOW, i)` and `with(WINDOW = STATE)`. The index `i` is a `u8` computed
before the block, or a constant, which for a family names the member.

A `[[with]]` block is a macro, so `return`, and `break` or `continue` of a loop
around it, are errors inside it. A loop inside it is its own. A `switch` inside
it is the block's own code.

The address of what the block shows may be kept in a local of the block or
handed to a call in the block. Stored anywhere else it escapes the block, which
is an error.

Code in a Pane cannot switch its own Window. A direct call from a function
`[[in(P)]]` into a function in another Pane of that Window is an error. Write it
in a `[[with(Q), trampoline]]` block, or in an `[[under(P)]]` function.

`trampoline` where none is needed is an error, and so is `with` alone where one
is needed. The form states the cost, so the tool holds it to the place.

A call is wrapped in `.with PANE` where its callee is in a Pane the calling code
is not in. You do not write that.

### Phases

See [Phases](04-phases.md) and [Slots](07-slots.md).

`[[transition(PHASE)]]` stands on a bare `return;` in a `void` function. A
transition never comes back, so a `return` with a value and a function with a
result are errors. So is a transition in a `[[with]]` block, because the block
is a macro.

In a case of a `switch` a transition stands, and ends that case.

`[[slot]] extern` is the one `extern` that names nothing of the assembler's. The
Binding comes from the type:

```c
[[slot]] extern void update(void);      // .slot update, vector
[[slot]] extern u8* const screen;       // .slot screen, pointer, zeropage
```

The pointer is written `const` because the Cell is the Transition routine's to
write.

A Slot to a function may take and return. Its Temporaries stand in a Namespace
of its own name, every Implementation reads them, and an Implementation declares
none of its own.

`[[implements(NAME)]]` stands on a function definition or a file-scope object,
`static` included.

### `[[striped]]`

An array of `u16`, `i16`, or a `struct` of members of one or two bytes,
pointers, and `struct`s of such. At most 256 elements. Each byte of an element
becomes a Section of its own.

An element is indexed by a `u8` or by a constant inside the array. A byte of a
stripe has an address. An element, a member of more than a byte, and the array
itself have none.

### `[[placement(...)]]`

`zeropage` or `absolute`. Its argument is a word of the model rather than a name
you declared.

Every byte the program names takes the class of the innermost `[[placement]]`
over its declaration. The compiler's own scratch follows the function. With
nothing over it a byte is `zeropage` inside a function and `absolute` at file
scope.

A pointer lies in the zero page whatever you ask. A `struct` or a `union` of
more than four bytes lies off it. A `[[striped]]` array settles its own place.
Asking for the class already in force is a warning.

## Function types

A pointer to a function is always a pointer to a named type, and the name comes
from the one `typedef` the dialect has:

```c
typedef u8 chooser(u8 x);
```

The name is a type as a pointer alone, `chooser* pick;`. A value of the type is
an error, because a function type has no bytes. Every parameter carries a name,
because those names are the Temporaries the type's members read.

A function becomes a member of the type its address is taken as. A member reads
its arguments from the type's Temporaries and declares none of its own, so a
direct call writes the type's bytes as a call through a pointer does.

A function's name alone is its address, so `pick = twice` and `pick = &twice`
are one thing.

A function taken as two types is an error. So is an address whose signature is
not the type's, at the line that takes it.

A member may not call its own type, through a pointer of it or by naming another
member. It would write the Temporaries it reads. A member that dispatches writes
the second step through another type, or calls it by name.

There is no array of pointers, so hold handlers in objects.

A `.proc` of the assembler joins a type by saying `.proc play, as chooser`. A
Proc that says nothing is no member.

## Statements and expressions

The statements are C's, less `goto`. Two differences:

`if` and `switch` take an initialiser, as C++17 has it:

```c
if (u8 c = next(); c != 0) { ... }
```

A `switch`'s labels stand directly in its braces. A statement before the first
label is a parse error.

The binary operators bind as C's levels do. `=` binds loosest and to the right.

`(Light)x` is not a cast. A cast takes a keyword type, so a `(` followed by a
type name and `)` is a cast, and any other `(` a parenthesised expression.

A statement that opens with a name and then a name, `const`, or `*` and a name
is a declaration. `Light *p;` declares, where `a * b;` would be an expression
statement that neither assigns nor calls.

The grammar reads these and the compiler does not compile them yet, so each is
an error where it stands: a call of anything but a name, an assignment to
anything but a name, an expression statement that neither assigns nor calls, a
shift by a value that is not a `u8`, a comparison of two constants, and any
operator of a pointer but `+`, `-`, `==` and `!=`.

## What a program may not count on

The compiler is written as if no program did these. A program that does them is
wrong, and what it gets is whatever the output does with it.

**An element's index stays within its array.** The compiler takes a store to an
element of `t` to change `t` and nothing else. An index past the end reaches the
next object without complaint.

**A global nobody assigned holds what was there.** See `static` storage above.
The test machine hands out `$A5` rather than zero so that a program counting on
a zero fails there.

**The bytes of a function are its own.** Its parameters, its result and its
locals are Temporaries the compiler keeps where it chooses: in a register for
the length of a call, in no byte at all where nothing reads them, or in a byte
shared with another Temporary. What a caller writes before the `jsr` and reads
after it is what the `.declare` lines name, and that is all.

An assembler Module that takes the address of any other of them, or reads or
writes one otherwise, is outside what the tool promises. So is a pointer of C
aimed at one by arithmetic from another object.

## Limits

Operators nest at most 64 deep in one expression, and parentheses at most 64
deep. A call, a prefix operator and `=` each count as one level.

Blocks and the statements `if`, `while`, `do` and `for` nest at most 127 deep
together, which is C's own translation limit.

Past any of the three the parser refuses before it descends, once, and skips
what it refused.

## Errors

The lexer reports every error in the file and carries on. Lexical findings are
`NGA70xx`.

A parse error names what was expected and what was found. The parser then skips
to the `;` that ends the statement, or past the `}` of a block, and carries on.

A file that holds an error compiles to nothing and its Module holds no Sections.
It still defines what it parsed, so no other file is told those names are
undeclared.