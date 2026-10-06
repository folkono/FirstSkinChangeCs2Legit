"""
Local viewmodel segmentation training (binary: 0 = background, 1 = viewmodel).

Dataset layout (created by CS2ViewmodelTrainer):
    <dataset>/dataset.json
    <dataset>/images/000001.png ...   ROI crops, any resolution
    <dataset>/masks/000001.png  ...   grayscale, >127 = viewmodel

Output (<out>):
    model.onnx       input  "input" [1,3,H,W] RGB 0..1 (normalization is inside the model)
                     output "mask"  [1,1,H,W] probability
    metadata.json
    checkpoint.pt    resumable state (--resume)

Control files in <out>: pause.flag pauses, stop.flag checkpoints and exits.
Everything stays local. Nothing is uploaded.
"""
import argparse
import ctypes
import json
import math
import os
import random
import struct
import sys
import time
from ctypes import wintypes
from datetime import date
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image

PRESETS = {  # spec §77
    "quick": dict(epochs=15, base=12),
    "balanced": dict(epochs=40, base=16),
    "high": dict(epochs=80, base=24),
}


def log(*parts):
    print(*parts, flush=True)


# ---------------------------------------------------------------- model

def conv_block(i, o):
    return nn.Sequential(
        nn.Conv2d(i, o, 3, padding=1, bias=False), nn.BatchNorm2d(o), nn.ReLU(inplace=True),
        nn.Conv2d(o, o, 3, padding=1, bias=False), nn.BatchNorm2d(o), nn.ReLU(inplace=True),
    )


ARCH_VERSION = 2


class TinyUNet(nn.Module):
    """4-level U-Net with coordinate channels and a dilated context block. Latency over extreme accuracy (spec §22).

    - Coordinates (CoordConv): the viewmodel always sits in the same screen area, so position is a strong cue.
      A purely convolutional net cannot know where a pixel is and marks any similar colour anywhere.
    - Dilated bottleneck: receptive field covers the whole input, so the decision uses the overall shape
      (arm -> hand -> weapon) instead of local colour/texture.
    """

    def __init__(self, c=16):
        super().__init__()
        self.register_buffer("mean", torch.tensor([0.485, 0.456, 0.406]).view(1, 3, 1, 1))
        self.register_buffer("std", torch.tensor([0.229, 0.224, 0.225]).view(1, 3, 1, 1))
        self.e1, self.e2 = conv_block(3 + 2, c), conv_block(c, 2 * c)
        self.e3, self.e4 = conv_block(2 * c, 4 * c), conv_block(4 * c, 6 * c)
        ctx = 8 * c
        self.mid = nn.Sequential(*[m for i, d in enumerate((1, 2, 4, 8)) for m in (
            nn.Conv2d(6 * c if i == 0 else ctx, ctx, 3, padding=d, dilation=d, bias=False),
            nn.BatchNorm2d(ctx), nn.ReLU(inplace=True))])
        self.d4, self.d3 = conv_block(ctx + 6 * c, 6 * c), conv_block(10 * c, 4 * c)
        self.d2, self.d1 = conv_block(6 * c, 2 * c), conv_block(3 * c, c)
        self.head = nn.Conv2d(c, 1, 1)

    @staticmethod
    def up(x, skip):
        return torch.cat([F.interpolate(x, size=skip.shape[-2:], mode="bilinear", align_corners=False), skip], 1)

    def forward(self, x):
        b, _, h, w = x.shape
        ys = (torch.arange(h, device=x.device, dtype=x.dtype) / max(h - 1, 1) * 2 - 1).view(1, 1, h, 1).expand(b, 1, h, w)
        xs = (torch.arange(w, device=x.device, dtype=x.dtype) / max(w - 1, 1) * 2 - 1).view(1, 1, 1, w).expand(b, 1, h, w)
        x = torch.cat([(x - self.mean) / self.std, xs, ys], 1)
        e1 = self.e1(x)
        e2 = self.e2(F.max_pool2d(e1, 2))
        e3 = self.e3(F.max_pool2d(e2, 2))
        e4 = self.e4(F.max_pool2d(e3, 2))
        m = self.mid(F.max_pool2d(e4, 2))
        d = self.d4(self.up(m, e4))
        d = self.d3(self.up(d, e3))
        d = self.d2(self.up(d, e2))
        d = self.d1(self.up(d, e1))
        return self.head(d)  # logits


