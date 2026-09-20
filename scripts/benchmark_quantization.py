"""Benchmark a FP32/INT8 ONNX pair on the CPU.

The benchmark intentionally measures model inference only. Camera decoding,
image preprocessing, postprocessing, rendering, and network I/O are excluded.
CPU utilization is calculated from the benchmark process CPU time, so the
reported ``cpu_percent_one_core`` treats one fully-used logical CPU as 100%.
``cpu_percent_machine`` is normalized by the number of logical CPUs visible to
the process.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import statistics
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort
import psutil


def percentile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round((len(ordered) - 1) * q)))
    return ordered[index]


def cpu_seconds(times) -> float:
    return float(times.user + times.system)


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


def measure(model: Path, threads: int, warmup: int, runs: int, seed: int) -> dict:
    session = make_session(model, threads)
    input_info = session.get_inputs()[0]
    if any(not isinstance(dim, int) for dim in input_info.shape):
        raise ValueError(f"static input shape required: {input_info.shape}")
    data = np.random.default_rng(seed).random(input_info.shape, dtype=np.float32)

    for _ in range(warmup):
        session.run(None, {input_info.name: data})

    process = psutil.Process(os.getpid())
    before_cpu = cpu_seconds(process.cpu_times())
    start_wall = time.perf_counter()
    latencies: list[float] = []
    output = None
    for _ in range(runs):
        start = time.perf_counter()
        output = session.run(None, {input_info.name: data})
        latencies.append((time.perf_counter() - start) * 1000.0)
    wall_seconds = time.perf_counter() - start_wall
    cpu_used_seconds = cpu_seconds(process.cpu_times()) - before_cpu

    logical_cpus = os.cpu_count() or 1
    file_size = model.stat().st_size
    result = {
        "model": str(model),
        "file_size_bytes": file_size,
        "file_size_mib": round(file_size / 1024 / 1024, 3),
        "input_shape": list(input_info.shape),
        "input_type": input_info.type,
        "runs": runs,
        "warmup": warmup,
        "threads": threads,
        "latency_ms": {
            "mean": round(statistics.fmean(latencies), 3),
            "median": round(statistics.median(latencies), 3),
            "p95": round(percentile(latencies, 0.95), 3),
            "min": round(min(latencies), 3),
            "max": round(max(latencies), 3),
        },
        "wall_seconds": round(wall_seconds, 4),
        "cpu_seconds": round(cpu_used_seconds, 4),
        "cpu_ms_per_inference": round(cpu_used_seconds * 1000.0 / runs, 3),
        "cpu_percent_one_core": round(cpu_used_seconds / wall_seconds * 100.0, 2),
        "cpu_percent_machine": round(
            cpu_used_seconds / wall_seconds / logical_cpus * 100.0, 2
        ),
        "output_shapes": [list(value.shape) for value in output],
    }
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fp32", type=Path, required=True)
    parser.add_argument("--int8", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--runs", type=int, default=100)
    parser.add_argument("--seed", type=int, default=4200)
    args = parser.parse_args()
    if args.threads < 1 or args.warmup < 0 or args.runs < 1:
        parser.error("threads/runs must be positive and warmup cannot be negative")

    results = {
        "benchmark": "FP32 vs INT8 ONNX Runtime CPU benchmark",
        "provider": "CPUExecutionProvider",
        "onnxruntime": ort.__version__,
        "python": platform.python_version(),
        "platform": platform.platform(),
        "logical_cpus": os.cpu_count(),
        "settings": {
            "threads": args.threads,
            "warmup": args.warmup,
            "runs": args.runs,
            "seed": args.seed,
            "execution_mode": "ORT_SEQUENTIAL",
            "graph_optimization": "ORT_ENABLE_ALL",
            "spinning": False,
        },
        "fp32": measure(args.fp32, args.threads, args.warmup, args.runs, args.seed),
        "int8": measure(args.int8, args.threads, args.warmup, args.runs, args.seed),
    }
    fp32 = results["fp32"]
    int8 = results["int8"]
    results["comparison"] = {
        "latency_speedup_x": round(
            fp32["latency_ms"]["mean"] / int8["latency_ms"]["mean"], 3
        ),
        "latency_change_percent": round(
            (int8["latency_ms"]["mean"] / fp32["latency_ms"]["mean"] - 1.0)
            * 100.0,
            2,
        ),
        "cpu_ms_per_inference_change_percent": round(
            (
                int8["cpu_ms_per_inference"] / fp32["cpu_ms_per_inference"] - 1.0
            )
            * 100.0,
            2,
        ),
        "file_size_reduction_percent": round(
            (1.0 - int8["file_size_bytes"] / fp32["file_size_bytes"]) * 100.0,
            2,
        ),
    }
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
