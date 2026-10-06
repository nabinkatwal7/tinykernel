#include "elf.h"

#include "kstring.h"

int elf_is_elf(const uint8_t *img, uint32_t size)
{
	return size >= sizeof(struct elf32_ehdr) && !memcmp(img, ELF_MAGIC, 4);
}

int elf_load(const uint8_t *img, uint32_t size, uint8_t *mem, uint32_t base, uint32_t limit,
	     uint32_t *entry, uint32_t *end)
{
	const struct elf32_ehdr *eh = (const struct elf32_ehdr *)img;
	uint32_t i, loaded = 0, top = base;

	if (!elf_is_elf(img, size))
		return -1;
	if (eh->ident[4] != 1 || eh->ident[5] != 1) /* 32-bit, little endian */
		return -2;
	if (eh->type != ELF_ET_EXEC || eh->machine != ELF_EM_386)
		return -3;
	if (eh->phentsize != sizeof(struct elf32_phdr) || eh->phnum == 0
	    || eh->phoff > size || eh->phnum * sizeof(struct elf32_phdr) > size - eh->phoff)
		return -4;

	for (i = 0; i < eh->phnum; i++) {
		const struct elf32_phdr *ph =
			(const struct elf32_phdr *)(img + eh->phoff + i * sizeof *ph);
		uint32_t off;

		if (ph->type != ELF_PT_LOAD)
			continue;
		if (ph->filesz > ph->memsz || ph->offset > size || ph->filesz > size - ph->offset)
			return -5;
		if (ph->vaddr < base || ph->vaddr - base > limit || ph->memsz > limit - (ph->vaddr - base))
			return -6; /* outside the address range this program may use */
		off = ph->vaddr - base;
		memcpy(mem + off, img + ph->offset, ph->filesz);
		memset(mem + off + ph->filesz, 0, ph->memsz - ph->filesz);
		loaded++;
		if (ph->vaddr + ph->memsz > top)
			top = ph->vaddr + ph->memsz;
	}
	if (!loaded || eh->entry < base || eh->entry >= base + limit)
		return -7;
	*entry = eh->entry;
	*end = top;
	return 0;
}