class ExportWrapper(nn.Module):
    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, x):
        return torch.sigmoid(self.net(x))


# ---------------------------------------------------------------- data

def crop_box(roi, width, height):
    """ROI fractions to a PIL box with the same rounding as Roi.ToPixels in C#."""
    x = min(max(int(roi["X"] * width), 0), width - 2)
    y = min(max(int(roi["Y"] * height), 0), height - 2)
    w = min(max(int(roi["W"] * width), 2), width - x)
    h = min(max(int(roi["H"] * height), 2), height - y)
    return x, y, x + w, y + h


def load_dataset(root: Path, w: int, h: int, crop=None):
    images = sorted((root / "images").glob("*.png"))
    pairs = [(p, root / "masks" / p.name) for p in images if (root / "masks" / p.name).exists()]
    xs, ms = [], []
    for img_path, mask_path in pairs:
        img = Image.open(img_path).convert("RGB")
        mask = Image.open(mask_path).convert("L")
        if crop:
            img, mask = img.crop(crop_box(crop, *img.size)), mask.crop(crop_box(crop, *mask.size))
        # PIL bilinear downscale is antialiased, close to the runtime GPU box downsample.
        img = img.resize((w, h), Image.BILINEAR)
        mask = mask.resize((w, h), Image.BILINEAR)
        xs.append(np.asarray(img, dtype=np.uint8).transpose(2, 0, 1))
        ms.append((np.asarray(mask) > 127).astype(np.uint8)[None])
    if not xs:
        return None, None, []
    return torch.from_numpy(np.stack(xs)), torch.from_numpy(np.stack(ms)), [p.name for p, _ in pairs]


def temporal_split(n, block=10, seed=0):
    """70/20/10 split by blocks of consecutive frames so near-identical neighbours never leak across sets (§21)."""
    idx = list(range(n))
    if n < 3 * block:
        return idx, idx, idx  # tiny dataset: validate on train, reported IoU is optimistic
    blocks = [idx[i:i + block] for i in range(0, n, block)]
    random.Random(seed).shuffle(blocks)
    train, val, test = [], [], []
    for b in blocks:
        filled = len(train) + len(val) + len(test)
        target = train if filled < 0.7 * n else val if filled < 0.9 * n else test
        target.extend(b)
    return train, val or train, test or val or train


def augment(x, m):
    """Photometric augmentation + only minor geometry: viewmodel position matters (§20).
    Colour is deliberately made unreliable (background swap, grayscale, channel shuffle) so the net learns shape."""
    b, dev = x.shape[0], x.device
    if b > 1 and random.random() < 0.6:
        # Background swap: this frame's viewmodel over another frame's scene. The other frame's own viewmodel
        # stays where it is not covered and stays labelled, so labels remain exact.
        perm = torch.randperm(b, device=dev)
        x = m * x + (1 - m) * x[perm]
        m = torch.maximum(m, m[perm])
    theta = torch.zeros(b, 2, 3, device=dev)
    s = 1 + (torch.rand(b, device=dev) - 0.5) * 0.08
    theta[:, 0, 0] = s
    theta[:, 1, 1] = s
    theta[:, :, 2] = (torch.rand(b, 2, device=dev) - 0.5) * 0.06
    grid = F.affine_grid(theta, list(x.shape), align_corners=False)
    x = F.grid_sample(x, grid, padding_mode="border", align_corners=False)
    m = F.grid_sample(m, grid, padding_mode="border", align_corners=False)

    r = lambda *shape: torch.rand(*shape, device=dev)
    x = x * (0.5 + 1.0 * r(b, 1, 1, 1))  # brightness: in-game lighting varies a lot
    if random.random() < 0.5:  # uneven lighting: soft light and shadow patches across the frame
        field = F.interpolate(0.55 + 0.9 * r(b, 1, 3, 5), size=x.shape[-2:], mode="bicubic", align_corners=False)
        x = x * field.clamp(0.35, 1.6)
    x = x * (1 + m * (r(b, 1, 1, 1) - 0.5) * 0.6)  # the viewmodel lit differently from the scene
    mean = x.mean((1, 2, 3), keepdim=True)
    x = (x - mean) * (0.7 + 0.6 * r(b, 1, 1, 1)) + mean  # contrast
    x = x.clamp(1e-4, 1) ** torch.exp(torch.randn(b, 1, 1, 1, device=dev) * 0.2)  # gamma
    x = x * (1 + (r(b, 3, 1, 1) - 0.5) * 0.12)  # colour temperature
    gray = x.mean(1, keepdim=True)
    x = gray + (x - gray) * (0.4 + 1.2 * r(b, 1, 1, 1))  # saturation
    pick = (r(b, 1, 1, 1) < 0.2).float()
    x = pick * gray.expand_as(x) + (1 - pick) * x  # grayscale
    if random.random() < 0.2:
        x = x[:, torch.randperm(3, device=dev)]  # channel shuffle: skins/gloves come in any colour
    if random.random() < 0.3:
        x = F.avg_pool2d(x, 3, 1, 1, count_include_pad=False)  # slight blur
    if random.random() < 0.2:  # compression-like detail loss
        x = F.interpolate(F.interpolate(x, scale_factor=0.5, mode="bilinear"), size=x.shape[-2:], mode="bilinear")
    x = x + torch.randn_like(x) * 0.02 * r(b, 1, 1, 1)  # noise
    return x.clamp(0, 1), (m > 0.5).float()


