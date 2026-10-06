#ifndef CPU_H
#define CPU_H

#include <stdint.h>

struct cpu_info {
	int has_cpuid;
	uint32_t max_leaf, max_ext_leaf;
	char vendor[13];     /* "GenuineIntel", "AuthenticAMD", ... */
	char brand[49];      /* marketing name, empty if the CPU has none */
	uint32_t family, model, stepping;
	uint32_t features_edx, features_ecx;   /* CPUID leaf 1 */
};

/* Leaf 1 EDX feature bits */
#define CPU_FPU  (1u << 0)
#define CPU_PSE  (1u << 3)
#define CPU_TSC  (1u << 4)
#define CPU_MSR  (1u << 5)
#define CPU_PAE  (1u << 6)
#define CPU_CX8  (1u << 8)
#define CPU_APIC (1u << 9)
#define CPU_SEP  (1u << 11)
#define CPU_PGE  (1u << 13)
#define CPU_CMOV (1u << 15)
#define CPU_MMX  (1u << 23)
#define CPU_FXSR (1u << 24)
#define CPU_SSE  (1u << 25)
#define CPU_SSE2 (1u << 26)
/* Leaf 1 ECX feature bits */
#define CPU_SSE3   (1u << 0)
#define CPU_SSSE3  (1u << 9)
#define CPU_SSE41  (1u << 19)
#define CPU_SSE42  (1u << 20)
#define CPU_POPCNT (1u << 23)
#define CPU_HYPERV (1u << 31)

void cpu_init(void);                         /* detect once, at boot */
const struct cpu_info *cpu_get(void);
uint64_t cpu_rdtsc(void);
uint32_t cpu_mhz_estimate(void);             /* measured against the PIT; 0 without a TSC */

#endif
