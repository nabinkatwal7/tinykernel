# PS/2 mouse protocol notes

The PS/2 mouse hangs off the second ("auxiliary") port of the same 8042 keyboard controller:

- data port `0x60`, status/command port `0x64`; status bit 0 = output buffer full, bit 1 = input buffer full, **bit 5 = the byte in the output buffer came from the mouse**;
- controller commands (written to `0x64`): `0xA8` enable the aux port, `0x20` read the controller configuration byte, `0x60` write it (bit 1 enables the mouse IRQ12, bit 5 would disable the mouse clock), `0xD4` send the *next byte written to `0x60`* to the mouse;
- mouse commands: `0xF6` set defaults, `0xF4` enable data reporting; every command is acknowledged with `0xFA`.

## Movement packets

In streaming mode each event is three bytes:

| byte | contents |
| --- | --- |
| 0 | bit 0 left button, bit 1 right button, bit 2 middle button, **bit 3 always 1**, bit 4 X sign, bit 5 Y sign, bit 6/7 overflow |
| 1 | X movement (low 8 bits; with the sign bit it is a 9-bit two's complement value) |
| 2 | Y movement, same encoding; positive Y is **up** |

The always-set bit 3 lets a driver resynchronise if a byte is lost: if byte 0 of a packet does not have it, drop the byte. An IRQ12 fires per byte, so the handler accumulates bytes and acts on every third one. The cursor position is the running sum of the deltas, clamped to the screen (and with Y negated, because screens count down).
