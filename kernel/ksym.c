#include "ksym.h"

#include "kstring.h"

/* Filled in by tools/ksymgen after linking. The leading 1 puts the blob in .data (initialised) so that it exists in the image. */
struct ksym_blob {
	uint32_t count;
	struct {
		uint32_t addr, name_off;
	} sym[KSYM_MAX_SYMS];
	char names[KSYM_NAMES_SIZE];
};

struct ksym_blob ksym_blob = { .count = 1 };

uint32_t ksym_count(void)
{
	return ksym_blob.count > KSYM_MAX_SYMS ? 0 : ksym_blob.count;
}

const char *ksym_at(uint32_t i, uint32_t *addr)
{
	if (i >= ksym_count())
		return 0;
	if (addr)
		*addr = ksym_blob.sym[i].addr;
	return ksym_blob.names + ksym_blob.sym[i].name_off;
}

/* Binary search for the last symbol at or below addr. */
const char *ksym_lookup(uint32_t addr, uint32_t *offset)
{
	uint32_t lo = 0, hi = ksym_count();

	if (!hi || addr < ksym_blob.sym[0].addr)
		return 0;
	while (hi - lo > 1) {
		uint32_t mid = (lo + hi) / 2;

		if (ksym_blob.sym[mid].addr <= addr)
			lo = mid;
		else
			hi = mid;
	}
	if (addr - ksym_blob.sym[lo].addr > 0x10000) /* far past the last function: not ours */
		return 0;
	if (offset)
		*offset = addr - ksym_blob.sym[lo].addr;
	return ksym_blob.names + ksym_blob.sym[lo].name_off;
}

uint32_t ksym_find(const char *name)
{
	uint32_t i;

	for (i = 0; i < ksym_count(); i++)
		if (!kstrcmp(ksym_blob.names + ksym_blob.sym[i].name_off, name))
			return ksym_blob.sym[i].addr;
	return 0;
}
