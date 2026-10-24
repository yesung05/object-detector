"""
Tier 2 ONNX 추론 지연 측정 (ONNX Runtime CPU, 배포와 같은 경로)

앱은 detector_ort.c 로 ORT C API 를 쓰고 Tier 2 는 FP32 CPU 라서, 같은 ORT 를 파이썬으로
호출해도 모델 연산 시간은 같다(전·후처리 제외). 스레드는 현장 권장값인 2로 고정한다
(docs/i5-4200u-pose-tier2-load-estimate.md: 4200U 는 물리 코어 2개).

개발 PC 수치를 i5-4200U 로 환산하려면 배율이 필요한데, 이 스크립트는 환산하지 않고
실측값만 낸다. 환산은 보고서에서 기존 문서의 방법을 인용해 별도로 적는다.

사용:
  .venv-train\\Scripts\\python.exe -I scripts\\bench_tier2_latency.py --model models\\yolo11n_tier2_fp32.onnx
"""

import argparse
import json
import platform
import statistics
import subprocess
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort


def cpu_name():
    try:
        out = subprocess.run(["wmic", "cpu", "get", "name"], capture_output=True, text=True, timeout=10).stdout
        lines = [l.strip() for l in out.splitlines() if l.strip() and l.strip() != "Name"]
        return lines[0] if lines else platform.processor()
    except Exception:
        return platform.processor()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--model", required=True)
    p.add_argument("--threads", type=int, default=2)
    p.add_argument("--warmup", type=int, default=10)
    p.add_argument("--runs", type=int, default=100)
    a = p.parse_args()
    so = ort.SessionOptions()
    so.intra_op_num_threads = a.threads
    so.inter_op_num_threads = 1
    sess = ort.InferenceSession(a.model, so, providers=["CPUExecutionProvider"])
    inp = sess.get_inputs()[0]
    shape = [d if isinstance(d, int) else 1 for d in inp.shape]
    x = np.random.default_rng(0).random(shape, dtype=np.float32)
    for _ in range(a.warmup):
        sess.run(None, {inp.name: x})
    ms = []
    for _ in range(a.runs):
        t = time.perf_counter()
        sess.run(None, {inp.name: x})
        ms.append((time.perf_counter() - t) * 1000)
    ms.sort()
    res = {"model": a.model, "input": shape, "output": [o.shape for o in sess.get_outputs()],
           "size_mb": round(Path(a.model).stat().st_size / 1e6, 2), "threads": a.threads,
           "mean_ms": round(statistics.mean(ms), 2), "p50_ms": round(ms[len(ms) // 2], 2),
           "p95_ms": round(ms[int(len(ms) * 0.95) - 1], 2), "cpu": cpu_name(), "ort": ort.__version__}
    print(json.dumps(res, ensure_ascii=False))


if __name__ == "__main__":
    main()
