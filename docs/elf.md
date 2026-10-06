# ELF file format notes

An ELF32 executable is a header, a table of **program headers** (what to load) and optionally a table of section headers (what the linker saw). A loader only needs the first two.

```
offset 0   e_ident   7F 'E' 'L' 'F', class (1 = 32-bit), data (1 = little endian), version
       16  e_type    2 = executable
       18  e_machine 3 = Intel 80386
       24  e_entry   virtual address of the first instruction
       28  e_phoff   file offset of the program header table
       42  e_phentsize, 44 e_phnum
```

Each 32-byte program header (`PT_LOAD` = type 1 matters):

| field | meaning |
| --- | --- |
| `p_offset` | where the bytes start in the file |
| `p_vaddr` | where they must appear in memory |
| `p_filesz` | bytes to copy from the file |
| `p_memsz` | bytes the segment occupies in memory; the tail beyond `p_filesz` is zero (`.bss`) |
| `p_flags` | R=4, W=2, X=1 |

Loader algorithm: validate the magic, class, endianness, machine and type; for every `PT_LOAD` check that `[p_vaddr, p_vaddr + p_memsz)` fits the address range the program may use, copy `p_filesz` bytes, zero the rest; jump to `e_entry`.

Toolchain note: MinGW's `ld` cannot emit ELF directly, so the build links a PE image at the user address and converts it with `objcopy -O elf32-i386`, which yields a valid ELF with one `PT_LOAD` per output section group.
