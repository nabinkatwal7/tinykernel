/* Multiboot v1 header — lets QEMU -kernel load us before we have a bootloader. */
__attribute__((section(".multiboot"), aligned(4)))
const unsigned int multiboot_header[] = {
	0x1BADB002u, /* magic */
	0x00000000u, /* flags */
	0xE4524FFEu, /* checksum = -(magic + flags) */
};
