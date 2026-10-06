#include "acpi.h"

#include "io.h"
#include "kstring.h"
#include "klog.h"
#include "paging.h"
#include "pmm.h"

struct rsdp {
	char signature[8];
	uint8_t checksum;
	char oem[6];
	uint8_t revision;
	uint32_t rsdt;
} __attribute__((packed));

struct sdt_header {
	char signature[4];
	uint32_t length;
	uint8_t revision, checksum;
	char oem[6], oem_table[8];
	uint32_t oem_rev, creator, creator_rev;
} __attribute__((packed));

struct fadt {
	struct sdt_header h;
	uint32_t firmware_ctrl, dsdt;
	uint8_t reserved;
	uint8_t pm_profile;
	uint16_t sci_int;
	uint32_t smi_cmd;
	uint8_t acpi_enable, acpi_disable, s4bios_req, pstate_cnt;
	uint32_t pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt;
} __attribute__((packed));

#define MAX_TABLES 24

static int ok;
static uint8_t rsdp_rev;
static char oem_id[7];
static uint32_t pm1a_cnt, pm1b_cnt, smi_cmd;
static uint8_t acpi_enable_val;
static int slp_typa = -1, slp_typb = -1;
static struct acpi_table_info tables[MAX_TABLES];
static int ntables;

static int checksum_ok(const void *p, uint32_t len)
{
	const uint8_t *b = p;
	uint8_t sum = 0;
	uint32_t i;

	for (i = 0; i < len; i++)
		sum = (uint8_t)(sum + b[i]);
	return sum == 0;
}

/* The firmware tables sit in "reserved" RAM near the top of memory, outside our identity map. */
static const void *map_phys(uint32_t phys, uint32_t len)
{
	uint32_t page;

	for (page = phys & ~0xFFFu; page < phys + len; page += PAGE_SIZE)
		if (!paging_is_mapped(page) && paging_map(0, page, page, PTE_RW))
			return 0;
	return (const void *)phys;
}

static const struct sdt_header *map_table(uint32_t phys)
{
	const struct sdt_header *h = map_phys(phys, sizeof(struct sdt_header));

	if (!h)
		return 0;
	return map_phys(phys, h->length) ? h : 0; /* map the rest now that the length is known */
}

static const struct rsdp *find_rsdp(void)
{
	uint32_t a;
	/* the spec: the first KiB of the EBDA, or the BIOS ROM area 0xE0000-0xFFFFF, on 16-byte boundaries */
	uint32_t ebda = (uint32_t)(*(volatile uint16_t *)0x40E) << 4;

	for (a = ebda; ebda && a < ebda + 1024; a += 16)
		if (!memcmp((void *)a, "RSD PTR ", 8) && checksum_ok((void *)a, 20))
			return (const struct rsdp *)a;
	for (a = 0xE0000; a < 0x100000; a += 16)
		if (!memcmp((void *)a, "RSD PTR ", 8) && checksum_ok((void *)a, 20))
			return (const struct rsdp *)a;
	return 0;
}

/* The DSDT's AML contains Name(\_S5_, Package(){SLP_TYPa, SLP_TYPb, ...}); find the byte values. */
static void parse_s5(const struct sdt_header *dsdt)
{
	const uint8_t *p = (const uint8_t *)dsdt;
	uint32_t i, len = dsdt->length;

	for (i = 0; i + 8 < len; i++) {
		uint32_t j;

		if (memcmp(p + i, "_S5_", 4))
			continue;
		if (!(i >= 1 && p[i - 1] == 0x08) && !(i >= 2 && p[i - 2] == 0x08 && p[i - 1] == '\\'))
			continue; /* not preceded by NameOp */
		if (p[i + 4] != 0x12)
			continue; /* not followed by PackageOp */
		j = i + 5;
		j += ((p[j] & 0xC0) >> 6) + 2; /* skip the PkgLength (1-4 bytes) and the element count */
		if (p[j] == 0x0A)
			j++; /* BytePrefix */
		slp_typa = p[j];
		j++;
		if (p[j] == 0x0A)
			j++;
		slp_typb = p[j];
		return;
	}
}

