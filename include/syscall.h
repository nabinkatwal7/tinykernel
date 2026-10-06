#ifndef SYSCALL_H
#define SYSCALL_H

#include "idt.h"

/* int 0x80 ABI: eax = number, ebx/ecx = arguments, result returned in eax. */
#define SYS_EXIT     1 /* ebx = exit code */
#define SYS_WRITE    2 /* ebx = pointer, ecx = length */
#define SYS_PUTCHAR  3 /* ebx = char */
#define SYS_TICKS    4 /* returns timer ticks */
#define SYS_GETKEY   5 /* blocks, returns a key code */
#define SYS_SLEEP    6 /* ebx = milliseconds */
#define SYS_OPEN     7 /* ebx = path, ecx = flags; returns fd or a negative error */
#define SYS_CLOSE    8 /* ebx = fd */
#define SYS_READ     9 /* ebx = fd, ecx = buffer, edx = count; returns bytes, 0 at EOF, or < 0 */
#define SYS_GETPID  11 /* id of the task the program runs in */
#define SYS_GETPPID 12
#define SYS_SBRK    13 /* ebx = delta; returns the old program break or -1 */
#define SYS_EXEC    14 /* ebx = path, ecx = NULL-terminated argv (or 0); does not return on success */
#define SYS_SPAWN   15 /* like exec, but runs the program to completion and returns its exit code */
#define SYS_KCMD    16 /* ebx = command line for the kernel shell; returns its status */
#define SYS_DUP     17 /* ebx = fd; returns a new descriptor */
#define SYS_DUP2    18 /* ebx = fd, ecx = target */
#define SYS_MKDIR   19 /* ebx = path */
#define SYS_RMDIR   20 /* ebx = path */
#define SYS_GETCWD  21 /* ebx = buffer, ecx = size; returns length or -1 */
#define SYS_CHDIR   22 /* ebx = path */
#define SYS_LSEEK   23 /* ebx = fd, ecx = offset, edx = whence (0 set, 1 cur, 2 end); returns the new position */
#define SYS_CLOCK   24 /* ebx = clock id, ecx = struct timespec * (sec, nsec); returns 0 or -1 */
#define SYS_NANOSLEEP 25 /* ebx = seconds, ecx = nanoseconds */
#define SYS_FORK    26 /* returns the child's pid in the parent and 0 in the child */
#define SYS_WAITPID 27 /* ebx = pid, ecx = int * status (or 0); returns 0, or -1 if there is no such child */
#define SYS_SHMGET  28 /* ebx = key, ecx = size: segment id or -1 */
#define SYS_SHMAT   29 /* ebx = id: address or 0 */
#define SYS_SHMDT   30 /* ebx = address */
#define SYS_MMAP    31 /* ebx = path, ecx = length (0 = whole file), edx = flags (1 = shared); returns an address or 0 */
#define SYS_MUNMAP  32 /* ebx = address */
#define SYS_MSYNC   33 /* ebx = address */
#define SYS_PIPE    34 /* ebx = int[2] receiving the read and write descriptors */
#define SYS_TRYKEY  35 /* a key if one is waiting, else -1 (never blocks) */
#define SYS_PUTAT   36 /* ebx = column, ecx = row, edx = (attribute << 8) | character: write straight to the text screen */
#define SYS_CLS     37 /* clear the screen */
#define SYS_SOCKET  40 /* ebx = SOCK_STREAM (1) or SOCK_DGRAM (2); returns a descriptor */
#define SYS_CONNECT 41 /* ebx = socket, ecx = IPv4 address (host byte order), edx = port */
#define SYS_BIND    42 /* ebx = socket, ecx = port */
#define SYS_LISTEN  43 /* ebx = socket */
#define SYS_ACCEPT  44 /* ebx = listening socket; returns the connection's descriptor */
#define SYS_SENDTO  45 /* ebx = socket, ecx = struct { buf, len, ip, port } */
#define SYS_RECVFROM 46 /* ebx = socket, ecx = struct { buf, cap, uint32 *ip, uint32 *port }; the last two may be NULL */
#define SYS_RESOLVE 47 /* ebx = host name, ecx = uint32 receiving the address */
#define SYS_DIRLIST 48 /* ebx = path, ecx = buffer, edx = size: one "name size d|f" line per entry; returns the byte count or a negative error */
#define SYS_FLOCK   39 /* ebx = fd, ecx = LOCK_SH / LOCK_EX / LOCK_UN, optionally | LOCK_NB */
#define SYS_GFX     38 /* ebx = operation, ecx = int[6] of arguments: the 320x200 graphics screen (see GFX_* below) */
#define GFX_ENTER   0  /* switch to graphics mode */
#define GFX_LEAVE   1  /* back to text mode */
#define GFX_RECT    2  /* x, y, w, h, color: filled rectangle */
#define GFX_TEXT    3  /* x, y, fg, bg (-1 = transparent), string pointer */
#define GFX_CLEAR   4  /* color */
#define GFX_PALETTE 5  /* index, r, g, b (0-255) */
#define SYS_FWRITE  10 /* ebx = fd, ecx = buffer, edx = count; fd 1 and 2 are the console */

void syscall_dispatch(struct regs *r);

#endif
