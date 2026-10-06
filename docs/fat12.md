# FAT12 layout notes

FAT12 is the filesystem of 1.44 MB / 720 KB floppies. A volume is a sequence of 512-byte sectors:

```
sector 0            boot sector with the BIOS Parameter Block (BPB)
1 ..                reserved sectors (usually just the boot sector)
FAT #1, FAT #2      the File Allocation Table, stored twice
root directory      a fixed array of 32-byte entries (not part of the data area)
data area           clusters numbered from 2
```

## BPB fields we need (offsets in the boot sector)

| offset | size | field |
| --- | --- | --- |
| 11 | 2 | bytes per sector (512) |
| 13 | 1 | sectors per cluster |
| 14 | 2 | reserved sectors |
| 16 | 1 | number of FATs |
| 17 | 2 | root directory entries |
| 19 | 2 | total sectors |
| 22 | 2 | sectors per FAT |

Derived: `root_start = reserved + fats * sectors_per_fat`, `root_sectors = root_entries * 32 / 512`, `data_start = root_start + root_sectors`; cluster `n` lives at sector `data_start + (n - 2) * sectors_per_cluster`.

## The FAT

One 12-bit entry per cluster, packed: entry `n` starts at byte `n + n/2`; read a 16-bit little-endian word there and take the low 12 bits if `n` is even, else shift right by 4.

| value | meaning |
| --- | --- |
| `0x000` | free cluster |
| `0x002 - 0xFEF` | next cluster of the chain |
| `0xFF8 - 0xFFF` | last cluster of the chain |
| `0xFF7` | bad cluster |

A file is a chain: start from the cluster in its directory entry and follow the FAT until an end marker.

## Directory entry (32 bytes)

| offset | size | field |
| --- | --- | --- |
| 0 | 11 | name: 8 + 3 characters, space padded, upper case, no dot |
| 11 | 1 | attributes: `0x01` read-only, `0x02` hidden, `0x04` system, `0x08` volume label, `0x10` directory, `0x0F` = long-file-name fragment (skip) |
| 26 | 2 | first cluster |
| 28 | 4 | file size in bytes |

First byte `0x00` ends the directory, `0xE5` marks a deleted entry. Subdirectories are ordinary clusters full of 32-byte entries (with `.` and `..` first). Names are case-insensitive: compare after upper-casing.
