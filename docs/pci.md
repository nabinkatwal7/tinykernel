# PCI configuration space notes

Every PCI function has 256 bytes of configuration registers. On x86 the legacy "mechanism #1" reaches them through two I/O ports:

- write a 32-bit **address** to `0xCF8`: bit 31 = enable, bits 23-16 = bus, 15-11 = device (slot), 10-8 = function, 7-2 = register (dword aligned);
- then read or write the **data** at `0xCFC`.

```
address = 0x80000000 | bus << 16 | slot << 11 | func << 8 | (reg & 0xFC)
```

## Standard header (type 0)

| offset | size | field |
| --- | --- | --- |
| 0x00 | 2 | vendor id (`0xFFFF` = nothing here) |
| 0x02 | 2 | device id |
| 0x04 | 2 | command (bit 0 I/O space, bit 1 memory space, bit 2 bus master) |
| 0x06 | 2 | status |
| 0x08 | 1 | revision |
| 0x09 | 3 | prog-if, subclass, **class code** |
| 0x0E | 1 | header type (bit 7 = multi-function device, 0 = normal, 1 = PCI-PCI bridge) |
| 0x10 | 6 x 4 | BAR0..BAR5: base addresses of the device's registers |
| 0x3C | 1 | interrupt line (the legacy IRQ number the BIOS routed it to) |

A BAR with bit 0 set is an I/O port range (mask the low 2 bits); otherwise it is memory (mask the low 4 bits).

## Enumeration

For each bus 0-255 and slot 0-31 read the vendor id of function 0; `0xFFFF` means the slot is empty. If header-type bit 7 is set probe functions 1-7 too. A PCI-PCI bridge (header type 1) has a secondary bus number at offset 0x19 which should be scanned recursively; scanning every bus number brute-force also works and is simpler. QEMU's default machine (i440FX) puts the host bridge at 00:00.0, the ISA bridge/IDE/ACPI at 00:01.x, a VGA adapter at 00:02.0 and the NIC from slot 3.
