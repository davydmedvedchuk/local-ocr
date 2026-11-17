#!/usr/bin/env python3
"""Local OCR from the terminal. Same clean-up as the web page (deskew + adaptive black and white), then Tesseract.

    python3 cli/ocr.py scan.jpg -l eng+por
    python3 cli/ocr.py a.png b.png -l rus -o text.txt
    python3 cli/ocr.py scan.jpg --no-clean --save-clean cleaned.png

Needs the tesseract program installed (apt install tesseract-ocr, brew install tesseract) with the language
packs you ask for, plus: pip install -r cli/requirements.txt
"""
import argparse
import sys

import numpy as np
import pytesseract
from PIL import Image

MAXPX = 3000


def load(path):
    im = Image.open(path).convert("L")
    k = min(1.0, MAXPX / max(im.size))
    if k < 1:
        im = im.resize((round(im.width * k), round(im.height * k)), Image.LANCZOS)
    return im


def binarize(gray, win, ratio=0.85):
    """Bradley adaptive threshold. 0 = ink, 255 = paper."""
    a = gray.astype(np.float64)
    h, w = a.shape
    ii = np.zeros((h + 1, w + 1))
    ii[1:, 1:] = a.cumsum(0).cumsum(1)
    r = max(2, win // 2)
    y0 = np.clip(np.arange(h) - r, 0, h)[:, None]
    y1 = np.clip(np.arange(h) + r + 1, 0, h)[:, None]
    x0 = np.clip(np.arange(w) - r, 0, w)[None, :]
    x1 = np.clip(np.arange(w) + r + 1, 0, w)[None, :]
    s = ii[y1, x1] - ii[y0, x1] - ii[y1, x0] + ii[y0, x0]
    cnt = (y1 - y0) * (x1 - x0)
    return np.where(a * cnt < s * ratio, 0, 255).astype(np.uint8)


def skew_angle(bw):
    """Degrees (-15..15); rotating clockwise by this levels the lines (projection profile)."""
    h, w = bw.shape
    step = max(1, max(w, h) // 900)
    ys, xs = np.nonzero(bw[::step, ::step] == 0)
    if len(xs) < 200:
        return 0.0
    x = xs * step - w / 2
    y = ys * step - h / 2
    bins = h // step + w // step + 2

    def score(deg):
        t = np.radians(deg)
        yr = ((x * np.sin(t) + y * np.cos(t)) / step + bins / 2).astype(int)
        yr = yr[(yr >= 0) & (yr < bins)]
        c = np.bincount(yr, minlength=bins).astype(np.float64)
        return (c * c).sum()

    coarse = np.arange(-15, 15.01, 0.5)
    best = max(coarse, key=score)
    fine = np.arange(best - 0.5, best + 0.51, 0.1)
    return float(max(fine, key=score))


def clean(gray_img):
    gray = np.asarray(gray_img)
    h, w = gray.shape
    win = max(15, w >> 4) | 1
    bw = binarize(gray, win)
    ang = skew_angle(bw)
    if abs(ang) >= 0.2:
        lvl = np.asarray(gray_img.rotate(-ang, resample=Image.BICUBIC, fillcolor=255))   # PIL turns counter-clockwise
        bw = binarize(lvl, win)
    m = max(8, round(min(w, h) * 0.02))
    bw[:m, :] = 255; bw[-m:, :] = 255; bw[:, :m] = 255; bw[:, -m:] = 255
    return Image.fromarray(bw), ang


def main():
    ap = argparse.ArgumentParser(description="OCR with straightening and clean-up, all on this machine.")
    ap.add_argument("files", nargs="+")
    ap.add_argument("-l", "--lang", default="eng", help="Tesseract languages, e.g. eng+por (default eng)")
    ap.add_argument("-o", "--out", help="write the text to this file instead of the screen")
    ap.add_argument("--no-clean", action="store_true", help="skip straightening and black and white")
    ap.add_argument("--save-clean", metavar="PNG", help="save the cleaned image (first file only)")
    a = ap.parse_args()
    parts = []
    for i, f in enumerate(a.files):
        im = load(f)
        if not a.no_clean:
            im, ang = clean(im)
            print(f"{f}: straightened {ang:.1f} degrees", file=sys.stderr)
            if a.save_clean and i == 0:
                im.save(a.save_clean)
        text = pytesseract.image_to_string(im, lang=a.lang).strip()
        parts.append(text if len(a.files) == 1 else f"--- {f} ---\n{text}")
    text = "\n\n".join(parts) + "\n"
    if a.out:
        open(a.out, "w", encoding="utf-8").write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
