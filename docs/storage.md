# Block devices, partitions and ext2

## Block layer (`kernel/blk.c`)
Everything that stores sectors is a *block device*: a name, a size in 512-byte sectors and read/write functions.

| name | what |
|------|------|
| `hd0`, `hd1` | the ATA drives (primary master / slave) |
| `ram0`... | RAM disks created with `ramdisk create` |
| `loop0`... | files presented as disks with `losetup` |
| `hd0p1`, `ram0p2`... | partitions found in an MBR |

`blkdev`, `ramdisk`, `losetup` and `fdisk` are the shell commands.

## ext2 layout (what the read-only driver understands)
Block size is 1024, 2048 or 4096 bytes (`1024 << s_log_block_size`).

```
block 0       boot block (unused)
block 1       superblock (always at byte offset 1024): inode count, block count, block size,
              blocks/inodes per group, magic 0xEF53, revision, inode size
block 2       block group descriptor table: per group, the block numbers of the block bitmap,
              inode bitmap and inode table
...           per group: block bitmap, inode bitmap, inode table, data blocks
```

* An **inode** (128 bytes in revision 0, `s_inode_size` in revision 1) holds mode, size, times and 15 block
  pointers: 12 direct, then single, double and triple indirect.
* Inode numbers start at 1; the root directory is inode 2. Inode *n* lives in group `(n-1) / inodes_per_group`,
  at index `(n-1) % inodes_per_group` of that group's inode table.
* A **directory** is a file of variable-length records: `inode(4) rec_len(2) name_len(1) type(1) name`.
  `rec_len` chains the records; inode 0 marks an unused one.
* Short symbolic links (< 60 bytes) keep their target in the pointer area of the inode itself.

The driver (`kernel/ext2.c`) reads superblock, group descriptors, inodes, directories and file data through direct
and singly/doubly indirect blocks. It mounts on a path with `ext2 mount DEVICE /path`.
`tools/mkext2.c` builds a small test image (optionally behind an MBR) on the host.
