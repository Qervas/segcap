"""
pack.py -- bundle a capture session into one self-describing container.

    python tools/pack.py --dir build/bin --out session.segcap
    python tools/pack.py --dir build/bin --out session.segcap --verify
    python tools/pack.py --list session.segcap
    python tools/pack.py --extract session.segcap --to out/

WHY THE COMPRESSION IS HERE AND NOT IN THE DLL
----------------------------------------------
The capture path runs inside someone else's process, on the thread that presents
frames. Everything on it has been kept to the minimum that cannot be done later:
a GPU copy onto the game's own queue, a fence poll, and a memcpy out of a mapped
readback buffer. Linking a compressor into that path would add third-party code
to a shipped game's address space and CPU time to its present loop, to save disk
space that is not scarce -- the same trade the project has refused everywhere
else.

Compression is a pure function of bytes already written. It belongs in a tool.

CODEC
-----
zlib (deflate) by default, lzma optionally. Both are Python standard library, so
the container can be read anywhere Python runs with no install step. zstd would
compress better and faster and is the right choice if a dependency is
acceptable; the format records its codec per record, so adding it later does not
break existing files.

FORMAT
------
Masks are 8-bit indexed images with large flat regions, which is close to the
best case for a dictionary coder. Colour frames are photographic and compress
poorly -- that is expected and is reported separately rather than hidden in an
average.

  magic     8 bytes   "SEGCAP01"
  hdrlen    u32       length of the header JSON
  header    utf-8     {"width":..,"height":..,"codec":..,"created":..}
  records   ...       each: type u8, frame u64, rawlen u32, complen u32, payload
  index     ...       [{type, frame, offset, rawlen, complen}, ...] as JSON
  idxlen    u32       length of the index JSON
  footer    8 bytes   "SEGCAPIX"

The index is at the END and its length is in the last 12 bytes, so a reader can
seek to any frame without scanning the file, while the writer never has to know
the record layout in advance. Same reason zip puts its directory last.

Record types:
  1 MASK     raw 8-bit, width*height bytes, row-major, no padding
  2 FRAME    raw RGB24, width*height*3 bytes
  3 SIDECAR  utf-8 JSON, the slot -> object table for that frame

A mask is undecodable without its sidecar, so `--verify` fails a container in
which any mask lacks one. That is a property worth enforcing in the packer
rather than documenting and hoping.
"""

import argparse
import glob
import json
import os
import re
import struct
import sys
import zlib

MAGIC = b"SEGCAP01"
FOOTER = b"SEGCAPIX"

T_MASK, T_FRAME, T_SIDECAR = 1, 2, 3
TYPE_NAME = {T_MASK: "mask", T_FRAME: "frame", T_SIDECAR: "sidecar"}

# Each entry is (compress(bytes)->bytes, decompress(bytes)->bytes). Every codec
# is wrapped in a single-argument lambda so the table is uniform -- the stdlib
# functions have wildly different optional parameters, and letting those leak
# into the table makes the call sites depend on which codec is selected.
CODECS = {
    "zlib": (lambda b: zlib.compress(b, 9),
             lambda b: zlib.decompress(b)),
}
try:
    import lzma
    CODECS["lzma"] = (lambda b: lzma.compress(b),
                      lambda b: lzma.decompress(b))
except ImportError:      # pragma: no cover - lzma is stdlib but can be absent
    pass
try:
    import zstandard      # pragma: no cover - not installed here; used if present
    CODECS["zstd"] = (lambda b: zstandard.ZstdCompressor(level=10).compress(b),
                      lambda b: zstandard.ZstdDecompressor().decompress(b))
except ImportError:
    pass


# ---------------------------------------------------------------- readers

def frame_index(path):
    m = re.search(r"_(\d+)\.", os.path.basename(path))
    return int(m.group(1)) if m else -1


def read_pgm(path):
    """Binary PGM (P5) -> (width, height, raw bytes).

    Parsed by hand rather than with an image library so the container stores the
    EXACT bytes the DLL wrote. Round-tripping through a decoder risks a silent
    normalisation, and this file is the thing --verify is supposed to trust.
    """
    with open(path, "rb") as f:
        data = f.read()
    return _read_netpbm(data, b"P5", 1, path)


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    return _read_netpbm(data, b"P6", 3, path)


def _read_netpbm(data, magic, channels, path):
    if not data.startswith(magic):
        raise SystemExit("%s: not a %s file" % (path, magic.decode()))
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, _maxval = fields
    need = w * h * channels
    payload = data[pos:pos + need]
    if len(payload) != need:
        raise SystemExit("%s: truncated, wanted %d bytes got %d" % (path, need, len(payload)))
    return w, h, payload


