/*
 * Host tool: fills the kernel's symbol table after linking.
 *   nm -n build/kernel.pe | ksymgen build/kernel.bin 0xC0100000
 * Reads nm's "address type name" lines, keeps the text symbols (T/t), and writes
 *   u32 count | count x { u32 address, u32 name offset } | NUL-separated names
 * into the reserved blob (symbol _ksym_blob) inside kernel.bin, whose first byte is at the base address.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SYMS   2048
#define NAMES_SIZE 24576
#define BLOB_SIZE  (4 + MAX_SYMS * 8 + NAMES_SIZE)

int main(int argc, char **argv)
{
	static uint32_t addr[MAX_SYMS], name_off[MAX_SYMS];
	static char names[NAMES_SIZE];
	char line[512], type, name[400];
	unsigned long a, blob = 0, base;
	unsigned n = 0, names_len = 0, i;
	uint8_t *img;
	long size;
	FILE *f;

	if (argc != 3) {
		fprintf(stderr, "usage: nm -n kernel.pe | ksymgen kernel.bin base\n");
		return 1;
	}
	base = strtoul(argv[2], 0, 0);
	while (fgets(line, sizeof line, stdin)) {
		if (sscanf(line, "%lx %c %399s", &a, &type, name) != 3)
			continue;
		if (!strcmp(name, "_ksym_blob"))
			blob = a;
		if ((type != 'T' && type != 't') || name[0] == '.')
			continue;
		if (name[0] == '_')
			memmove(name, name + 1, strlen(name)); /* C name: drop the COFF underscore */
		if (n >= MAX_SYMS || names_len + strlen(name) + 1 > NAMES_SIZE) {
			fprintf(stderr, "ksymgen: symbol table full (raise MAX_SYMS/NAMES_SIZE)\n");
			return 1;
		}
		addr[n] = (uint32_t)a;
		name_off[n] = names_len;
		strcpy(names + names_len, name);
		names_len += (unsigned)strlen(name) + 1;
		n++;
	}
	if (!blob) {
		fprintf(stderr, "ksymgen: no _ksym_blob symbol\n");
		return 1;
	}
	f = fopen(argv[1], "r+b");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	if (blob - base + BLOB_SIZE > (unsigned long)size) {
		fprintf(stderr, "ksymgen: blob lies outside the image\n");
		return 1;
	}
	img = calloc(1, BLOB_SIZE);
	memcpy(img, &n, 4);
	for (i = 0; i < n; i++) {
		memcpy(img + 4 + i * 8, &addr[i], 4);
		memcpy(img + 4 + i * 8 + 4, &name_off[i], 4);
	}
	memcpy(img + 4 + MAX_SYMS * 8, names, names_len);
	fseek(f, (long)(blob - base), SEEK_SET);
	fwrite(img, 1, BLOB_SIZE, f);
	fclose(f);
	printf("ksymgen: %u symbols, %u bytes of names\n", n, names_len);
	return 0;
}
