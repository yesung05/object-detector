"""
AI-Hub 이상행동 데이터의 관절 라벨로 YOLO11n-pose 를 파인튜닝한다(쓰러진 사람 포즈 개선). 사전등록: docs/prereg_pose_aihub.md

왜: 배포 포즈 모델은 누운 사람에서 사람 점수와 관절 위치가 무너진다(docs/pose-aihub-gt-diagnosis-2026-11-07.md). 쓰러짐 룰의
병목(저신뢰 20%, 몸통 자세 판정 불가 15%)이 여기서 온다. 데이터에 전도 구간의 17관절 정답이 프레임마다 있다.

분할: 학습은 pool 분할 장면만 쓴다. dev 는 전후 진단(diag_pose_gt.py, diagnose_fall_gates.py)용으로 남기고, test 는 쓰지 않는다.
COCO 일반 포즈(datasets/pose_original_12000 학습 목록)를 섞어 일반 장면 퇴화를 막고, 같은 COCO held-out 으로 퇴화를 잰다.

라벨 변환(Kinect 17 → COCO 17): 어깨·팔꿈치·엉덩이·무릎은 그대로, 손→손목, 발→발목(근사). 얼굴 5점(코·눈·귀)은 정답이 없어
v=0(손실에서 위치 제외). 사람 박스는 관절 외곽 + 15% 여백(diag_pose_gt.py 와 같은 정의).
알려진 위험: 라벨이 없는 주변 사람(구경꾼)은 배경으로 학습된다 — COCO held-out·dev 의 서 있는 사람 검출률로 퇴화를 확인한다.

프레임 추출은 로컬 ffmpeg 로만 한다. 프레임 이미지를 AI 도구로 열지 않는다(README 의 AI-Hub 이용 규칙).

사용:
  python scripts/finetune_pose_aihub.py --prepare-only      # 데이터만 만들기
  python scripts/finetune_pose_aihub.py                     # 학습 + 416×224 ONNX + COCO held-out 평가
"""
from __future__ import annotations

import argparse
import csv
import json
import random
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from diag_pose_gt import load_gt  # noqa: E402  같은 GT 파서·매핑을 쓴다

INDEX = Path("E:/hunik-anomaly/anomaly_index.csv")
FFMPEG = "ffmpeg"
STRIDE = {"07": 4, "13": 11, "08": 11}   # 전도는 촘촘히, 서 있는 클래스는 대조군으로 성기게(합 약 1.2만 장)
OUT_W = 960                              # 1920→960: 라벨은 정규화 좌표라 축소해도 같다(디스크·학습 I/O 절약)


def coco_line(joints, w, h):
    xs = [p[0] for p in joints.values()]; ys = [p[1] for p in joints.values()]
    m = 0.15 * max(max(xs) - min(xs), max(ys) - min(ys), 20.0)
    x1, y1 = max(0.0, min(xs) - m), max(0.0, min(ys) - m)
    x2, y2 = min(float(w), max(xs) + m), min(float(h), max(ys) + m)
    vals = [0, (x1 + x2) / 2 / w, (y1 + y2) / 2 / h, (x2 - x1) / w, (y2 - y1) / h]
    for k in range(17):
        if k in joints:
            vals += [joints[k][0] / w, joints[k][1] / h, 2]
        else:
            vals += [0.0, 0.0, 0]
    return " ".join(f"{v:.6f}" if isinstance(v, float) else str(v) for v in vals)


def extract(video, frames, out_dir, stem):
    """frames 의 이미지를 out_dir/{stem}_{frame:04d}.jpg 로 뽑는다(select 필터: 디코딩 1회)."""
    sel = "+".join(f"eq(n\\,{f})" for f in frames)
    tmp = out_dir / f"_{stem}_%04d.jpg"
    cmd = [FFMPEG, "-v", "error", "-y", "-i", str(video), "-vf", f"select='{sel}',scale={OUT_W}:-2",
           "-fps_mode", "passthrough", "-q:v", "3", str(tmp)]
    subprocess.run(cmd, check=True)
    for i, f in enumerate(frames, 1):
        src = out_dir / f"_{stem}_{i:04d}.jpg"
        if src.exists():
            src.rename(out_dir / f"{stem}_{f:04d}.jpg")


