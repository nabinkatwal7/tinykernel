#ifndef ELF_H
#define ELF_H

#include <stdint.h>

#define ELF_MAGIC "\x7f" "ELF"

struct elf32_ehdr {
	uint8_t  ident[16];
	uint16_t type, machine;
	uint32_t version, entry, phoff, shoff, flags;
	uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf32_phdr {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed));

#define ELF_ET_EXEC 2
#define ELF_EM_386  3
#define ELF_PT_LOAD 1

int elf_is_elf(const uint8_t *img, uint32_t size);

/*
 * Copies every PT_LOAD segment into 'mem', which backs virtual addresses [base, base+limit).
 * Fails (negative) on a malformed header or a segment that does not fit. *entry gets e_entry.
 */
int elf_load(const uint8_t *img, uint32_t size, uint8_t *mem, uint32_t base, uint32_t limit,
	     uint32_t *entry);

#endif
