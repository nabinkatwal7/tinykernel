#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Block until a key is pressed; returns raw set-1 scancode (make code). */
unsigned char keyboard_read_scancode(void);

/* Block until a printable/control key is pressed; returns ASCII. */
char keyboard_getchar(void);

#endif
