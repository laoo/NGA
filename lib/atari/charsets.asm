; The Atari's two codes for the same letters, as Charsets.
;
; `atascii` is what the character I/O takes: the machine's own code, with the
; graphics characters where an ASCII machine keeps its control codes, and the
; letters where ASCII has them — which is why an unprefixed literal of plain
; letters has always worked and why nothing else has. `\n` is `$9B`, the end of
; line, and not `$0A`.
;
; `screen` is what the display reads out of screen memory, which is the same
; letters at other numbers. A program that writes where the display looks
; rather than through the OS wants this one.
;
; The graphics characters are written here as the Unicode the box-drawing and
; block characters have, so a picture drawn in the source is the picture the
; machine draws. A letter in inverse video is one of those too — Unicode squares
; them off in a negative, and `atascii"PRESS 🆂"` is seven bytes with the last
; one inverse. A whole line of inverse text is better asked of a Charset
; derived from this one, `.charset bright : atascii ^ $80`, which is what a
; derivation is for. Two of them have no exact Unicode: `$02` and `$0D` are a
; quarter of a cell where the nearest character is an eighth, and are written
; as `▕` and `▔`. Everything else, the inverse entries included, was read off
; the machine's own font and matches it.
;
; This Module emits nothing: it is two names and two tables.

.export atascii, screen

; tag atascii
.charset atascii
  "♥├▕┘┤┐╱╲◢▗◣▝▘▔▂▖♣┌─┼●▄▎┬┴▌└"                                       = $00
  "↑↓←→"                                                              = $1C
  " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_" = $20
  "♦"                                                                 = $60
  "abcdefghijklmnopqrstuvwxyz"                                        = $61
  "♠"                                                                 = $7B
  "│"                                                                 = $7C
  "\n"                                                                = $9B

  ; Inverse video is the same glyph with the bits the other way round, which
  ; for the blocks and the triangles is another character Unicode draws. Every
  ; one of these was read off the machine's own font and is exact; the rest of
  ; the inverse half has no glyph to be written as, and is reached by adding
  ; $80 or by a Charset derived from this one.
  "▊" = $82        ; ▕
  "◤" = $88        ; ◢
  "▛" = $89        ; ▗
  "◥" = $8A        ; ◣
  "▙" = $8B        ; ▝
  "▟" = $8C        ; ▘
  "▆" = $8D        ; ▔
  "▜" = $8F        ; ▖
  "▀" = $95        ; ▄
  "▐" = $99        ; ▌
  "█" = $A0        ; the space

  ; And the letters, which Unicode squares off in a negative.
  "🅰🅱🅲🅳🅴🅵🅶🅷🅸🅹🅺🅻🅼🅽🅾🅿🆀🆁🆂🆃🆄🆅🆆🆇🆈🆉" = $C1
.endch
; end atascii

.charset screen
  " !\"#$%&'()*+,-./0123456789:;<=>?"                                 = $00
  "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"                                 = $20
  "♥├▕┘┤┐╱╲◢▗◣▝▘▔▂▖♣┌─┼●▄▎┬┴▌└"                                       = $40
  "♦"                                                                 = $60
  "abcdefghijklmnopqrstuvwxyz"                                        = $61
  "♠"                                                                 = $7B
  "│"                                                                 = $7C

  ; The same inverses, at the display's numbers.
  "▊" = $C2
  "◤" = $C8
  "▛" = $C9
  "◥" = $CA
  "▙" = $CB
  "▟" = $CC
  "▆" = $CD
  "▜" = $CF
  "▀" = $D5
  "▐" = $D9
  "█" = $80
  "🅰🅱🅲🅳🅴🅵🅶🅷🅸🅹🅺🅻🅼🅽🅾🅿🆀🆁🆂🆃🆄🆅🆆🆇🆈🆉" = $A1
.endch
