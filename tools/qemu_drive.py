#!/usr/bin/env python3
"""Boot the OS headless in QEMU, type shell commands through the QEMU monitor, print the serial log.

usage: python tools/qemu_drive.py [--screenshot out.ppm] [--fresh-disk] "cmd1" "cmd2" ...
A command may be 'wait:<seconds>' to just wait.
"""
import socket, subprocess, sys, time, os, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
KEYS = {" ": "spc", "\n": "ret", ".": "dot", "-": "minus", "/": "slash", ",": "comma",
        "=": "equal", ";": "semicolon", "'": "apostrophe", "\\": "backslash", "_": "shift-minus",
        '"': "shift-apostrophe", ":": "shift-semicolon", "!": "shift-1", "(": "shift-9",
        ")": "shift-0", "*": "shift-8", "+": "shift-equal", "?": "shift-slash",
        "$": "shift-4", "%": "shift-5", "#": "shift-3", "@": "shift-2", "^": "shift-6", "&": "shift-7",
        "[": "bracket_left", "]": "bracket_right", "{": "shift-bracket_left", "}": "shift-bracket_right",
        "<": "shift-comma", ">": "shift-dot", "|": "shift-backslash", "`": "grave_accent", "~": "shift-grave_accent"}

def key(c):
    if c in KEYS: return KEYS[c]
    if c.isupper(): return "shift-" + c.lower()
    return c

def main():
    args = sys.argv[1:]
    shot = None
    fresh = False
    while args and args[0].startswith("--"):
        if args[0] == "--screenshot":
            shot = args[1]; args = args[2:]
        elif args[0] == "--fresh-disk":
            fresh = True; args = args[1:]
    disk = os.path.join(BUILD, "test-disk.img")
    if fresh or not os.path.exists(disk):
        with open(disk, "wb") as f: f.write(b"\0" * 512 * 2048)
    serial = os.path.join(BUILD, "serial-test.log")
    if os.path.exists(serial): os.remove(serial)
    port = 55000 + os.getpid() % 1000
    q = subprocess.Popen(["qemu-system-i386",
        "-drive", f"format=raw,file={os.path.join(BUILD,'os-image.bin')},if=floppy",
        "-drive", f"format=raw,file={disk},if=ide,index=0",
        "-drive", f"format=raw,file={os.path.join(BUILD,'fat.img')},if=ide,index=1", "-boot", "a",
        "-display", "none",
        "-chardev", f"socket,id=s0,host=127.0.0.1,port={port + 1},server=on,wait=off,logfile={serial}",
        "-serial", "chardev:s0",
        "-monitor", f"tcp:127.0.0.1:{port},server,nowait"])
    time.sleep(1.5)
    s = socket.create_connection(("127.0.0.1", port))
    ser = socket.create_connection(("127.0.0.1", port + 1))
    def mon(cmd):
        s.sendall((cmd + "\n").encode()); time.sleep(0.05)
    time.sleep(1.0)
    for line in args:
        if line.startswith("ser:"):  # text typed on the serial port; the two characters backslash-r mean Enter
            ser.sendall(line[4:].replace(chr(92) + "r", chr(13)).encode()); time.sleep(0.6); continue
        if line.startswith("mon:"):
            mon(line[4:]); time.sleep(0.4); continue
        if line.startswith("key:"):
            mon("sendkey " + line[4:]); time.sleep(0.4); continue
        if line.startswith("raw:"):
            for c in line[4:]:
                mon("sendkey " + key(c)); time.sleep(0.03)
            time.sleep(0.4); continue
        if line.startswith("wait:"):
            time.sleep(float(line[5:])); continue
        for c in line + "\n":
            mon("sendkey " + key(c)); time.sleep(0.03)
        time.sleep(0.6)
    time.sleep(1.0)
    if shot:
        mon("screendump " + shot); time.sleep(0.5)
    mon("quit")
    q.wait(timeout=10)
    print(open(serial, errors="replace").read())

main()