# ---------------------------------------------------------------- writing

def collect(directory):
    masks = {frame_index(p): p for p in glob.glob(os.path.join(directory, "segcap_mask_*.pgm"))}
    frames = {frame_index(p): p for p in glob.glob(os.path.join(directory, "segcap_frame_*.ppm"))}
    cars = {frame_index(p): p for p in glob.glob(os.path.join(directory, "segcap_mask_*.json"))}
    return masks, frames, cars


def pack(directory, out_path, codec, labels_only=False):
    """Build a container.

    labels_only omits the colour frames. Worth having as a first-class option
    rather than an afterthought: masks and sidecars for a 75-frame session are
    0.8MB, while the same session with colour is 116MB, and the colour is
    already available as video. For anything that consumes the LABELS -- reading
    them back, checking identity stability, shipping an example -- the small one
    is the useful artifact.
    """
    compress, _ = CODECS[codec]
    masks, frames, cars = collect(directory)
    if not masks:
        raise SystemExit("no masks found in %s" % directory)

    # Every mask must carry its sidecar. An 8-bit slot number means nothing on
    # its own -- slot 42 is a different object in different frames -- so a mask
    # without its table is not merely inconvenient, it is undecodable.
    orphans = sorted(set(masks) - set(cars))
    if orphans:
        raise SystemExit("refusing to pack: %d mask(s) have no sidecar: %s"
                         % (len(orphans), orphans[:8]))

    w = h = None
    index = []
    raw_by_type = {T_MASK: 0, T_FRAME: 0, T_SIDECAR: 0}
    cmp_by_type = {T_MASK: 0, T_FRAME: 0, T_SIDECAR: 0}

    body = bytearray()

    def emit(rtype, idx, raw):
        nonlocal body
        blob = compress(raw)
        offset = len(body)
        body += struct.pack("<BQII", rtype, idx, len(raw), len(blob))
        body += blob
        index.append({"type": rtype, "frame": idx, "offset": offset,
                      "rawlen": len(raw), "complen": len(blob)})
        raw_by_type[rtype] += len(raw)
        cmp_by_type[rtype] += len(blob)

    for idx in sorted(masks):
        mw, mh, mraw = read_pgm(masks[idx])
        if w is None:
            w, h = mw, mh
        elif (mw, mh) != (w, h):
            raise SystemExit("frame %d is %dx%d but the session is %dx%d"
                             % (idx, mw, mh, w, h))
        emit(T_MASK, idx, mraw)

        with open(cars[idx], "rb") as f:
            emit(T_SIDECAR, idx, f.read())

        if idx in frames and not labels_only:
            fw, fh, fraw = read_ppm(frames[idx])
            if (fw, fh) != (w, h):
                raise SystemExit("frame %d colour is %dx%d, mask is %dx%d -- these "
                                 "are not the same frame grid" % (idx, fw, fh, w, h))
            emit(T_FRAME, idx, fraw)

    header = json.dumps({
        "width": w, "height": h, "codec": codec,
        "records": len(index),
        "frames": sorted(masks),
    }, separators=(",", ":")).encode()
    index_blob = json.dumps(index, separators=(",", ":")).encode()

    with open(out_path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(header)))
        f.write(header)
        f.write(body)
        f.write(index_blob)
        f.write(struct.pack("<I", len(index_blob)))
        f.write(FOOTER)

    total_raw = sum(raw_by_type.values())
    total_cmp = sum(cmp_by_type.values())
    print("packed %s" % out_path)
    print("  %d frames, %dx%d, codec=%s" % (len(masks), w, h, codec))
    print()
    print("  %-9s %12s %12s %8s" % ("", "raw", "packed", "ratio"))
    for t in (T_MASK, T_FRAME, T_SIDECAR):
        if raw_by_type[t] == 0:
            continue
        print("  %-9s %11.1fMB %11.1fMB %7.1fx"
              % (TYPE_NAME[t], raw_by_type[t] / 1e6, cmp_by_type[t] / 1e6,
                 raw_by_type[t] / max(cmp_by_type[t], 1)))
    print("  %-9s %11.1fMB %11.1fMB %7.1fx"
          % ("TOTAL", total_raw / 1e6, total_cmp / 1e6, total_raw / max(total_cmp, 1)))
    return out_path


# ---------------------------------------------------------------- reading

