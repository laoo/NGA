# Targets

A raw memory image is not a file a machine will load. NGA writes the machine's
own format itself, with no packer to run afterwards, and the project asks for
it in a word: `container xex` is the Atari binary DOS reads.

<!-- include 03-targets/rainbow/main.ngp -->
```
modules { "rainbow.asm" }

container xex
```
<!-- end -->

<!-- include 03-targets/rainbow/rainbow.asm -->
```asm
COLBK  = $D01A
COLPF2 = $D018
WSYNC  = $D40A
VCOUNT = $D40B

.proc entry
@top    lda VCOUNT
        jne @top

        ldx #0
@line   sta WSYNC
        stx COLBK
        stx COLPF2
        inx
        cpx #240
        jcc @line

        jmp @top
.endp
```
<!-- end -->

This is the straightforward implementation of the classic Atari rainbow.
The implementation is fine, but NGA applied on it default placement rules which restrict memory usage to $2000-$9fff.

NGA can be told about specific needs about the placement rules for a project using a `target` block. Within we can among others tell the allocator about the `regions` we require.

## Regions

You tell it in a `target` block, one line per range.

<!-- include 03-targets/machine/main.ngp tag=ram -->
```
target {
  region        $0080 .. $00FF  ram
  region        $2000 .. $BFFF  ram
}
```
<!-- end -->

`ram` is where the solver may put things and it's the only thing what is really needed to instruct the allocator.

`reserved` is where it may not, though you may still pin a section there with
`at`. Then the tool warns rather than refuses, because you may know something
the machine's description does not — and the finding names both lines.

<!-- include 03-targets/pinned/main.ngp tag=res -->
```
target {
  region        $0080 .. $00FF  ram
  region dos    $0700 .. $1FFF  reserved
  region        $2000 .. $BFFF  ram
}
```
<!-- end -->

<!-- include 03-targets/pinned/pinned.asm tag=sec -->
```asm
.section absolute at $0800
belowDos
        .res 1
.ends
```
<!-- end -->

<!-- diagnostics 03-targets/pinned -->
```
warning[NGA5218]: `belowDos` is pinned at $800, in the reserved region `dos`
 --> pinned.asm:2:1
  |
2 | .section absolute at $0800
  | ^^^^^^^^
note: the region `dos` is declared here
 --> main.ngp:4:3
  |
4 |   region dos    $0700 .. $1FFF  reserved
  |   ^^^^^^
```
<!-- end -->

`register` is where it cannot. Pinning anything within a region `register` generates an error.

A region's name is a symbol every module sees, whose value is the address it
starts at, which is how a hardware register is declared. There is a shorter
form for that, and a two-byte register is written by adding `, 2` after the
address.

<!-- include 03-targets/registers/main.ngp tag=registers -->
```
  region COLBK    $D01A .. $D01A  register
  register COLPF2 $D018
  register WSYNC  $D40A
  register VCOUNT $D40B
```
<!-- end -->

With that in the project's `target` block, the module stops declaring those four names. Redeclaration or shadowing registers is an error.