def prepare(root: Path):
    if (root / "audit.json").exists():
        return root / "pose.yaml"
    assert not root.exists() or not any(root.iterdir()), f"새 폴더를 쓰세요: {root}"
    img_dir, lab_dir = root / "images" / "train", root / "labels" / "train"
    img_dir.mkdir(parents=True); lab_dir.mkdir(parents=True)
    rows = []
    with open(INDEX, encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            if r["split"] == "pool" and r["class_code"] in STRIDE and r["video"] and int(r["joint_label_frames"] or 0) > 0:
                rows.append(r)
    counts = defaultdict(int)
    for n, r in enumerate(rows, 1):
        sub = "Training" if r["split_orig"] == "Training" else "Validation"
        xml = next((INDEX.parent / sub / "라벨링데이터").glob(f"{r['class_code']}.*/{r['clip']}.xml"))
        gt = load_gt(xml)
        by_frame = defaultdict(list)
        for (fr, _pid), joints in gt.items():
            if len(joints) >= 6:
                by_frame[fr].append(joints)
        frames = sorted(by_frame)[:: STRIDE[r["class_code"]]]
        if not frames:
            continue
        w, h = int(r["width"]), int(r["height"])
        stem = r["clip"]
        extract(r["video"], frames, img_dir, stem)
        for fr in frames:
            if not (img_dir / f"{stem}_{fr:04d}.jpg").exists():
                continue
            (lab_dir / f"{stem}_{fr:04d}.txt").write_text(
                "\n".join(coco_line(j, w, h) for j in by_frame[fr]) + "\n", encoding="utf-8")
            counts[r["class_code"]] += 1
        if n % 50 == 0:
            print(f"[PREPARE] {n}/{len(rows)} {dict(counts)}", flush=True)
    aihub = sorted(p.as_posix() for p in img_dir.glob("*.jpg"))
    coco = [l for l in (ROOT / "datasets/pose_original_12000/train.txt").read_text(encoding="utf-8").split() if l]
    train = aihub + coco
    random.Random(4300).shuffle(train)
    (root / "train.txt").write_text("\n".join(train) + "\n", encoding="utf-8")
    base = (ROOT / "datasets/pose_original_12000/pose.yaml").read_text(encoding="utf-8")
    val = (ROOT / "datasets/pose_original_12000/val.txt").as_posix()
    held = (ROOT / "datasets/pose_original_12000/test.txt").as_posix()
    lines = [l for l in base.splitlines() if not l.startswith(("path:", "train:", "val:", "test:"))]
    (root / "pose.yaml").write_text("\n".join([f"train: {(root / 'train.txt').as_posix()}", f"val: {val}"] + lines) + "\n", encoding="utf-8")
    (root / "heldout.yaml").write_text("\n".join([f"train: {(root / 'train.txt').as_posix()}", f"val: {held}"] + lines) + "\n", encoding="utf-8")
    audit = {"aihub_images": len(aihub), "aihub_by_class": dict(counts), "coco_images": len(coco),
             "split": "pool only (dev kept for diagnosis, test unused)", "stride": STRIDE, "seed": 4300}
    (root / "audit.json").write_text(json.dumps(audit, ensure_ascii=False, indent=2), encoding="utf-8")
    print("[PREPARE] done", audit, flush=True)
    return root / "pose.yaml"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data-root", type=Path, default=ROOT / "datasets/pose_aihub_pool")
    ap.add_argument("--epochs", type=int, default=12)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--workers", type=int, default=2)
    ap.add_argument("--lr", type=float, default=0.00005)
    ap.add_argument("--name", default="pose-aihub-finetune")
    ap.add_argument("--prepare-only", action="store_true")
    a = ap.parse_args()
    data = prepare(a.data_root)
    if a.prepare_only:
        return
    from ultralytics import YOLO
    original = ROOT / "yolo11n-pose.pt"
    model = YOLO(str(original))
    # 하이퍼파라미터는 finetune_pose_original.py 와 같게 고정(사전등록: 탐색 없음).
    model.train(data=str(data), epochs=a.epochs, batch=a.batch, imgsz=416, rect=True, device=0,
                workers=a.workers, optimizer="AdamW", lr0=a.lr, lrf=0.1, warmup_epochs=1,
                warmup_bias_lr=a.lr, weight_decay=0.0001, patience=4, mosaic=0.0, mixup=0.0,
                scale=0.15, translate=0.05, degrees=0, hsv_h=0.01, hsv_s=0.3, hsv_v=0.2, seed=4300,
                amp=True, plots=False, project=str(ROOT / "runs/pose"), name=a.name)
    run = Path(model.trainer.save_dir)
    best = run / "weights/best.pt"
    exported = YOLO(str(best)).export(format="onnx", imgsz=[224, 416], dynamic=False, simplify=True)
    shutil.copy2(exported, run / "yolo11n-pose-aihub-416x224.onnx")
    held = {}
    for label, path in [("original", original), ("finetuned", best)]:
        m = YOLO(str(path)).val(data=str(data.parent / "heldout.yaml"), imgsz=416, rect=True, batch=a.batch,
                                device=0, workers=0, plots=False, project=str(run), name="heldout-" + label)
        held[label] = m.results_dict
    (run / "heldout-comparison.json").write_text(json.dumps(held, indent=2), encoding="utf-8")
    print("[COMPLETE]", run, json.dumps(held, indent=2), flush=True)


if __name__ == "__main__":
    sys.exit(main())