def open_container(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(MAGIC):
        raise SystemExit("%s: bad magic" % path)
    if not data.endswith(FOOTER):
        raise SystemExit("%s: bad footer -- file is truncated or still being written" % path)

    idxlen = struct.unpack("<I", data[-12:-8])[0]
    index = json.loads(data[-12 - idxlen:-12])
    hdrlen = struct.unpack("<I", data[8:12])[0]
    header = json.loads(data[12:12 + hdrlen])
    body_start = 12 + hdrlen
    return data, header, index, body_start


def record_bytes(data, header, body_start, rec):
    _, decompress = CODECS[header["codec"]]
    off = body_start + rec["offset"]
    rtype, frame, rawlen, complen = struct.unpack("<BQII", data[off:off + 17])
    blob = data[off + 17:off + 17 + complen]
    raw = decompress(blob)
    if len(raw) != rawlen:
        raise SystemExit("record for frame %d: expected %d bytes, decompressed %d"
                         % (frame, rawlen, len(raw)))
    return rtype, frame, raw


def verify(container, directory):
    """Decompress every record and byte-compare it with the file it came from.

    A compression bug that corrupts one frame in a thousand would otherwise
    surface as a mysteriously bad training example months later. Comparing
    against the originals is the only check that actually proves lossless.
    """
    data, header, index, body_start = open_container(container)
    masks, frames, cars = collect(directory)
    sources = {T_MASK: masks, T_FRAME: frames, T_SIDECAR: cars}

    checked = 0
    for rec in index:
        rtype, frame, raw = record_bytes(data, header, body_start, rec)
        src = sources[rtype].get(frame)
        if not src:
            raise SystemExit("frame %d %s has no source file to compare against"
                             % (frame, TYPE_NAME[rtype]))
        if rtype == T_MASK:
            _, _, original = read_pgm(src)
        elif rtype == T_FRAME:
            _, _, original = read_ppm(src)
        else:
            with open(src, "rb") as f:
                original = f.read()
        if raw != original:
            raise SystemExit("MISMATCH: frame %d %s differs from %s"
                             % (frame, TYPE_NAME[rtype], src))
        checked += 1

    print("verified %d records against their source files: byte identical" % checked)
    return True


def list_container(container):
    data, header, index, body_start = open_container(container)
    print("container: %s" % container)
    print("  %dx%d, codec=%s, %d records over %d frames"
          % (header["width"], header["height"], header["codec"],
             header["records"], len(header["frames"])))
    print()
    print("  %-8s %-9s %12s %12s" % ("frame", "type", "raw", "packed"))
    for rec in index[:12]:
        print("  %-8d %-9s %12d %12d"
              % (rec["frame"], TYPE_NAME[rec["type"]], rec["rawlen"], rec["complen"]))
    if len(index) > 12:
        print("  ... %d more" % (len(index) - 12))


def extract(container, to_dir):
    data, header, index, body_start = open_container(container)
    os.makedirs(to_dir, exist_ok=True)
    w, h = header["width"], header["height"]
    n = 0
    for rec in index:
        rtype, frame, raw = record_bytes(data, header, body_start, rec)
        if rtype == T_MASK:
            path = os.path.join(to_dir, "segcap_mask_%d.pgm" % frame)
            with open(path, "wb") as f:
                f.write(b"P5\n%d %d\n255\n" % (w, h))
                f.write(raw)
        elif rtype == T_FRAME:
            path = os.path.join(to_dir, "segcap_frame_%d.ppm" % frame)
            with open(path, "wb") as f:
                f.write(b"P6\n%d %d\n255\n" % (w, h))
                f.write(raw)
        else:
            path = os.path.join(to_dir, "segcap_mask_%d.json" % frame)
            with open(path, "wb") as f:
                f.write(raw)
        n += 1
    print("extracted %d records to %s" % (n, to_dir))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="build/bin", help="directory of captured files")
    ap.add_argument("--out", help="container to write")
    ap.add_argument("--codec", default="zlib", choices=sorted(CODECS))
    ap.add_argument("--labels-only", action="store_true",
                    help="omit colour frames; masks + sidecars only")
    ap.add_argument("--verify", action="store_true",
                    help="after packing, decompress everything and byte-compare")
    ap.add_argument("--list", dest="list_path", help="list a container's contents")
    ap.add_argument("--extract", dest="extract_path", help="container to extract")
    ap.add_argument("--to", default="extracted", help="directory for --extract")
    args = ap.parse_args()

    if args.list_path:
        list_container(args.list_path)
        return 0
    if args.extract_path:
        extract(args.extract_path, args.to)
        return 0
    if not args.out:
        ap.error("--out is required when packing")

    pack(args.dir, args.out, args.codec, args.labels_only)
    if args.verify:
        print()
        verify(args.out, args.dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
