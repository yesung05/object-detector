"""Benchmark the Pose and Tier2 ONNX models with deployment-like ORT settings.

This is a synthetic model-only benchmark. It excludes camera decoding, tracking,
rendering, JPEG encoding, networking, and dashboard overhead.
"""

from __future__ import annotations

import argparse
import json
import statistics
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort


def make_session(model: Path, threads: int) -> ort.InferenceSession:
    options = ort.SessionOptions()
    options.intra_op_num_threads = threads
    options.inter_op_num_threads = 1
    options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    options.add_session_config_entry("session.intra_op.allow_spinning", "0")
    options.add_session_config_entry("session.inter_op.allow_spinning", "0")
    return ort.InferenceSession(
        str(model), sess_options=options, providers=["CPUExecutionProvider"]
    )


def percentile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round((len(ordered) - 1) * q)))
    return ordered[index]


def summary(values: list[float]) -> dict[str, float]:
    return {
        "mean_ms": round(statistics.fmean(values), 3),
        "median_ms": round(statistics.median(values), 3),
        "p95_ms": round(percentile(values, 0.95), 3),
        "min_ms": round(min(values), 3),
        "max_ms": round(max(values), 3),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pose", type=Path, default=Path("yolo11n-pose-416-int8.onnx"))
    parser.add_argument(
        "--tier2", type=Path, default=Path("models/yolo11n_tier2_int8.onnx")
    )
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--runs", type=int, default=30)
    args = parser.parse_args()

    pose_session = make_session(args.pose, args.threads)
    tier2_session = make_session(args.tier2, args.threads)

    pose_input = pose_session.get_inputs()[0]
    tier2_input = tier2_session.get_inputs()[0]
    rng = np.random.default_rng(4200)
    pose_data = rng.random(pose_input.shape, dtype=np.float32)
    tier2_data = rng.random(tier2_input.shape, dtype=np.float32)

    for _ in range(args.warmup):
        pose_session.run(None, {pose_input.name: pose_data})
        tier2_session.run(None, {tier2_input.name: tier2_data})

    pose_times: list[float] = []
    tier2_times: list[float] = []
    pair_times: list[float] = []
    for _ in range(args.runs):
        pair_start = time.perf_counter()

        start = time.perf_counter()
        pose_session.run(None, {pose_input.name: pose_data})
        pose_times.append((time.perf_counter() - start) * 1000)

        start = time.perf_counter()
        tier2_session.run(None, {tier2_input.name: tier2_data})
        tier2_times.append((time.perf_counter() - start) * 1000)

        pair_times.append((time.perf_counter() - pair_start) * 1000)

    print(
        json.dumps(
            {
                "provider": "CPUExecutionProvider",
                "threads": args.threads,
                "runs": args.runs,
                "pose": {"model": str(args.pose), **summary(pose_times)},
                "tier2": {"model": str(args.tier2), **summary(tier2_times)},
                "serial_pair": summary(pair_times),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
