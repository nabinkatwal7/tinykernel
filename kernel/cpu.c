#include "cpu.h"

#include "io.h"
#include "klog.h"
#include "kstring.h"
#include "timer.h"

static struct cpu_info info;

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* CPUID exists iff software can flip bit 21 (ID) of EFLAGS. */
static int cpuid_supported(void)
{
	uint32_t before, after;

	__asm__ volatile (
		"pushfl\n\t"
		"pushfl\n\t"
		"popl %0\n\t"
		"movl %0, %1\n\t"
		"xorl $0x200000, %0\n\t"
		"pushl %0\n\t"
		"popfl\n\t"
		"pushfl\n\t"
		"popl %0\n\t"
		"popfl"
		: "=&r"(after), "=&r"(before) : : "cc");
	return (after ^ before) & 0x200000;
}

static void put_reg(char *dst, uint32_t r)
{
	int i;

	for (i = 0; i < 4; i++)
		dst[i] = (char)(r >> (8 * i));
}

void cpu_init(void)
{
	uint32_t a, b, c, d, i;

	memset(&info, 0, sizeof info);
	info.has_cpuid = cpuid_supported() != 0;
	if (!info.has_cpuid) {
		kstrlcpy(info.vendor, "unknown", sizeof info.vendor);
		klog(LOG_INFO, "cpu: no CPUID (pre-Pentium?)");
		return;
	}
	cpuid(0, &a, &b, &c, &d);
	info.max_leaf = a;
	put_reg(info.vendor, b);
	put_reg(info.vendor + 4, d); /* the vendor string is EBX, EDX, ECX in that order */
	put_reg(info.vendor + 8, c);
	info.vendor[12] = '\0';

	if (info.max_leaf >= 1) {
		cpuid(1, &a, &b, &c, &d);
		info.stepping = a & 0xF;
		info.model = (a >> 4) & 0xF;
		info.family = (a >> 8) & 0xF;
		if (info.family == 0xF)
			info.family += (a >> 20) & 0xFF;
		if (info.family == 0xF || info.family == 6)
			info.model |= ((a >> 16) & 0xF) << 4;
		info.features_ecx = c;
		info.features_edx = d;
	}
	cpuid(0x80000000u, &a, &b, &c, &d);
	info.max_ext_leaf = a;
	if (info.max_ext_leaf >= 0x80000004u) {
		for (i = 0; i < 3; i++) {
			cpuid(0x80000002u + i, &a, &b, &c, &d);
			put_reg(info.brand + 16 * i, a);
			put_reg(info.brand + 16 * i + 4, b);
			put_reg(info.brand + 16 * i + 8, c);
			put_reg(info.brand + 16 * i + 12, d);
		}
		info.brand[48] = '\0';
	}
	klog(LOG_INFO, "cpu: %s family %u model %u stepping %u", info.vendor, info.family, info.model,
	     info.stepping);
}

const struct cpu_info *cpu_get(void)
{
	return &info;
}

uint64_t cpu_rdtsc(void)
{
	uint32_t lo, hi;

	__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

/* Count TSC cycles across ~10 timer ticks (100 ms). */
uint32_t cpu_mhz_estimate(void)
{
	uint32_t start_tick;
	uint64_t t0, t1;

	if (!(info.features_edx & CPU_TSC))
		return 0;
	start_tick = timer_ticks();
	while (timer_ticks() == start_tick) /* align to a tick edge */
		;
	start_tick = timer_ticks();
	t0 = cpu_rdtsc();
	while (timer_ticks() - start_tick < 10)
		;
	t1 = cpu_rdtsc();
	/* cycles per 10 ticks -> MHz: cycles / (10 * 1000000 / hz) microseconds */
	return (uint32_t)(t1 - t0) / (10u * 1000000u / timer_hz()); /* cycles per microsecond */
}
