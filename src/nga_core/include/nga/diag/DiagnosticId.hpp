#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// <wingdi.h> defines ERROR as a preprocessor macro, which would mangle
// Severity::ERROR on Windows. Nothing in NGA wants the macro.
#ifdef ERROR
#undef ERROR
#endif

namespace nga::diag
{

enum class Severity : std::uint8_t
{
  ERROR,
  WARNING,
  NOTE,
};

// The single source of truth for the diagnostic catalog: identifier, number,
// default severity and message template all declared once, in one row.
//
// Numbers are grouped by the pipeline Step that raises them (see
// docs/spec/diagnostics.md) and are NEVER reused for a different meaning, even
// after a diagnostic is retired.
//
// Message templates take named arguments as {name}, optionally with a rendering
// hint: {name:hex} prints $1A2B and {name:n} groups digits as 41,208.
#define NGA_DIAGNOSTIC_CATALOG( X )                                                                                    \
  X( UNEXPECTED_CHARACTER, 0001, ERROR, "unexpected character `{character}`" )                                         \
  X( UNTERMINATED_STRING, 0002, ERROR, "unterminated string literal" )                                                 \
  X( LINE_END_IN_STRING, 0003, ERROR, "string literal is not closed before the end of the line" )                      \
  X( UNTERMINATED_CHARACTER, 0004, ERROR, "unterminated character literal" )                                           \
  X( EMPTY_CHARACTER, 0005, ERROR, "character literal is empty" )                                                      \
  X( MULTI_CHARACTER, 0006, ERROR, "character literal holds more than one character" )                                 \
  X( UNKNOWN_ESCAPE, 0007, ERROR, "unknown escape sequence `\\{escape}`" )                                             \
  X( INVALID_UTF8, 0009, ERROR, "invalid UTF-8 encoding" )                                                             \
  X( EMPTY_NUMBER, 0010, ERROR, "`{prefix}` is not followed by any digits" )                                           \
  X( INVALID_DIGIT, 0011, ERROR, "`{character}` is not valid in a {base} literal" )                                    \
  X( MISPLACED_DIGIT_SEPARATOR, 0012, ERROR, "`_` in a numeric literal must stand between digits" )                    \
  X( HEX_PREFIX_NOT_SUPPORTED, 0013, ERROR, "hexadecimal literals are written `$1F`, not `0x1F`" )                     \
  X( NUMERIC_LOCAL_LABEL, 0014, ERROR, "numeric local labels do not exist; use `@`, `@+`, `@-`, or a name" )           \
  X( NUMBER_TOO_LARGE, 0016, ERROR, "this number is too large to be represented" )                                     \
  X( CONTROL_CHARACTER, 0015, ERROR, "control character {code:hex} in source" )                                        \
  X( EXPECTED_EXPRESSION, 0100, ERROR, "expected an expression" )                                                      \
  X( UNCLOSED_PARENTHESIS, 0101, ERROR, "expected `)` to close this parenthesis" )                                     \
  X( SHIFT_IN_PREFIX_POSITION, 0102, ERROR, "`<<` is a shift; the low byte of an operand is `<`" )                     \
  X( PARENTHESES_REQUIRED, 0103, ERROR, "`{outer}` and `{inner}` need parentheses to say which one binds first" )      \
  X( EXPRESSION_TOO_DEEP, 0104, ERROR, "this expression nests more than {limit} deep" )                                \
  X( STATEMENT_EXPECTED, 0110, ERROR, "`{token}` cannot begin a statement" )                                           \
  X( TRAILING_TOKENS, 0111, ERROR, "unexpected `{token}` after the end of the statement" )                             \
  X( INDENT_EXPECTED, 0112, ERROR, "`{name}` at column one defines a label; indent it to write an instruction" )       \
  X( LABEL_NAMED_LIKE_MNEMONIC, 0113, WARNING, "`{name}` at column one defines a label named like an instruction" )    \
  X( DIRECTION_IN_DEFINITION, 0114, ERROR, "a direction marks a reference, not a definition" )                         \
  X( LOCAL_LABEL_OUTSIDE_PROC, 0115, ERROR, "a local label has no scope outside a `.proc`" )                           \
  X( DECLARE_OUTSIDE_PROC, 0116, ERROR, "`.declare` says what a byte of a proc is, and this stands outside one" )      \
  X( DECLARE_NEEDS_TEMPORARY,                                                                                          \
     0117,                                                                                                             \
     ERROR,                                                                                                            \
     "`.declare` says what the `.ztemp` or `.temp` below it is, and this is not one" )                                 \
  X( DECLARE_NEEDS_KIND, 0118, ERROR, "`.declare` says `arg` or `ret`, found `{token}`" )                              \
  X( DECLARE_UNKNOWN_TYPE,                                                                                             \
     0119,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is not a type or a place; a declared byte is `u8`, `i8`, `u16`, `i16`, `bool` or `u8[N]`, or lies in "  \
     "`a`, `x`, `y` or `m`, a letter a byte from the low one" )                                                        \
  X( DECLARE_SECOND_RESULT, 0130, ERROR, "this `.proc` already declares its result, and a proc has one" )              \
  X( DECLARE_TWICE_ARGUMENT, 0139, ERROR, "this byte is already an argument, and a byte is one argument or none" )     \
  X( DECLARE_REGISTER_TAKES_NO_TEMPORARY,                                                                              \
     0189,                                                                                                             \
     ERROR,                                                                                                            \
     "this `.declare` keeps every byte in a register, and a `.ztemp` or `.temp` stands below it" )                     \
  X( DECLARE_REGISTER_TWICE, 0190, ERROR, "`{register}` already carries an argument of this proc" )                    \
  X( DECLARE_PLACE_REPEATS, 0191, ERROR, "`{place}` names one register for two bytes" )                                \
  X( UNKNOWN_DIRECTIVE, 0120, ERROR, "unknown directive `.{name}`" )                                                   \
  X( AMBIGUOUS_END_DIRECTIVE, 0121, ERROR, "`.end` does not say what it closes; write `.ends` or `.endp`" )            \
  X( EXPECTED_NAME, 0122, ERROR, "expected a name after `{after}`" )                                                   \
  X( UNKNOWN_SECTION_ATTRIBUTE, 0123, ERROR, "`{name}` is not a section attribute" )                                   \
  X( SECTION_NOT_CLOSED, 0124, ERROR, "this `.section` is never closed" )                                              \
  X( PROC_NOT_CLOSED, 0125, ERROR, "this `.proc` is never closed" )                                                    \
  X( UNMATCHED_SECTION_END, 0126, ERROR, "`{directive}` without an open `.section`" )                                  \
  X( UNMATCHED_PROC_END, 0127, ERROR, "`{directive}` without an open `.proc`" )                                        \
  X( SECTION_INSIDE_SECTION, 0128, ERROR, "a `.section` cannot open inside another; close the first one" )             \
  X( PROC_INSIDE_PROC, 0129, ERROR, "a `.proc` cannot open inside another" )                                           \
  X( DUPLICATE_SECTION_ATTRIBUTE, 0131, ERROR, "`{name}` gives a placement this `.section` already has" )              \
  X( DUPLICATE_PLACEMENT_QUALIFIER, 0132, ERROR, "`{name}` is already given for this placement" )                      \
  X( REPEATED_SECTION_ATTRIBUTE, 0133, ERROR, "`{name}` is already given for this `.section`" )                        \
  X( EXPECTED_BINDING, 0134, ERROR, "a slot is `pointer` or `vector`, and this is {name}" )                            \
  X( EXPECTED_PLACEMENT, 0135, ERROR, "a slot's placement is `zeropage` or `absolute`, and this is {name}" )           \
  X( TEMPORARY_NEEDS_NAME, 0136, ERROR, "`.ztemp` and `.temp` declare a variable and need its name at column one" )    \
  X( PROC_INSIDE_SECTION, 0137, ERROR, "a `.proc` is a section of its own and cannot open inside a `.section`" )       \
  X( OUTSIDE_SECTION,                                                                                                  \
     0138,                                                                                                             \
     ERROR,                                                                                                            \
     "`{what}` stands in no section; what has an address is written in a `.section` or a `.proc`" )                    \
  X( TEMPORARY_EXCLUDES, 0179, ERROR, "`temporary` does not go with `{other}`" )                                       \
  X( PROC_NOT_TEMPORARY, 0180, ERROR, "a `.proc` is code, and `temporary` is a reservation" )                          \
  X( READONLY_EXCLUDES, 0192, ERROR, "`readonly` does not go with `{other}`" )                                         \
  X( PROC_IS_READONLY,                                                                                                 \
     0193,                                                                                                             \
     WARNING,                                                                                                          \
     "a `.proc` holds code, which nothing writes unless a statement says so, and `readonly` changes nothing" )         \
  X( SOURCE_TAKES_PATH_AND_LINE,                                                                                       \
     0181,                                                                                                             \
     ERROR,                                                                                                            \
     "`.source` is followed by a quoted path, a comma and a line number, found `{token}`" )                            \
  X( SOURCE_LINE_FROM_ONE, 0182, ERROR, "a source line is counted from 1, and this is {line}" )                        \
  X( EXPECTED_PAYLOAD, 0183, ERROR, "a `.{encoding}` payload is written as a quoted literal, found `{token}`" )        \
  X( PAYLOAD_CHARACTER, 0184, ERROR, "`{character}` is not valid in a `.{encoding}` payload" )                         \
  X( PAYLOAD_PADDING, 0185, ERROR, "`=` pads the end of a `.base64` payload and stands nowhere else" )                 \
  X( PAYLOAD_NOT_WHOLE_BYTES,                                                                                          \
     0186,                                                                                                             \
     ERROR,                                                                                                            \
     "a `.{encoding}` payload takes {group} characters to the byte, and this one has {count}" )                        \
  X( PAYLOAD_NAMES_CHARSET, 0187, ERROR, "a payload is bytes and not text, so it names no character set" )             \
  X( CHARSET_NOT_CLOSED, 0150, ERROR, "this `.charset` is never closed" )                                              \
  X( UNMATCHED_CHARSET_END, 0151, ERROR, "`{directive}` without an open `.charset`" )                                  \
  X( EXPECTED_CHARSET_ENTRY, 0152, ERROR, "expected a character set entry, written `\"...\" = value`" )                \
  X( EMPTY_CHARSET_ENTRY, 0153, ERROR, "a character set entry maps at least one character" )                           \
  X( CHARSET_ENTRY_PREFIXED,                                                                                           \
     0154,                                                                                                             \
     ERROR,                                                                                                            \
     "the left side of an entry names no character set; the set is the one being declared" )                           \
  X( MACRO_NOT_AT_TOP_LEVEL, 0155, ERROR, "`.macro` cannot open inside a `.section`, a `.proc` or another `.macro`" )  \
  X( MACRO_NOT_CLOSED, 0156, ERROR, "this `.macro` is never closed" )                                                  \
  X( UNMATCHED_MACRO_END, 0157, ERROR, "`{directive}` without an open `.macro`" )                                      \
  X( NOT_IN_MACRO_BODY, 0158, ERROR, "{what} cannot stand in a macro body, which holds instructions and data" )        \
  X( NAMESPACE_NOT_AT_TOP_LEVEL, 0160, ERROR, "`.namespace` cannot open inside a `.section` or a `.macro`" )           \
  X( NAMESPACE_IN_PROC_HOLDS,                                                                                          \
     0188,                                                                                                             \
     ERROR,                                                                                                            \
     "a `.namespace` in a `.proc` holds sections, temporaries and constants, and `{what}` is none of them" )           \
  X( NAMESPACE_NOT_CLOSED, 0161, ERROR, "this `.namespace` is never closed" )                                          \
  X( UNMATCHED_NAMESPACE_END, 0162, ERROR, "`{directive}` without an open `.namespace`" )                              \
  X( NAMESPACE_END_INSIDE,                                                                                             \
     0163,                                                                                                             \
     ERROR,                                                                                                            \
     "`{directive}` closes a namespace while a `.section`, `.proc` or `.macro` is still open" )                        \
  X( CHARSET_IN_NAMESPACE, 0164, ERROR, "a `.charset` stands at the top level: a literal's prefix is one name" )       \
  X( EXPECTED_SELECT_COLON, 0165, ERROR, "expected `:` here, and the answer for a false condition after it" )          \
  X( CONDITIONAL_NOT_CLOSED, 0166, ERROR, "this `.if` is never closed" )                                               \
  X( UNMATCHED_CONDITIONAL_END, 0167, ERROR, "`{directive}` without an open `.if`" )                                   \
  X( ELSIF_AFTER_ELSE, 0168, ERROR, "`.elsif` cannot follow `.else`, which is the branch taken when none is" )         \
  X( PACK_NOT_LAST, 0171, ERROR, "`{name}...` takes every argument left, so it stands last" )                          \
  X( MATCH_OUTSIDE_BODY, 0172, ERROR, "`.match` stands in a macro body, which is where a pack is" )                    \
  X( CASE_OUTSIDE_MATCH, 0173, ERROR, "`.case` without an open `.match`" )                                             \
  X( UNMATCHED_MATCH_END, 0174, ERROR, "`{directive}` without an open `.match`" )                                      \
  X( MATCH_NOT_CLOSED, 0175, ERROR, "this `.match` is never closed" )                                                  \
  X( STATEMENT_BEFORE_CASE, 0176, ERROR, "a `.match` holds `.case`s, and this stands before the first" )               \
  X( SPREAD_NEEDS_NAME, 0177, ERROR, "`...` spreads a pack and follows its name, touching it" )                        \
  X( MATCH_WITHOUT_CASE, 0178, ERROR, "this `.match` has no `.case`, so nothing could ever fit" )                      \
  X( NOT_IN_CONDITIONAL,                                                                                               \
     0169,                                                                                                             \
     ERROR,                                                                                                            \
     "{what} cannot stand in a conditional branch, which holds instructions and data" )                                \
  X( LABEL_IN_CONDITIONAL,                                                                                             \
     0170,                                                                                                             \
     ERROR,                                                                                                            \
     "a label in a conditional branch is written `@name`: a branch defines no symbol, and two branches want one "      \
     "name" )                                                                                                          \
  X( NO_SUCH_ADDRESSING_MODE, 0140, ERROR, "there is no `(expression,{register})` addressing mode" )                   \
  X( PARENTHESES_ARE_GROUPING,                                                                                         \
     0141,                                                                                                             \
     WARNING,                                                                                                          \
     "these parentheses group rather than indirect; `(expression,x)` is the indirect form" )                           \
  X( OFF_UNKNOWN_CODE, 0143, ERROR, "`{code}` names no diagnostic" )                                                   \
  X( OFF_SILENCED_NOTHING, 0144, WARNING, "`.off {code}` silenced nothing: no such warning stood on the line below" )  \
  X( EXPECTED_INDEX_REGISTER, 0142, ERROR, "expected `x` or `y` after `,`, found `{token}`" )                          \
  X( UNKNOWN_MODULE, 1101, ERROR, "project refers to `{module}`, which names no module and no group" )                 \
  X( MODULE_NAME_COLLISION,                                                                                            \
     1102,                                                                                                             \
     ERROR,                                                                                                            \
     "`{module}` would name more than one file; naming modules is a Project file's business" )                         \
  X( NOT_A_PROJECT, 1103, ERROR, "`{path}` is not a project file, and a run is given nothing else" )                   \
  X( MODULE_NAMED_BY_PROJECT,                                                                                          \
     1104,                                                                                                             \
     NOTE,                                                                                                             \
     "a source file becomes a module where a project's `modules` block names it" )                                     \
  X( EXPECTED_CONSTANT_ENTRY, 1105, ERROR, "a constant is a name, `=` and a number, found `{token}`" )                 \
  X( CONSTANT_ALREADY_SET, 1106, ERROR, "the value of `{name}` is set more than once" )                                \
  X( PREVIOUS_CONSTANT, 1107, NOTE, "`{name}` was already set here" )                                                  \
  X( UNKNOWN_CONTAINER, 1108, ERROR, "`{name}` names no container; the tool writes {known}" )                          \
  X( CONTAINER_ALREADY_SET, 1109, ERROR, "the container is set more than once" )                                       \
  X( PREVIOUS_CONTAINER, 1140, NOTE, "the container was already set here" )                                            \
  X( CONTAINER_NOT_TAKEN, 1141, ERROR, "this machine does not take a `{name}`; it takes {taken}" )                     \
  X( CONTAINERS_DECLARED_HERE, 1142, NOTE, "the machine says what it takes here" )                                     \
  X( CONTAINER_TAKES_NO_FORMAT, 1216, ERROR, "a `{name}` is one thing and takes no format; only `car` names a board" ) \
  X( CARTRIDGE_WITHOUT_FORMAT,                                                                                         \
     1217,                                                                                                             \
     ERROR,                                                                                                            \
     "a `.car` is an image of one board and this says none; write `container car \"8k\"`, and the boards are "         \
     "{known}" )                                                                                                       \
  X( UNKNOWN_CARTRIDGE, 1218, ERROR, "`{name}` names no cartridge board; the boards are {known}" )                     \
  X( CARTRIDGE_FIXED_PART, 1219, ERROR, "a `{name}` cartridge is ROM at {range} and this machine has {found}" )        \
  X( CARTRIDGE_WINDOW,                                                                                                 \
     1220,                                                                                                             \
     ERROR,                                                                                                            \
     "a `{name}` cartridge switches banks into {range} and this machine declares no window of that one range" )        \
  X( CARTRIDGE_UNITS,                                                                                                  \
     1221,                                                                                                             \
     ERROR,                                                                                                            \
     "a `{name}` cartridge has {units:n} banks of storage and window `{window}` shows a set of {found:n}" )            \
  X( UNKNOWN_INTENT, 1143, ERROR, "`{name}` is not what a program is optimised for; the words are {known}" )           \
  X( INTENT_ALREADY_SET, 1144, ERROR, "what the program is optimised for is set more than once" )                      \
  X( PREVIOUS_INTENT, 1145, NOTE, "it was already set here" )                                                          \
  X( UNKNOWN_CPU, 1146, ERROR, "`{name}` names no processor; the words are {known}" )                                  \
  X( CPU_ALREADY_SET, 1149, ERROR, "the processor is set more than once" )                                             \
  X( PREVIOUS_CPU, 1152, NOTE, "the processor was already set here" )                                                  \
  X( UNKNOWN_PROJECT_BLOCK, 1110, ERROR, "`{name}` is not a project block" )                                           \
  X( PROJECT_BLOCK_NOT_CLOSED, 1111, ERROR, "this `{name}` block is never closed" )                                    \
  X( EXPECTED_BLOCK_OR_INCLUDE, 1112, ERROR, "expected a block or an `include`, found `{token}`" )                     \
  X( EXPECTED_BLOCK_BODY, 1113, ERROR, "expected `{{` to open the body of `{name}`" )                                  \
  X( EXPECTED_MODULE_ENTRY, 1114, ERROR, "a module is a quoted path, optionally followed by `as` and a name" )         \
  X( EXPECTED_ALIAS, 1115, ERROR, "expected a name after `as`" )                                                       \
  X( PATH_NAMES_CHARSET, 1116, ERROR, "a path is not translated, so it names no character set" )                       \
  X( EXPECTED_SEVERITY, 1117, ERROR, "expected `deny`, `allow` or `off`, found `{token}`" )                            \
  X( UNKNOWN_DIAGNOSTIC_CODE, 1118, ERROR, "`{code}` names no diagnostic" )                                            \
  X( UNKNOWN_FACT_SET, 1215, ERROR, "`{name}` names no set of facts; the sets are {sets}" )                            \
  X( SEVERITY_ALREADY_SET, 1119, ERROR, "the severity of `{code}` is set more than once" )                             \
  X( PREVIOUS_SEVERITY, 1120, NOTE, "`{code}` was already set here" )                                                  \
  X( INCLUDE_CYCLE, 1121, ERROR, "this include reaches a file that is already being read" )                            \
  X( CANNOT_READ_FILE, 1122, ERROR, "cannot read `{path}`" )                                                           \
  X( PROJECT_HAS_NO_MODULES, 1123, ERROR, "this project declares no modules" )                                         \
  X( EXPECTED_PHASE_NAME, 1125, ERROR, "`phase` is followed by the phase's name, found `{token}`" )                    \
  X( EXPECTED_PHASE_ENTRY, 1126, ERROR, "expected `needs`, `then` or `entry`, found `{token}`" )                       \
  X( EXPECTED_NAME_IN_LIST, 1127, ERROR, "expected a name after `{after}`" )                                           \
  X( EXPECTED_RESIDENT_ENTRY, 1128, ERROR, "a resident entry names a module or a group, found `{token}`" )             \
  X( EXPECTED_COMMA, 1129, ERROR, "expected `,` between `{previous}` and `{next}`" )                                   \
  X( UNKNOWN_PHASE, 1130, ERROR, "project refers to phase `{phase}`, which is not declared" )                          \
  X( ENTRY_ALREADY_SET, 1131, ERROR, "the entry phase is set more than once" )                                         \
  X( PREVIOUS_ENTRY, 1132, NOTE, "the entry phase was already set here" )                                              \
  X( PROJECT_HAS_NO_ENTRY, 1133, ERROR, "this project declares phases and no `entry`" )                                \
  X( MODULE_IN_NO_PHASE, 1134, WARNING, "module `{module}` is in no phase, so it is never in memory" )                 \
  X( EXPECTED_TRANSFORM_NAME, 1150, ERROR, "`transform` is followed by the transform's name, found `{token}`" )        \
  X( EXPECTED_QUALIFIED_NAME, 1151, ERROR, "a transform entry names a section as `module.section`, found `{token}`" )  \
  X( EXPECTED_TARGET_ENTRY,                                                                                            \
     1135,                                                                                                             \
     ERROR,                                                                                                            \
     "expected `region`, `register`, `units`, `window` or `containers`, found `{token}`" )                             \
  X( EXPECTED_VALUE, 1136, ERROR, "expected a value after `{after}`" )                                                 \
  X( VALUE_NAMES_SOMETHING, 1137, ERROR, "a value here is written out, and `{name}` names nothing" )                   \
  X( VALUE_HAS_NO_NUMBER, 1138, ERROR, "this does not come to a number" )                                              \
  X( EXPECTED_RANGE, 1139, ERROR, "a range is written `start .. end`" )                                                \
  X( PHASE_ENTRY_ALREADY_SET, 1147, ERROR, "the entry of phase `{phase}` is set more than once" )                      \
  X( PREVIOUS_PHASE_ENTRY, 1148, NOTE, "it was set here" )                                                             \
  X( EXPECTED_REGION_PROPERTY,                                                                                         \
     1155,                                                                                                             \
     ERROR,                                                                                                            \
     "a region's range is followed by `ram`, `register` or `reserved`, not by `{token}`" )                             \
  X( UNKNOWN_REGION_PROPERTY,                                                                                          \
     1156,                                                                                                             \
     ERROR,                                                                                                            \
     "`{word}` is not a region property; write `ram`, `rom`, `register` or `reserved`" )                               \
  X( REGION_NOT_A_RANGE,                                                                                               \
     1157,                                                                                                             \
     ERROR,                                                                                                            \
     "a region runs from a lower address to a higher one inside the address space, and this one does not" )            \
  X( REGION_ALREADY_NAMED, 1158, ERROR, "a region named `{name}` is already declared" )                                \
  X( PREVIOUS_REGION, 1159, NOTE, "`{name}` was declared here" )                                                       \
  X( REGISTER_NOT_AN_ADDRESS, 1160, ERROR, "a register is an address, and {value:hex} is not one" )                    \
  X( REGISTER_WIDTH, 1161, ERROR, "a register is one or two bytes, and {width} is neither" )                           \
  X( REGION_DECLARED_HERE, 1163, NOTE, "the region `{region}` is declared here" )                                      \
  X( EXPECTED_STORAGE_ENTRY, 1164, ERROR, "expected `units` or `size`, found `{token}`" )                              \
  X( STORAGE_SIZE_MISSING, 1165, ERROR, "storage has units by count and no `size`, and a unit holds something" )       \
  X( STORAGE_SIZE_TWICE, 1166, ERROR, "the size of a unit is set more than once" )                                     \
  X( PREVIOUS_STORAGE_SIZE, 1167, NOTE, "it was set here" )                                                            \
  X( STORAGE_UNITS_TWICE,                                                                                              \
     1168,                                                                                                             \
     ERROR,                                                                                                            \
     "the units are declared more than once; `units` names a set of the target or counts them" )                       \
  X( PREVIOUS_STORAGE_UNITS, 1169, NOTE, "they were declared here" )                                                   \
  X( TOO_MANY_UNITS, 1170, ERROR, "{count} units, and a unit is named in a byte" )                                     \
  X( UNIT_SIZE_NOT_A_SIZE,                                                                                             \
     1171,                                                                                                             \
     ERROR,                                                                                                            \
     "a unit holds between one byte and the address space, and {value:hex} is neither" )                               \
  X( GROUP_NAME_COLLISION,                                                                                             \
     1172,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` names a group and a module, and a name in a list has to mean one thing" )                               \
  X( MODULE_DECLARED_HERE, 1173, NOTE, "the module `{name}` is declared here" )                                        \
  X( GROUP_CYCLE, 1174, ERROR, "group `{group}` reaches itself here, so listing it would never end" )                  \
  X( EMPTY_GROUP, 1175, WARNING, "group `{group}` holds no module, so naming it adds nothing" )                        \
  X( GROUP_IS_NOT_A_MODULE, 1176, ERROR, "`{name}` is a group, and one module is expected here" )                      \
  X( EXPECTED_GROUP_ENTRY, 1177, ERROR, "a group entry names a module or a group, found `{token}`" )                   \
  X( EXPECTED_VIEWS, 1179, ERROR, "a window lists what it shows after `views`, found `{token}`" )                      \
  X( TARGET_NAME_TAKEN, 1180, ERROR, "`{name}` already names a {kind} of the target" )                                 \
  X( PREVIOUS_TARGET_NAME, 1181, NOTE, "it was declared here" )                                                        \
  X( WINDOW_NOT_A_RANGE,                                                                                               \
     1182,                                                                                                             \
     ERROR,                                                                                                            \
     "a window's range runs from a lower address to a higher one inside the address space, and this one does not" )    \
  X( WINDOW_STATE_REPEATED, 1183, ERROR, "`{state}` is listed more than once among what `{window}` shows" )            \
  X( WINDOW_BASE_UNKNOWN,                                                                                              \
     1184,                                                                                                             \
     ERROR,                                                                                                            \
     "`{base}` is not a named state of window `{window}`; a base is a named state, never a unit set" )                 \
  X( STORAGE_UNITS_UNKNOWN, 1185, ERROR, "`{name}` is not a unit set of the target" )                                  \
  X( STORAGE_UNITS_NOT_SHOWN, 1186, ERROR, "no window shows `{name}`, so what one of its units holds is unknown" )     \
  X( WINDOW_SIZES_DIFFER,                                                                                              \
     1187,                                                                                                             \
     ERROR,                                                                                                            \
     "windows `{window}` and `{other}` both show `{name}` and differ in size, so a unit has no one size" )             \
  X( STORAGE_SIZE_DERIVED, 1188, ERROR, "storage names a unit set, and what a unit holds is its window's size" )       \
  X( UNIT_SET_EMPTY, 1189, ERROR, "a unit set holds at least one unit" )                                               \
  X( EXPECTED_PANES_HEADER, 1190, ERROR, "`panes` is followed by `in` and a window, found `{token}`" )                 \
  X( PANES_WINDOW_UNKNOWN, 1191, ERROR, "`{name}` is not a window of the target" )                                     \
  X( PANES_STATE_UNKNOWN, 1192, ERROR, "`{state}` is not a named state of window `{window}`" )                         \
  X( PANE_FAMILY_COUNT, 1193, ERROR, "a family holds between 1 and 255 panes, and {count} is neither" )                \
  X( PANES_WINDOW_HAS_NO_UNITS,                                                                                        \
     1194,                                                                                                             \
     ERROR,                                                                                                            \
     "window `{window}` shows no unit set, so a pane in it is pinned to a named state: write `= STATE`" )              \
  X( PANE_FAMILY_PINNED, 1195, ERROR, "a family takes consecutive banks, and `{pane}` is pinned to one state" )        \
  X( EXPECTED_PANE_ENTRY, 1196, ERROR, "expected a pane's name, found `{token}`" )                                     \
  X( EXPECTED_BASE_FORM, 1197, ERROR, "`base` is written `base WINDOW = STATE`, found `{token}`" )                     \
  X( BASE_WINDOW_UNKNOWN, 1198, ERROR, "`{name}` is not a window of the target" )                                      \
  X( BASE_STATE_UNKNOWN, 1199, ERROR, "`{state}` is not a named state of window `{window}`" )                          \
  X( BASE_TWICE, 1200, ERROR, "phase `{phase}` gives window `{window}` its base more than once" )                      \
  X( BASES_DISAGREE,                                                                                                   \
     1201,                                                                                                             \
     ERROR,                                                                                                            \
     "the groups phase `{phase}` needs give window `{window}` different bases, `{state}` and `{other}`" )              \
  X( BASE_GIVEN_HERE, 1202, NOTE, "`{state}` is given here" )                                                          \
  X( GENERATOR_UNKNOWN, 1203, ERROR, "`{name}` is no generator; `binary` reads a file" )                               \
  X( GENERATOR_ARGUMENT_UNKNOWN, 1204, ERROR, "`{name}` is no argument of this generator" )                            \
  X( GENERATOR_ARGUMENT_TWICE, 1205, ERROR, "this argument is given more than once" )                                  \
  X( GENERATOR_ARGUMENT_KIND, 1206, ERROR, "`{name}` takes {wanted}, and this is {given}" )                            \
  X( GENERATOR_NEEDS_FILE, 1207, ERROR, "`{generator}` names the file it works from first, as a quoted path" )         \
  X( GENERATOR_DESCRIPTION, 1208, ERROR, "what `{generator}` produced cannot be emitted: {where}" )                    \
  X( EXPECTED_CALL_END, 1209, ERROR, "a generator's arguments are closed by `)`, found `{token}`" )                    \
  X( SCRIPT_FAILED, 1210, ERROR, "`{path}` failed: {message}" )                                                        \
  X( SCRIPT_BUDGET, 1211, ERROR, "`{path}` did not finish within the work a generator is given" )                      \
  X( SCRIPT_REPORTED_ERROR, 1212, ERROR, "{module}: {message}" )                                                       \
  X( SCRIPT_REPORTED_WARNING, 1213, WARNING, "{module}: {message}" )                                                   \
  X( SCRIPT_REPORTED_AT, 1214, NOTE, "the generator said so at {where}" )                                              \
  X( EXPECTED_GROUP_NAME, 1178, ERROR, "`group` is followed by the group's name, found `{token}`" )                    \
  X( DUPLICATE_SYMBOL, 2201, ERROR, "symbol `{symbol}` is defined more than once in phase `{phase}`" )                 \
  X( PREVIOUS_DEFINITION, 2202, NOTE, "previous definition of `{symbol}` is here" )                                    \
  X( SYMBOL_REDEFINED, 2203, ERROR, "`{symbol}` is already defined in this module" )                                   \
  X( EXPORT_OF_UNDEFINED_SYMBOL, 2204, ERROR, "`{symbol}` is exported but not defined in this module" )                \
  X( DUPLICATE_EXPORT, 2205, ERROR, "`{symbol}` is exported by more than one module" )                                 \
  X( UNKNOWN_SYMBOL, 2206, ERROR, "`{symbol}` is not defined" )                                                        \
  X( CONSTANT_CYCLE, 2207, ERROR, "`{symbol}` is defined in terms of itself" )                                         \
  X( EXPECTED_INTEGER, 2220, ERROR, "expected an integer here, and this is {type}" )                                   \
  X( ADDRESSES_NOT_ADDABLE, 2221, ERROR, "two addresses cannot be added; `a - b` is the distance between them" )       \
  X( ADDRESS_SUBTRACTED_FROM_INTEGER, 2222, ERROR, "an address cannot be subtracted from a number" )                   \
  X( ADDRESSES_NOT_RELATED,                                                                                            \
     2223,                                                                                                             \
     ERROR,                                                                                                            \
     "these addresses are not in one section, so this is a property of the layout rather than of the program" )        \
  X( NOT_A_SECTION, 2224, ERROR, "only a label reaches the attributes of its section, and this is {type}" )            \
  X( DATA_ITEM_TYPE, 2225, ERROR, "`.{directive}` takes {allowed}, and this is {type}" )                               \
  X( OPERAND_TYPE, 2226, ERROR, "an operand is an address or a number, and this is {type}" )                           \
  X( NO_SUCH_ATTRIBUTE, 2227, ERROR, "`{name}` is not an attribute of a section" )                                     \
  X( SELECT_ARMS_DIFFER,                                                                                               \
     2228,                                                                                                             \
     ERROR,                                                                                                            \
     "a conditional chooses between two of one type, and these are {type} and {other}" )                               \
  X( SELECT_ARMS_UNRELATED,                                                                                            \
     2229,                                                                                                             \
     ERROR,                                                                                                            \
     "a conditional between addresses of two sections has no section of its own, and subtraction and width need "      \
     "one" )                                                                                                           \
  X( NOT_A_CHARSET, 2230, ERROR, "`{symbol}` is not a character set" )                                                 \
  X( CHARACTER_NOT_IN_CHARSET, 2231, ERROR, "`{charset}` does not map `{character}`" )                                 \
  X( CHARSET_CYCLE, 2232, ERROR, "character set `{charset}` is derived from itself" )                                  \
  X( DUPLICATE_CHARSET_ENTRY, 2233, ERROR, "`{character}` is already mapped by this character set" )                   \
  X( CHARSET_RUN_PAST_BYTE, 2234, ERROR, "a character set maps to bytes, and this run covers {start} to {end}" )       \
  X( CHARSET_VALUE_NOT_DECLARED,                                                                                       \
     2235,                                                                                                             \
     ERROR,                                                                                                            \
     "a character set is resolved before any size or address exists, so this needs a value written in the source" )    \
  X( CHARSET_NOT_A_VALUE, 2236, ERROR, "`{symbol}` is a character set, and a character set has no value" )             \
  X( NON_ASCII_WITHOUT_CHARSET, 2237, ERROR, "`{character}` is not ASCII and this literal names no character set" )    \
  X( TRANSITION_TO_UNKNOWN_PHASE, 2240, ERROR, "`{phase}` is not a phase of this project" )                            \
  X( TRANSITION_WITHOUT_EDGE,                                                                                          \
     2241,                                                                                                             \
     ERROR,                                                                                                            \
     "this transition to `{to}` stands in code present in phase `{from}`, which has no `then {to}`" )                  \
  X( TOO_MANY_PHASES, 2242, ERROR, "the target numbers phases in a byte, and this project has {count:n}" )             \
  X( TRANSITION_LOADS_TOO_MUCH,                                                                                        \
     2243,                                                                                                             \
     ERROR,                                                                                                            \
     "the transition from `{from}` to `{to}` loads {count:n} sections, and a frame counts them in a byte" )            \
  X( EDGE_NOT_TAKEN,                                                                                                   \
     2246,                                                                                                             \
     WARNING,                                                                                                          \
     "phase `{from}` declares `then {to}`, and no `.transition {to}` is present in it, so the edge is taken by "       \
     "nothing" )                                                                                                       \
  X( NO_SUCH_TRANSFORM, 2244, ERROR, "`{transform}` is not a format the tool can encode" )                             \
  X( NO_SUCH_SECTION, 2245, ERROR, "module `{module}` has no label named `{section}`, and a section is named by one" ) \
  X( TRANSFORM_SECTION_TWICE,                                                                                          \
     2248,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` names the section `{previous}` already lists, and a section is transformed once" )                   \
  X( SECTION_WITHOUT_LABEL, 2273, ERROR, "this section holds no label, and a section is named by one" )                \
  X( NOT_A_SLOT, 2250, ERROR, "`{symbol}` is not a slot" )                                                             \
  X( IMPLEMENTATION_IN_PANE,                                                                                           \
     2255,                                                                                                             \
     ERROR,                                                                                                            \
     "`{symbol}` fills slot `{slot}` from pane `{pane}`, and the cell holds an address but not the state that shows "  \
     "it" )                                                                                                            \
  X( IMPLEMENTATION_NOT_HERE, 2251, ERROR, "`{symbol}` is not a label or a section of this module" )                   \
  X( SLOT_IMPLEMENTED_TWICE,                                                                                           \
     2252,                                                                                                             \
     ERROR,                                                                                                            \
     "`{slot}` has two implementations live in phase `{phase}`: `{symbol}` and `{other}`" )                            \
  X( OTHER_IMPLEMENTATION, 2253, NOTE, "`{symbol}` implements it here" )                                               \
  X( TOO_MANY_SLOTS,                                                                                                   \
     2254,                                                                                                             \
     ERROR,                                                                                                            \
     "a frame counts cell writes in a byte, so a program has at most 255 slots, and this one has {count:n}" )          \
  X( SECTION_HAS_NO_PAYLOAD,                                                                                           \
     2247,                                                                                                             \
     WARNING,                                                                                                          \
     "`{module}.{section}` is never loaded by a transition, so nothing transforms it" )                                \
  X( NO_SUCH_LOCAL_LABEL, 2210, ERROR, "no definition of `@{name}` matches this reference" )                           \
  X( AMBIGUOUS_LOCAL_LABEL,                                                                                            \
     2211,                                                                                                             \
     ERROR,                                                                                                            \
     "`@{name}` has more than one definition here; write `@+{name}` or `@-{name}`" )                                   \
  X( ANONYMOUS_LABEL_NEEDS_DIRECTION, 2212, ERROR, "an anonymous local label is reached with `@+` or `@-`" )           \
  X( LOCAL_LABEL_REDEFINED, 2213, ERROR, "`{name}` is already a label of this macro body" )                            \
  X( LABEL_IS_A_PARAMETER, 2214, ERROR, "`{name}` is a parameter of this macro, and a label cannot take its name" )    \
  X( MACRO_NAMED_LIKE_MNEMONIC, 2300, ERROR, "`{name}` is an instruction, and a macro cannot take its name" )          \
  X( MACRO_PARAMETER_REPEATED, 2301, ERROR, "parameter `{name}` is given twice" )                                      \
  X( MACRO_PARAMETER_SHADOWS, 2302, ERROR, "parameter `{name}` has the name of a symbol this module defines" )         \
  X( NOT_INSTRUCTION_NOR_MACRO, 2303, ERROR, "`{name}` is neither an instruction nor a macro in scope" )               \
  X( INSTRUCTION_GIVEN_LIST,                                                                                           \
     2304,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is an instruction and takes one operand; an index register is `x` or `y`" )                             \
  X( MACRO_ARGUMENT_SHAPE,                                                                                             \
     2305,                                                                                                             \
     ERROR,                                                                                                            \
     "a macro's argument is an expression: no `#`, no `,x` or `,y`, and no parenthesis that indirects" )               \
  X( MACRO_ARITY, 2306, ERROR, "`{name}` takes {expected} arguments and was given {given}" )                           \
  X( MACRO_NOT_A_VALUE, 2308, ERROR, "`{symbol}` is a macro and stands where a value is expected" )                    \
  X( DRIVER_NOT_A_MACRO,                                                                                               \
     2309,                                                                                                             \
     ERROR,                                                                                                            \
     "`{label}` is a {kind}, and a role is a macro the tool expands where it uses the role" )                          \
  X( NAMESPACE_RESERVED, 2310, ERROR, "`nga` is the tool's namespace, and no module defines it" )                      \
  X( NO_SUCH_NAMESPACE, 2311, ERROR, "`{space}` is not a namespace" )                                                  \
  X( NO_SUCH_TOOL_NAME, 2312, ERROR, "`nga.{name}` names nothing the tool defines" )                                   \
  X( ROLE_WITHOUT_DRIVER, 2313, ERROR, "`nga.{name}` is used, and no module declares a driver" )                       \
  X( TOOL_NAME_COLLISION, 2314, ERROR, "`{symbol}` is a name the tool defines, and a module may not define it" )       \
  X( TOOL_NAME_DEFINED_HERE, 2315, NOTE, "the tool defines `{symbol}` here" )                                          \
  X( NAMESPACE_NOT_A_VALUE, 2316, ERROR, "`{name}` is a namespace and stands where a value is expected" )              \
  X( NAMESPACE_NAME_COLLISION, 2317, ERROR, "`{symbol}` is also a namespace, and one name cannot be both" )            \
  X( NAME_IS_AN_ATTRIBUTE, 2319, ERROR, "`{name}` is an attribute of a section, and a name in a proc cannot take it" ) \
  X( EXPORT_FROM_INSIDE_PROC,                                                                                          \
     2320,                                                                                                             \
     ERROR,                                                                                                            \
     "`{symbol}` is inside proc `{proc}`, and nothing outside the proc can reach it" )                                 \
  X( CONDITION_NOT_DECLARED,                                                                                           \
     2321,                                                                                                             \
     ERROR,                                                                                                            \
     "a condition decides how many bytes there are, so it is a declared value: nothing naming a position or a size "   \
     "may stand in it" )                                                                                               \
  X( MACRO_EXPANDS_WITHOUT_END,                                                                                        \
     2322,                                                                                                             \
     ERROR,                                                                                                            \
     "this expansion is {depth} uses deep at `{name}` and shows no sign of ending" )                                   \
  X( EXPANSION_BEGAN_HERE, 2323, NOTE, "the expansion began at this use" )                                             \
  X( MACRO_EXPANDS_TOO_MUCH,                                                                                           \
     2332,                                                                                                             \
     ERROR,                                                                                                            \
     "this expansion has produced {count:n} statements, more than the target has bytes to put them in" )               \
  X( EXPANDED_EXPRESSION_TOO_DEEP,                                                                                     \
     2333,                                                                                                             \
     ERROR,                                                                                                            \
     "with its arguments in place this expression nests {depth} deep, more than {limit}" )                             \
  X( NOT_A_MACRO, 2324, ERROR, "`{name}` is a {kind}, and stands where an instruction or a macro is expected" )        \
  X( SPREAD_NOT_A_PACK, 2325, ERROR, "`{name}` is not a pack, and only a pack spreads" )                               \
  X( MATCH_NOT_A_PACK, 2326, ERROR, "`{name}` is not a pack, and `.match` counts a pack's elements" )                  \
  X( CASE_NAME_SHADOWS, 2327, ERROR, "`{name}` is already bound around this `.case`, and a case hides nothing" )       \
  X( NO_CASE_FITS, 2328, ERROR, "`{name}` was given {count} elements, and no `.case` of `.match {name}` fits that" )   \
  X( MATCH_HERE, 2329, NOTE, "the `.match` is here" )                                                                  \
  X( CASE_NEVER_FITS, 2330, WARNING, "a `.case` above this one fits everything this one does, so it is never taken" )  \
  X( MACRO_ARITY_AT_LEAST, 2331, ERROR, "`{name}` takes at least {expected} arguments and was given {given}" )         \
  X( LOCAL_LABEL_ACROSS_BRANCHES,                                                                                      \
     2215,                                                                                                             \
     ERROR,                                                                                                            \
     "no definition of `@{name}` outside this branch matches, and one in another branch may not be there at all" )     \
  X( WITH_NEEDS_CODE,                                                                                                  \
     3001,                                                                                                             \
     ERROR,                                                                                                            \
     "`.with` applies to the instruction or macro use on the line below, and this is not one" )                        \
  X( WITH_FORM,                                                                                                        \
     3002,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is a {kind}, and `.with` takes a pane, a family with `, x`, a window with `, x` or a window `= "        \
     "STATE`" )                                                                                                        \
  X( WITH_STATE_UNKNOWN, 3003, ERROR, "`{state}` is not a named state of window `{window}`" )                          \
  X( WITH_DOES_NOTHING, 3005, WARNING, "`{window}` shows this state already, so this `.with` changes nothing" )        \
  X( WITH_NESTED_SAME_WINDOW,                                                                                          \
     3006,                                                                                                             \
     ERROR,                                                                                                            \
     "`{window}` is shown by a `.with` on this statement already, and a window shows one state at a time" )            \
  X( WITH_RETURNS, 3007, ERROR, "a statement under `.with` may not `{mnemonic}`: it would leave the window as shown" ) \
  X( WITH_REACHES_OTHER_PANE,                                                                                          \
     3008,                                                                                                             \
     ERROR,                                                                                                            \
     "what this statement reaches names `{symbol}`, in pane `{pane}`, which this `.with` does not show" )              \
  X( NAMED_HERE, 3009, NOTE, "`{symbol}` is named here" )                                                              \
  X( TAKING_NEEDS_STATEMENT,                                                                                           \
     3012,                                                                                                             \
     ERROR,                                                                                                            \
     "`{directive}` applies to the instruction or data on the line below, and this is not one" )                       \
  X( OWN_AND_ROOT,                                                                                                     \
     3013,                                                                                                             \
     ERROR,                                                                                                            \
     "`.own` and `.root` on one statement: an address is followed by this section or by the hardware, not both" )      \
  X( ROOT_TAKES_NO_NAME, 3014, ERROR, "`.root` hands the address to the hardware, and names nobody" )                  \
  X( WITH_FROM_OWN_WINDOW,                                                                                             \
     3015,                                                                                                             \
     ERROR,                                                                                                            \
     "this section is in pane `{pane}` of window `{window}`, and code in a pane cannot switch its own window: the "    \
     "switch would take the code with it; a proc in fixed, `.proc NAME, under {pane}`, does it" )                      \
  X( UNDER_ATTRIBUTE, 3016, ERROR, "`under {name}` {reason}" )                                                         \
  X( UNDER_NOT_SHOWN,                                                                                                  \
     3017,                                                                                                             \
     ERROR,                                                                                                            \
     "`{symbol}` runs under pane `{pane}`, which must be shown where it is called, and here it is not" )               \
  X( WITH_BASES_DIFFER,                                                                                                \
     3010,                                                                                                             \
     ERROR,                                                                                                            \
     "the phases this code is in give window `{window}` different bases, `{state}` in `{phase}` and `{other}` in "     \
     "`{otherPhase}`, so `.with` cannot show one base again; split the module" )                                       \
  X( STATE_NOT_SHOWN,                                                                                                  \
     3011,                                                                                                             \
     ERROR,                                                                                                            \
     "`{symbol}` is in state `{state}` of window `{window}`, which phase `{phase}` does not show here" )               \
  X( CROSS_VIEW_REFERENCE,                                                                                             \
     2412,                                                                                                             \
     ERROR,                                                                                                            \
     "`{symbol}` is in pane `{pane}`, and this code does not run with that pane shown: put the statement "             \
     "under a `.with`, or the code in a Proc declared `under` it" )                                                    \
  X( PANE_UNKNOWN, 2418, ERROR, "`{name}` is not a pane the Project declares" )                                        \
  X( PANE_SECTION_MOVABLE,                                                                                             \
     2419,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` is in pane `{pane}`, and a pane's section stands in one bank at one address; it cannot be movable" ) \
  X( PANE_ZERO_PAGE, 2420, ERROR, "`{section}` is in pane `{pane}`, and a window covers no zero page" )                \
  X( PANE_NOT_A_NUMBER,                                                                                                \
     2421,                                                                                                             \
     ERROR,                                                                                                            \
     "a pane's value is the state the solver chose, and this is arithmetic on it; a pane stands as an immediate, a "   \
     "data item or a role's argument" )                                                                                \
  X( PANE_MEMBER_OUT_OF_RANGE, 2422, ERROR, "`{name}` has {count} members, and {index} is not one of them" )           \
  X( PANE_NOT_AN_ADDRESS, 2423, ERROR, "a pane is a state of a window and not an address, and this operand is one" )   \
  X( VIEWS_SHARE_WINDOW,                                                                                               \
     2413,                                                                                                             \
     NOTE,                                                                                                             \
     "`{fromView}` and `{toView}` share window {windowStart:hex}-{windowEnd:hex} and "                                 \
     "are never visible together" )                                                                                    \
  X( CROSS_RESIDENCY_REFERENCE,                                                                                        \
     2414,                                                                                                             \
     ERROR,                                                                                                            \
     "reference to `{symbol}`, which is not in memory in phase `{phase}` while `{section}` is" )                       \
  X( SLOT_NOT_IMPLEMENTED_IN_PHASE,                                                                                    \
     2415,                                                                                                             \
     ERROR,                                                                                                            \
     "reference to `{slot}`, which has no implementation in phase `{phase}` while `{section}` is" )                    \
  X( PROC_ENTERED_INSIDE, 2416, ERROR, "`{symbol}` is inside proc `{proc}`, which is reached only through its name" )  \
  X( BRANCH_ACROSS_SECTIONS,                                                                                           \
     2417,                                                                                                             \
     ERROR,                                                                                                            \
     "a branch to `{symbol}` in `{section}`: a branch reaches its own proc or one chained to it by `then`; use "       \
     "`jmp`" )                                                                                                         \
  X( DISPATCH_LEAVES_SECTION,                                                                                          \
     2424,                                                                                                             \
     ERROR,                                                                                                            \
     "a dispatch to `{symbol}`, which is not a position of `{section}`: a dispatch goes where it stands, and `.own` "  \
     "is how a table reaches other sections" )                                                                         \
  X( DISPATCH_TOO_MANY_TARGETS, 2425, ERROR, "a dispatch of {count} targets, and an index reaches {most}" )            \
  X( THEN_NOT_A_PROC, 2260, ERROR, "`{name}` is not a proc of this module, and `then` names one" )                     \
  X( THEN_ALREADY_FOLLOWED, 2261, ERROR, "`{name}` already follows `{previous}`, and can follow only one proc" )       \
  X( THEN_PLACED, 2262, ERROR, "`{name}` follows `{previous}`, and cannot also be pinned or aligned" )                 \
  X( THEN_MOVABILITY, 2263, ERROR, "`{name}` follows `{previous}`, so both are movable or neither is" )                \
  X( THEN_CYCLE, 2264, ERROR, "`{name}` follows itself through `then`" )                                               \
  X( THEN_PANE, 2265, ERROR, "`{name}` follows `{previous}`, so both are in one pane or neither is" )                  \
  X( TRANSFORM_LABEL_NOT_HERE,                                                                                         \
     2274,                                                                                                             \
     ERROR,                                                                                                            \
     "`{label}` is not defined in this module, and a decoder is one of its labels" )                                   \
  X( TRANSFORM_NOT_A_LABEL, 2275, ERROR, "`{label}` is a {kind}, and a decoder is entered at a label" )                \
  X( UNKNOWN_FORMAT,                                                                                                   \
     2276,                                                                                                             \
     ERROR,                                                                                                            \
     "`{format}` is not a format the tool can encode, so nothing produces what this decodes" )                         \
  X( FORMAT_ALREADY_DECODED, 2277, ERROR, "`{format}` already has a decoder, and a format has one" )                   \
  X( DECODER_IS_HERE, 2278, NOTE, "the decoder of `{format}` is declared here" )                                       \
  X( NO_COPY_DECODER,                                                                                                  \
     2279,                                                                                                             \
     ERROR,                                                                                                            \
     "no module declares a decoder for `copy`, and every Transition copies; the storage driver declares one "          \
     "with `.transform copy`" )                                                                                        \
  X( NO_DECODER, 2280, ERROR, "no module declares a decoder for `{transform}`" )                                       \
  X( NO_DRIVER,                                                                                                        \
     2281,                                                                                                             \
     ERROR,                                                                                                            \
     "a `.transition` is taken and no module declares the storage driver; one module declares it with "                \
     "`.driver`, and a machine variant usually lists that module" )                                                    \
  X( SECOND_DRIVER, 2282, ERROR, "this module declares the storage driver, and `{module}` already does" )              \
  X( DRIVER_IS_HERE, 2283, NOTE, "the driver is declared here" )                                                       \
  X( UNKNOWN_DRIVER_ROLE,                                                                                              \
     2284,                                                                                                             \
     ERROR,                                                                                                            \
     "`{role}` is not a role of the driver; the roles are `open`, `read`, `stream`, `show` and `showAt`" )             \
  X( DRIVER_ROLE_MISSING, 2285, ERROR, "the driver declares no `{role}`, and the routine calls it" )                   \
  X( DRIVER_LABEL_NOT_HERE, 2286, ERROR, "`{label}` is not defined in this module, and a role is one of its labels" )  \
  X( DRIVER_NOT_RESIDENT, 2289, ERROR, "the driver is present in some phases only, and every Transition calls it" )    \
  X( DECODER_NOT_RESIDENT,                                                                                             \
     2290,                                                                                                             \
     ERROR,                                                                                                            \
     "this decoder's module is present in some phases only, and any Transition may call it" )                          \
  X( DRIVER_ROLE_REPEATED, 2291, ERROR, "`{role}` is declared more than once" )                                        \
  X( DRIVER_ROLE_NAMES, 2293, ERROR, "`{role}` takes {count} names, and {given} are given" )                           \
  X( DRIVER_WINDOW_UNKNOWN, 2294, ERROR, "`{window}` is not a window of the target" )                                  \
  X( DRIVER_WINDOW_ROLE_MISSING,                                                                                       \
     2295,                                                                                                             \
     ERROR,                                                                                                            \
     "the driver declares no `{role}` for window `{window}`, and the target declares the window" )                     \
  X( DRIVER_STREAM_MISSING,                                                                                            \
     2296,                                                                                                             \
     ERROR,                                                                                                            \
     "storage is the units of `{name}`, and the driver names no `stream` to read them through" )                       \
  X( STREAM_NOT_STORAGE,                                                                                               \
     2297,                                                                                                             \
     ERROR,                                                                                                            \
     "the stream reads through `{window}`, which does not show the units storage is" )                                 \
  X( SHOW_NEEDS_WINDOW, 2298, ERROR, "`nga.{role}` names a window first, and this does not" )                          \
  X( WINDOW_NOT_A_VALUE, 2299, ERROR, "`{symbol}` names a window of the target, and a window is not a value" )         \
  X( AS_ATTRIBUTE, 2287, ERROR, "`as {name}` {reason}" )                                                               \
  X( AS_WITH_SIGNATURE,                                                                                                \
     2288,                                                                                                             \
     ERROR,                                                                                                            \
     "`{proc}` is `as {type}`, so its arguments are `{type}`'s temporaries, and it declares {what} of its own" )       \
  X( REGION_NAME_COLLISION,                                                                                            \
     2272,                                                                                                             \
     ERROR,                                                                                                            \
     "`{symbol}` names a region, a unit set or a window of the target, and a module cannot define it" )                \
  X( TEMPORARY_HOLDS_BYTES, 2271, ERROR, "a temporary section holds reservations only, and this is not one" )          \
  X( DECLARED_TYPE_BYTES, 2266, ERROR, "`{type}` is a {want}-byte type, and this reserves {got}" )                     \
  X( DECLARED_PLACE_BYTES, 2334, ERROR, "`{place}` keeps {want} of its bytes here, and this reserves {got}" )          \
  X( RESERVE_IN_PROC,                                                                                                  \
     2270,                                                                                                             \
     ERROR,                                                                                                            \
     "a proc holds code, and a reservation is not code; a variable is a `.ztemp` or lives in a `.section`" )           \
  X( NO_SUCH_MODE, 5102, ERROR, "`{mnemonic}` has no {mode} form" )                                                    \
  X( INSTRUCTION_NEEDS_CPU, 5107, ERROR, "`{mnemonic}` is a `{needs}` instruction, and this target is a `{cpu}`" )     \
  X( PLACEMENT_CLASS_UNKNOWN,                                                                                          \
     5103,                                                                                                             \
     ERROR,                                                                                                            \
     "the placement class of this operand cannot be determined, so its width cannot be either" )                       \
  X( SIZE_CYCLE, 5104, ERROR, "this size depends on itself" )                                                          \
  X( RESERVATION_NOT_A_COUNT, 5105, ERROR, "a reservation of {size:n} bytes is not a reservation" )                    \
  X( ADDRESS_OUT_OF_RANGE, 5106, ERROR, "{value:hex} is not an address" )                                              \
  X( SECTION_OVERLAP, 5201, ERROR, "`{section}` overlaps `{other}` at {address:hex}" )                                 \
  X( NO_ROOM, 5202, ERROR, "there is no room for `{section}`, which needs {size:n} bytes" )                            \
  X( PIN_NOT_AN_ADDRESS, 5203, ERROR, "a section cannot be pinned at {address:hex}" )                                  \
  X( ZERO_PAGE_DOES_NOT_FIT, 5204, ERROR, "`{section}` is zero page and does not fit below $100" )                     \
  X( NO_LAYOUT, 5206, ERROR, "no layout satisfies every constraint at once" )                                          \
  X( CANNOT_BE_KEPT_APART,                                                                                             \
     5207,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` and `{other}` cannot be kept apart: no layout holds both without one lying over the other" )         \
  X( SECTION_IS_HERE, 5208, NOTE, "`{section}` is declared here" )                                                     \
  X( EXPLANATION_NOT_MINIMAL,                                                                                          \
     5209,                                                                                                             \
     WARNING,                                                                                                          \
     "the search stopped before it could show that fewer pairs would do; the ones named suffice" )                     \
  X( PAYLOAD_PINNED_IN_WINDOW,                                                                                         \
     5205,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` is loaded from storage, and cannot live where the stream reads through, at {address:hex}" )          \
  X( NO_STORAGE, 5222, ERROR, "there is no storage for `{section}`, which needs {size:n} bytes" )                      \
  X( ROOT_EVICTED,                                                                                                     \
     5224,                                                                                                             \
     WARNING,                                                                                                          \
     "`{section}` is `root` and holds no bytes to restore, and is evicted in phase `{gap}` between `{before}` and "    \
     "`{after}`, which both need it" )                                                                                 \
  X( NO_BANKS, 5223, NOTE, "the target has no storage; a `storage` block is where its units are named" )               \
  X( PANE_NO_BANK,                                                                                                     \
     5225,                                                                                                             \
     ERROR,                                                                                                            \
     "pane `{pane}` takes {size:n} bytes in phase `{phase}`, and a bank of `{set}` holds {available:n}" )              \
  X( PANE_FAMILY_EXCEEDS_SET, 5229, ERROR, "family `{pane}` needs {count} banks in a row, and `{set}` has {banks}" )   \
  X( PIN_OUTSIDE_PANE_WINDOW,                                                                                          \
     5226,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` is pinned at {address:hex}, outside the window of pane `{pane}`" )                                   \
  X( PIN_UNDER_WITH,                                                                                                   \
     5228,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` is pinned at {address:hex}, inside window `{window}`, which code reached under a `.with` on it "     \
     "would not see" )                                                                                                 \
  X( PANE_STATE_PAYLOAD,                                                                                               \
     5227,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` has bytes and is in pane `{pane}`, which is pinned to a named state: no Container fills "            \
     "one, so a pane pinned to a state holds only sections that reserve" )                                             \
  X( LAYOUT_OVERLAP,                                                                                                   \
     5240,                                                                                                             \
     ERROR,                                                                                                            \
     "the layout has `{section}` and `{other}` sharing {address:hex}, and phase `{phase}` holds both" )                \
  X( LAYOUT_PIN_MOVED, 5241, ERROR, "the layout puts `{section}` at {address:hex}, and it is pinned at {pinned:hex}" ) \
  X( LAYOUT_UNDER_WITH,                                                                                                \
     5252,                                                                                                             \
     ERROR,                                                                                                            \
     "the layout puts `{section}` at {address:hex}, inside window `{window}`, which it runs under a `.with` on" )      \
  X( LAYOUT_NOT_ZERO_PAGE,                                                                                             \
     5242,                                                                                                             \
     ERROR,                                                                                                            \
     "the layout puts zero page `{section}` at {address:hex}, and it reaches {last:hex}" )                             \
  X( LAYOUT_PAYLOAD_IN_WINDOW,                                                                                         \
     5243,                                                                                                             \
     ERROR,                                                                                                            \
     "the layout puts `{section}` at {address:hex}..{last:hex}, reaching into the window it waits behind" )            \
  X( LAYOUT_OUTSIDE_POOL,                                                                                              \
     5244,                                                                                                             \
     ERROR,                                                                                                            \
     "the layout puts `{section}` at {address:hex}..{last:hex}, outside the pool it was allocated from" )              \
  X( LAYOUT_NOT_PLACED, 5245, ERROR, "the layout gives `{section}` no address" )                                       \
  X( LAYOUT_PAST_MEMORY,                                                                                               \
     5246,                                                                                                             \
     ERROR,                                                                                                            \
     "the layout puts `{section}` at {address:hex}, and it reaches past the end of memory" )                           \
  X( BAD_ALIGNMENT, 5212, ERROR, "a section cannot be aligned to {alignment}" )                                        \
  X( BAD_BOUNDARY, 5213, ERROR, "a section cannot be kept within a boundary of {boundary}" )                           \
  X( SECTION_EXCEEDS_BOUNDARY,                                                                                         \
     5214,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` is {size:n} bytes, and cannot lie within a boundary of {boundary:n}" )                               \
  X( PIN_NOT_ALIGNED, 5215, ERROR, "`{section}` is pinned at {address:hex}, which is not aligned to {alignment:n}" )   \
  X( PIN_CROSSES_BOUNDARY,                                                                                             \
     5216,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` is pinned at {address:hex}, and reaches {last:hex} across a boundary of {boundary:n}" )              \
  X( PIN_IN_REGISTER,                                                                                                  \
     5217,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` is pinned at {address:hex}, in the register region `{region}`, where nothing can be placed" )        \
  X( PIN_IN_RESERVED, 5218, WARNING, "`{section}` is pinned at {address:hex}, in the reserved region `{region}`" )     \
  X( LAYOUT_NOT_ALIGNED,                                                                                               \
     5247,                                                                                                             \
     ERROR,                                                                                                            \
     "the layout puts `{section}` at {address:hex}, which is not aligned to {alignment:n}" )                           \
  X( LAYOUT_CROSSES_BOUNDARY,                                                                                          \
     5248,                                                                                                             \
     ERROR,                                                                                                            \
     "the layout puts `{section}` at {address:hex}..{last:hex}, across a boundary of {boundary:n}" )                   \
  X( LAYOUT_MOVED_UNDER_REFERENCE,                                                                                     \
     5249,                                                                                                             \
     ERROR,                                                                                                            \
     "the layout puts `{section}` at {address:hex} in phase `{phase}` and at {other:hex} in phase `{otherPhase}`, "    \
     "and a reference holds it to one address across both" )                                                           \
  X( ASSERTION_FAILED, 5210, ERROR, "this assertion does not hold" )                                                   \
  X( ASSERTION_NOT_DECIDABLE, 5211, ERROR, "this assertion cannot be decided" )                                        \
  X( PHASE_DOES_NOT_FIT,                                                                                               \
     5631,                                                                                                             \
     ERROR,                                                                                                            \
     "phase `{phase}` does not fit in view `{view}`: requires {required:n} bytes, {available:n} available" )           \
  X( ZERO_PAGE_DOES_NOT_FIT_IN_PHASE,                                                                                  \
     5632,                                                                                                             \
     ERROR,                                                                                                            \
     "phase `{phase}` needs {required:n} bytes of zero page, and {available:n} are available" )                        \
  X( LARGEST_CONTRIBUTOR, 5633, NOTE, "`{section}` accounts for {size:n} bytes" )                                      \
  X( VALUE_DOES_NOT_FIT, 6101, ERROR, "{value} does not fit in the {width:n} bytes written here" )                     \
  X( BRANCH_OUT_OF_RANGE,                                                                                              \
     6102,                                                                                                             \
     ERROR,                                                                                                            \
     "`{mnemonic}` reaches -128 to 127 and this is {distance:n} bytes away; write `{jcc}`, which is five bytes "       \
     "where two do not reach" )                                                                                        \
  X( OVERLAY_IN_RAW_IMAGE,                                                                                             \
     6201,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` and `{other}` share {address:hex}, and a raw image cannot hold both" )                               \
  X( XEX_WITHOUT_DRIVER,                                                                                               \
     6204,                                                                                                             \
     ERROR,                                                                                                            \
     "the .xex fills storage through the driver's `showAt`, and no module declares a driver" )                         \
  X( XEX_NEEDS_WINDOW,                                                                                                 \
     6205,                                                                                                             \
     ERROR,                                                                                                            \
     "the .xex fills a unit by writing into the window the stream reads through, and this driver names none" )         \
  X( XEX_UNIT_NOT_WINDOW,                                                                                              \
     6206,                                                                                                             \
     ERROR,                                                                                                            \
     "a unit holds {unit:n} bytes and window `{name}` {window:n}, and the .xex fills a unit through the window" )      \
  X( XEX_WINDOW_NOT_ONE_RANGE,                                                                                         \
     6214,                                                                                                             \
     ERROR,                                                                                                            \
     "the .xex fills a unit through one range, and window `{name}` has {count}" )                                      \
  X( ATR_STORAGE_IS_A_UNIT_SET,                                                                                        \
     6221,                                                                                                             \
     ERROR,                                                                                                            \
     "an .atr reads storage a sector at a time and shows no Window, and this storage is the unit set                   \
     `{name}`" )                                                                                                       \
  X( ATR_UNIT_IS_NOT_SECTORS,                                                                                          \
     6222,                                                                                                             \
     ERROR,                                                                                                            \
     "a unit of storage on a diskette is the {size:n} bytes 256 sectors of {sector:n} hold, and {unit:n} "             \
     "is not that" )                                                                                                   \
  X( ATR_DRIVER_NAMES_A_WINDOW,                                                                                        \
     6223,                                                                                                             \
     ERROR,                                                                                                            \
     "the boot record of an .atr shows no Window, and this driver streams through `{name}`" )                          \
  X( ATR_DOES_NOT_FIT,                                                                                                 \
     6224,                                                                                                             \
     ERROR,                                                                                                            \
     "a sector is numbered in two bytes, so an image holds {available:n} of them, and this program "                   \
     "needs {required:n}" )                                                                                            \
  X( ATR_SECTION_IN_BOOT_RECORD,                                                                                       \
     6225,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` stands at {address:hex}, inside the boot record the .atr loads over {begin:hex} to "                 \
     "{end:hex}, which is still loading the program when those bytes are written" )                                    \
  X( CAR_STORAGE_PAST_THE_END,                                                                                         \
     6231,                                                                                                             \
     ERROR,                                                                                                            \
     "storage comes to {required:n} bytes and a `{name}` cartridge holds {available:n}" )                              \
  X( CAR_SECTION_OUTSIDE_ROM,                                                                                          \
     6232,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` holds bytes at {address:hex}, and a `{name}` cartridge is ROM at {range} and nothing else, so "      \
     "nothing would ever put them there" )                                                                             \
  X( CAR_WITHOUT_HEADER, 6233, ERROR, "a cartridge is started through the six bytes at its top, and none were made" )  \
  X( ATR_WITHOUT_BOOT_RECORD, 6226, ERROR, "an .atr is booted by its first sectors, and none were made" )              \
  X( ATR_BOOT_RECORD_TOO_LARGE,                                                                                        \
     6227,                                                                                                             \
     ERROR,                                                                                                            \
     "the boot record holds {available:n} bytes and its loader came to {required:n}" )                                 \
  X( ENTRY_NOT_DEFINED,                                                                                                \
     6211,                                                                                                             \
     ERROR,                                                                                                            \
     "phase `{phase}` has no entry: no label `{name}` is defined in the modules it needs" )                            \
  X( ENTRY_AMBIGUOUS,                                                                                                  \
     6212,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is defined in more than one module that phase `{phase}` needs, so it does not say where the phase "     \
     "starts" )                                                                                                        \
  X( ENTRY_NOT_A_LABEL, 6213, ERROR, "`{name}` is a {kind}, and an entry is a label" )                                 \
  X( UNREACHABLE_PHASE, 4501, WARNING, "phase `{phase}` is not reachable from the entry phase" )                       \
  X( UNREACHABLE_PIN,                                                                                                  \
     4505,                                                                                                             \
     WARNING,                                                                                                          \
     "`{section}` is pinned at {address:hex} and nothing reaches it, so it is dropped; mark it `root` if the "         \
     "hardware reads it" )                                                                                             \
  X( TEMPORARY_ESCAPED,                                                                                                \
     4601,                                                                                                             \
     ERROR,                                                                                                            \
     "the address of `{section}` is taken, and a temporary has none to give: its bytes are another's while it is "     \
     "not active" )                                                                                                    \
  X( OWN_DOES_NOTHING,                                                                                                 \
     4603,                                                                                                             \
     WARNING,                                                                                                          \
     "nothing this statement names is code or a temporary, so `.own` changes nothing" )                                \
  X( CODE_ADDRESS_ESCAPES,                                                                                             \
     4606,                                                                                                             \
     ERROR,                                                                                                            \
     "the address of `{section}` is taken and nothing says who follows it: `.own` if this section does, `.own NAME` "  \
     "if another does, `.root` if the hardware does" )                                                                 \
  X( OWN_JUMP_FOLLOWS_NOTHING,                                                                                         \
     4607,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` jumps through a pointer and owns no address of code, so nothing says where the jump goes" )          \
  X( ROOT_NAMES_NOTHING, 4608, WARNING, "nothing this statement names is a section, so `.root` marks nothing" )        \
  X( OWN_FOLLOWER_UNKNOWN,                                                                                             \
     4609,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is not a section, a proc or a label, and `.own` names who follows the address" )                        \
  X( OWN_FOLLOWER_OUTLIVES_TARGET,                                                                                     \
     4611,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` is handed to `{follower}`, which is present in phase `{phase}` where `{section}` is not: a jump "    \
     "from there would land in nothing" )                                                                              \
  X( TEMPORARY_UNDER_RECURSION,                                                                                        \
     4602,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` is temporary while `{owner}` runs, and `{owner}` can be entered again before it returns" )           \
  X( READONLY_WRITTEN, 5230, ERROR, "`{section}` says `readonly` and this writes it" )                                 \
  X( READONLY_DECLARED_HERE, 5231, NOTE, "`readonly` is declared here" )                                               \
  X( ROM_DOES_NOT_FIT,                                                                                                 \
     5234,                                                                                                             \
     ERROR,                                                                                                            \
     "the sections nothing writes come to {required:n} bytes and this machine has {available:n} of ROM" )              \
  X( PIN_IN_ROM,                                                                                                       \
     5233,                                                                                                             \
     ERROR,                                                                                                            \
     "`{section}` is pinned at {address:hex} in `{region}`, which is ROM, and something writes it" )                   \
  X( READONLY_WITHOUT_BYTES,                                                                                           \
     5232,                                                                                                             \
     WARNING,                                                                                                          \
     "`{section}` reserves space and emits no bytes, so `readonly` says nothing about it" )                            \
  X( LAYOUT_UNREACHABLE_PLACED, 5250, ERROR, "the layout gives `{section}` an address, and nothing reaches it" )       \
  X( LAYOUT_NOT_FOLLOWING,                                                                                             \
     5251,                                                                                                             \
     ERROR,                                                                                                            \
     "the layout ends `{section}` at {address:hex} in phase `{phase}`, and `{next}`, which follows it, starts at "     \
     "{other:hex}" )                                                                                                   \
  X( C_UNEXPECTED_CHARACTER, 7001, ERROR, "unexpected character `{character}`" )                                       \
  X( C_CONTROL_CHARACTER, 7002, ERROR, "control character {code:hex} in source" )                                      \
  X( C_INVALID_UTF8, 7003, ERROR, "invalid UTF-8 encoding" )                                                           \
  X( C_UNTERMINATED_COMMENT, 7004, ERROR, "`/*` is not closed before the end of the file" )                            \
  X( C_LINE_SPLICE, 7005, ERROR, "a backslash at the end of a line does not carry a `//` comment onto the next" )      \
  X( C_OCTAL_CONSTANT,                                                                                                 \
     7006,                                                                                                             \
     ERROR,                                                                                                            \
     "a leading zero does not make `{constant}` octal; write it in decimal, `0x` or `0b`" )                            \
  X( C_INTEGER_SUFFIX, 7007, ERROR, "`{suffix}`: an integer constant takes no suffix" )                                \
  X( C_FLOATING_CONSTANT, 7008, ERROR, "there are no floating-point constants" )                                       \
  X( C_INVALID_DIGIT, 7009, ERROR, "`{character}` is not valid in a {base} constant" )                                 \
  X( C_EMPTY_CONSTANT, 7010, ERROR, "`{prefix}` is not followed by any digits" )                                       \
  X( C_MISPLACED_DIGIT_SEPARATOR, 7011, ERROR, "`'` in an integer constant must stand between digits" )                \
  X( C_CONSTANT_TOO_LARGE, 7012, ERROR, "this constant is too large to be represented" )                               \
  X( C_RESERVED_IDENTIFIER,                                                                                            \
     7013,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is reserved: C keeps a name that begins with `__`, or with `_` and a capital, for the compiler" )       \
  X( C_UNTERMINATED_LITERAL, 7014, ERROR, "this {what} is not closed before the end of its line" )                     \
  X( C_UNKNOWN_ESCAPE,                                                                                                 \
     7015,                                                                                                             \
     ERROR,                                                                                                            \
     "`{escape}` is no escape: the six are `\\\\`, `\\\"`, `\\'`, `\\n`, `\\t` and `\\0`" )                            \
  X( C_NON_ASCII_UNPREFIXED, 7016, ERROR, "`{character}` is not ASCII, and this literal names no character set" )      \
  X( C_CHARACTER_COUNT, 7017, ERROR, "a character constant holds one character, and this holds {what}" )               \
  X( C_EXPECTED, 7100, ERROR, "expected {expected}, found {found}" )                                                   \
  X( C_NESTING_TOO_DEEP, 7101, ERROR, "{what} nest more than {limit} deep" )                                           \
  X( C_TYPE_SPELLED_OTHERWISE, 7102, ERROR, "the subset spells `{found}` as `{spelling}`" )                            \
  X( C_LITERALS_OF_TWO_PREFIXES, 7103, ERROR, "a literal of {second} is not joined to one of {first}" )                \
  X( C_NOT_COMPILED_YET, 7200, ERROR, "the compiler does not compile {construct} yet" )                                \
  X( C_NOT_DECLARED, 7201, ERROR, "`{name}` is not declared" )                                                         \
  X( C_NOT_A_FUNCTION, 7202, ERROR, "`{name}` is not a function, and only a function or a `.proc` can be called" )     \
  X( C_NOT_ASSIGNABLE, 7203, ERROR, "`{name}` is a function, and a function cannot be assigned" )                      \
  X( C_REDEFINITION, 7204, ERROR, "`{name}` is already defined in this file" )                                         \
  X( C_PREVIOUS_DEFINITION, 7205, NOTE, "the previous definition of `{name}` is here" )                                \
  X( C_CONSTANT_OUT_OF_RANGE, 7206, ERROR, "{value} does not fit in a `{type}`" )                                      \
  X( C_MIXED_SIGNEDNESS, 7207, ERROR, "`{left}` and `{right}` differ in signedness, and only a cast mixes them" )      \
  X( C_NARROWING_ASSIGNMENT, 7208, ERROR, "a `{from}` does not fit in the `{to}` it is assigned to without a cast" )   \
  X( C_BOOL_MIXED,                                                                                                     \
     7209,                                                                                                             \
     ERROR,                                                                                                            \
     "a `bool` holds `true`, `false` or a comparison, and is neither an operand nor an integer" )                      \
  X( C_COMPARISON_ALWAYS,                                                                                              \
     7210,                                                                                                             \
     WARNING,                                                                                                          \
     "{value} is outside what a `{type}` holds, so this comparison is always {outcome}" )                              \
  X( C_SHIFT_OUT_OF_RANGE, 7211, ERROR, "a `{type}` shifted by {count}: the count is from 0 to {largest}" )            \
  X( C_CONDITION_NOT_BOOL, 7212, ERROR, "a condition is a `bool`, and this is {what}" )                                \
  X( C_OUTSIDE_LOOP, 7213, ERROR, "`{keyword}` stands in no loop" )                                                    \
  X( C_NOT_A_TYPE, 7214, ERROR, "`{name}` is not a type" )                                                             \
  X( C_NOT_AN_ENUMERATION, 7215, ERROR, "`{name}` is not an `enum struct`" )                                           \
  X( C_NO_SUCH_ENUMERATOR, 7216, ERROR, "`{type}` has no enumerator `{name}`" )                                        \
  X( C_ENUMERATION_COMPUTED,                                                                                           \
     7217,                                                                                                             \
     ERROR,                                                                                                            \
     "`{type}` is an `enum struct`, whose values are compared and never computed" )                                    \
  X( C_TYPES_DIFFER, 7218, ERROR, "{left} and {right} are different types" )                                           \
  X( C_SWITCH_NOT_ENUMERATION,                                                                                         \
     7219,                                                                                                             \
     ERROR,                                                                                                            \
     "a `switch` takes a value of an `enum` or an integer, and this is {what}" )                                       \
  X( C_CASE_NOT_ENUMERATOR, 7220, ERROR, "`{label}` is not an enumerator of `{type}`" )                                \
  X( C_LABEL_REPEATED, 7221, ERROR, "`{label}` is a label of this `switch` already" )                                  \
  X( C_ENUMERATORS_UNHANDLED, 7222, WARNING, "no case names {names} of `{type}`, and the `switch` has no `default`" )  \
  X( C_TOO_MANY_ENUMERATORS, 7223, ERROR, "`{name}` has {count} enumerators, and an `enum struct` holds 256 at most" ) \
  X( C_ARGUMENT_COUNT,                                                                                                 \
     7224,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` takes an argument for each of its {count} parameters, and this call gives {given}" )                    \
  X( C_NOTHING_RETURNED_READ, 7225, ERROR, "`{name}` returns nothing, and this reads what it returns" )                \
  X( C_RETURN_WITHOUT_VALUE, 7226, ERROR, "`{name}` returns {type}, and this `return` gives no value" )                \
  X( C_RETURN_WITH_VALUE, 7227, ERROR, "`{name}` returns nothing, and this `return` gives a value" )                   \
  X( C_END_REACHED, 7228, ERROR, "`{name}` returns {type}, and the end of its body is reached with no `return`" )      \
  X( C_RECURSION, 7229, ERROR, "this call closes a cycle of calls, {cycle}, and a function never reaches itself" )     \
  X( C_CAST_REFUSED, 7230, ERROR, "{what} is not cast to a `{type}`" )                                                 \
  X( C_NOT_CONSTANT, 7231, ERROR, "{what} is given a constant, and this is not one" )                                  \
  X( C_CONST_WITHOUT_VALUE, 7232, ERROR, "`{name}` is `const`, and is given no value where it is declared" )           \
  X( C_CONSTANT_CYCLE, 7233, ERROR, "a constant is defined through itself, {cycle}" )                                  \
  X( C_CONST_ASSIGNED, 7234, ERROR, "`{name}` is `const`, and is not assigned" )                                       \
  X( C_READ_IN_OWN_INITIALISER, 7235, ERROR, "`{name}` is read in the value it is given, before it holds one" )        \
  X( C_INDEX_OUTSIDE, 7236, ERROR, "{index} is outside `{name}`, whose {count} elements are indexed from 0" )          \
  X( C_INDEX_NOT_U8, 7237, ERROR, "an index is {allowed}, and this is {what}" )                                        \
  X( C_ARRAY_ASSIGNED, 7238, ERROR, "`{name}` is an array, and is not assigned but for its elements" )                 \
  X( C_NOT_AN_ARRAY, 7239, ERROR, "`{name}` is neither an array nor a pointer, and nothing else is indexed" )          \
  X( C_DIVISION_BY_ZERO, 7240, ERROR, "this divides by zero" )                                                         \
  X( C_LIST_TOO_LONG, 7241, ERROR, "`{name}` has {count} elements, and is given {given}" )                             \
  X( C_LIST_MISMATCH, 7242, ERROR, "`{name}` is {what}, and is given {given}" )                                        \
  X( C_ARRAY_SIZE, 7243, ERROR, "the size of `{name}` is a constant from 1 to {largest}" )                             \
  X( C_MIXED_DECLARATORS, 7244, ERROR, "`{name}` is {what}, and a declaration declares pointers or none" )             \
  X( C_ADDRESS_OF_PARAMETER, 7245, ERROR, "`{name}` is a parameter, whose address is not taken; copy it to a local" )  \
  X( C_POINTERS_DIFFER, 7246, ERROR, "{left} and {right} do not meet" )                                                \
  X( C_WRITE_THROUGH_CONST, 7247, ERROR, "what is written here is `const`, reached through a pointer to `const`" )     \
  X( C_NOT_A_POINTER, 7248, ERROR, "{what} is no pointer, and only a pointer is dereferenced" )                        \
  X( C_ARRAY_PARAMETER_SIZE, 7249, ERROR, "`{name}` is a pointer, and C would ignore the size written for it" )        \
  X( C_EMPTY_AGGREGATE, 7250, ERROR, "`{name}` has no members, and C gives such a type no size" )                      \
  X( C_AGGREGATE_HOLDS_ITSELF, 7251, ERROR, "a type holds itself by value and has no size, {cycle}" )                  \
  X( C_MEMBER_REFUSED, 7252, ERROR, "`{name}` is a member, which is given no value and is not `const`" )               \
  X( C_NO_SUCH_MEMBER, 7253, ERROR, "`{type}` has no member `{name}`" )                                                \
  X( C_MEMBER_TWICE, 7254, ERROR, "`{name}` is a member of `{type}` already" )                                         \
  X( C_NOT_AN_AGGREGATE, 7255, ERROR, "{what} is no `struct` or `union`, and only one has members" )                   \
  X( C_LIST_SHAPE, 7256, ERROR, "{what} is given {given}" )                                                            \
  X( C_AGGREGATE_OPERATOR, 7257, ERROR, "{what} is a `struct` or a `union`, which no operator takes" )                 \
  X( C_UNKNOWN_ATTRIBUTE, 7258, ERROR, "`{name}` is no attribute of the subset" )                                      \
  X( C_ATTRIBUTE_MISPLACED, 7259, ERROR, "`[[{attribute}]]` applies to {applies}, and `{name}` is none" )              \
  X( C_NOT_STRIPABLE, 7260, ERROR, "`{name}` is an array of {what}, which lies in no stripes" )                        \
  X( C_STRIPED_TOO_LONG, 7261, ERROR, "`{name}` has {count} elements, and a striped array holds 256 at most" )         \
  X( C_STRIPED_ELEMENT, 7262, ERROR, "`{name}` lies in stripes, and has no address of its own" )                       \
  X( C_NO_SHAPE, 7263, ERROR, "`{name}` is a Label of the assembler with no shape in C: {reason}" )                    \
  X( C_NOT_READ, 7264, ERROR, "`{name}` is {reason} of the assembler, which C does not read" )                         \
  X( C_RESERVED_REGION, 7265, ERROR, "`{name}` is a `reserved` Region, which C does not reach" )                       \
  X( C_COUNT_UNKNOWN, 7266, ERROR, "`{name}` holds a string, so the compiler does not know how many elements it has" ) \
  X( C_DEFINED_HERE, 7267, NOTE, "`{name}` is defined here" )                                                          \
  X( C_BYTES_HANDED_OVER,                                                                                              \
     7268,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` takes or returns `u8[N]`, bytes that a call from C does not hand over" )                                \
  X( C_EXTERN_OF_C, 7269, ERROR, "`{name}` is defined in C, and `extern` declares only what the assembler exports" )   \
  X( C_EXTERN_OF_NOTHING,                                                                                              \
     7270,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is declared `extern`, and nothing the assembler exports is named so" )                                  \
  X( C_EXTERN_TWICE,                                                                                                   \
     7271,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is declared `extern` already, and a name is declared once in the program" )                             \
  X( C_EXTERN_REFUSED, 7272, ERROR, "`{name}` is declared `extern`, and {what}" )                                      \
  X( C_EXTERN_SIZE, 7273, ERROR, "`{name}` {what}" )                                                                   \
  X( C_EXTERN_NOT_DATA, 7274, ERROR, "`{name}` reads past what the assembler wrote as data: {reason}" )                \
  X( C_EXTERN_SPLITS, 7275, ERROR, "`{path}`, at offset {offset}, splits an item the assembler wrote" )                \
  X( C_EXTERN_VALUE, 7276, ERROR, "{what} holds {value}, which is no `{type}`" )                                       \
  X( C_EXTERN_SIGNATURE, 7277, ERROR, "`{name}` {what}" )                                                              \
  X( C_NOT_A_CHARSET, 7278, ERROR, "`{name}` is no character set, and a literal's prefix names one" )                  \
  X( C_PANE_NAME, 7279, ERROR, "`{name}` is {what}, and `{form}` takes {takes}" )                                      \
  X( C_CALL_INTO_FAMILY,                                                                                               \
     7280,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is in family `{family}`, whose member is known at run time alone: call it in a `with({family}, i)` "    \
     "block" )                                                                                                         \
  X( C_CALL_ACROSS_BLOCK_WINDOW,                                                                                       \
     7281,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is in pane `{pane}` of window `{window}`, which this block shows in another state: the exit of the "    \
     "call's `.with` would put back what the function's own code shows, not the block's" )                             \
  X( C_WITH_NESTED_WINDOW,                                                                                             \
     7282,                                                                                                             \
     ERROR,                                                                                                            \
     "window `{window}` is shown by the block this one stands in, and a window shows one state at a time" )            \
  X( C_LEAVES_WITH_BLOCK,                                                                                              \
     7283,                                                                                                             \
     ERROR,                                                                                                            \
     "`{keyword}` leaves the `with` block, which would leave the window as the block shows it" )                       \
  X( C_ADDRESS_LEAVES_BLOCK,                                                                                           \
     7284,                                                                                                             \
     ERROR,                                                                                                            \
     "the address of `{name}`, in what this block shows, is kept only in a local of the block or handed to a call "    \
     "in it: here it {escapes}" )                                                                                      \
  X( C_WITH_INDEX, 7286, ERROR, "the index of `with({name}, i)` is {what}, and `X` takes a `u8` or a constant" )       \
  X( C_ATTRIBUTE_ARGUMENTS, 7287, ERROR, "`[[{attribute}]]` takes {takes}" )                                           \
  X( C_UNDER_WITH_IN,                                                                                                  \
     7288,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is `[[in]]` a pane and `[[under]]` one, and code under a pane is in none" )                             \
  X( C_TRAMPOLINE_NEEDED,                                                                                              \
     7289,                                                                                                             \
     ERROR,                                                                                                            \
     "this block shows another state of window `{window}` from code in pane `{pane}`, which cannot switch its own "    \
     "window: write `[[with(...), trampoline]]`, and pay a Proc in fixed and a `jsr`" )                                \
  X( C_TRAMPOLINE_NOT_NEEDED,                                                                                          \
     7290,                                                                                                             \
     ERROR,                                                                                                            \
     "`trampoline` on a block that needs none: {where}, and the block is a macro under `.with`" )                      \
  X( C_WITH_OWN_PANE, 7291, ERROR, "this block shows `{pane}`, and the code is in `{pane}` already" )                  \
  X( C_CALL_ACROSS_OWN_WINDOW,                                                                                         \
     7292,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is in pane `{pane}` of window `{window}`, and this code is in pane `{own}` of it, which cannot switch " \
     "its own window: call it in a `[[with({pane}), trampoline]]` block or an `[[under({own})]]` function" )           \
  X( C_UNDER_CALLED_ELSEWHERE,                                                                                         \
     7293,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` runs under pane `{pane}`, called from code in the pane, from another function under it, or in a block " \
     "that shows it, and this is none" )                                                                               \
  X( C_TRANSITION_RETURNS,                                                                                             \
     7294,                                                                                                             \
     ERROR,                                                                                                            \
     "a transition enters a phase and never comes back, so it stands on a bare `return` in a `void` function, and "    \
     "{what}" )                                                                                                        \
  X( C_SLOT_TYPE,                                                                                                      \
     7295,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is a slot, and its binding is its type's: a `void(void)` binds a vector and a `T* const` a pointer, "   \
     "and {what}" )                                                                                                    \
  X( C_IMPLEMENTS_TYPE,                                                                                                \
     7296,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` fills slot `{slot}`, whose cell holds an address the program reads as {wanted}, and {what}" )           \
  X( C_TAKEN_SIGNATURE, 7297, ERROR, "`{name}` is no `{type}`: {what}" )                                               \
  X( C_TAKEN_TWICE,                                                                                                    \
     7298,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is taken as `{type}` here and as `{other}` elsewhere, and a function reads its arguments from one "     \
     "place" )                                                                                                         \
  X( C_FUNCTION_TYPE_VALUE,                                                                                            \
     7299,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` names a function type, which is a type only as a pointer: `{name}*`" )                                  \
  X( C_CALLS_OWN_TYPE,                                                                                                 \
     7300,                                                                                                             \
     ERROR,                                                                                                            \
     "this call is of the function type its caller is a member of, which would write the temporaries the caller "      \
     "reads; a member dispatches through another type, or through a name" )                                            \
  X( C_AUTO_WITHOUT_TYPE,                                                                                              \
     7301,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is written `auto` and given {what}, which takes its type from where it is used and so"                  \
     " has none to give" )                                                                                             \
  X( C_INLINE_REFUSED, 7302, ERROR, "`{name}` is written `inline` and {what}, so the call stays a call" )              \
  X( C_INLINE_WITHOUT_BODY, 7303, ERROR, "`inline` is written on {what}, which has no body to wrap into a caller" )    \
  X( C_STEP_REACHED_AGAIN,                                                                                             \
     7304,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is assigned by `{operator}` where it is used as a value, and reached again in the same expression, "    \
     "which is an order the subset does not fix" )                                                                     \
  X( C_PLACEMENT_ON_POINTER,                                                                                           \
     7305,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is a pointer, which `(zp),y` reads through, so it lies in the zero page whatever is asked" )            \
  X( C_PLACEMENT_AND_STRIPED,                                                                                          \
     7306,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` lies in stripes, whose place its layout settles, so `[[placement]]` is a second answer" )               \
  X( C_PLACEMENT_SAYS_NOTHING, 7307, WARNING, "`{name}` is `{class}` already, so this says nothing" )                  \
  X( C_STATIC_READ_BEFORE_WRITTEN,                                                                                     \
     7308,                                                                                                             \
     ERROR,                                                                                                            \
     "`{name}` is read here before anything writes it, and a `static` local holds what was there; give it a value "    \
     "where it is declared" )

// A compiler-class tool outgrows 255 diagnostics, and renumbering the catalog
// later is not an option.
// NOLINTNEXTLINE(performance-enum-size)
enum class DiagnosticId : std::uint16_t
{
#define NGA_DIAGNOSTIC_ENUMERATOR( name, number, severity, text ) name,
  NGA_DIAGNOSTIC_CATALOG( NGA_DIAGNOSTIC_ENUMERATOR )
#undef NGA_DIAGNOSTIC_ENUMERATOR
  COUNT,
};

/// What the catalog knows about one diagnostic.
struct CatalogEntry
{
  std::string_view code;    ///< user-visible identifier, "NGA2412"
  std::string_view name;    ///< stable machine-readable name, "CROSS_VIEW_REFERENCE"
  Severity defaultSeverity; ///< before any policy override
  std::string_view messageTemplate;
};

CatalogEntry const& catalogEntryFor( DiagnosticId id );

/// The identifier a user-visible code names, or nothing when no diagnostic
/// carries it. Both `--deny=NGA2410` and the inline test expectations arrive as
/// text, and neither may guess.
std::optional<DiagnosticId> diagnosticIdForCode( std::string_view code );

} // namespace nga::diag
