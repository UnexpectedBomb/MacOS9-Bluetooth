#!/usr/bin/env python3
"""rsrc-types.py -- list the resource types in a classic Mac resource fork.

Read-only. Format per Inside Macintosh: More Macintosh Toolbox, Resource Manager.
  header: dataOff(4) mapOff(4) dataLen(4) mapLen(4)
  map:    +24 typeListOff(2)  +26 nameListOff(2)
  types:  count-1(2), then 8-byte entries: type(4) count-1(2) refListOff(2)
"""
import struct, sys

def types(path):
    d = open(path, 'rb').read()
    if len(d) < 16: return []
    dataOff, mapOff, dataLen, mapLen = struct.unpack('>IIII', d[:16])
    if mapOff + 30 > len(d): return []
    m = d[mapOff:mapOff + mapLen] if mapLen else d[mapOff:]
    tlOff, = struct.unpack('>H', m[24:26])
    n, = struct.unpack('>H', m[tlOff:tlOff + 2])
    out = []
    for i in range(n + 1):
        e = m[tlOff + 2 + i * 8: tlOff + 10 + i * 8]
        if len(e) < 8: break
        t, cnt, _ = struct.unpack('>4sHH', e)
        out.append((t.decode('mac-roman'), cnt + 1))
    return sorted(out)

for p in sys.argv[1:]:
    print(f"== {p}")
    ts = types(p)
    if not ts:
        print("   (unreadable / not a resource fork)")
        continue
    line = "   "
    for t, c in ts:
        line += f"{t}:{c}  "
        if len(line) > 92:
            print(line); line = "   "
    if line.strip(): print(line)
