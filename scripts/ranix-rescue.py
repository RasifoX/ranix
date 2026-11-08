#!/usr/bin/env python3
"""ranix-rescue: talk to a Ranix machine's kernel rescue channel (kernel/rescue.c).

Works when SSH and the rest of userspace are gone, as long as the kernel answers
ping. Usage:
    ranix-rescue.py [-k KEYFILE] [-p PORT] HOST COMMAND...
    COMMAND: ping | ps | stuck | log [bytes] | kill PID [SIG] | killpg PGID [SIG]
             | sync | reboot [grace_s] | reboot-now | poweroff | deadline [s] | claims
KEYFILE holds the 64-hex key the machine loads into /dev/rescue at boot
(/etc/fjord/rescue.key there). Default: $RANIX_RESCUE_KEY, else
./rescue.key next to this script. The key is never printed.
Exit: 0 answered, 1 refused (ERR), 2 no answer.
"""
import hashlib, hmac, os, socket, sys

PORT = 7395


def main():
    a = sys.argv[1:]
    keyf = os.environ.get("RANIX_RESCUE_KEY") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "rescue.key")
    port = PORT
    while a and a[0].startswith("-"):
        if a[0] == "-k" and len(a) > 1: keyf = a[1]; a = a[2:]
        elif a[0] == "-p" and len(a) > 1: port = int(a[1]); a = a[2:]
        else: print(__doc__, file=sys.stderr); return 2
    if len(a) < 2:
        print(__doc__, file=sys.stderr); return 2
    host, cmd = a[0], " ".join(a[1:])
    key = bytes.fromhex(open(keyf).read().split()[0])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    dst = (socket.gethostbyname(host), port)
    for attempt in range(3):
        try:
            s.sendto(b"RX1 HELLO", dst)
            r = s.recv(2048).decode(errors="replace").strip()
            break
        except socket.timeout:
            r = None
    if not r:
        print("no answer from %s:%d (kernel down, wrong address, or channel off)" % dst, file=sys.stderr)
        return 2
    if not r.startswith("RX1 NONCE "):
        print(r, file=sys.stderr); return 1
    nonce = r.split()[2]
    pre = ("RX1 CMD %s " % nonce).encode()
    mac = hmac.new(key, pre + cmd.encode(), hashlib.sha256).hexdigest()
    s.sendto(pre + mac.encode() + b" " + cmd.encode(), dst)
    parts = {}
    while True:
        try:
            p = s.recv(4096)
        except socket.timeout:
            if parts: break
            print("no reply to the command", file=sys.stderr); return 2
        if p.startswith(b"RX1 ERR"):
            print(p.decode(errors="replace").strip(), file=sys.stderr); return 1
        nl = p.index(b"\n")
        head, body = p[:nl], p[nl + 1:]
        f = head.split(b" ")            # RX1 R nonce seq more mac
        if len(f) != 6 or f[2].decode() != nonce:
            continue
        want = hmac.new(key, b" ".join(f[:5]) + b"\n" + body, hashlib.sha256).hexdigest()
        if not hmac.compare_digest(want, f[5].decode()):
            print("reply MAC wrong: not from the machine holding this key", file=sys.stderr); return 1
        parts[int(f[3])] = body
        if f[4] == b"0":
            break
    out = b"".join(parts[i] for i in sorted(parts))
    sys.stdout.write(out.decode(errors="replace"))
    if len(parts) != max(parts) + 1:
        print("(some reply packets lost)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
