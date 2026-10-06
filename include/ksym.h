#ifndef KSYM_H
#define KSYM_H

#include <stdint.h>

/*
 * Kernel symbol table: the build links the kernel, lists its functions with nm, and tools/ksymgen
 * writes them into a reserved blob inside the finished image. Used for stack traces and the ksym command.
 */
#define KSYM_MAX_SYMS   2048
#define KSYM_NAMES_SIZE 24576

const char *ksym_lookup(uint32_t addr, uint32_t *offset);   /* name of the function containing addr (offset into it), or NULL */
uint32_t    ksym_find(const char *name);                    /* address of a function, 0 if unknown */
uint32_t    ksym_count(void);
const char *ksym_at(uint32_t index, uint32_t *addr);        /* for listing, in address order */

#endif
