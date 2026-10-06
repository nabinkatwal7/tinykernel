/*
 * Host tool: build a 720 KB FAT12 floppy image from a directory tree.
 *   mkfat12 out.img rootdir
 * File names are converted to upper-case 8.3; anything that does not fit is skipped.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SECTORS   1440
#define SPC       2       /* sectors per cluster */
#define RESERVED  1
#define NFATS     2
#define SPF       3
#define ROOT_ENT  112
#define ROOT_SECS (ROOT_ENT * 32 / 512)
#define DATA_START (RESERVED + NFATS * SPF + ROOT_SECS)
#define NCLUSTERS ((SECTORS - DATA_START) / SPC)

static uint8_t img[SECTORS * 512];
static uint16_t fat[NCLUSTERS + 2];
static int next_free = 2;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

static uint8_t *cluster_ptr(int c)
{
	return img + (DATA_START + (c - 2) * SPC) * 512;
}

/* Allocate a chain big enough for 'bytes' (at least one cluster); returns the first cluster. */
static int alloc_chain(uint32_t bytes)
{
	int n = (int)((bytes + SPC * 512 - 1) / (SPC * 512)), first, i;

	if (n == 0)
		n = 1;
	if (next_free + n > NCLUSTERS + 2) {
		fprintf(stderr, "mkfat12: image full\n");
		exit(1);
	}
	first = next_free;
	for (i = 0; i < n; i++)
		fat[first + i] = (uint16_t)(i + 1 < n ? first + i + 1 : 0xFFF);
	next_free += n;
	return first;
}

static int make_name(const char *name, uint8_t out[11])
{
	const char *dot = strrchr(name, '.');
	size_t base = dot ? (size_t)(dot - name) : strlen(name), ext = dot ? strlen(dot + 1) : 0, i;

	if (base == 0 || base > 8 || ext > 3)
		return -1;
	memset(out, ' ', 11);
	for (i = 0; i < base; i++)
		out[i] = (uint8_t)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
	for (i = 0; i < ext; i++)
		out[8 + i] = (uint8_t)(dot[1 + i] >= 'a' && dot[1 + i] <= 'z' ? dot[1 + i] - 32 : dot[1 + i]);
	return 0;
}

static void set_entry(uint8_t *e, const uint8_t name[11], int attr, int cluster, uint32_t size)
{
	memcpy(e, name, 11);
	e[11] = (uint8_t)attr;
	put16(e + 14, 0x6000); /* time */
	put16(e + 16, 0x4A21); /* date 2017-01-01-ish */
	put16(e + 24, 0x4A21);
	put16(e + 26, (uint16_t)cluster);
	put32(e + 28, size);
}

/* Fill directory 'dirbuf' (capacity 'cap' entries) from host directory 'path'. */
static void fill_dir(const char *path, uint8_t *dirbuf, int cap, int self_cluster, int parent_cluster)
{
	DIR *d = opendir(path);
	struct dirent *de;
	int n = 0;

	if (!d) {
		fprintf(stderr, "mkfat12: cannot open %s\n", path);
		exit(1);
	}
	if (self_cluster) { /* subdirectories start with . and .. */
		uint8_t dot[11], dotdot[11];

		memset(dot, ' ', 11);
		memset(dotdot, ' ', 11);
		dot[0] = '.';
		dotdot[0] = dotdot[1] = '.';
		set_entry(dirbuf + n++ * 32, dot, 0x10, self_cluster, 0);
		set_entry(dirbuf + n++ * 32, dotdot, 0x10, parent_cluster, 0);
	}
	while ((de = readdir(d))) {
		char full[512];
		struct stat st;
		uint8_t name[11];

		if (de->d_name[0] == '.' || make_name(de->d_name, name))
			continue;
		snprintf(full, sizeof full, "%s/%s", path, de->d_name);
		if (stat(full, &st))
			continue;
		if (n >= cap) {
			fprintf(stderr, "mkfat12: directory %s has too many entries\n", path);
			exit(1);
		}
		if (S_ISDIR(st.st_mode)) {
			int count = 2, cl;
			DIR *sub = opendir(full);
			struct dirent *se;

			while (sub && (se = readdir(sub)))
				if (se->d_name[0] != '.')
					count++;
			if (sub)
				closedir(sub);
			cl = alloc_chain((uint32_t)count * 32);
			{
				/* a chain may span clusters: they are consecutive, so one flat buffer works */
				fill_dir(full, cluster_ptr(cl), count, cl, self_cluster ? self_cluster : 0);
			}
			set_entry(dirbuf + n++ * 32, name, 0x10, cl, 0);
		} else {
			FILE *f = fopen(full, "rb");
			uint8_t *buf;
			int cl = 0;

			if (!f)
				continue;
			buf = malloc((size_t)st.st_size + 1);
			if (fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size)
				st.st_size = 0;
			fclose(f);
			if (st.st_size > 0) {
				cl = alloc_chain((uint32_t)st.st_size);
				memcpy(cluster_ptr(cl), buf, (size_t)st.st_size);
			}
			free(buf);
			set_entry(dirbuf + n++ * 32, name, 0x20, cl, (uint32_t)st.st_size);
		}
	}
	closedir(d);
}

int main(int argc, char **argv)
{
	uint8_t *bs = img;
	int i;
	FILE *out;

	if (argc != 3) {
		fprintf(stderr, "usage: mkfat12 out.img rootdir\n");
		return 1;
	}
	bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
	memcpy(bs + 3, "TINYOS  ", 8);
	put16(bs + 11, 512);
	bs[13] = SPC;
	put16(bs + 14, RESERVED);
	bs[16] = NFATS;
	put16(bs + 17, ROOT_ENT);
	put16(bs + 19, SECTORS);
	bs[21] = 0xF9;
	put16(bs + 22, SPF);
	put16(bs + 24, 9);
	put16(bs + 26, 2);
	bs[38] = 0x29;
	put32(bs + 39, 0x12345678);
	memcpy(bs + 43, "TINYFAT    ", 11);
	memcpy(bs + 54, "FAT12   ", 8);
	bs[510] = 0x55;
	bs[511] = 0xAA;

	fat[0] = 0xFF9;
	fat[1] = 0xFFF;
	fill_dir(argv[2], img + (RESERVED + NFATS * SPF) * 512, ROOT_ENT, 0, 0);

	for (i = 0; i < NFATS; i++) { /* pack the 12-bit entries into both FAT copies */
		uint8_t *f = img + (RESERVED + i * SPF) * 512;
		int c;

		for (c = 0; c < NCLUSTERS + 2; c++) {
			int off = c + c / 2;

			if (c & 1) {
				f[off] = (uint8_t)((f[off] & 0x0F) | ((fat[c] & 0x0F) << 4));
				f[off + 1] = (uint8_t)(fat[c] >> 4);
			} else {
				f[off] = (uint8_t)fat[c];
				f[off + 1] = (uint8_t)((f[off + 1] & 0xF0) | ((fat[c] >> 8) & 0x0F));
			}
		}
	}
	out = fopen(argv[1], "wb");
	if (!out || fwrite(img, 1, sizeof img, out) != sizeof img) {
		fprintf(stderr, "mkfat12: cannot write %s\n", argv[1]);
		return 1;
	}
	fclose(out);
	printf("mkfat12: %s: %d clusters used of %d\n", argv[1], next_free - 2, NCLUSTERS);
	return 0;
}
