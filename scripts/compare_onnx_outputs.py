"""Compare FP32 and quantized ONNX raw outputs on real images.

This is a numerical sanity check, not a replacement for task-level mAP/PCK.
"""

from __future__ import annotations

import argparse
import random
from pathlib import Path

import numpy as np
import onnxruntime as ort
from PIL import Image


def preprocess(path: Path, height: int, width: int) -> np.ndarray:
    image = Image.open(path).convert("RGB")
    iw, ih = image.size
    scale = min(width / iw, height / ih)
    rw, rh = int(iw * scale), int(ih * scale)
    canvas = Image.new("RGB", (width, height), (114, 114, 114))
    canvas.paste(image.resize((rw, rh)), ((width - rw) // 2, (height - rh) // 2))
    return np.asarray(canvas, dtype=np.float32).transpose(2, 0, 1)[None] / 255.0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fp32", type=Path, required=True)
    parser.add_argument("--quantized", type=Path, required=True)
    parser.add_argument("--images", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=100)
    parser.add_argument("--seed", type=int, default=4200)
    parser.add_argument(
        "--score-end", type=int, default=0,
        help="Exclusive score-channel end; 0 means all channels after box channel 4",
    )
    args = parser.parse_args()

    fp = ort.InferenceSession(str(args.fp32), providers=["CPUExecutionProvider"])
    quant = ort.InferenceSession(str(args.quantized), providers=["CPUExecutionProvider"])
    fp_input, quant_input = fp.get_inputs()[0], quant.get_inputs()[0]
    if fp_input.shape != quant_input.shape:
        raise SystemExit(f"input mismatch: {fp_input.shape} != {quant_input.shape}")

    files = sorted(
        p for p in args.images.iterdir()
        if p.suffix.lower() in {".jpg", ".jpeg", ".png", ".bmp"}
    )
    files = random.Random(args.seed).sample(files, min(args.limit, len(files)))
    _, _, height, width = fp_input.shape
    score_end = args.score_end or fp.get_outputs()[0].shape[1]

    raw_mae: list[float] = []
    score_mae: list[float] = []
    top_score_delta: list[float] = []
    same_top_anchor = 0
    for path in files:
        data = preprocess(path, height, width)
        a = fp.run(None, {fp_input.name: data})[0]
        b = quant.run(None, {quant_input.name: data})[0]
        if a.shape != b.shape:
            raise SystemExit(f"output mismatch: {a.shape} != {b.shape}")
        raw_mae.append(float(np.mean(np.abs(a - b))))
        a_score = a[:, 4:score_end, :]
        b_score = b[:, 4:score_end, :]
        score_mae.append(float(np.mean(np.abs(a_score - b_score))))
        top_score_delta.append(float(abs(a_score.max() - b_score.max())))
        same_top_anchor += int(np.argmax(a_score) == np.argmax(b_score))

    print(f"images={len(files)} input={width}x{height} score_channels=4:{score_end}")
    print(f"raw_mae_mean={np.mean(raw_mae):.6f}")
    print(f"score_mae_mean={np.mean(score_mae):.6f}")
    print(f"top_score_abs_delta_mean={np.mean(top_score_delta):.6f}")
    print(f"same_top_anchor={same_top_anchor / len(files):.1%}")
    print("note=Numerical sanity check only; use task-level validation for accuracy.")


if __name__ == "__main__":
    main()
