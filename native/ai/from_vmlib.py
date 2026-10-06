"""Train the sc2 Tiny U-Net from jange's existing green-screen .vmlib and real backgrounds.

Usage: python native/ai/from_vmlib.py tr_knife20 [--samples 1200] [--epochs 12]
The .vmlib stores luma and masks, so the model is trained and run on grayscale frames.
"""
import argparse
import ctypes
import json
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def decompress_vmlib(path):
    data = path.read_bytes()
    if data[:4] != b"VMLB" or struct.unpack_from("<I", data, 4)[0] != 7:
        raise ValueError("Expected a version 7 VMLB library")
    size = struct.unpack_from("<Q", data, 8)[0]
    cabinet = ctypes.WinDLL("cabinet")
    cabinet.CreateDecompressor.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    cabinet.Decompress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                    ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    cabinet.CloseDecompressor.argtypes = [ctypes.c_void_p]
    handle = ctypes.c_void_p()
    if not cabinet.CreateDecompressor(4, None, ctypes.byref(handle)):
        raise OSError("CreateDecompressor(XPRESS_HUFF) failed")
    raw = ctypes.create_string_buffer(size)
    packed = ctypes.create_string_buffer(data[16:])
    written = ctypes.c_size_t()
    try:
        if not cabinet.Decompress(handle, packed, len(data) - 16, raw, size, ctypes.byref(written)) or written.value != size:
            raise OSError("Could not decompress the .vmlib")
    finally:
        cabinet.CloseDecompressor(handle)
    return raw


def read_library(path):
    raw = decompress_vmlib(path)
    data = memoryview(raw).cast("B")
    w, h, rx, ry, rw, rh = struct.unpack_from("<6i", data, 0)
    if min(w, h, rw, rh) <= 0 or rx < 0 or ry < 0 or rx + rw > w or ry + rh > h or rw % 2 or rh % 2:
        raise ValueError("Invalid library dimensions")
    lw, lh = rw // 2, rh // 2
    nb, nl = (rw * rh + 7) // 8, lw * lh
    pos = 26 + 4 * nb  # slot, handedness, four idle masks
    samples = []
    for action in range(7):
        takes = struct.unpack_from("<I", data, pos)[0]; pos += 4
        for take in range(takes):
            prev, gap, combo, cut, count = struct.unpack_from("<ifiBI", data, pos)
            pos += struct.calcsize("<ifiBI")
            for frame in range(count):
                elapsed, state = struct.unpack_from("<fB", data, pos); pos += 5
                samples.append((action, take, elapsed, state, pos, pos + nb))
                pos += nb + nl
    print(f"Library: {len(samples)} frames, ROI {rx},{ry} {rw}x{rh}", flush=True)
    return raw, data, (w, h, rx, ry, rw, rh), samples


