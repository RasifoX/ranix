#!/usr/bin/env python3
"""Compare two listings, one of ref.py on ntfs-3g and one of Ranix: path, size and sha256.
Long names may differ by the driver's shortened form, matched through a hash suffix.
Usage: cmp.py REF OURS. Lines other than 'sha  size  path' in OURS are counted apart."""
import re, sys


def load(fn):
    files, other = {}, []
    for raw in open(fn, "rb"):
        line = raw.decode("utf-8", "surrogateescape").rstrip("\n")
        m = re.match(r"^([0-9a-f]{64})  (\d+)  (.*)$", line)
        if m:
            files[m.group(3)] = (m.group(1), int(m.group(2)))
        else:
            other.append(line)
    return files, other


def short(p):
    # the driver's form of a component over 255 bytes is head~hash, compare the head only
    out = []
    for c in p.split("/"):
        b = c.encode("utf-8", "surrogateescape")
        if len(b) > 255:
            c = b[:230].decode("utf-8", "ignore")
        out.append(c)
    return "/".join(out)


ref, ro = load(sys.argv[1])
our, oo = load(sys.argv[2])
missing, diff, ok = [], [], 0
seen = set()
for p, v in ref.items():
    hit = our.get(p)
    if hit is not None:
        seen.add(p)
    else:
        # a component over 255 bytes comes back as head~hash: same directory, same head
        head = p.encode("utf-8", "surrogateescape")[:200].decode("utf-8", "ignore")
        for q, ov in our.items():
            if q not in seen and re.search(r"~[0-9a-f]{16}(/|$)", q) and ov == v and \
               q.startswith(head[:len(head) - 2]):
                hit = ov
                seen.add(q)
                break
    if hit is None:
        missing.append(p)
    elif hit != v:
        diff.append((p, v, hit))
    else:
        ok += 1
extra = [p for p in our if p not in seen]
print("ref files %d  ours %d  match %d  missing %d  different %d  extra %d" % (len(ref), len(our), ok, len(missing), len(diff), len(extra)))
print("ref other lines: %d, ours other lines: %d" % (len(ro), len(oo)))
for p in missing[:20]: print("MISSING", p[:150])
for p, a, b in diff[:20]: print("DIFF", p[:150], a, b)
for p in extra[:20]: print("EXTRA", p[:150])
for l in ro[:12]: print("REF:", l[:170])
for l in oo[:40]: print("OUR:", l[:170])
sys.exit(1 if missing or diff or extra else 0)
