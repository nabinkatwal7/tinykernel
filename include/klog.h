#ifndef KLOG_H
#define KLOG_H

enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };

/* Timestamped kernel log. Goes to a ring buffer (dmesg) and COM1; WARN+ also hits the screen. */
void klog(int level, const char *fmt, ...);
void klog_dump(void);
int  klog_copy(char *buf, unsigned cap);  /* oldest-first copy of the ring, NUL-terminated; bytes copied */          /* print the ring buffer to the console */
void klog_set_console_level(int level);

#define kinfo(...)  klog(LOG_INFO, __VA_ARGS__)
#define kwarn(...)  klog(LOG_WARN, __VA_ARGS__)
#define kerror(...) klog(LOG_ERROR, __VA_ARGS__)

#endif
