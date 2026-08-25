#!/usr/bin/env python3
"""
Verify the JPEG encoder against an independent decoder.

The C tests check structure - markers in the right places, sizes moving
the right way. That is not enough: an encoder can emit a
perfectly-shaped file that no decoder accepts, or one that decodes to
something unlike the input. Here Pillow decodes what we produced and the
result is compared with the source pixels.

The second half tests the claim the restart markers exist for: that
losing a run of bytes damages a bounded strip rather than everything
after it. That is a design claim in SPEC.md 7, and until now it was only
an argument.

    python3 tests/verify_jpeg.py <dir-with-samples>
"""
import pathlib
import sys

try:
    from PIL import Image, ImageFile
except ImportError:
    print("Pillow is required for this check")
    raise SystemExit(2)

ImageFile.LOAD_TRUNCATED_IMAGES = True

W, H = 320, 240
PASS, FAIL = [], []


def check(name, cond, detail=""):
    (PASS if cond else FAIL).append(name)
    print(f"  {'ok  ' if cond else 'FAIL'} {name}" + (f"  {detail}" if detail else ""))


def decode(path):
    with Image.open(path) as im:
        im.load()
        return im.convert("L").tobytes(), im.size


def mae(a, b):
    """Mean absolute error per pixel, 0..255."""
    n = min(len(a), len(b))
    if n == 0:
        return 255.0
    return sum(abs(a[i] - b[i]) for i in range(n)) / n


def rows_damaged(src, got, threshold=40):
    """How many pixel rows differ from the source by more than `threshold`."""
    bad = 0
    for y in range(H):
        row_a = src[y * W:(y + 1) * W]
        row_b = got[y * W:(y + 1) * W]
        if len(row_b) < W:
            bad += 1
            continue
        if mae(row_a, row_b) > threshold:
            bad += 1
    return bad


def corrupt(data, at_fraction=0.5, length=300):
    """Overwrite a run of entropy-coded bytes, as a lost packet would."""
    b = bytearray(data)
    sos = b.find(b"\xff\xda")
    start = sos + int((len(b) - sos) * at_fraction)
    for i in range(start, min(start + length, len(b) - 2)):
        b[i] = 0x5A
    return bytes(b)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    d = pathlib.Path(sys.argv[1])
    src = (d / "source.raw").read_bytes()

    print("\ndecoding with Pillow")
    for tag, max_mae in (("q90_rst", 4.0), ("q50_rst", 12.0),
                         ("q50", 12.0), ("q15_rst", 30.0)):
        path = d / f"{tag}.jpg"
        if not path.exists():
            check(f"{tag} present", False)
            continue
        try:
            got, size = decode(path)
        except Exception as exc:                       # noqa: BLE001
            check(f"{tag} decodes", False, str(exc))
            continue
        check(f"{tag} decodes", True,
              f"{path.stat().st_size} B")
        check(f"{tag} has the right dimensions", size == (W, H), str(size))
        err = mae(src, got)
        check(f"{tag} resembles the source", err <= max_mae,
              f"mean abs error {err:.2f} of 255 (limit {max_mae})")

    print("\nquality ordering survives a real decode")
    e90 = mae(src, decode(d / "q90_rst.jpg")[0])
    e50 = mae(src, decode(d / "q50_rst.jpg")[0])
    e15 = mae(src, decode(d / "q15_rst.jpg")[0])
    check("higher quality decodes closer to the source",
          e90 < e50 < e15, f"Q90 {e90:.2f} < Q50 {e50:.2f} < Q15 {e15:.2f}")

    print("\nrestart markers bound the damage (SPEC.md 7)")
    raw_rst = (d / "q50_rst.jpg").read_bytes()
    raw_plain = (d / "q50.jpg").read_bytes()

    tmp_rst = d / "_damaged_rst.jpg"
    tmp_plain = d / "_damaged_plain.jpg"
    tmp_rst.write_bytes(corrupt(raw_rst))
    tmp_plain.write_bytes(corrupt(raw_plain))

    try:
        got_rst, _ = decode(tmp_rst)
        ok_rst = True
    except Exception:                                  # noqa: BLE001
        got_rst, ok_rst = b"", False
    try:
        got_plain, _ = decode(tmp_plain)
        ok_plain = True
    except Exception:                                  # noqa: BLE001
        got_plain, ok_plain = b"", False

    check("a damaged file with restart markers still decodes", ok_rst)

    dmg_rst = rows_damaged(src, got_rst) if ok_rst else H
    dmg_plain = rows_damaged(src, got_plain) if ok_plain else H
    print(f"       300 corrupted bytes at the midpoint:")
    print(f"         with    restart markers: {dmg_rst:3d} of {H} rows damaged")
    print(f"         without restart markers: {dmg_plain:3d} of {H} rows damaged")

    check("restart markers reduce the damage",
          dmg_rst < dmg_plain,
          f"{dmg_rst} rows vs {dmg_plain}")
    check("damage stays in the bottom half of the picture",
          dmg_rst < H // 2,
          f"{dmg_rst} rows, i.e. {100.0 * dmg_rst / H:.0f}% of the image")

    tmp_rst.unlink(missing_ok=True)
    tmp_plain.unlink(missing_ok=True)

    print(f"\n{len(PASS)} passed, {len(FAIL)} failed")
    for f in FAIL:
        print(f"  FAILED: {f}")
    return 1 if FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main())
