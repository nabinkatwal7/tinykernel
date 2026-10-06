# AHCI basics

AHCI is the standard PCI interface to SATA disks (PCI class 01:06, programming interface 01). Everything is
memory-mapped: the *ABAR* in BAR5 points at the HBA (host bus adapter) registers.

| offset | register | meaning |
|--------|----------|---------|
| 0x00 | CAP | ports (bits 0-4, minus one), command slots (bits 8-12, minus one), 64-bit addressing (bit 31) |
| 0x04 | GHC | global control: bit 31 AHCI enable, bit 1 interrupt enable, bit 0 HBA reset |
| 0x08 | IS  | interrupt status, one bit per port |
| 0x0C | PI  | ports implemented, one bit per port |
| 0x10 | VS  | version (major.minor in the high and low halves) |
| 0x100 + 0x80 * n | port n | CLB/FB (command list and received-FIS base), IS, IE, CMD, TFD, SIG, SSTS, SCTL, SERR, SACT, CI |

Per port, **SSTS** bits 0-3 (DET) say whether a device is present (3 = present and talking), bits 4-7 give the link
speed generation. **SIG** identifies what it is: 0x00000101 SATA disk, 0xEB140101 ATAPI, 0xC33C0101 enclosure,
0x96690101 port multiplier.

To move data a driver builds, in memory, a *command list* (32 slots) per port; each slot points to a *command
table* holding a register FIS (the ATA command, for example READ DMA EXT) and a list of physical regions (PRDT).
Setting a bit in the port **CI** register starts the slot; the HBA fetches the table by DMA, performs the command and
posts a D2H register FIS and an interrupt.

`ahci` (kernel/ahci.c) maps the registers, prints this information and stops there. With QEMU:

```
QEMU_EXTRA="-device ahci,id=ahci -drive id=sd0,file=build/test-disk.img,format=raw,if=none -device ide-hd,drive=sd0,bus=ahci.0" \
    python tools/qemu_drive.py --menu 1 ahci
```