def backgrounds(path, geometry, out_dir, limit):
    w, h, rx, ry, rw, rh = geometry
    lw, lh = rw // 2, rh // 2
    raw = path.read_bytes()
    bw, bh = struct.unpack_from("<ii", raw, 4)
    if bw != w // 2 or bh != h // 2:
        raise ValueError("Background resolution differs from the mask library")
    step = 12 + bw * bh
    count = len(raw) // step
    out_dir.mkdir(parents=True, exist_ok=True)
    mask_dir = out_dir.parent / "masks"
    mask_dir.mkdir(parents=True, exist_ok=True)
    (out_dir.parent / "dataset.json").write_text(json.dumps({"Kind": "Backgrounds", "FullFrame": True}))
    for i in np.linspace(0, count - 1, min(count, limit), dtype=int):
        off = int(i) * step
        if raw[off:off + 4] != b"VMBG":
            raise ValueError("Invalid VMBG frame")
        frame = np.frombuffer(raw, dtype=np.uint8, count=bw * bh, offset=off + 12).reshape(bh, bw)
        crop = frame[ry // 2:ry // 2 + lh, rx // 2:rx // 2 + lw]
        name = f"{i:06d}.png"
        Image.fromarray(np.repeat(crop[:, :, None], 3, axis=2)).save(out_dir / name)
        Image.fromarray(np.zeros((lh, lw), np.uint8)).save(mask_dir / name)
    print(f"Backgrounds: {min(count, limit)} real frames", flush=True)


def export_green(data, geometry, frames, out_dir, limit, target=None, first_number=0):
    _, _, _, _, rw, rh = geometry
    lw, lh = rw // 2, rh // 2
    target = target or geometry
    _, _, tx, ty, tw, th = target
    ox, oy = (geometry[2] - tx) // 2, (geometry[3] - ty) // 2
    if ox < 0 or oy < 0 or ox + lw > tw // 2 or oy + lh > th // 2:
        raise ValueError("Target ROI does not cover source ROI")
    nb = (rw * rh + 7) // 8
    images, masks = out_dir / "images", out_dir / "masks"
    images.mkdir(parents=True, exist_ok=True)
    masks.mkdir(parents=True, exist_ok=True)
    (out_dir / "dataset.json").write_text(json.dumps({"Kind": "GreenScreen", "FullFrame": False}))
    # Uniform spacing preserves complete action sequences and the input/state variety.
    for number, idx in enumerate(np.linspace(0, len(frames) - 1, min(len(frames), limit), dtype=int)):
        _, _, _, _, bit_off, luma_off = frames[int(idx)]
        bits = np.unpackbits(np.frombuffer(data[bit_off:bit_off + nb], dtype=np.uint8), bitorder="little")[:rw * rh]
        mask = bits.reshape(rh, rw).reshape(lh, 2, lw, 2).max((1, 3))
        if mask.sum() < 10:
            continue
        luma = np.frombuffer(data[luma_off:luma_off + lw * lh], dtype=np.uint8).reshape(lh, lw)
        image = np.empty((th // 2, tw // 2, 3), np.uint8)
        image[:] = (0, 255, 0)
        piece = image[oy:oy + lh, ox:ox + lw]
        piece[mask != 0] = luma[mask != 0, None]
        full_mask = np.zeros((th // 2, tw // 2), np.uint8)
        full_mask[oy:oy + lh, ox:ox + lw] = mask
        name = f"{first_number + number:06d}.png"
        Image.fromarray(image).save(images / name)
        Image.fromarray(full_mask * 255).save(masks / name)
    print(f"Green-screen pairs: {len(list(images.glob('*.png')))}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("library", nargs="+", help="names of native/masks/<name>.vmlib")
    ap.add_argument("--name", help="output model name (default: names joined by underscores)")
    ap.add_argument("--samples", type=int, default=1200, help="total frames, balanced across libraries")
    ap.add_argument("--epochs", type=int, default=12)
    ap.add_argument("--device", choices=["auto", "gpu", "cpu"], default="auto")
    ap.add_argument("--init", help="existing best.pt weights for fine-tuning")
    ap.add_argument("--lr", type=float, default=2e-3)
    args = ap.parse_args()
    geometries = []
    for name in args.library:
        raw, data, geom, frames = read_library(ROOT / "masks" / (name + ".vmlib"))
        geometries.append(geom)
        del data, raw, frames
    if any(g[:2] != geometries[0][:2] for g in geometries):
        raise ValueError("Libraries must have the same screen resolution")
    x0 = min(g[2] for g in geometries); y0 = min(g[3] for g in geometries)
    x1 = max(g[2] + g[4] for g in geometries); y1 = max(g[3] + g[5] for g in geometries)
    geom = (*geometries[0][:2], x0, y0, x1 - x0, y1 - y0)
    if any(v & 1 for v in geom[2:]):
        raise ValueError("Combined ROI must remain even-aligned")
    name = args.name or "_".join(args.library)
    dataset = ROOT / "ai" / "datasets" / name
    quota = max(1, args.samples // len(args.library))
    for i, source in enumerate(args.library):
        raw, data, source_geom, frames = read_library(ROOT / "masks" / (source + ".vmlib"))
        export_green(data, source_geom, frames, dataset, quota, geom, i * 100000)
        del data, raw, frames
    backgrounds(ROOT / "bg" / "fundos.vmbg", geom, dataset.parent / "backgrounds" / "images", 160)
    model = ROOT / "ai" / "models" / name
    model.mkdir(parents=True, exist_ok=True)
    (model / "geometry.txt").write_text(" ".join(map(str, geom)))
    cmd = [sys.executable, str(ROOT / "ai" / "train.py"), "--dataset", str(dataset), "--out", str(model),
           "--width", str(geom[4] // 2), "--height", str(geom[5] // 2), "--preset", "quick",
           "--epochs", str(args.epochs), "--batch", "12", "--device", args.device,
           "--lr", str(args.lr),
           "--pause-while-gaming", "0"]
    if args.init:
        cmd += ["--init", args.init]
    raise SystemExit(subprocess.call(cmd))


if __name__ == "__main__":
    main()
