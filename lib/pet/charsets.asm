; The PET's two codes for the same letters, as Charsets.
;
; `petscii` is what the character I/O takes. Over the range a program prints —
; space to `_` — it is ASCII with two exceptions: `$5C` is the pound sign and
; not a backslash, and `$5E` and `$5F` are the two arrows. `\n` is `$0D`, the
; carriage return.
;
; `screen` is what the video circuit reads out of the display's memory, which is
; the same letters at other numbers: `@` at zero, the alphabet from one, and
; ASCII's `$20` to `$3F` where ASCII has them. A program that writes into
; `screenRam` rather than through the ROM wants this one — see pet/screen.asm.
;
; **The graphics half is not here.** Both codes carry a hundred and twenty-eight
; more characters, and on this machine which of two sets is displayed depends on
; a bit the program writes; every one of them would have to be read off the
; character generator ROM and matched to the Unicode its glyph is drawn as, as
; atari/charsets.asm was. Until that is done a literal of one is refused by
; name, which is the right answer — a byte guessed here would be a picture the
; machine does not draw.
;
; This Module emits nothing: it is two names and two tables.

.export petscii, screen

; The code the ROM's character output takes, over the range that is not
; graphics. In the set a PET shows at power-on the letters are capitals, and
; `$C1` to `$DA` show them a second time.
.charset petscii
  " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ" = $20
  "[£]↑←"                                                        = $5B
  "\n"                                                           = $0D
.endch

; The code the display reads. `@` is zero, which is the thing to remember: a
; space is `$20` here as it is in ASCII, but a letter is not.
.charset screen
  "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[£]↑←"                = $00
  " !\"#$%&'()*+,-./0123456789:;<=>?"                = $20
.endch