# ---------------------------------------------------------------- green screen

def despill(x, m):
    """Removes the green cast the green-screen map throws on the viewmodel (only inside the mask)."""
    limit = torch.maximum(x[:, 0:1], x[:, 2:3]) + 0.04
    g = torch.where(m > 0.5, torch.minimum(x[:, 1:2], limit), x[:, 1:2])
    return torch.cat([x[:, 0:1], g, x[:, 2:3]], 1)


def composite(xf, mf, xb, mb):
    """Viewmodel over a background frame. A viewmodel already in the background stays labelled where visible."""
    return mf * xf + (1 - mf) * xb, torch.maximum(mf, mb)


def procedural_backgrounds(b, h, w, dev, gen=None):
    """Fallback scenery when no real background frames exist: smooth colour fields, noise and boxes."""
    rand = lambda *s: torch.rand(*s, device=dev, generator=gen)
    x = F.interpolate(rand(b, 3, 5, 9), size=(h, w), mode="bicubic", align_corners=False)
    x = x + (F.interpolate(rand(b, 3, max(h // 16, 1), max(w // 16, 1)), size=(h, w), mode="bilinear",
                           align_corners=False) - 0.5) * 0.4
    ys, xs = torch.arange(h, device=dev).view(1, 1, h, 1), torch.arange(w, device=dev).view(1, 1, 1, w)
    for _ in range(8):
        x0, y0 = rand(b, 1, 1, 1) * w, rand(b, 1, 1, 1) * h
        bw, bh = rand(b, 1, 1, 1) * w * 0.4, rand(b, 1, 1, 1) * h * 0.4
        box = (xs >= x0) & (xs < x0 + bw) & (ys >= y0) & (ys < y0 + bh)
        x = torch.where(box, rand(b, 3, 1, 1), x)
    return x.clamp(0, 1)


def load_background_pool(dataset: Path, w, h, crop):
    """Real scenery for green-screen training: Backgrounds datasets (viewmodel hidden, empty masks)
    plus finished hand-annotated full-frame datasets (their viewmodel stays labelled)."""
    xs, ms, real = [], [], 0
    for d in sorted(p for p in dataset.parent.iterdir() if p.is_dir() and p != dataset):
        info_path = d / "dataset.json"
        if not info_path.exists():
            continue
        info = json.loads(info_path.read_text(encoding="utf-8-sig"))
        kind = info.get("Kind", "Normal")
        if not info.get("FullFrame") or kind == "GreenScreen":
            continue
        x, m, names = load_dataset(d, w, h, crop)
        if x is None:
            continue
        if kind == "Normal":  # gameplay always shows the viewmodel: near-empty hand masks are unfinished
            keep = m.float().mean((1, 2, 3)) > 0.005
            x, m = x[keep], m[keep]
            real += int(keep.sum())
        if len(x):
            xs.append(x)
            ms.append(m)
    if not xs:
        return None, None, 0
    return torch.cat(xs), torch.cat(ms), real


def erase_viewmodel(x, m):
    """A real frame without its viewmodel: the (slightly grown) mask area is filled from the surrounding scene
    with push-pull, like the overlay does. Used as 'no viewmodel here' training examples."""
    m = F.max_pool2d(m, 9, 1, 4)
    levels, cx, cw = [], x * (1 - m), 1 - m
    while min(cx.shape[-2:]) > 4:
        levels.append((cx, cw))
        cx, cw = F.avg_pool2d(cx, 2), F.avg_pool2d(cw, 2)
    fill = cx / cw.clamp_min(1e-4)
    for lx, lw in reversed(levels):
        fill = lx + (1 - lw) * F.interpolate(fill, size=lx.shape[-2:], mode="bilinear", align_corners=False)
    return fill.clamp(0, 1)


def fixed_negatives(src_x, src_m, count, seed):
    """Deterministic 'no viewmodel' frames for validation/test: hallucinated pixels count against IoU."""
    if count == 0 or len(src_x) == 0:
        return src_x[:0], src_m[:0]
    gen = torch.Generator().manual_seed(seed)
    k = torch.randint(len(src_x), (count,), generator=gen)
    x = erase_viewmodel(src_x[k].float() / 255, src_m[k].float())
    return (x * 255).round().to(torch.uint8), torch.zeros_like(src_m[k])


def fixed_composites(xs, ms, idx, bg_x, bg_m, seed):
    """Deterministic composites for validation/test so the score is comparable between epochs."""
    if not idx:
        return xs[:0], ms[:0]
    gen = torch.Generator().manual_seed(seed)
    xf, mf = xs[idx].float() / 255, ms[idx].float()
    if bg_x is not None:
        k = torch.randint(len(bg_x), (len(idx),), generator=gen)
        xb, mb = bg_x[k].float() / 255, bg_m[k].float()
    else:
        xb, mb = procedural_backgrounds(len(idx), xf.shape[2], xf.shape[3], "cpu", gen), torch.zeros_like(mf)
    x, m = composite(despill(xf, mf), mf, xb, mb)
    return (x * 255).round().to(torch.uint8), (m > 0.5).to(torch.uint8)


def write_shapes(xs, ms, out: Path):
    """Recorded silhouettes on the 64x36 grid used by ShapePrior.cs (cell on when >= 40 % viewmodel), deduplicated,
    plus each silhouette's appearance: mean luminance of the viewmodel pixels per cell. The overlay clears masks
    whose shape or light/dark pattern matches nothing recorded (menus, hallucinations)."""
    grid = (F.adaptive_avg_pool2d(ms.float(), (36, 64)) >= 0.4).flatten(1)
    keep = grid.any(1)
    xs, ms, grid = xs[keep], ms[keep], grid[keep]
    gray = (xs.float() * torch.tensor([0.299, 0.587, 0.114]).view(1, 3, 1, 1)).sum(1, keepdim=True)
    mf = (ms > 0).float()
    app = F.adaptive_avg_pool2d(gray * mf, (36, 64)) / F.adaptive_avg_pool2d(mf, (36, 64)).clamp_min(1e-6)
    app = app.flatten(1).clamp(0, 255).round().to(torch.uint8).numpy()
    bits, first = np.unique(np.packbits(grid.numpy().astype(np.uint8), axis=1, bitorder="little"), axis=0, return_index=True)
    with open(out / "shapes.bin", "wb") as f:
        f.write(struct.pack("<iii", len(bits), 64, 36))
        f.write(bits.tobytes())
        f.write(app[first].tobytes())
    return len(bits)


def iou(model, xs, ms, dev, bs=16):
    model.eval()
    inter = union = 0
    with torch.no_grad():
        for i in range(0, len(xs), bs):
            x = xs[i:i + bs].to(dev).float() / 255
            t = ms[i:i + bs].to(dev) > 0
            p = model(x) > 0
            inter += (p & t).sum().item()
            union += (p | t).sum().item()
    model.train()
    return inter / union if union else 1.0


# ---------------------------------------------------------------- resource control

def foreground_process_name():
    try:
        user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(pid))
        handle = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ""
        try:
            buf = ctypes.create_unicode_buffer(1024)
            size = wintypes.DWORD(1024)
            if kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
                return Path(buf.value).stem.lower()
        finally:
            kernel32.CloseHandle(handle)
    except Exception:
        pass
    return ""


def apply_impact(impact):
    """Low Impact / Balanced / Maximum Training Speed (§25). Never raises priority above normal."""
    cores = os.cpu_count() or 4
    threads, priority = {"low": (max(1, cores // 4), 0x40), "balanced": (max(1, cores // 2), 0x4000),
                         "max": (cores, 0x20)}[impact]
    torch.set_num_threads(threads)
    try:
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), priority)
    except Exception:
        pass


def pick_device(requested):
    if requested in ("auto", "gpu"):
        if torch.cuda.is_available():
            return torch.device("cuda"), "CUDA " + torch.cuda.get_device_name(0)
        try:  # AMD / Intel through DirectML, if torch-directml is installed
            import torch_directml
            return torch_directml.device(), "DirectML"
        except ImportError:
            pass
        if requested == "gpu":
            log("STATUS no supported GPU training backend, falling back to CPU")
    return torch.device("cpu"), "CPU"


def wait_if_paused(out: Path, args, ckpt_fn):
    announced = None
    while True:
        if (out / "stop.flag").exists():
            ckpt_fn()
            (out / "stop.flag").unlink(missing_ok=True)
            log("STOPPED checkpoint saved, run again with --resume")
            sys.exit(3)
        reason = None
        if (out / "pause.flag").exists():
            reason = "paused by user"
        elif args.pause_while_gaming and foreground_process_name() == args.game_process:
            reason = "paused while CS2 is in the foreground"
        if reason is None:
            if announced:
                log("STATUS resumed")
            return
        if reason != announced:
            ckpt_fn()
            log("STATUS " + reason)
            announced = reason
        time.sleep(1)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default="")
    ap.add_argument("--weapon", default="universal")
    ap.add_argument("--width", type=int, default=384)
    ap.add_argument("--height", type=int, default=216)
    ap.add_argument("--preset", choices=PRESETS, default="balanced")
    ap.add_argument("--epochs", type=int, default=0)
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--device", choices=["auto", "gpu", "cpu"], default="auto")
    ap.add_argument("--impact", choices=["low", "balanced", "max"], default="balanced")
    ap.add_argument("--pause-while-gaming", type=int, default=1)
    ap.add_argument("--game-process", default="cs2")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--init", default="", help="initialize weights from an existing best.pt")
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--shapes-only", action="store_true",
                    help="only (re)build shapes.bin for the existing model in --out from --dataset, no training")
    ap.add_argument("--roi", type=float, nargs=4, metavar=("X", "Y", "W", "H"),
                    help="model input area as fractions of full-frame images (default: full frame)")
    args = ap.parse_args()

    preset = PRESETS[args.preset]
    epochs = args.epochs or preset["epochs"]
    dataset, out = Path(args.dataset), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    random.seed(args.seed)
    torch.manual_seed(args.seed)
    apply_impact(args.impact)
    dev, dev_name = pick_device(args.device)
    log(f"STATUS device {dev_name}")

    ds_json = dataset / "dataset.json"
    ds_info = json.loads(ds_json.read_text(encoding="utf-8-sig")) if ds_json.exists() else {}
    if ds_info.get("FullFrame"):
        roi = dict(zip(("X", "Y", "W", "H"), args.roi)) if args.roi else {"X": 0, "Y": 0, "W": 1, "H": 1}
        crop = roi
    else:  # legacy dataset: images are already cropped to its ROI
        roi, crop = ds_info.get("Roi"), None
    log(f"STATUS model input area {roi}")

    if args.shapes_only:
        meta = json.loads((out / "metadata.json").read_text(encoding="utf-8-sig"))
        crop = meta.get("Roi") if ds_info.get("FullFrame") else None
        shape_images, shape_masks, _ = load_dataset(dataset, meta["InputWidth"], meta["InputHeight"], crop)
        log(f"STATUS shapes.bin: {write_shapes(shape_images, shape_masks, out)} recorded silhouettes")
        return

    xs, ms, names = load_dataset(dataset, args.width, args.height, crop)
    if xs is None:
        log("ERROR no annotated images (images/*.png with matching masks/*.png)")
        sys.exit(2)
    train_idx, val_idx, test_idx = temporal_split(len(names), seed=args.seed)
    log(f"STATUS {len(names)} annotated images: train {len(train_idx)}, val {len(val_idx)}, test {len(test_idx)}")
    if len(names) < 30:
        log("STATUS WARNING fewer than 30 frames: validation uses the training frames, so IoU is not a real accuracy. "
            "Annotate 150+ frames from different maps and spots.")
    (out / "split.json").write_text(json.dumps({
        "train": [names[i] for i in train_idx], "val": [names[i] for i in val_idx], "test": [names[i] for i in test_idx]}))

    kind = ds_info.get("Kind", "Normal")
    xs_dev, ms_dev = xs.to(dev), ms.to(dev)
    bg_dev = bgm_dev = None
    if kind == "GreenScreen":
        bg_x, bg_m, real = load_background_pool(dataset, args.width, args.height, crop)
        if bg_x is None:
            log("STATUS WARNING no Backgrounds or annotated datasets found: using synthetic backgrounds. "
                "Record a Backgrounds dataset (r_drawviewmodel 0 on real maps) so the model works in real matches.")
        else:
            log(f"STATUS green screen: viewmodel pasted over {len(bg_x)} real background frames "
                f"({real} from hand-annotated datasets)")
            bg_dev, bgm_dev = bg_x.to(dev), bg_m.to(dev)
        val_x, val_m = fixed_composites(xs, ms, val_idx, bg_x, bg_m, seed=1)
        test_x, test_m = fixed_composites(xs, ms, test_idx, bg_x, bg_m, seed=2)
    else:
        bg_x = bg_m = None
        val_x, val_m, test_x, test_m = xs[val_idx], ms[val_idx], xs[test_idx], ms[test_idx]

    # Negatives: every recorded frame shows a viewmodel, so without these the net learns "there is always one
    # there" and draws it on menus and empty screens. Real scenes with the viewmodel erased + synthetic scenes;
    # Backgrounds datasets (viewmodel hidden, menus) are true negatives and are used as they are.
    neg_src_x, neg_src_m = (bg_x, bg_m) if bg_x is not None else (xs, ms)
    nx, nm = fixed_negatives(neg_src_x, neg_src_m, max(2, len(val_idx) // 3), seed=3)
    val_x, val_m = torch.cat([val_x, nx]), torch.cat([val_m, nm])
    nx, nm = fixed_negatives(neg_src_x, neg_src_m, max(2, len(test_idx) // 3), seed=4)
    test_x, test_m = torch.cat([test_x, nx]), torch.cat([test_m, nm])
    neg_dev_x, neg_dev_m = neg_src_x.to(dev), neg_src_m.to(dev)

    def negatives(n):
        k = torch.randint(len(neg_dev_x), (n,), device=dev)
        x = erase_viewmodel(neg_dev_x[k].float() / 255, neg_dev_m[k].float())
        if random.random() < 0.3:
            x = procedural_backgrounds(n, x.shape[2], x.shape[3], dev)
        return x, torch.zeros_like(x[:, :1])

    def sample(j):
        if random.random() < 0.2:
            return negatives(len(j))
        x, m = xs_dev[j].float() / 255, ms_dev[j].float()
        if kind != "GreenScreen":
            return x, m
        if bg_dev is not None and random.random() < 0.3:
            # Real scenes as they are: "no viewmodel here" and real HUD/lighting.
            k = torch.randint(len(bg_dev), (len(j),), device=dev)
            return bg_dev[k].float() / 255, bgm_dev[k].float()
        if bg_dev is not None:
            k = torch.randint(len(bg_dev), (len(j),), device=dev)
            xb, mb = bg_dev[k].float() / 255, bgm_dev[k].float()
        else:
            xb, mb = procedural_backgrounds(len(j), x.shape[2], x.shape[3], dev), torch.zeros_like(m)
        return composite(despill(x, m), m, xb, mb)

    model = TinyUNet(preset["base"]).to(dev)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    steps_per_epoch = max(1, math.ceil(len(train_idx) / args.batch))
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=epochs * steps_per_epoch)
    use_amp = dev.type == "cuda"
    scaler = torch.amp.GradScaler("cuda", enabled=use_amp)
    start_epoch, best_iou = 0, -1.0
    ckpt_path, best_path = out / "checkpoint.pt", out / "best.pt"
    state = {"epoch": 0}

    ck = torch.load(ckpt_path, map_location="cpu") if args.resume and ckpt_path.exists() else None
    if ck is not None and ck.get("arch") != ARCH_VERSION:
        log("STATUS checkpoint is from an older model architecture, starting a fresh run")
        ck = None
    if ck is not None:
        model.load_state_dict(ck["model"])
        opt.load_state_dict(ck["opt"])
        sched.load_state_dict(ck["sched"])
        start_epoch, best_iou = ck["epoch"], ck["best_iou"]
        log(f"STATUS resumed from epoch {start_epoch}")
    elif args.init:
        model.load_state_dict(torch.load(args.init, map_location=dev, weights_only=True))
        log(f"STATUS initialized from {args.init}")

    def save_checkpoint():
        torch.save({"model": model.state_dict(), "opt": opt.state_dict(), "sched": sched.state_dict(),
                    "epoch": state["epoch"], "best_iou": best_iou, "arch": ARCH_VERSION}, ckpt_path)

    for epoch in range(start_epoch, epochs):
        state["epoch"] = epoch
        t0, loss_sum = time.time(), 0.0
        order = train_idx[:]
        random.shuffle(order)
        for step in range(steps_per_epoch):
            if step % 10 == 0:
                wait_if_paused(out, args, save_checkpoint)
            j = order[step * args.batch:(step + 1) * args.batch] or order[:args.batch]
            x, m = augment(*sample(j))
            with torch.autocast("cuda", enabled=use_amp):
                logits = model(x)
                bce = F.binary_cross_entropy_with_logits(logits.float(), m)
                p = torch.sigmoid(logits.float())
                dice = 1 - (2 * (p * m).sum() + 1) / (p.sum() + m.sum() + 1)
                loss = bce + dice
            opt.zero_grad(set_to_none=True)
            scaler.scale(loss).backward()
            scaler.step(opt)
            scaler.update()
            sched.step()
            loss_sum += loss.item()
            log(f"PROGRESS {(epoch + (step + 1) / steps_per_epoch) / epochs:.4f}")

        val_iou = iou(model, val_x, val_m, dev)
        if val_iou > best_iou:
            best_iou = val_iou
            torch.save(model.state_dict(), best_path)
        state["epoch"] = epoch + 1
        save_checkpoint()
        log(f"EPOCH {epoch + 1}/{epochs} loss={loss_sum / steps_per_epoch:.4f} valIoU={val_iou:.4f} "
            f"best={best_iou:.4f} time={time.time() - t0:.1f}s")

    model.load_state_dict(torch.load(best_path, map_location=dev))
    test_iou = iou(model, test_x, test_m, dev)
    log(f"STATUS test IoU {test_iou:.4f}")

    export(model, out, args, len(train_idx), test_iou, dataset, preset["base"], epochs, roi)
    log(f"STATUS shapes.bin: {write_shapes(xs, ms, out)} recorded silhouettes")
    log(f"DONE {out}")


def export(model, out: Path, args, train_images, test_iou, dataset: Path, base, epochs, roi):
    # eval() on the wrapper: export restores the wrapper's training flag recursively afterwards.
    net = ExportWrapper(model.to("cpu").float()).eval()
    dummy = torch.rand(1, 3, args.height, args.width)
    onnx_path = out / "model.onnx"
    torch.onnx.export(net, dummy, str(onnx_path), input_names=["input"], output_names=["mask"],
                      opset_version=17, do_constant_folding=True, dynamo=False)

    try:  # verify the exported graph matches PyTorch
        import onnxruntime as ort
        sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        with torch.no_grad():
            ref = net(dummy).numpy()
        diff = float(np.abs(sess.run(None, {"input": dummy.numpy()})[0] - ref).max())
        log(f"STATUS ONNX verified, max abs diff {diff:.2e}")
        if diff > 1e-3:
            log("ERROR exported ONNX output differs from PyTorch")
            sys.exit(4)
    except ImportError:
        log("STATUS onnxruntime not installed, skipped ONNX verification")

    meta = {
        "Name": args.name or f"{dataset.name} {out.name}",
        "Weapon": args.weapon,
        "InputWidth": args.width,
        "InputHeight": args.height,
        "Classes": 2,
        "TrainingImages": train_images,
        "TrainedAt": date.today().isoformat(),
        "RecommendedInferenceFps": 45,
        "Dataset": dataset.name,
        "TestIoU": round(test_iou, 4),
        "Roi": roi,
        "Architecture": f"TinyUNet{ARCH_VERSION}-c{base}",
        "Epochs": epochs,
    }
    (out / "metadata.json").write_text(json.dumps(meta, indent=2))


if __name__ == "__main__":
    main()
