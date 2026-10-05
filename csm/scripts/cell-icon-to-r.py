#!/usr/bin/env python3
"""cell-icon-to-r.py -- turn a clean RGBA PNG into a 16x16 Rez icon family.

    cell-icon-to-r.py <in.png> <out.r> <resource-id>

WHY A PNG IS ACCEPTABLE HERE, WHEN THE PROJECT RULE SAYS OTHERWISE
------------------------------------------------------------------
os9_control_strip_module says: do NOT transcribe an icon from a *picture* of it, get
the raw resource bytes. That rule earned itself on a screenshot -- scaled, antialiased,
with the 8-bit colour already mangled. It does not apply to an exact export, and this
script REFUSES anything that is not one:

  * every pixel must be fully opaque or fully transparent (no partial alpha), so the
    mask is exact rather than a threshold guess;
  * the distinct-colour count must be small (a handful), so nothing was antialiased.

If either check fails the right answer is a resource-fork donor, not a cleverer script,
and the script says so and exits rather than silently producing a blurred icon.

WHAT IT EMITS
-------------
ics#  16x16 1-bit image + 1-bit MASK   (64 bytes)  <- the mask is what lets the strip
ics4  16x16 4-bit, standard 16-colour CLUT (128)      background show through
ics8  16x16 8-bit, Mac system palette      (256)

A source narrower than 16px is CENTRED, not stretched: this is pixel art at icon size
and resampling it would destroy the very edges that make it legible.

⚠ The 8-bit palette is the validated one from os9_control_strip_module: a 6x6x6 cube
where index = r*36 + g*6 + b and each channel index 0..5 maps to level
[255,204,153,102,51,0]. Black is 0xFF (NOT the cube's 215, which real icons do not use).

Writes a NEW file; never opens an original for writing. See claude-os9/CLAUDE.md rule 1.
"""
import sys, struct, zlib

# ---- the 4-bit standard Mac CLUT ------------------------------------------
CLUT4 = [
    (0xFF, 0xFF, 0xFF), (0xFC, 0xF3, 0x05), (0xFF, 0x64, 0x03), (0xDD, 0x09, 0x07),
    (0xF2, 0x08, 0x84), (0x47, 0x00, 0xA5), (0x00, 0x00, 0xD3), (0x02, 0xAB, 0xEA),
    (0x1F, 0xB7, 0x14), (0x00, 0x64, 0x12), (0x56, 0x2C, 0x05), (0x90, 0x71, 0x3A),
    (0xC0, 0xC0, 0xC0), (0x80, 0x80, 0x80), (0x40, 0x40, 0x40), (0x00, 0x00, 0x00),
]
LEVELS = [255, 204, 153, 102, 51, 0]


