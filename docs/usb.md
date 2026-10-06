# USB basics and the keyboard

## Controllers (`usb`)
PCI class 0C:03; the programming interface says which generation: UHCI and OHCI (USB 1.1), EHCI (2.0), xHCI (3.x).
The kernel only detects them. A real driver would reset the controller, build its schedule in memory (frame list /
queue heads / rings), and enumerate the root ports.

## Enumeration, in short
1. A device is plugged in; the controller reports a connect change on the root port.
2. The host resets the port, then talks to the device at address 0 on the *default control pipe* (endpoint 0).
3. `GET_DESCRIPTOR(device)` tells the maximum packet size; `SET_ADDRESS` gives the device its own address.
4. `GET_DESCRIPTOR(configuration)` returns the configuration, interface and endpoint descriptors in one blob.
5. `SET_CONFIGURATION` activates it. A keyboard has one interface with class 3 (HID), subclass 1 (boot interface),
   protocol 1 (keyboard) and one interrupt-IN endpoint.

## Boot-protocol keyboard reports
`SET_PROTOCOL(0)` selects the simple boot protocol, so no report descriptor has to be parsed. The host polls the
interrupt endpoint (every 8-10 ms) and receives 8 bytes:

| byte | meaning |
|------|---------|
| 0 | modifier bits: 0 LCtrl, 1 LShift, 2 LAlt, 3 LGUI, 4 RCtrl, 5 RShift, 6 RAlt, 7 RGUI |
| 1 | reserved |
| 2-7 | up to six *usage codes* of the keys held down (0 = none, 1 = too many keys) |

Usage codes are not ASCII: 4-29 are a-z, 30-39 are 1-9 and 0, 40 Enter, 41 Esc, 42 Backspace, 43 Tab, 44 Space,
45-56 punctuation, 79-82 the arrows. The driver compares each report with the previous one: a usage that is new is a
key press (typematic repeat is software), one that disappeared is a release.

`kernel/usbhid.c` implements that decoder (`usbkbd_report`) and `usbkbd test` replays sample reports through it.
