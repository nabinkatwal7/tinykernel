#ifndef USB_H
#define USB_H

/* usb : list the USB host controllers on the PCI bus */
int cmd_usb(int argc, char **argv);
int usb_count_controllers(void);

#endif
