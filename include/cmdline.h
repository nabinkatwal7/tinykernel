#ifndef CMDLINE_H
#define CMDLINE_H

/*
 * Kernel parameters: space-separated words from the bootloader's menu choice (safe, debug, gfx)
 * followed by the contents of /boot.cfg when it exists. key=value words are supported too.
 *
 *   safe    no network, no mouse, no speaker      debug    show INFO logs on the screen
 *   gfx     start with the graphical console      nonet / nomouse / nobeep   one device each
 */
void        cmdline_init(void);                     /* reads the boot menu choice (call early) */
void        cmdline_load_config(void);              /* appends /boot.cfg (call once the disk is mounted) */
int         cmdline_has(const char *word);          /* is the bare word present? */
const char *cmdline_get(const char *key);           /* value of key=value, or NULL */
const char *cmdline_all(void);

#endif