int acpi_init(void)
{
	const struct rsdp *r = find_rsdp();
	const struct sdt_header *rsdt;
	uint32_t i, n;

	ok = 0;
	if (!r) {
		klog(LOG_INFO, "acpi: no RSDP");
		return -1;
	}
	rsdp_rev = r->revision;
	memcpy(oem_id, r->oem, 6);
	oem_id[6] = '\0';
	rsdt = map_table(r->rsdt);
	if (!rsdt || memcmp(rsdt->signature, "RSDT", 4) || !checksum_ok(rsdt, rsdt->length)) {
		klog(LOG_WARN, "acpi: bad RSDT");
		return -1;
	}
	n = (rsdt->length - sizeof *rsdt) / 4;
	ntables = 0;
	for (i = 0; i < n && ntables < MAX_TABLES; i++) {
		uint32_t phys = ((const uint32_t *)((const uint8_t *)rsdt + sizeof *rsdt))[i];
		const struct sdt_header *t = map_table(phys);

		if (!t)
			continue;
		memcpy(tables[ntables].signature, t->signature, 4);
		tables[ntables].signature[4] = '\0';
		tables[ntables].address = phys;
		tables[ntables].length = t->length;
		ntables++;
		if (!memcmp(t->signature, "FACP", 4) && checksum_ok(t, t->length)) {
			const struct fadt *f = (const struct fadt *)t;
			const struct sdt_header *dsdt = map_table(f->dsdt);

			smi_cmd = f->smi_cmd;
			acpi_enable_val = f->acpi_enable;
			pm1a_cnt = f->pm1a_cnt;
			pm1b_cnt = f->pm1b_cnt;
			if (dsdt && !memcmp(dsdt->signature, "DSDT", 4))
				parse_s5(dsdt);
		}
	}
	ok = pm1a_cnt && slp_typa >= 0;
	klog(LOG_INFO, "acpi: rev %u, %d tables, PM1a_CNT=%x, SLP_TYP=%d, %s", rsdp_rev, ntables, pm1a_cnt, slp_typa,
	     ok ? "power-off available" : "no power-off info");
	return ok ? 0 : -1;
}

int acpi_available(void)
{
	return ok;
}

int acpi_table_count(void)
{
	return ntables;
}

int acpi_table_get(int i, struct acpi_table_info *out)
{
	if (i < 0 || i >= ntables)
		return -1;
	*out = tables[i];
	return 0;
}

void acpi_describe(uint8_t *revision, char oem[7], uint32_t *cnt, uint32_t *smi, int *typ, int *enabled)
{
	*revision = rsdp_rev;
	memcpy(oem, oem_id, 7);
	*cnt = pm1a_cnt;
	*smi = smi_cmd;
	*typ = slp_typa;
	*enabled = pm1a_cnt ? (inw((uint16_t)pm1a_cnt) & 1) : 0; /* SCI_EN: ACPI mode already active */
}

void acpi_poweroff(void)
{
	if (ok) {
		if (smi_cmd && acpi_enable_val && !(inw((uint16_t)pm1a_cnt) & 1)) {
			int i;

			outb((uint16_t)smi_cmd, acpi_enable_val); /* switch from legacy to ACPI mode */
			for (i = 0; i < 300 && !(inw((uint16_t)pm1a_cnt) & 1); i++)
				;
		}
		outw((uint16_t)pm1a_cnt, (uint16_t)((slp_typa << 10) | (1 << 13))); /* SLP_EN */
		if (pm1b_cnt && slp_typb >= 0)
			outw((uint16_t)pm1b_cnt, (uint16_t)((slp_typb << 10) | (1 << 13)));
	}
	/* emulator shortcuts: QEMU (newer) and Bochs/old QEMU/VirtualBox ports */
	outw(0x604, 0x2000);
	outw(0xB004, 0x2000);
	outw(0x4004, 0x3400);
}
