# CMOS real-time clock notes

The RTC lives behind two I/O ports: write a register index to `0x70`, then read or write the value at `0x71`. (Bit 7 of the index is the NMI-disable bit, so keep it clear.)

| Register | Meaning |
| --- | --- |
| `0x00` | seconds |
| `0x02` | minutes |
| `0x04` | hours |
| `0x07` | day of month |
| `0x08` | month |
| `0x09` | year (two digits) |
| `0x32` | century (usually; not guaranteed) |
| `0x0A` | status A: bit 7 = update in progress (UIP) |
| `0x0B` | status B: bit 1 = 24-hour mode, bit 2 = binary (not BCD) values |

Pitfalls:

- Values are **BCD** unless status B bit 2 is set, so convert `0x59` to 59.
- Reading while the chip updates (UIP set) can return a torn value. Wait for UIP to clear and read twice until two reads agree.
- In 12-hour mode the top bit of the hour register flags PM.
- QEMU initialises the RTC from the host clock (UTC by default).
