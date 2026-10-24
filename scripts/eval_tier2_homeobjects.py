"""
Tier 2 모델 전후 비교 평가 (읽기 전용: 배포 모델·앱 코드를 바꾸지 않고 결과 JSON 만 씁니다)

같은 모델을 두 평가 세트에서 잰다.
  (a) v3  : datasets/tier2_v4/tier2_v4.yaml 의 val = tier2_v3 val 3,315장(하드링크라 v3 와 동일).
            학습 중 모델 선택에 쓰인 세트라 낙관적이다(docs/tier2-accuracy-audit 와 같은 한계).
            쓰는 이유: 새 데이터가 기존 7클래스를 망가뜨리지 않았는지(회귀) 확인.
  (b) home: datasets/homeobjects-eval/eval.yaml. HomeObjects val 404장, chair/table 만 라벨.
            어느 모델도 학습에 쓰지 않은 세트라 일반화를 본다. 단 전부 거실이라 카페와 다르다.
            chair/table 외 클래스는 정답이 없어 AP 가 정의되지 않으므로 표에서 빼고 해석한다.

임계값별 정밀도/재현율은 audit_tier2_accuracy.py 와 같은 방식(해당 임계값에 가장 가까운 곡선 지점)이다.
Ultralytics 표준 검증(클래스별 NMS)이라 앱 후처리와 완전히 같지는 않다 — 앱 후처리의 영향은
tests/test_core.c 단위 테스트와 영상 실행으로 따로 본다.

사용:
  .venv-train\\Scripts\\python.exe -I scripts\\eval_tier2_homeobjects.py --label A0 --model models\\yolo11n_tier2_fp32.onnx --imgsz 320
"""

import argparse
import json
import time
from pathlib import Path

import numpy as np
import torch
from ultralytics import YOLO

ROOT = Path(__file__).resolve().parents[1]
SETS = {
    "v3val": ROOT / "datasets/tier2_v4/tier2_v4.yaml",
    "homeobjects": ROOT / "datasets/homeobjects-eval/eval.yaml",
}
THRESHOLDS = (0.2, 0.35, 0.5)


def evaluate(model_path: Path, data: Path, imgsz: int, device: str, out: Path, name: str):
    model = YOLO(str(model_path), task="detect")
    t0 = time.time()
    metrics = model.val(data=str(data), imgsz=imgsz, batch=1 if str(model_path).endswith(".onnx") else 16,
                        device=device, workers=0, conf=0.001, iou=0.45, rect=False, plots=False,
                        project=str(out), name=name, exist_ok=True, verbose=False)
    box = metrics.box
    rows = []
    for i, cls in enumerate(box.ap_class_index):
        row = {"class": metrics.names[int(cls)], "AP50": float(box.ap50[i]), "AP50_95": float(box.ap[i]),
               "thresholds": {}}
        for th in THRESHOLDS:
            idx = int(np.abs(box.px - th).argmin())
            row["thresholds"][str(th)] = {"precision": float(box.p_curve[i, idx]),
                                          "recall": float(box.r_curve[i, idx])}
        rows.append(row)
    return {"mAP50": float(box.map50), "mAP50_95": float(box.map), "classes": rows,
            "seconds": round(time.time() - t0, 1)}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--label", required=True, help="결과 파일 이름(A0, A1 ...)")
    p.add_argument("--model", required=True)
    p.add_argument("--imgsz", type=int, default=320)
    p.add_argument("--device", default="cpu", help="ONNX 는 cpu 권장(배포 경로와 동일)")
    p.add_argument("--sets", default="v3val,homeobjects")
    p.add_argument("--out", default="runs/tier2-homeobjects-eval")
    a = p.parse_args()
    torch.set_num_threads(4)
    out = ROOT / a.out
    out.mkdir(parents=True, exist_ok=True)
    result = {"label": a.label, "model": a.model, "imgsz": a.imgsz, "sets": {}}
    for s in a.sets.split(","):
        print(f"[eval] {a.label} on {s}", flush=True)
        result["sets"][s] = evaluate(ROOT / a.model, SETS[s], a.imgsz, a.device, out, f"{a.label}-{s}")
        (out / f"{a.label}.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    print("[eval] done", json.dumps({k: (v["mAP50"], v["mAP50_95"]) for k, v in result["sets"].items()}))


if __name__ == "__main__":
    main()
