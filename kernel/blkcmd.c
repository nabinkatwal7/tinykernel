#include "blk.h"

#include "console.h"
#include "fs.h"
#include "kstring.h"
#include "vfs.h"

/* Shell commands for the block layer. */

static int fail(const char *what, int rc)
{
	console_printf("%s: %s\n", what, fs_strerror(rc));
	return 1;
}

/* blkdev : list block devices */
int cmd_blkdev(int argc, char **argv)
{
	int i, n = 0;

	(void)argc;
	(void)argv;
	console_write("NAME      KIND  SECTORS   SIZE\n");
	for (i = 0; i < BLK_MAX; i++) {
		const struct blkdev *d = blk_get(i);

		if (!d)
			continue;
		console_printf("%-9s %-5s %7u  %u KiB%s%s\n", d->name, d->kind, d->sectors, d->sectors / 2,
			       d->parent ? "  of " : "", d->parent ? d->parent->name : "");
		n++;
	}
	if (!n)
		console_write("(none)\n");
	return 0;
}

/* ramdisk create NAME KIB | ramdisk destroy NAME | ramdisk test */
int cmd_ramdisk(int argc, char **argv)
{
	uint32_t kib;
	int rc;

	if (argc == 4 && !kstrcmp(argv[1], "create") && !kstrtoul(argv[3], &kib)) {
		rc = ramdisk_create(argv[2], kib);
		return rc ? fail(argv[2], rc) : 0;
	}
	if (argc == 3 && !kstrcmp(argv[1], "destroy")) {
		rc = blk_remove(argv[2]);
		return rc ? fail(argv[2], rc) : 0;
	}
	if (argc == 2 && !kstrcmp(argv[1], "test")) { /* write a pattern to every sector, read it all back */
		uint8_t sec[512], back[512];
		struct blkdev *d;
		uint32_t s, bad = 0;
		int i;

		if (ramdisk_create("ramtest", 64))
			return 1;
		d = blk_find("ramtest");
		for (s = 0; s < d->sectors; s++) {
			for (i = 0; i < 512; i++)
				sec[i] = (uint8_t)(s * 7 + (uint32_t)i);
			blk_write(d, s, 1, sec);
		}
		for (s = 0; s < d->sectors; s++) {
			blk_read(d, s, 1, back);
			for (i = 0; i < 512; i++)
				bad += back[i] != (uint8_t)(s * 7 + (uint32_t)i);
		}
		bad += blk_read(d, d->sectors, 1, back) == 0; /* out of range must fail */
		blk_remove("ramtest");
		console_printf("ramdisk test: %u errors: %s\n", bad, bad ? "FAILED" : "ok");
		return bad != 0;
	}
	console_write("usage: ramdisk create NAME KIB | ramdisk destroy NAME | ramdisk test\n");
	return 1;
}

/* losetup NAME FILE | losetup -d NAME | losetup -s NAME (write back to the file) */
int cmd_losetup(int argc, char **argv)
{
	int rc;

	if (argc == 3 && !kstrcmp(argv[1], "-d")) {
		rc = blk_remove(argv[2]);
		return rc ? fail(argv[2], rc) : 0;
	}
	if (argc == 3 && !kstrcmp(argv[1], "-s")) {
		rc = loop_sync(argv[2]);
		return rc ? fail(argv[2], rc) : 0;
	}
	if (argc == 3) {
		rc = loop_attach(argv[1], argv[2]);
		return rc ? fail(argv[2], rc) : 0;
	}
	if (argc == 2 && !kstrcmp(argv[1], "test")) { /* a file as a disk: write through the device, detach, attach again, read back */
		static uint8_t content[4096];
		uint8_t sec[512], back[512];
		int bad = 0, i;

		for (i = 0; i < 512; i++)
			sec[i] = (uint8_t)(i * 3 + 1);
		if (vfs_write("looptest.img", content, sizeof content) || loop_attach("looptest", "looptest.img"))
			return 1;
		bad += blk_write(blk_find("looptest"), 5, 1, sec) != 0;
		bad += blk_read(blk_find("looptest"), 8, 1, back) == 0; /* past the end */
		blk_remove("looptest"); /* flushes to the file */
		if (loop_attach("looptest", "looptest.img"))
			return 1;
		bad += blk_read(blk_find("looptest"), 5, 1, back) != 0 || memcmp(back, sec, 512);
		blk_remove("looptest");
		vfs_unlink("looptest.img");
		console_printf("losetup test: %d errors: %s\n", bad, bad ? "FAILED" : "ok");
		return bad != 0;
	}
	console_write("usage: losetup NAME FILE | losetup -d NAME | losetup -s NAME\n");
	return 1;
}

/* fdisk DEVICE : print the partition table and register the partitions; fdisk -demo DEVICE writes a sample table */
int cmd_fdisk(int argc, char **argv)
{
	struct blkdev *d;
	struct part_entry pe[4];
	int i, n;

	if (argc == 3 && !kstrcmp(argv[1], "-demo")) {
		d = blk_find(argv[2]);
		if (!d || kstrcmp(d->kind, "ram"))
			return fail(argv[2], FS_EINVAL); /* only for RAM disks: never overwrite a real disk by accident */
		return part_write_demo(d) ? 1 : 0;
	}
	if (argc == 2 && !kstrcmp(argv[1], "test")) { /* a partition must map to the right sectors of its parent */
		uint8_t sec[512], back[512];
		struct blkdev *r, *p2;
		int bad = 0;

		if (ramdisk_create("fdtest", 256))
			return 1;
		r = blk_find("fdtest");
		bad += part_write_demo(r) != 0;
		bad += part_scan(r) != 2;
		p2 = blk_find("fdtestp2");
		bad += !p2 || !blk_find("fdtestp1");
		memset(sec, 0x5A, sizeof sec);
		if (p2) {
			bad += blk_write(p2, 3, 1, sec) != 0;
			bad += blk_read(r, p2->offset + 3, 1, back) != 0 || memcmp(back, sec, 512);
			bad += blk_read(p2, p2->sectors, 1, back) == 0; /* beyond the partition */
		}
		blk_remove("fdtest");
		bad += blk_find("fdtestp1") != 0; /* partitions disappear with their disk */
		console_printf("fdisk test: %d errors: %s\n", bad, bad ? "FAILED" : "ok");
		return bad != 0;
	}
	if (argc != 2) {
		console_write("usage: fdisk DEVICE | fdisk -demo RAMDISK\n");
		return 1;
	}
	d = blk_find(argv[1]);
	if (!d)
		return fail(argv[1], FS_ENOENT);
	n = part_read_table(d, pe);
	if (n < 0) {
		console_printf("%s: no partition table (no 55AA signature)\n", d->name);
		return 1;
	}
	console_printf("%s: %u sectors, %d partition(s)\n", d->name, d->sectors, n);
	console_write("  #  boot type      start   sectors\n");
	for (i = 0; i < 4; i++)
		if (pe[i].type)
			console_printf("  %d  %-4s  0x%02x %9u %9u\n", i + 1, pe[i].boot == 0x80 ? "yes" : "no", pe[i].type,
				       pe[i].start, pe[i].sectors);
	n = part_scan(d);
	if (n > 0)
		console_printf("registered %d partition device(s) (see blkdev)\n", n);
	return 0;
}
