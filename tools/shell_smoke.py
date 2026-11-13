#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
"""
MAKH interactive smoke test.

Boots the normal (non-test) ISO, waits for the shell prompt on the serial
console, types commands through the QEMU monitor's `sendkey` (i.e. through
the real PS/2 keyboard driver), and checks the output of each one.

usage: tools/shell_smoke.py [makhos.iso] [--timeout SECONDS]
exit status: 0 = every check passed, 1 = a check failed, 2 = setup error
"""
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

QEMU = "qemu-system-x86_64"

# (command, substring that must appear in its output). The default boot runs
# the ring-3 /bin/sh, so each command is a real fork+execve from the initrd.
SCRIPT = [
    ("help", "ping <ip> [count]"),
    ("ifconfig", "inet 10.0.2.15"),
    ("ping 10.0.2.2 3", "3 received"),
    ("ping 127.0.0.1 2", "2 received"),
    ("uname -s | cut -c 2-", "AKH"),
    ("cat /hello.txt", "phase 18 vfs brick"),
    ("ls /bin", "ufuzz"),
    ("nosuchcmd", "nosuchcmd: not found"),
]

# The ring-3 shell's prompt: "makh:<cwd>$ ", name and path wrapped in SGR colors.
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = re.compile(r"makh:\S*\$ ")


def at_prompt(s):
    return PROMPT.search(ANSI.sub("", s)) is not None


KEYMAP = {" ": "spc", ".": "dot", "-": "minus", "<": "shift-comma",
          ">": "shift-dot", "/": "slash", "|": "shift-backslash", "_": "shift-minus", ":": "shift-semicolon"}


def keys_for(text):
    for ch in text:
        if ch.isalpha() and ch.isupper():
            yield "shift-" + ch.lower()
        elif ch.isalnum():
            yield ch
        elif ch in KEYMAP:
            yield KEYMAP[ch]
        else:
            raise ValueError(f"no key mapping for {ch!r}")
    yield "ret"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    iso = args[0] if args else "makhos.iso"
    timeout = 60
    if "--timeout" in sys.argv:
        timeout = int(sys.argv[sys.argv.index("--timeout") + 1])
    if not os.path.exists(iso):
        print(f"error: ISO not found: {iso}", file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="makh-smoke-")
    mon_path = os.path.join(tmp, "mon.sock")
    log_path = os.path.join(tmp, "serial.log")
    cmd = [QEMU, "-cdrom", iso, "-m", "256M", "-display", "none",
           "-serial", f"file:{log_path}", "-no-reboot", "-accel", "tcg",
           "-monitor", f"unix:{mon_path},server,nowait"]
    print("qemu:", " ".join(cmd), flush=True)
    qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def serial():
        try:
            with open(log_path, "r", errors="replace") as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def wait_for(pred, secs):
        end = time.time() + secs
        while time.time() < end:
            if pred(serial()):
                return True
            if qemu.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def connect_monitor():
        for _ in range(50):
            try:
                m = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                m.connect(mon_path)
                m.settimeout(0.5)
                return m
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("cannot connect to the QEMU monitor")

    def type_line(mon, text, newline=True):
        for k in keys_for(text):
            if k == "ret" and not newline:
                continue
            mon.sendall(f"sendkey {k}\n".encode())
            time.sleep(0.03)

    failed = 0
    try:
        mon = connect_monitor()
        if not wait_for(lambda s: "[KEYBOARD] Ready for input" in s, timeout):
            print("FAIL: keyboard never came up")
            print(serial()[-2000:])
            return 1
        if not wait_for(at_prompt, timeout):
            print("FAIL: never reached the shell prompt")
            print(serial()[-2000:])
            return 1

        for command, expect in SCRIPT:
            before = len(serial())
            type_line(mon, command)
            ok = wait_for(lambda s: expect in s[before:], 20)
            # Type the next line only once the shell is reading again: keys sent
            # while a command still runs belong to that command, not the shell.
            ok = ok and wait_for(lambda s: at_prompt(s[s.index(expect, before):]), 20)
            out = serial()[before:]
            status = "ok  " if ok else "FAIL"
            print(f"[{status}] {command!r:24} expects {expect!r}")
            if not ok:
                failed += 1
                print("      output:", out[-600:].replace("\n", "\n              "))
        try:
            mon.sendall(b"quit\n")
        except OSError:
            pass
    finally:
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()

    print(f"\nshell smoke: {len(SCRIPT) - failed}/{len(SCRIPT)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
