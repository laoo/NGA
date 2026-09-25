# Diagnostics

Every finding the tool reports carries an identifier. It is `NGA` and four
digits, and it never changes meaning. This chapter lists all of them.

Read it when the tool said something you want to look up. Do not read it in
order.

## How to read a finding

```
error[NGA5205]: `level.map` is loaded from storage, and cannot live where the
                stream reads through, at $4000
 --> main.ngp:12:3
```

The identifier comes first. The message follows, with the names of the program
written into it. The location points at the line that caused the finding, and a
finding about the whole program has no location.

The number says which part of the tool raised it, so the number alone says
where to look. The tables below are grouped that way.

A number is never reused. A diagnostic that is retired takes its number with
it.

## Severity

An error stops the build at the next gate. A warning does not stop anything. A
note never stands alone and always explains the finding above it.

The severity in the tables is the default. A Project changes it in its
[`diagnostics` block](14-project-file.md#diagnostics), and the command line
changes it with `--deny`, `--allow` and `--off`. The command line wins.

## What the message shows here

The tables show the message as the tool holds it, with named arguments in
braces. A real finding has values there instead. So `` `{section}` is pinned at
{address:hex} `` is printed as `` `hud` is pinned at $2000 ``.

Some findings have more to say than the message has room for. Those carry a
note below the tables, and their identifier is a link to it.

<!-- catalogue 17-diagnostics/notes.md -->
### Source lexing

| Code | Severity | Message |
|---|---|---|
| `NGA0001` | error | unexpected character `{character}` |
| `NGA0002` | error | unterminated string literal |
| `NGA0003` | error | string literal is not closed before the end of the line |
| `NGA0004` | error | unterminated character literal |
| `NGA0005` | error | character literal is empty |
| `NGA0006` | error | character literal holds more than one character |
| `NGA0007` | error | unknown escape sequence `\{escape}` |
| `NGA0009` | error | invalid UTF-8 encoding |
| `NGA0010` | error | `{prefix}` is not followed by any digits |
| `NGA0011` | error | `{character}` is not valid in a {base} literal |
| `NGA0012` | error | `_` in a numeric literal must stand between digits |
| `NGA0013` | error | hexadecimal literals are written `$1F`, not `0x1F` |
| `NGA0014` | error | numeric local labels do not exist; use `@`, `@+`, `@-`, or a name |
| `NGA0015` | error | control character {code:hex} in source |
| `NGA0016` | error | this number is too large to be represented |

### Source parsing

| Code | Severity | Message |
|---|---|---|
| `NGA0100` | error | expected an expression |
| `NGA0101` | error | expected `)` to close this parenthesis |
| `NGA0102` | error | `<<` is a shift; the low byte of an operand is `<` |
| `NGA0103` | error | `{outer}` and `{inner}` need parentheses to say which one binds first |
| `NGA0104` | error | this expression nests more than {limit} deep |
| `NGA0110` | error | `{token}` cannot begin a statement |
| `NGA0111` | error | unexpected `{token}` after the end of the statement |
| `NGA0112` | error | `{name}` at column one defines a label; indent it to write an instruction |
| [`NGA0113`](#nga0113) | warning | `{name}` at column one defines a label named like an instruction |
| `NGA0114` | error | a direction marks a reference, not a definition |
| `NGA0115` | error | a local label has no scope outside a `.proc` |
| `NGA0116` | error | `.declare` says what a byte of a proc is, and this stands outside one |
| `NGA0117` | error | `.declare` says what the `.ztemp` or `.temp` below it is, and this is not one |
| `NGA0118` | error | `.declare` says `arg` or `ret`, found `{token}` |
| `NGA0119` | error | `{name}` is not a type or a place; a declared byte is `u8`, `i8`, `u16`, `i16`, `bool` or `u8[N]`, or lies in `a`, `x`, `y` or `m`, a letter a byte from the low one |
| `NGA0120` | error | unknown directive `.{name}` |
| `NGA0121` | error | `.end` does not say what it closes; write `.ends` or `.endp` |
| `NGA0122` | error | expected a name after `{after}` |
| `NGA0123` | error | `{name}` is not a section attribute |
| `NGA0124` | error | this `.section` is never closed |
| `NGA0125` | error | this `.proc` is never closed |
| `NGA0126` | error | `{directive}` without an open `.section` |
| `NGA0127` | error | `{directive}` without an open `.proc` |
| `NGA0128` | error | a `.section` cannot open inside another; close the first one |
| `NGA0129` | error | a `.proc` cannot open inside another |
| `NGA0130` | error | this `.proc` already declares its result, and a proc has one |
| `NGA0131` | error | `{name}` gives a placement this `.section` already has |
| `NGA0132` | error | `{name}` is already given for this placement |
| `NGA0133` | error | `{name}` is already given for this `.section` |
| `NGA0134` | error | a slot is `pointer` or `vector`, and this is {name} |
| `NGA0135` | error | a slot's placement is `zeropage` or `absolute`, and this is {name} |
| `NGA0136` | error | `.ztemp` and `.temp` declare a variable and need its name at column one |
| `NGA0137` | error | a `.proc` is a section of its own and cannot open inside a `.section` |
| [`NGA0138`](#nga0138) | error | `{what}` stands in no section; what has an address is written in a `.section` or a `.proc` |
| `NGA0139` | error | this byte is already an argument, and a byte is one argument or none |
| `NGA0140` | error | there is no `(expression,{register})` addressing mode |
| [`NGA0141`](#nga0141) | warning | these parentheses group rather than indirect; `(expression,x)` is the indirect form |
| `NGA0142` | error | expected `x` or `y` after `,`, found `{token}` |
| `NGA0143` | error | `{code}` names no diagnostic |
| `NGA0144` | warning | `.off {code}` silenced nothing: no such warning stood on the line below |
| `NGA0150` | error | this `.charset` is never closed |
| `NGA0151` | error | `{directive}` without an open `.charset` |
| `NGA0152` | error | expected a character set entry, written `"..." = value` |
| `NGA0153` | error | a character set entry maps at least one character |
| `NGA0154` | error | the left side of an entry names no character set; the set is the one being declared |
| `NGA0155` | error | `.macro` cannot open inside a `.section`, a `.proc` or another `.macro` |
| `NGA0156` | error | this `.macro` is never closed |
| `NGA0157` | error | `{directive}` without an open `.macro` |
| [`NGA0158`](#nga0158) | error | {what} cannot stand in a macro body, which holds instructions and data |
| `NGA0160` | error | `.namespace` cannot open inside a `.section` or a `.macro` |
| `NGA0161` | error | this `.namespace` is never closed |
| `NGA0162` | error | `{directive}` without an open `.namespace` |
| `NGA0163` | error | `{directive}` closes a namespace while a `.section`, `.proc` or `.macro` is still open |
| `NGA0164` | error | a `.charset` stands at the top level: a literal's prefix is one name |
| `NGA0165` | error | expected `:` here, and the answer for a false condition after it |
| `NGA0166` | error | this `.if` is never closed |
| `NGA0167` | error | `{directive}` without an open `.if` |
| `NGA0168` | error | `.elsif` cannot follow `.else`, which is the branch taken when none is |
| [`NGA0169`](#nga0169) | error | {what} cannot stand in a conditional branch, which holds instructions and data |
| `NGA0170` | error | a label in a conditional branch is written `@name`: a branch defines no symbol, and two branches want one name |
| `NGA0171` | error | `{name}...` takes every argument left, so it stands last |
| `NGA0172` | error | `.match` stands in a macro body, which is where a pack is |
| `NGA0173` | error | `.case` without an open `.match` |
| `NGA0174` | error | `{directive}` without an open `.match` |
| `NGA0175` | error | this `.match` is never closed |
| `NGA0176` | error | a `.match` holds `.case`s, and this stands before the first |
| `NGA0177` | error | `...` spreads a pack and follows its name, touching it |
| `NGA0178` | error | this `.match` has no `.case`, so nothing could ever fit |
| `NGA0179` | error | `temporary` does not go with `{other}` |
| `NGA0180` | error | a `.proc` is code, and `temporary` is a reservation |
| `NGA0181` | error | `.source` is followed by a quoted path, a comma and a line number, found `{token}` |
| `NGA0182` | error | a source line is counted from 1, and this is {line} |
| `NGA0183` | error | a `.{encoding}` payload is written as a quoted literal, found `{token}` |
| `NGA0184` | error | `{character}` is not valid in a `.{encoding}` payload |
| `NGA0185` | error | `=` pads the end of a `.base64` payload and stands nowhere else |
| `NGA0186` | error | a `.{encoding}` payload takes {group} characters to the byte, and this one has {count} |
| `NGA0187` | error | a payload is bytes and not text, so it names no character set |
| `NGA0188` | error | a `.namespace` in a `.proc` holds sections, temporaries and constants, and `{what}` is none of them |
| `NGA0189` | error | this `.declare` keeps every byte in a register, and a `.ztemp` or `.temp` stands below it |
| `NGA0190` | error | `{register}` already carries an argument of this proc |
| `NGA0191` | error | `{place}` names one register for two bytes |

### Project file and run configuration

| Code | Severity | Message |
|---|---|---|
| `NGA1101` | error | project refers to `{module}`, which names no module and no group |
| `NGA1102` | error | `{module}` would name more than one file; naming modules is a Project file's business |
| `NGA1103` | error | `{path}` is not a project file, and a run is given nothing else |
| `NGA1104` | note | a source file becomes a module where a project's `modules` block names it |
| `NGA1105` | error | a constant is a name, `=` and a number, found `{token}` |
| `NGA1106` | error | the value of `{name}` is set more than once |
| `NGA1107` | note | `{name}` was already set here |
| `NGA1108` | error | `{name}` names no container; the tool writes {known} |
| `NGA1109` | error | the container is set more than once |
| `NGA1110` | error | `{name}` is not a project block |
| `NGA1111` | error | this `{name}` block is never closed |
| `NGA1112` | error | expected a block or an `include`, found `{token}` |
| `NGA1113` | error | expected `{{` to open the body of `{name}` |
| `NGA1114` | error | a module is a quoted path, optionally followed by `as` and a name |
| `NGA1115` | error | expected a name after `as` |
| `NGA1116` | error | a path is not translated, so it names no character set |
| `NGA1117` | error | expected `deny`, `allow` or `off`, found `{token}` |
| `NGA1118` | error | `{code}` names no diagnostic |
| `NGA1119` | error | the severity of `{code}` is set more than once |
| `NGA1120` | note | `{code}` was already set here |
| `NGA1121` | error | this include reaches a file that is already being read |
| `NGA1122` | error | cannot read `{path}` |
| `NGA1123` | error | this project declares no modules |
| `NGA1125` | error | `phase` is followed by the phase's name, found `{token}` |
| `NGA1126` | error | expected `needs`, `then` or `entry`, found `{token}` |
| `NGA1127` | error | expected a name after `{after}` |
| `NGA1128` | error | a resident entry names a module or a group, found `{token}` |
| `NGA1129` | error | expected `,` between `{previous}` and `{next}` |
| `NGA1130` | error | project refers to phase `{phase}`, which is not declared |
| `NGA1131` | error | the entry phase is set more than once |
| `NGA1132` | note | the entry phase was already set here |
| `NGA1133` | error | this project declares phases and no `entry` |
| `NGA1134` | warning | module `{module}` is in no phase, so it is never in memory |
| `NGA1135` | error | expected `region`, `register`, `units`, `window` or `containers`, found `{token}` |
| `NGA1136` | error | expected a value after `{after}` |
| `NGA1137` | error | a value here is written out, and `{name}` names nothing |
| `NGA1138` | error | this does not come to a number |
| `NGA1139` | error | a range is written `start .. end` |
| `NGA1140` | note | the container was already set here |
| `NGA1141` | error | this machine does not take a `{name}`; it takes {taken} |
| `NGA1142` | note | the machine says what it takes here |
| `NGA1143` | error | `{name}` is not what a program is optimised for; the words are {known} |
| `NGA1144` | error | what the program is optimised for is set more than once |
| `NGA1145` | note | it was already set here |
| `NGA1146` | error | `{name}` names no processor; the words are {known} |
| `NGA1147` | error | the entry of phase `{phase}` is set more than once |
| `NGA1148` | note | it was set here |
| `NGA1149` | error | the processor is set more than once |
| `NGA1150` | error | `transform` is followed by the transform's name, found `{token}` |
| `NGA1151` | error | a transform entry names a section as `module.section`, found `{token}` |
| `NGA1152` | note | the processor was already set here |
| `NGA1155` | error | a region's range is followed by `ram`, `register` or `reserved`, not by `{token}` |
| `NGA1156` | error | `{word}` is not a region property; write `ram`, `register` or `reserved` |
| `NGA1157` | error | a region runs from a lower address to a higher one inside the address space, and this one does not |
| `NGA1158` | error | a region named `{name}` is already declared |
| `NGA1159` | note | `{name}` was declared here |
| `NGA1160` | error | a register is an address, and {value:hex} is not one |
| `NGA1161` | error | a register is one or two bytes, and {width} is neither |
| `NGA1163` | note | the region `{region}` is declared here |
| `NGA1164` | error | expected `units` or `size`, found `{token}` |
| `NGA1165` | error | storage has units by count and no `size`, and a unit holds something |
| `NGA1166` | error | the size of a unit is set more than once |
| `NGA1167` | note | it was set here |
| `NGA1168` | error | the units are declared more than once; `units` names a set of the target or counts them |
| `NGA1169` | note | they were declared here |
| `NGA1170` | error | {count} units, and a unit is named in a byte |
| `NGA1171` | error | a unit holds between one byte and the address space, and {value:hex} is neither |
| `NGA1172` | error | `{name}` names a group and a module, and a name in a list has to mean one thing |
| `NGA1173` | note | the module `{name}` is declared here |
| `NGA1174` | error | group `{group}` reaches itself here, so listing it would never end |
| `NGA1175` | warning | group `{group}` holds no module, so naming it adds nothing |
| `NGA1176` | error | `{name}` is a group, and one module is expected here |
| `NGA1177` | error | a group entry names a module or a group, found `{token}` |
| `NGA1178` | error | `group` is followed by the group's name, found `{token}` |
| `NGA1179` | error | a window lists what it shows after `views`, found `{token}` |
| `NGA1180` | error | `{name}` already names a {kind} of the target |
| `NGA1181` | note | it was declared here |
| `NGA1182` | error | a window's range runs from a lower address to a higher one inside the address space, and this one does not |
| `NGA1183` | error | `{state}` is listed more than once among what `{window}` shows |
| `NGA1184` | error | `{base}` is not a named state of window `{window}`; a base is a named state, never a unit set |
| `NGA1185` | error | `{name}` is not a unit set of the target |
| `NGA1186` | error | no window shows `{name}`, so what one of its units holds is unknown |
| `NGA1187` | error | windows `{window}` and `{other}` both show `{name}` and differ in size, so a unit has no one size |
| `NGA1188` | error | storage names a unit set, and what a unit holds is its window's size |
| `NGA1189` | error | a unit set holds at least one unit |
| `NGA1190` | error | `panes` is followed by `in` and a window, found `{token}` |
| `NGA1191` | error | `{name}` is not a window of the target |
| `NGA1192` | error | `{state}` is not a named state of window `{window}` |
| `NGA1193` | error | a family holds between 1 and 255 panes, and {count} is neither |
| `NGA1194` | error | window `{window}` shows no unit set, so a pane in it is pinned to a named state: write `= STATE` |
| `NGA1195` | error | a family takes consecutive banks, and `{pane}` is pinned to one state |
| `NGA1196` | error | expected a pane's name, found `{token}` |
| `NGA1197` | error | `base` is written `base WINDOW = STATE`, found `{token}` |
| `NGA1198` | error | `{name}` is not a window of the target |
| `NGA1199` | error | `{state}` is not a named state of window `{window}` |
| `NGA1200` | error | phase `{phase}` gives window `{window}` its base more than once |
| `NGA1201` | error | the groups phase `{phase}` needs give window `{window}` different bases, `{state}` and `{other}` |
| `NGA1202` | note | `{state}` is given here |
| `NGA1203` | error | `{name}` is no generator; `binary` reads a file |
| `NGA1204` | error | `{name}` is no argument of this generator |
| `NGA1205` | error | this argument is given more than once |
| `NGA1206` | error | `{name}` takes {wanted}, and this is {given} |
| `NGA1207` | error | `{generator}` names the file it works from first, as a quoted path |
| `NGA1208` | error | what `{generator}` produced cannot be emitted: {where} |
| `NGA1209` | error | a generator's arguments are closed by `)`, found `{token}` |
| `NGA1210` | error | `{path}` failed: {message} |
| `NGA1211` | error | `{path}` did not finish within the work a generator is given |
| [`NGA1212`](#nga1212) | error | {module}: {message} |
| `NGA1213` | warning | {module}: {message} |
| `NGA1214` | note | the generator said so at {where} |
| `NGA1215` | error | `{name}` names no set of facts; the sets are {sets} |

### Symbols, Merge, duplicates and Slots

| Code | Severity | Message |
|---|---|---|
| `NGA2201` | error | symbol `{symbol}` is defined more than once in phase `{phase}` |
| `NGA2202` | note | previous definition of `{symbol}` is here |
| `NGA2203` | error | `{symbol}` is already defined in this module |
| `NGA2204` | error | `{symbol}` is exported but not defined in this module |
| `NGA2205` | error | `{symbol}` is exported by more than one module |
| `NGA2206` | error | `{symbol}` is not defined |
| `NGA2207` | error | `{symbol}` is defined in terms of itself |
| `NGA2210` | error | no definition of `@{name}` matches this reference |
| `NGA2211` | error | `@{name}` has more than one definition here; write `@+{name}` or `@-{name}` |
| `NGA2212` | error | an anonymous local label is reached with `@+` or `@-` |
| `NGA2213` | error | `{name}` is already a label of this macro body |
| `NGA2214` | error | `{name}` is a parameter of this macro, and a label cannot take its name |
| `NGA2215` | error | no definition of `@{name}` outside this branch matches, and one in another branch may not be there at all |
| `NGA2220` | error | expected an integer here, and this is {type} |
| `NGA2221` | error | two addresses cannot be added; `a - b` is the distance between them |
| `NGA2222` | error | an address cannot be subtracted from a number |
| `NGA2223` | error | these addresses are not in one section, so this is a property of the layout rather than of the program |
| `NGA2224` | error | only a label reaches the attributes of its section, and this is {type} |
| `NGA2225` | error | `.{directive}` takes {allowed}, and this is {type} |
| `NGA2226` | error | an operand is an address or a number, and this is {type} |
| `NGA2227` | error | `{name}` is not an attribute of a section |
| `NGA2228` | error | a conditional chooses between two of one type, and these are {type} and {other} |
| `NGA2229` | error | a conditional between addresses of two sections has no section of its own, and subtraction and width need one |
| `NGA2230` | error | `{symbol}` is not a character set |
| `NGA2231` | error | `{charset}` does not map `{character}` |
| `NGA2232` | error | character set `{charset}` is derived from itself |
| `NGA2233` | error | `{character}` is already mapped by this character set |
| `NGA2234` | error | a character set maps to bytes, and this run covers {start} to {end} |
| `NGA2235` | error | a character set is resolved before any size or address exists, so this needs a value written in the source |
| `NGA2236` | error | `{symbol}` is a character set, and a character set has no value |
| [`NGA2237`](#nga2237) | error | `{character}` is not ASCII and this literal names no character set |
| `NGA2240` | error | `{phase}` is not a phase of this project |
| `NGA2241` | error | this transition to `{to}` stands in code present in phase `{from}`, which has no `then {to}` |
| `NGA2242` | error | the target numbers phases in a byte, and this project has {count:n} |
| `NGA2243` | error | the transition from `{from}` to `{to}` loads {count:n} sections, and a frame counts them in a byte |
| `NGA2244` | error | `{transform}` is not a format the tool can encode |
| `NGA2245` | error | module `{module}` has no label named `{section}`, and a section is named by one |
| `NGA2246` | warning | phase `{from}` declares `then {to}`, and no `.transition {to}` is present in it, so the edge is taken by nothing |
| `NGA2247` | warning | `{module}.{section}` is never loaded by a transition, so nothing transforms it |
| `NGA2248` | error | `{section}` names the section `{previous}` already lists, and a section is transformed once |
| `NGA2250` | error | `{symbol}` is not a slot |
| `NGA2251` | error | `{symbol}` is not a label or a section of this module |
| [`NGA2252`](#nga2252) | error | `{slot}` has two implementations live in phase `{phase}`: `{symbol}` and `{other}` |
| `NGA2253` | note | `{symbol}` implements it here |
| `NGA2254` | error | a frame counts cell writes in a byte, so a program has at most 255 slots, and this one has {count:n} |
| `NGA2255` | error | `{symbol}` fills slot `{slot}` from pane `{pane}`, and the cell holds an address but not the state that shows it |
| `NGA2260` | error | `{name}` is not a proc of this module, and `then` names one |
| `NGA2261` | error | `{name}` already follows `{previous}`, and can follow only one proc |
| `NGA2262` | error | `{name}` follows `{previous}`, and cannot also be pinned or aligned |
| `NGA2263` | error | `{name}` follows `{previous}`, so both are movable or neither is |
| `NGA2264` | error | `{name}` follows itself through `then` |
| `NGA2265` | error | `{name}` follows `{previous}`, so both are in one pane or neither is |
| `NGA2266` | error | `{type}` is a {want}-byte type, and this reserves {got} |
| `NGA2270` | error | a proc holds code, and a reservation is not code; a variable is a `.ztemp` or lives in a `.section` |
| `NGA2271` | error | a temporary section holds reservations only, and this is not one |
| `NGA2272` | error | `{symbol}` names a region, a unit set or a window of the target, and a module cannot define it |
| `NGA2273` | error | this section holds no label, and a section is named by one |
| `NGA2274` | error | `{label}` is not defined in this module, and a decoder is one of its labels |
| `NGA2275` | error | `{label}` is a {kind}, and a decoder is entered at a label |
| `NGA2276` | error | `{format}` is not a format the tool can encode, so nothing produces what this decodes |
| `NGA2277` | error | `{format}` already has a decoder, and a format has one |
| `NGA2278` | note | the decoder of `{format}` is declared here |
| `NGA2279` | error | no module declares a decoder for `copy`, and every Transition copies; the storage driver declares one with `.transform copy` |
| `NGA2280` | error | no module declares a decoder for `{transform}` |
| `NGA2281` | error | a `.transition` is taken and no module declares the storage driver; one module declares it with `.driver`, and a machine variant usually lists that module |
| `NGA2282` | error | this module declares the storage driver, and `{module}` already does |
| `NGA2283` | note | the driver is declared here |
| `NGA2284` | error | `{role}` is not a role of the driver; the roles are `open`, `read`, `stream`, `show` and `showAt` |
| `NGA2285` | error | the driver declares no `{role}`, and the routine calls it |
| `NGA2286` | error | `{label}` is not defined in this module, and a role is one of its labels |
| `NGA2287` | error | `as {name}` {reason} |
| `NGA2288` | error | `{proc}` is `as {type}`, so its arguments are `{type}`'s temporaries, and it declares {what} of its own |
| `NGA2289` | error | the driver is present in some phases only, and every Transition calls it |
| `NGA2290` | error | this decoder's module is present in some phases only, and any Transition may call it |
| `NGA2291` | error | `{role}` is declared more than once |
| `NGA2293` | error | `{role}` takes {count} names, and {given} are given |
| `NGA2294` | error | `{window}` is not a window of the target |
| `NGA2295` | error | the driver declares no `{role}` for window `{window}`, and the target declares the window |
| `NGA2296` | error | storage is the units of `{name}`, and the driver names no `stream` to read them through |
| `NGA2297` | error | the stream reads through `{window}`, which does not show the units storage is |
| `NGA2298` | error | `nga.{role}` names a window first, and this does not |
| `NGA2299` | error | `{symbol}` names a window of the target, and a window is not a value |
| `NGA2300` | error | `{name}` is an instruction, and a macro cannot take its name |
| `NGA2301` | error | parameter `{name}` is given twice |
| `NGA2302` | error | parameter `{name}` has the name of a symbol this module defines |
| `NGA2303` | error | `{name}` is neither an instruction nor a macro in scope |
| `NGA2304` | error | `{name}` is an instruction and takes one operand; an index register is `x` or `y` |
| `NGA2305` | error | a macro's argument is an expression: no `#`, no `,x` or `,y`, and no parenthesis that indirects |
| `NGA2306` | error | `{name}` takes {expected} arguments and was given {given} |
| `NGA2308` | error | `{symbol}` is a macro and stands where a value is expected |
| `NGA2309` | error | `{label}` is a {kind}, and a role is a macro the tool expands where it uses the role |
| `NGA2310` | error | `nga` is the tool's namespace, and no module defines it |
| `NGA2311` | error | `{space}` is not a namespace |
| `NGA2312` | error | `nga.{name}` names nothing the tool defines |
| `NGA2313` | error | `nga.{name}` is used, and no module declares a driver |
| `NGA2314` | error | `{symbol}` is a name the tool defines, and a module may not define it |
| `NGA2315` | note | the tool defines `{symbol}` here |
| `NGA2316` | error | `{name}` is a namespace and stands where a value is expected |
| `NGA2317` | error | `{symbol}` is also a namespace, and one name cannot be both |
| `NGA2319` | error | `{name}` is an attribute of a section, and a name in a proc cannot take it |
| `NGA2320` | error | `{symbol}` is inside proc `{proc}`, and nothing outside the proc can reach it |
| `NGA2321` | error | a condition decides how many bytes there are, so it is a declared value: nothing naming a position or a size may stand in it |
| `NGA2322` | error | this expansion is {depth} uses deep at `{name}` and shows no sign of ending |
| `NGA2323` | note | the expansion began at this use |
| `NGA2324` | error | `{name}` is a {kind}, and stands where an instruction or a macro is expected |
| `NGA2325` | error | `{name}` is not a pack, and only a pack spreads |
| `NGA2326` | error | `{name}` is not a pack, and `.match` counts a pack's elements |
| `NGA2327` | error | `{name}` is already bound around this `.case`, and a case hides nothing |
| `NGA2328` | error | `{name}` was given {count} elements, and no `.case` of `.match {name}` fits that |
| `NGA2329` | note | the `.match` is here |
| `NGA2330` | warning | a `.case` above this one fits everything this one does, so it is never taken |
| `NGA2331` | error | `{name}` takes at least {expected} arguments and was given {given} |
| `NGA2332` | error | this expansion has produced {count:n} statements, more than the target has bytes to put them in |
| `NGA2333` | error | with its arguments in place this expression nests {depth} deep, more than {limit} |
| `NGA2334` | error | `{place}` keeps {want} of its bytes here, and this reserves {got} |
| [`NGA2412`](#nga2412) | error | `{symbol}` is in pane `{pane}`, and this code does not run with that pane shown: put the statement under a `.with`, or the code in a Proc declared `under` it |
| `NGA2413` | note | `{fromView}` and `{toView}` share window {windowStart:hex}-{windowEnd:hex} and are never visible together |
| [`NGA2414`](#nga2414) | error | reference to `{symbol}`, which is not in memory in phase `{phase}` while `{section}` is |
| `NGA2415` | error | reference to `{slot}`, which has no implementation in phase `{phase}` while `{section}` is |
| `NGA2416` | error | `{symbol}` is inside proc `{proc}`, which is reached only through its name |
| `NGA2417` | error | a branch to `{symbol}` in `{section}`: a branch reaches its own proc or one chained to it by `then`; use `jmp` |
| `NGA2418` | error | `{name}` is not a pane the Project declares |
| `NGA2419` | error | `{section}` is in pane `{pane}`, and a pane's section stands in one bank at one address; it cannot be movable |
| `NGA2420` | error | `{section}` is in pane `{pane}`, and a window covers no zero page |
| `NGA2421` | error | a pane's value is the state the solver chose, and this is arithmetic on it; a pane stands as an immediate, a data item or a role's argument |
| `NGA2422` | error | `{name}` has {count} members, and {index} is not one of them |
| `NGA2423` | error | a pane is a state of a window and not an address, and this operand is one |
| `NGA2424` | error | a dispatch to `{symbol}`, which is not a position of `{section}`: a dispatch goes where it stands, and `.own` is how a table reaches other sections |
| `NGA2425` | error | a dispatch of {count} targets, and an index reaches {most} |

### Context, Requirements and interrupt handlers

| Code | Severity | Message |
|---|---|---|
| `NGA3001` | error | `.with` applies to the instruction or macro use on the line below, and this is not one |
| `NGA3002` | error | `{name}` is a {kind}, and `.with` takes a pane, a family with `, x`, a window with `, x` or a window `= STATE` |
| [`NGA3003`](#nga3003) | error | `{state}` is not a named state of window `{window}` |
| `NGA3004` | error | window `{window}` has no base, so nothing can be shown again after the statement |
| `NGA3005` | warning | `{window}` shows this state already, so this `.with` changes nothing |
| `NGA3006` | error | `{window}` is shown by a `.with` on this statement already, and a window shows one state at a time |
| [`NGA3007`](#nga3007) | error | a statement under `.with` may not `{mnemonic}`: it would leave the window as shown |
| [`NGA3008`](#nga3008) | error | what this statement reaches names `{symbol}`, in pane `{pane}`, which this `.with` does not show |
| `NGA3009` | note | `{symbol}` is named here |
| `NGA3010` | error | the phases this code is in give window `{window}` different bases, `{state}` in `{phase}` and `{other}` in `{otherPhase}`, so `.with` cannot show one base again; split the module |
| `NGA3011` | error | `{symbol}` is in state `{state}` of window `{window}`, which phase `{phase}` does not show here |
| `NGA3012` | error | `{directive}` applies to the instruction or data on the line below, and this is not one |
| `NGA3013` | error | `.own` and `.root` on one statement: an address is followed by this section or by the hardware, not both |
| `NGA3014` | error | `.root` hands the address to the hardware, and names nobody |
| `NGA3015` | error | this section is in pane `{pane}` of window `{window}`, and code in a pane cannot switch its own window: the switch would take the code with it; a proc in fixed, `.proc NAME, under {pane}`, does it |
| `NGA3016` | error | `under {name}` {reason} |
| `NGA3017` | error | `{symbol}` runs under pane `{pane}`, which must be shown where it is called, and here it is not |

### Prune and Trace

| Code | Severity | Message |
|---|---|---|
| [`NGA4501`](#nga4501) | warning | phase `{phase}` is not reachable from the entry phase |
| `NGA4505` | warning | `{section}` is pinned at {address:hex} and nothing reaches it, so it is dropped; mark it `root` if the hardware reads it |
| `NGA4601` | error | the address of `{section}` is taken, and a temporary has none to give: its bytes are another's while it is not active |
| `NGA4602` | error | `{section}` is temporary while `{owner}` runs, and `{owner}` can be entered again before it returns |
| `NGA4603` | warning | nothing this statement names is code or a temporary, so `.own` changes nothing |
| [`NGA4606`](#nga4606) | error | the address of `{section}` is taken and nothing says who follows it: `.own` if this section does, `.own NAME` if another does, `.root` if the hardware does |
| [`NGA4607`](#nga4607) | error | `{section}` jumps through a pointer and owns no address of code, so nothing says where the jump goes |
| `NGA4608` | warning | nothing this statement names is a section, so `.root` marks nothing |
| `NGA4609` | error | `{name}` is not a section, a proc or a label, and `.own` names who follows the address |
| `NGA4611` | error | `{section}` is handed to `{follower}`, which is present in phase `{phase}` where `{section}` is not: a jump from there would land in nothing |

### Size and Place

| Code | Severity | Message |
|---|---|---|
| `NGA5102` | error | `{mnemonic}` has no {mode} form |
| `NGA5103` | error | the placement class of this operand cannot be determined, so its width cannot be either |
| `NGA5104` | error | this size depends on itself |
| `NGA5105` | error | a reservation of {size:n} bytes is not a reservation |
| `NGA5106` | error | {value:hex} is not an address |
| [`NGA5107`](#nga5107) | error | `{mnemonic}` is a `{needs}` instruction, and this target is a `{cpu}` |
| `NGA5201` | error | `{section}` overlaps `{other}` at {address:hex} |
| `NGA5202` | error | there is no room for `{section}`, which needs {size:n} bytes |
| `NGA5203` | error | a section cannot be pinned at {address:hex} |
| `NGA5204` | error | `{section}` is zero page and does not fit below $100 |
| [`NGA5205`](#nga5205) | error | `{section}` is loaded from storage, and cannot live where the stream reads through, at {address:hex} |
| `NGA5206` | error | no layout satisfies every constraint at once |
| `NGA5207` | error | `{section}` and `{other}` cannot be kept apart: no layout holds both without one lying over the other |
| `NGA5208` | note | `{section}` is declared here |
| `NGA5209` | warning | the search stopped before it could show that fewer pairs would do; the ones named suffice |
| `NGA5210` | error | this assertion does not hold |
| `NGA5211` | error | this assertion cannot be decided |
| `NGA5212` | error | a section cannot be aligned to {alignment} |
| `NGA5213` | error | a section cannot be kept within a boundary of {boundary} |
| `NGA5214` | error | `{section}` is {size:n} bytes, and cannot lie within a boundary of {boundary:n} |
| `NGA5215` | error | `{section}` is pinned at {address:hex}, which is not aligned to {alignment:n} |
| `NGA5216` | error | `{section}` is pinned at {address:hex}, and reaches {last:hex} across a boundary of {boundary:n} |
| `NGA5217` | error | `{section}` is pinned at {address:hex}, in the register region `{region}`, where nothing can be placed |
| [`NGA5218`](#nga5218) | warning | `{section}` is pinned at {address:hex}, in the reserved region `{region}` |
| `NGA5222` | error | there is no storage for `{section}`, which needs {size:n} bytes |
| `NGA5223` | note | the target has no storage; a `storage` block is where its units are named |
| `NGA5224` | warning | `{section}` is `root` and holds no bytes to restore, and is evicted in phase `{gap}` between `{before}` and `{after}`, which both need it |
| `NGA5225` | error | pane `{pane}` takes {size:n} bytes in phase `{phase}`, and a bank of `{set}` holds {available:n} |
| `NGA5226` | error | `{section}` is pinned at {address:hex}, outside the window of pane `{pane}` |
| `NGA5227` | error | `{section}` has bytes and is in pane `{pane}`, which is pinned to a named state: no Container fills one, so a pane pinned to a state holds only sections that reserve |
| `NGA5228` | error | `{section}` is pinned at {address:hex}, inside window `{window}`, which code reached under a `.with` on it would not see |
| `NGA5229` | error | family `{pane}` needs {count} banks in a row, and `{set}` has {banks} |
| `NGA5250` | error | the layout gives `{section}` an address, and nothing reaches it |
| `NGA5251` | error | the layout ends `{section}` at {address:hex} in phase `{phase}`, and `{next}`, which follows it, starts at {other:hex} |
| `NGA5252` | error | the layout puts `{section}` at {address:hex}, inside window `{window}`, which it runs under a `.with` on |
| `NGA5631` | error | phase `{phase}` does not fit in view `{view}`: requires {required:n} bytes, {available:n} available |
| `NGA5632` | error | phase `{phase}` needs {required:n} bytes of zero page, and {available:n} are available |
| `NGA5633` | note | `{section}` accounts for {size:n} bytes |

### Layout verification

| Code | Severity | Message |
|---|---|---|
| `NGA5240` | error | the layout has `{section}` and `{other}` sharing {address:hex}, and phase `{phase}` holds both |
| `NGA5241` | error | the layout puts `{section}` at {address:hex}, and it is pinned at {pinned:hex} |
| `NGA5242` | error | the layout puts zero page `{section}` at {address:hex}, and it reaches {last:hex} |
| `NGA5243` | error | the layout puts `{section}` at {address:hex}..{last:hex}, reaching into the window it waits behind |
| `NGA5244` | error | the layout puts `{section}` at {address:hex}..{last:hex}, outside the pool it was allocated from |
| `NGA5245` | error | the layout gives `{section}` no address |
| `NGA5246` | error | the layout puts `{section}` at {address:hex}, and it reaches past the end of memory |
| `NGA5247` | error | the layout puts `{section}` at {address:hex}, which is not aligned to {alignment:n} |
| `NGA5248` | error | the layout puts `{section}` at {address:hex}..{last:hex}, across a boundary of {boundary:n} |
| `NGA5249` | error | the layout puts `{section}` at {address:hex} in phase `{phase}` and at {other:hex} in phase `{otherPhase}`, and a reference holds it to one address across both |

### Patch, Emit, compression and the Container

| Code | Severity | Message |
|---|---|---|
| `NGA6101` | error | {value} does not fit in the {width:n} bytes written here |
| [`NGA6102`](#nga6102) | error | `{mnemonic}` reaches -128 to 127 and this is {distance:n} bytes away; write `{jcc}`, which is five bytes where two do not reach |
| `NGA6201` | error | `{section}` and `{other}` share {address:hex}, and a raw image cannot hold both |
| `NGA6204` | error | the .xex fills storage through the driver's `showAt`, and no module declares a driver |
| `NGA6205` | error | the .xex fills a unit by writing into the window the stream reads through, and this driver names none |
| `NGA6206` | error | a unit holds {unit:n} bytes and window `{name}` {window:n}, and the .xex fills a unit through the window |
| `NGA6211` | error | phase `{phase}` has no entry: no label `{name}` is defined in the modules it needs |
| `NGA6212` | error | `{name}` is defined in more than one module that phase `{phase}` needs, so it does not say where the phase starts |
| `NGA6213` | error | `{name}` is a {kind}, and an entry is a label |
| `NGA6214` | error | the .xex fills a unit through one range, and window `{name}` has {count} |
| `NGA6221` | error | an .atr reads storage a sector at a time and shows no Window, and this storage is the unit set                        `{name}` |
| `NGA6222` | error | a unit of storage on a diskette is the {size:n} bytes 256 sectors of {sector:n} hold, and {unit:n} is not that |
| `NGA6223` | error | the boot record of an .atr shows no Window, and this driver streams through `{name}` |
| `NGA6224` | error | a sector is numbered in two bytes, so an image holds {available:n} of them, and this program needs {required:n} |
| `NGA6225` | error | `{section}` stands at {address:hex}, inside the boot record the .atr loads over {begin:hex} to {end:hex}, which is still loading the program when those bytes are written |
| `NGA6226` | error | an .atr is booted by its first sectors, and none were made |
| `NGA6227` | error | the boot record holds {available:n} bytes and its loader came to {required:n} |

### Lexing a `.ngc` Module

| Code | Severity | Message |
|---|---|---|
| `NGA7001` | error | unexpected character `{character}` |
| `NGA7002` | error | control character {code:hex} in source |
| `NGA7003` | error | invalid UTF-8 encoding |
| `NGA7004` | error | `/*` is not closed before the end of the file |
| `NGA7005` | error | a backslash at the end of a line does not carry a `//` comment onto the next |
| `NGA7006` | error | a leading zero does not make `{constant}` octal; write it in decimal, `0x` or `0b` |
| `NGA7007` | error | `{suffix}`: an integer constant takes no suffix |
| `NGA7008` | error | there are no floating-point constants |
| `NGA7009` | error | `{character}` is not valid in a {base} constant |
| `NGA7010` | error | `{prefix}` is not followed by any digits |
| `NGA7011` | error | `'` in an integer constant must stand between digits |
| `NGA7012` | error | this constant is too large to be represented |
| `NGA7013` | error | `{name}` is reserved: C keeps a name that begins with `__`, or with `_` and a capital, for the compiler |
| `NGA7014` | error | this {what} is not closed before the end of its line |
| `NGA7015` | error | `{escape}` is no escape: the six are `\\`, `\"`, `\'`, `\n`, `\t` and `\0` |
| `NGA7016` | error | `{character}` is not ASCII, and this literal names no character set |
| `NGA7017` | error | a character constant holds one character, and this holds {what} |

### Parsing a `.ngc` Module

| Code | Severity | Message |
|---|---|---|
| `NGA7100` | error | expected {expected}, found {found} |
| `NGA7101` | error | {what} nest more than {limit} deep |
| `NGA7102` | error | the subset spells `{found}` as `{spelling}` |
| `NGA7103` | error | a literal of {second} is not joined to one of {first} |

### Compiling a `.ngc` Module

| Code | Severity | Message |
|---|---|---|
| `NGA7200` | error | the compiler does not compile {construct} yet |
| `NGA7201` | error | `{name}` is not declared |
| `NGA7202` | error | `{name}` is not a function, and only a function or a `.proc` can be called |
| `NGA7203` | error | `{name}` is a function, and a function cannot be assigned |
| `NGA7204` | error | `{name}` is already defined in this file |
| `NGA7205` | note | the previous definition of `{name}` is here |
| `NGA7206` | error | {value} does not fit in a `{type}` |
| `NGA7207` | error | `{left}` and `{right}` differ in signedness, and only a cast mixes them |
| `NGA7208` | error | a `{from}` does not fit in the `{to}` it is assigned to without a cast |
| `NGA7209` | error | a `bool` holds `true`, `false` or a comparison, and is neither an operand nor an integer |
| `NGA7210` | warning | {value} is outside what a `{type}` holds, so this comparison is always {outcome} |
| `NGA7211` | error | a `{type}` shifted by {count}: the count is from 0 to {largest} |
| `NGA7212` | error | a condition is a `bool`, and this is {what} |
| `NGA7213` | error | `{keyword}` stands in no loop |
| `NGA7214` | error | `{name}` is not a type |
| `NGA7215` | error | `{name}` is not an `enum struct` |
| `NGA7216` | error | `{type}` has no enumerator `{name}` |
| `NGA7217` | error | `{type}` is an `enum struct`, whose values are compared and never computed |
| `NGA7218` | error | {left} and {right} are different types |
| `NGA7219` | error | a `switch` takes a value of an `enum` or an integer, and this is {what} |
| `NGA7220` | error | `{label}` is not an enumerator of `{type}` |
| `NGA7221` | error | `{label}` is a label of this `switch` already |
| `NGA7222` | warning | no case names {names} of `{type}`, and the `switch` has no `default` |
| `NGA7223` | error | `{name}` has {count} enumerators, and an `enum struct` holds 256 at most |
| `NGA7224` | error | `{name}` takes an argument for each of its {count} parameters, and this call gives {given} |
| `NGA7225` | error | `{name}` returns nothing, and this reads what it returns |
| `NGA7226` | error | `{name}` returns {type}, and this `return` gives no value |
| `NGA7227` | error | `{name}` returns nothing, and this `return` gives a value |
| `NGA7228` | error | `{name}` returns {type}, and the end of its body is reached with no `return` |
| `NGA7229` | error | this call closes a cycle of calls, {cycle}, and a function never reaches itself |
| `NGA7230` | error | {what} is not cast to a `{type}` |
| `NGA7231` | error | {what} is given a constant, and this is not one |
| `NGA7232` | error | `{name}` is `const`, and is given no value where it is declared |
| `NGA7233` | error | a constant is defined through itself, {cycle} |
| `NGA7234` | error | `{name}` is `const`, and is not assigned |
| `NGA7235` | error | `{name}` is read in the value it is given, before it holds one |
| `NGA7236` | error | {index} is outside `{name}`, whose {count} elements are indexed from 0 |
| `NGA7237` | error | an index is {allowed}, and this is {what} |
| `NGA7238` | error | `{name}` is an array, and is not assigned but for its elements |
| `NGA7239` | error | `{name}` is neither an array nor a pointer, and nothing else is indexed |
| `NGA7240` | error | this divides by zero |
| `NGA7241` | error | `{name}` has {count} elements, and is given {given} |
| `NGA7242` | error | `{name}` is {what}, and is given {given} |
| `NGA7243` | error | the size of `{name}` is a constant from 1 to {largest} |
| `NGA7244` | error | `{name}` is {what}, and a declaration declares pointers or none |
| `NGA7245` | error | `{name}` is a parameter, whose address is not taken; copy it to a local |
| `NGA7246` | error | {left} and {right} do not meet |
| `NGA7247` | error | what is written here is `const`, reached through a pointer to `const` |
| `NGA7248` | error | {what} is no pointer, and only a pointer is dereferenced |
| `NGA7249` | error | `{name}` is a pointer, and C would ignore the size written for it |
| `NGA7250` | error | `{name}` has no members, and C gives such a type no size |
| `NGA7251` | error | a type holds itself by value and has no size, {cycle} |
| `NGA7252` | error | `{name}` is a member, which is given no value and is not `const` |
| `NGA7253` | error | `{type}` has no member `{name}` |
| `NGA7254` | error | `{name}` is a member of `{type}` already |
| `NGA7255` | error | {what} is no `struct` or `union`, and only one has members |
| `NGA7256` | error | {what} is given {given} |
| `NGA7257` | error | {what} is a `struct` or a `union`, which no operator takes |
| `NGA7258` | error | `{name}` is no attribute of the subset |
| `NGA7259` | error | `[[{attribute}]]` applies to {applies}, and `{name}` is none |
| `NGA7260` | error | `{name}` is an array of {what}, which lies in no stripes |
| `NGA7261` | error | `{name}` has {count} elements, and a striped array holds 256 at most |
| `NGA7262` | error | `{name}` lies in stripes, and has no address of its own |
| `NGA7263` | error | `{name}` is a Label of the assembler with no shape in C: {reason} |
| `NGA7264` | error | `{name}` is {reason} of the assembler, which C does not read |
| `NGA7265` | error | `{name}` is a `reserved` Region, which C does not reach |
| `NGA7266` | error | `{name}` holds a string, so the compiler does not know how many elements it has |
| `NGA7267` | note | `{name}` is defined here |
| `NGA7268` | error | `{name}` takes or returns `u8[N]`, bytes that a call from C does not hand over |
| `NGA7269` | error | `{name}` is defined in C, and `extern` declares only what the assembler exports |
| `NGA7270` | error | `{name}` is declared `extern`, and nothing the assembler exports is named so |
| `NGA7271` | error | `{name}` is declared `extern` already, and a name is declared once in the program |
| `NGA7272` | error | `{name}` is declared `extern`, and {what} |
| `NGA7273` | error | `{name}` {what} |
| `NGA7274` | error | `{name}` reads past what the assembler wrote as data: {reason} |
| `NGA7275` | error | `{path}`, at offset {offset}, splits an item the assembler wrote |
| `NGA7276` | error | {what} holds {value}, which is no `{type}` |
| `NGA7277` | error | `{name}` {what} |
| `NGA7278` | error | `{name}` is no character set, and a literal's prefix names one |
| `NGA7279` | error | `{name}` is {what}, and `{form}` takes {takes} |
| `NGA7280` | error | `{name}` is in family `{family}`, whose member is known at run time alone: call it in a `with({family}, i)` block |
| `NGA7281` | error | `{name}` is in pane `{pane}` of window `{window}`, which this block shows in another state: the exit of the call's `.with` would put back what the function's own code shows, not the block's |
| `NGA7282` | error | window `{window}` is shown by the block this one stands in, and a window shows one state at a time |
| `NGA7283` | error | `{keyword}` leaves the `with` block, which would leave the window as the block shows it |
| `NGA7284` | error | the address of `{name}`, in what this block shows, is kept only in a local of the block or handed to a call in it: here it {escapes} |
| `NGA7286` | error | the index of `with({name}, i)` is {what}, and `X` takes a `u8` or a constant |
| `NGA7287` | error | `[[{attribute}]]` takes {takes} |
| `NGA7288` | error | `{name}` is `[[in]]` a pane and `[[under]]` one, and code under a pane is in none |
| `NGA7289` | error | this block shows another state of window `{window}` from code in pane `{pane}`, which cannot switch its own window: write `[[with(...), trampoline]]`, and pay a Proc in fixed and a `jsr` |
| `NGA7290` | error | `trampoline` on a block that needs none: {where}, and the block is a macro under `.with` |
| `NGA7291` | error | this block shows `{pane}`, and the code is in `{pane}` already |
| `NGA7292` | error | `{name}` is in pane `{pane}` of window `{window}`, and this code is in pane `{own}` of it, which cannot switch its own window: call it in a `[[with({pane}), trampoline]]` block or an `[[under({own})]]` function |
| `NGA7293` | error | `{name}` runs under pane `{pane}`, called from code in the pane, from another function under it, or in a block that shows it, and this is none |
| `NGA7294` | error | a transition enters a phase and never comes back, so it stands on a bare `return` in a `void` function, and {what} |
| `NGA7295` | error | `{name}` is a slot, and its binding is its type's: a `void(void)` binds a vector and a `T* const` a pointer, and {what} |
| `NGA7296` | error | `{name}` fills slot `{slot}`, whose cell holds an address the program reads as {wanted}, and {what} |
| `NGA7297` | error | `{name}` is no `{type}`: {what} |
| `NGA7298` | error | `{name}` is taken as `{type}` here and as `{other}` elsewhere, and a function reads its arguments from one place |
| `NGA7299` | error | `{name}` names a function type, which is a type only as a pointer: `{name}*` |
| `NGA7300` | error | this call is of the function type its caller is a member of, which would write the temporaries the caller reads; a member dispatches through another type, or through a name |
| `NGA7301` | error | `{name}` is written `auto` and given {what}, which takes its type from where it is used and so has none to give |
| `NGA7302` | error | `{name}` is written `inline` and {what}, so the call stays a call |
| `NGA7303` | error | `inline` is written on {what}, which has no body to wrap into a caller |
| `NGA7304` | error | `{name}` is assigned by `{operator}` where it is used as a value, and reached again in the same expression, which is an order the subset does not fix |
| `NGA7305` | error | `{name}` is a pointer, which `(zp),y` reads through, so it lies in the zero page whatever is asked |
| `NGA7306` | error | `{name}` lies in stripes, whose place its layout settles, so `[[placement]]` is a second answer |
| `NGA7307` | warning | `{name}` is `{class}` already, so this says nothing |
| `NGA7308` | error | `{name}` is read here before anything writes it, and a `static` local holds what was there; give it a value where it is declared |

## Notes

What a finding means beyond what it says, where there is more to say.

### NGA0113

A label in column one that is spelled like an instruction is almost always an
instruction that lost its indentation. The tool cannot tell the two apart,
because a label is marked by position and nothing else.

Turn the warning off where a program means it.

### NGA0138

A directive that produces bytes needs somewhere to put them. `.section` and
`.proc` are the two places that have an address.

### NGA0141

The processor has `(zp,x)` and `(zp),y` and no other indirect mode. So
`(table),x` is not indirect, and the parentheses group an expression instead.
The operand that comes out is an ordinary address, and the instruction is three
bytes where the indirect form is two.

The warning is there because the two spellings are one character apart.

### NGA0158

A macro body holds instructions and data. It does not hold declarations. A
macro is instantiated where it is used, so a declaration inside one would
declare the same name again at every use.

### NGA0169

A conditional branch holds instructions and data, for the same reason a macro
body does.

### NGA1212

The text after the colon comes from the generator, not from the tool. A script
that is given something it cannot use says so itself, and this is how it
reaches the reader.

### NGA2237

A literal without a prefix must be ASCII. Nothing says what a byte above 127
would mean, because the machine's own character set is not Unicode and not
ASCII either. Name a Charset and the question has an answer.

### NGA2252

A Slot has one live implementation in each Phase. Two Modules that implement it
in the same Phase leave the tool nothing to write into the Cell.

### NGA2412

Code that reaches a Symbol in a Pane must run while that Pane is shown. Put the
statement under a `.with`, or put the code in a Proc declared `under` that
Pane.

### NGA2414

A Section is in memory in the Phases it is declared in and in no others. A
reference across that boundary would read whatever the other Phase left there.

This is the refusal that makes Phases safe. The answer is usually a Slot.

### NGA3003

A Window shows one of the states it declares. A name it does not declare names
nothing, and so does the name of the unit set behind it.

### NGA3007

A statement under `.with` runs while the Window shows what the `.with` asked
for. An instruction that changes the Window would end that, and the rest of the
statement would run against a different memory.

### NGA3008

The `.with` shows one Pane. A statement under it that reaches into another one
is reaching into memory that is not there.

### NGA4501

A Phase nothing enters is built, placed and carried in the Container, and never
runs. It is usually a `then` that was renamed or removed.

Raise it to an error in a Project where that must not happen.

### NGA4606

Taking the address of a Section tells the tool nothing about who will follow
it. Say who does. `.own` means this Section does, `.own NAME` means another
one does, and `.root` means the hardware does.

Without that, Trace cannot tell what the program still reaches, and a Section
nothing reaches is dropped.

### NGA4607

A jump through a pointer goes wherever the pointer holds. The tool follows what
the program owns, so a Section that jumps that way has to own the code it can
reach.

### NGA5107

The Target says which processor the program is for. An instruction the
processor does not have cannot be assembled, whatever the machine in front of
you happens to be.

### NGA5205

A Window is where storage arrives. A Section that waits in storage cannot also
live at the address it arrives through, because the arrival would overwrite it
while it is being read.

### NGA5218

A reserved Region belongs to something else, usually the machine or its DOS. A
pin puts a Section there anyway, which the tool allows and reports. Read it as
a question: is this Region really reserved, or is this pin really wanted.

### NGA6102

A branch reaches 128 bytes back and 127 forward. The tool says how far the
target actually is, and names the conditional jump that reaches anywhere. It is
five bytes where a branch is two.
<!-- end -->
