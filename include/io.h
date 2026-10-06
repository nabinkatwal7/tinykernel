#ifndef IO_H
#define IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t v;

	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void outw(uint16_t port, uint16_t v)
{
	__asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
	uint16_t v;

	__asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void sti(void) { __asm__ volatile ("sti" : : : "memory"); }
static inline void cli(void) { __asm__ volatile ("cli" : : : "memory"); }
static inline void hlt(void) { __asm__ volatile ("hlt" : : : "memory"); }

/* Disable interrupts, returning the previous EFLAGS for irq_restore(). */
static inline uint32_t irq_save(void)
{
	uint32_t f;

	__asm__ volatile ("pushfl; popl %0; cli" : "=r"(f) : : "memory");
	return f;
}

static inline void irq_restore(uint32_t f)
{
	__asm__ volatile ("pushl %0; popfl" : : "r"(f) : "memory", "cc");
}

#endif