def read_png(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat = 8, b""
    while pos < len(d):
        ln, typ = struct.unpack_from(">I4s", d, pos)
        body = d[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            if depth != 8 or inter != 0 or ctype not in (2, 6):
                sys.exit(f"ERROR: need 8-bit non-interlaced RGB/RGBA, got "
                         f"depth={depth} ctype={ctype} interlace={inter}")
            nch = 4 if ctype == 6 else 3
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride, rows, prev, p = w * nch, [], bytearray(w * nch), 0
    for _ in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            b = prev[i]
            c = prev[i - nch] if i >= nch else 0
            if   f == 1: line[i] = (line[i] + a) & 0xFF
            elif f == 2: line[i] = (line[i] + b) & 0xFF
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        prev = line
        rows.append([(line[x * nch], line[x * nch + 1], line[x * nch + 2],
                      line[x * nch + 3] if nch == 4 else 255) for x in range(w)])
    return w, h, rows


def dist(c1, c2):
    """⚠ LUMA-WEIGHTED, not plain Euclidean RGB, and the difference is not academic.
    Unweighted distance maps this project's navy #08398C to the CLUT's PURPLE (#4700A5)
    by a 6% margin over its BLUE (#0000D3) -- because plain RGB treats a large error in
    the red channel as cheaply as the same error in green. Weighting by the luminance
    coefficients (30/59/11) puts blue ahead by 27%, which is also the answer a person
    gives. A purple Bluetooth glyph would have been the visible result."""
    return (30 * (c1[0] - c2[0]) ** 2
          + 59 * (c1[1] - c2[1]) ** 2
          + 11 * (c1[2] - c2[2]) ** 2)


#: CLUT entries with no hue: white, light/medium/dark grey, black.
ACHROMATIC4 = (0, 12, 13, 14, 15)
#: Above this max-minus-min spread, a colour's HUE is part of what it means.
CHROMA_FLOOR = 64


def nearest4(r, g, b):
    """⚠⚠ A STRONGLY COLOURED SOURCE MUST NOT COLLAPSE TO GREY, and nearest-colour on
    its own does exactly that. This project's navy #08398C is nearest PURPLE under plain
    RGB distance and nearest DARK GREY under luma-weighted distance -- neither is blue,
    and this is the Bluetooth glyph, where the hue is the identity of the thing. Both
    answers are arithmetically right and visibly wrong.

    So: when a pixel's channel spread exceeds CHROMA_FLOOR its hue is load-bearing, and
    the achromatic entries are taken off the table before matching. #08398C then lands on
    the CLUT's blue, which is the answer a person gives.

    ⚠ The floor matters and is not decoration. The icon's pale highlight #ADC6DE has a
    spread of only 49 -- it is a desaturated tint, and forcing IT chromatic would pick
    cyan or tan over the light grey that is actually correct. A blanket "prefer colour"
    rule would wreck the highlight to save the body.

    ⓘ This member only renders at 16-colour depth, which nothing on this hardware runs;
    ics8 below carries the real icon and maps the navy to #003399 without any of this.
    Getting it right anyway is cheap, and a wrong fallback is a landmine for whoever
    does boot at 16 colours."""
    pool = list(range(16))
    if max(r, g, b) - min(r, g, b) > CHROMA_FLOOR:
        # ⚠⚠ PRESERVE THE DOMINANT CHANNEL, rather than tuning the distance function.
        # Excluding greys alone was not enough: luma weighting is dominated by the GREEN
        # term, so a mid slate blue (#5A7BB5, green 123) matched CLUT *tan* over blue --
        # arithmetically closest, visibly a speck of dirt beside the navy. Two different
        # distance metrics each produced a different wrong answer here, which is the
        # signal that the metric was never the problem.
        #
        # A blue-dominant source can only sensibly become a blue-dominant CLUT entry, so
        # the pool is restricted to entries sharing the source's strongest channel and
        # the metric then only has to choose among plausible answers. For blue that is
        # {purple, blue, cyan}: #08398C picks blue, #5A7BB5 picks cyan.
        dom = (r, g, b).index(max(r, g, b))
        pool = [i for i in range(16) if i not in ACHROMATIC4
                and CLUT4[i].index(max(CLUT4[i])) == dom]
        if not pool:                       # no same-hue entry: fall back to all colours
            pool = [i for i in range(16) if i not in ACHROMATIC4]
    return min(pool, key=lambda i: dist(CLUT4[i], (r, g, b)))


def nearest8(r, g, b):
    if (r, g, b) == (0, 0, 0):
        return 0xFF                       # real icons use 0xFF for black, not cube 215
    best, bestd = 0, None
    for ri in range(6):
        for gi in range(6):
            for bi in range(6):
                d = dist((LEVELS[ri], LEVELS[gi], LEVELS[bi]), (r, g, b))
                if bestd is None or d < bestd:
                    best, bestd = ri * 36 + gi * 6 + bi, d
    return best


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    src, dst, rid = sys.argv[1], sys.argv[2], int(sys.argv[3])
    w, h, rows = read_png(src)

    # --- the two refusals -------------------------------------------------
    partial = [(x, y) for y in range(h) for x in range(w) if 0 < rows[y][x][3] < 255]
    if partial:
        sys.exit(f"ERROR: {len(partial)} pixel(s) have PARTIAL alpha (e.g. {partial[0]}). "
                 "The mask would be a guess. Supply a resource-fork donor instead.")
    colours = {p[:3] for row in rows for p in row if p[3] == 255}
    if len(colours) > 12:
        sys.exit(f"ERROR: {len(colours)} distinct opaque colours -- this looks "
                 "antialiased or resampled, so baking it would lose fidelity. "
                 "Supply a resource-fork donor instead.")
    if w > 16 or h > 16:
        sys.exit(f"ERROR: source is {w}x{h}; a cell icon must fit 16x16.")

    ox, oy = (16 - w) // 2, (16 - h) // 2
    print(f"cell-icon-to-r: {w}x{h}, {len(colours)} colours, binary alpha -- "
          f"centred at +{ox},+{oy} in 16x16")

    def at(x, y):
        sx, sy = x - ox, y - oy
        if 0 <= sx < w and 0 <= sy < h:
            return rows[sy][sx]
        return (0, 0, 0, 0)

    img1 = bytearray(); msk1 = bytearray()
    for y in range(16):
        ib = mb = 0
        for x in range(16):
            r, g, b, a = at(x, y)
            lum = (30 * r + 59 * g + 11 * b) // 100
            if a == 255:
                mb |= 1 << (15 - x)
                if lum < 128:                      # 1 = black in a 1-bit rendition
                    ib |= 1 << (15 - x)
        img1 += struct.pack(">H", ib)
        msk1 += struct.pack(">H", mb)

    d4 = bytearray()
    for y in range(16):
        for x in range(0, 16, 2):
            hi = 0 if at(x, y)[3] == 0 else nearest4(*at(x, y)[:3])
            lo = 0 if at(x + 1, y)[3] == 0 else nearest4(*at(x + 1, y)[:3])
            d4.append(((hi & 0xF) << 4) | (lo & 0xF))

    d8 = bytearray()
    for y in range(16):
        for x in range(16):
            r, g, b, a = at(x, y)
            d8.append(0x00 if a == 0 else nearest8(r, g, b))

    def hexblock(bs, per=16):
        out = []
        for i in range(0, len(bs), per):
            out.append('        $"' + " ".join(
                bs[i + j:i + j + 2].hex().upper() for j in range(0, per, 2)) + '"')
        return "\n".join(out)

    with open(dst, "w", encoding="utf-8") as f:
        f.write(f"""/*
 *  {dst.split('/')[-1]} -- Control Strip CELL icon, GENERATED. Do not hand-edit.
 *
 *  Regenerate with:
 *      scripts/cell-icon-to-r.py <in.png> {dst} {rid}
 *
 *  ⚠ ID {rid}, NOT -16455. -16455 is the FILE's Finder icon; this family is the glyph
 *  the module draws in its strip cell, and the two are deliberately different
 *  artwork. SBGetDetachIconSuite is asked for this ID at sdevInitModule.
 *
 *  Source was {w}x{h} with {len(colours)} opaque colours and fully binary alpha, so
 *  these bytes are exact -- centred, never resampled. ics# carries the 1-bit image
 *  AND the transparency mask, which is what lets the strip background show through.
 */

data 'ics#' ({rid}) {{
{hexblock(bytes(img1) + bytes(msk1))}
}};

data 'ics4' ({rid}) {{
{hexblock(bytes(d4))}
}};

data 'ics8' ({rid}) {{
{hexblock(bytes(d8))}
}};
""")
    print(f"cell-icon-to-r: wrote {dst}: ics#({len(img1)+len(msk1)}) "
          f"ics4({len(d4)}) ics8({len(d8)}) at ID {rid}")
    used4 = sorted({b >> 4 for b in d4} | {b & 0xF for b in d4})
    print(f"  4-bit CLUT indices used: {used4}")
    print(f"  8-bit palette indices used: {sorted(set(d8))}")


if __name__ == "__main__":
    main()
