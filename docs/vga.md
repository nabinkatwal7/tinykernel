# VGA graphics mode notes

A VGA card is programmed through five register groups, all reached with an index port / data port pair:

| group | index / data ports | role |
| --- | --- | --- |
| Miscellaneous output | write `0x3C2` | clock selection, I/O address (colour = CRTC at `0x3D4`), sync polarity |
| Sequencer | `0x3C4` / `0x3C5` | memory plane masks, chain-4 and odd/even addressing, clocking |
| CRT controller | `0x3D4` / `0x3D5` | the scan-out timings: totals, blanking, sync, offset (pitch), cursor |
| Graphics controller | `0x3CE` / `0x3CF` | how the CPU's reads/writes map to the four memory planes, and where video memory appears |
| Attribute controller | `0x3C0` (index and data share one port, toggled by reading `0x3DA`) | palette indexes and mode flags |

A video mode is nothing more than a table of values for these registers (the BIOS keeps tables for modes 03h, 13h, ...). Before writing the CRTC timing registers, clear the "protect" bit (bit 7 of CRTC register 0x11).

## Mode 13h: 320x200, 256 colours

- One byte per pixel, linear, at physical `0xA0000`: pixel `(x, y)` is byte `y * 320 + x`.
- Possible because "chain-4" addressing in the sequencer folds the four planes into one linear space.
- The byte is an index into the DAC palette: write the index to `0x3C8`, then three 6-bit values (R, G, B) to `0x3C9`.

## Mode 03h: 80x25 text

- Character cells at `0xB8000` (character byte + attribute byte); the glyph shapes live in plane 2 of VGA memory, loaded by the BIOS.
- **Switching to a graphics mode overwrites plane 2**, so a program that wants to come back to text mode must save the font first (read plane 2 through the graphics controller) and write it back after reprogramming the registers for mode 03h.

Switching without the BIOS (we are in protected mode, where `int 10h` is unavailable) therefore means: save font, program all five register groups, load the palette, and on the way back restore the text-mode registers and the font.
