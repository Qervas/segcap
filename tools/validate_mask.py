"""
validate_mask.py -- assert a captured stencil mask matches the fixture's known
ground truth, spatially.

Run:
    python tools/validate_mask.py segcap_mask_1.pgm

"The set of values present is {0,1..16}" is a weak check: a transposed,
sheared, or vertically flipped readback would still contain exactly those
values. This asserts that each ID appears at the pixel where that quad actually
is, which catches row-pitch mistakes, Y-flip mistakes, and plane mix-ups.

The fixture draws a 4x4 grid of quads in NDC. Quad (col,row) is centred at
    ndc = (-1 + cellW*(col+0.5), -1 + cellH*(row+0.5)),  cellW = cellH = 2/4
and writes stencil = row*4 + col + 1.

NDC maps to pixels as
    x_px = (ndc_x + 1)/2 * width
    y_px = (1 - ndc_y)/2 * height        <- note the Y flip
"""

import sys

GRID = 4


def read_pgm(path):
    with open(path, "rb") as f:
        data = f.read()

    # Binary PGM: "P5\n<w> <h>\n<maxval>\n" then raw bytes. Parsed by hand to
    # avoid a dependency, and because the header is fully known here.
    if not data.startswith(b"P5"):
        raise SystemExit("%s is not a binary PGM (no P5 magic)" % path)

    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos : pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # single whitespace after maxval

    w, h, maxval = fields
    pixels = data[pos : pos + w * h]
    if len(pixels) != w * h:
        raise SystemExit("truncated PGM: expected %d bytes, got %d" % (w * h, len(pixels)))
    return w, h, maxval, pixels


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: validate_mask.py <mask.pgm>")
    path = sys.argv[1]

    w, h, _, px = read_pgm(path)
    print("mask: %s  %dx%d" % (path, w, h))

    def at(x, y):
        return px[y * w + x]

    cell = 2.0 / GRID
    failures = []
    print()
    print("%-8s %-12s %-10s %-10s %s" % ("quad", "pixel", "expected", "actual", ""))
    print("-" * 56)

    for row in range(GRID):
        for col in range(GRID):
            ndc_x = -1.0 + cell * (col + 0.5)
            ndc_y = -1.0 + cell * (row + 0.5)
            x = int((ndc_x + 1.0) / 2.0 * w)
            y = int((1.0 - ndc_y) / 2.0 * h)
            x = min(max(x, 0), w - 1)
            y = min(max(y, 0), h - 1)

            expected = row * GRID + col + 1
            actual = at(x, y)
            ok = actual == expected
            if not ok:
                failures.append((col, row, x, y, expected, actual))
            print("%-8s (%4d,%4d) %-10d %-10d %s"
                  % ("c%dr%d" % (col, row), x, y, expected, actual, "" if ok else "<-- MISMATCH"))

    # Background must be 0. If a Y-flip or pitch error smeared the image, the
    # corners are where it shows up first.
    print()
    corners = [(1, 1), (w - 2, 1), (1, h - 2), (w - 2, h - 2)]
    for (x, y) in corners:
        v = at(x, y)
        print("background at (%4d,%4d): %d %s" % (x, y, v, "" if v == 0 else "<-- EXPECTED 0"))
        if v != 0:
            failures.append(("bg", "bg", x, y, 0, v))

    present = sorted({px[i] for i in range(0, len(px), 97)})  # sparse sample
    print()
    print("values present (sparse sample): %s" % present)

    print()
    if failures:
        print("FAIL: %d mismatch(es)" % len(failures))
        print()
        print("Likely causes, in order of likelihood:")
        print("  - row pitch ignored (readback pitch is padded to 256 bytes)")
        print("  - Y flip: D3D renders with +Y up in NDC, images store +Y down")
        print("  - wrong plane copied (plane 0 is depth, plane 1 is stencil)")
        return 1

    print("PASS: all %d quads carry their expected stencil ID at their expected"
          " pixel, and the background is clear." % (GRID * GRID))
    return 0


sys.exit(main())
