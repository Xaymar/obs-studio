#!/usr/bin/env python3
"""usage: psnr.py <outdir> - decodes each variant's .h264 with ffmpeg and computes per-plane PSNR against ref.nv12
(640x368 NV12). Pairs frames by index (the raw .h264 has no timestamps, ffmpeg's psnr filter mis-pairs them)."""
import math, os, subprocess, sys

W, H = 640, 368
FS = W * H * 3 // 2
d = sys.argv[1] if len(sys.argv) > 1 else "."
ref = open(os.path.join(d, "ref.nv12"), "rb").read()


def psnr(a, b):
    se = sum((x - y) * (x - y) for x, y in zip(a, b))
    return float("inf") if se == 0 else 10 * math.log10(255 * 255 * len(a) / se)


for name in ("buffer-devptr", "image-cuarray"):
    path = os.path.join(d, name + ".h264")
    if not os.path.getsize(path) if os.path.exists(path) else True:
        print(f"{name}: missing/empty")
        continue
    dec = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "nv12", "-"],
                         capture_output=True, check=True).stdout
    n = len(dec) // FS
    ys, uvs, worst = [], [], (float("inf"), -1)
    for i in range(n):
        a, b = dec[i * FS:(i + 1) * FS], ref[i * FS:(i + 1) * FS]
        y, uv = psnr(a[:W * H], b[:W * H]), psnr(a[W * H:], b[W * H:])
        ys.append(y)
        uvs.append(uv)
        worst = min(worst, (y, i))
    print(f"{name}: {n} frames decoded (ref {len(ref) // FS}); PSNR Y avg {sum(ys) / n:.2f} dB, "
          f"UV avg {sum(uvs) / n:.2f} dB, worst Y {worst[0]:.2f} dB at frame {worst[1]}")
