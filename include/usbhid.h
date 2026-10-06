#ifndef USBHID_H
#define USBHID_H

#include <stdint.h>

/* Decoder for USB HID boot-protocol keyboard reports (see docs/usb.md). */
#define USBKBD_UP    0x101   /* the same numbers the PS/2 keyboard driver uses */
#define USBKBD_DOWN  0x102
#define USBKBD_LEFT  0x103
#define USBKBD_RIGHT 0x104

void usbkbd_reset(void);
void usbkbd_report(const uint8_t report[8], void (*emit)(int key));
int  cmd_usbkbd(int argc, char **argv);

#endif
