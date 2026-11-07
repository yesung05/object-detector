"""
포즈 파인튜닝 3차용 라벨 재구성 — 1·2차 No-Go(일반 장면 COCO 포즈 mAP50 0.739→0.54) 원인 두 가지를 데이터에서 고친다.
사전등록: docs/prereg_pose_aihub.md 변경 이력(3차).

원인 1 — 라벨 없는 주변 사람: AI-Hub 은 이벤트 행위자만 관절을 달았다. 같은 프레임의 다른 손님·직원이 배경으로 학습돼
  모델이 사람에 소극적이 됐다(COCO 박스 재현율 0.82→0.62). dev 에서 라벨 프레임의 4~17% 에 점수 0.5 이상 무라벨 사람이 있었다.
  → 원본 모델(yolo11n-pose.pt)이 0.5 이상으로 찾은 무라벨 사람은 그 검출(박스·관절)을 의사 라벨로 넣는다.
    0.25~0.5 의 애매한 무라벨 검출이 있는 프레임은 통째로 뺀다(사람인지 모르는 것을 배경으로 가르치지 않음).
원인 2 — 박스 정의 차이: "관절 외곽 + 15%" 는 COCO 의 몸 윤곽 박스와 모양이 달라 박스 mAP50-95 가 0.68→0.34 로 무너졌다.
  → 원본 모델이 IoU ≥ 0.5 로 잡은 사람은 그 박스를 쓴다(COCO 방식). 못 잡은 사람(주로 누운 사람 — 고치려는 대상)은
    서 있는 사람에서 잰 "관절 외곽 → 모델 박스" 변별 여백(변마다, 키 대비 비율의 중앙값)으로 보정한 박스를 쓴다.
관절: 몸 12점은 AI-Hub 정답(배우려는 것), 얼굴 5점은 원본 모델 값(점수 ≥ 0.5 일 때만, 아니면 v=0). 1·2차는 얼굴을 늘 v=0 으로
  둬서 얼굴 관절 신뢰도를 깎았을 수 있다.

이미지는 1·2차와 같은 파일(datasets/pose_aihub_pool/images)을 하드링크로 재사용한다 — 디코딩 없음. 프레임은 AI 도구로 열지 않는다.

사용: python scripts/pseudolabel_pose_aihub.py   → datasets/pose_aihub_pool_v3 (pose.yaml, heldout.yaml, audit.json)
"""
from __future__ import annotations

import json
import os
import shutil
import random
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from diag_pose_gt import load_gt, iou  # noqa: E402
from finetune_pose_aihub import INDEX, load_heads  # noqa: E402

SRC = ROOT / "datasets/pose_aihub_pool"
DST = ROOT / "datasets/pose_aihub_pool_v3"
CONF_SURE, CONF_MAYBE = 0.5, 0.25
FACE = (0, 1, 2, 3, 4)


def gt_people(clip_rows):
    """{image_stem: [(joints{coco:(x,y)}, extent_box)]} — 원본 1920×1080 좌표."""
    import csv
    out = {}
    for r in clip_rows:
        sub = "Training" if r["split_orig"] == "Training" else "Validation"
        xml = next((INDEX.parent / sub / "라벨링데이터").glob(f"{r['class_code']}.*/{r['clip']}.xml"))
        gt, heads = load_gt(xml), load_heads(xml)
        for (fr, pid), j in gt.items():
            if len(j) < 6:
                continue
            pts = list(j.values()) + ([heads[(fr, pid)]] if (fr, pid) in heads else [])
            xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
            out.setdefault(f"{r['clip']}_{fr:04d}", []).append((j, (min(xs), min(ys), max(xs), max(ys))))
    return out


def main():
    import csv
    from ultralytics import YOLO
    assert not (DST / "audit.json").exists(), f"이미 있음: {DST}"
    rows = [r for r in csv.DictReader(open(INDEX, encoding="utf-8-sig"))
            if r["split"] == "pool" and r["class_code"] in ("07", "08", "13")]
    people = gt_people(rows)
    imgs = sorted((SRC / "images/train").glob("*.jpg"))
    model = YOLO(str(ROOT / "yolo11n-pose.pt"))
    preds = {}
    for i in range(0, len(imgs), 64):
        for p, res in zip(imgs[i:i + 64], model.predict([str(x) for x in imgs[i:i + 64]], imgsz=640, conf=CONF_MAYBE,
                                                        device=0, verbose=False, half=False)):
            b = res.boxes.xyxy.cpu().numpy().tolist(); c = res.boxes.conf.cpu().numpy().tolist()
            k = res.keypoints.data.cpu().numpy().tolist() if res.keypoints is not None else [[] for _ in b]
            preds[p.stem] = (res.orig_shape, list(zip(b, c, k)))
        if i % 1280 == 0:
            print(f"[PRED] {i}/{len(imgs)}", flush=True)
    # 1) 서 있는 사람(세로 외곽)에서 "관절 외곽 → 모델 박스" 변별 여백을 잰다(키 대비 비율, 중앙값).
    scale_of = {}
    margins = defaultdict(list)
    for stem, (shape, dets) in preds.items():
        s = shape[1] / 1920.0
        scale_of[stem] = s
        for j, ext in people.get(stem, []):
            e = [v * s for v in ext]
            if (e[2] - e[0]) >= (e[3] - e[1]):
                continue
            best = max(dets, key=lambda d: iou(e, d[0]), default=None)
            if best is None or best[1] < CONF_SURE or iou(e, best[0]) < 0.5:
                continue
            hgt = e[3] - e[1]
            margins["l"].append((e[0] - best[0][0]) / hgt); margins["t"].append((e[1] - best[0][1]) / hgt)
            margins["r"].append((best[0][2] - e[2]) / hgt); margins["b"].append((best[0][3] - e[3]) / hgt)
    mg = {k: statistics.median(v) for k, v in margins.items()}
    print("[MARGIN] (키 대비)", {k: round(v, 3) for k, v in mg.items()}, "n=", len(margins["l"]), flush=True)
    # 2) 라벨 쓰기
    (DST / "images/train").mkdir(parents=True, exist_ok=True); (DST / "labels/train").mkdir(parents=True, exist_ok=True)
    st = Counter()
    for stem, (shape, dets) in preds.items():
        h, w = shape
        s = scale_of[stem]
        gts = people.get(stem, [])
        used = set(); lines = []; skip = False
        for j, ext in gts:
            e = [v * s for v in ext]
            bi = max(range(len(dets)), key=lambda i: iou(e, dets[i][0]), default=-1)
            matched = bi >= 0 and dets[bi][1] >= CONF_SURE and iou(e, dets[bi][0]) >= 0.5
            kp = [[0.0, 0.0, 0]] * 17
            for c, (x, y) in j.items():
                kp[c] = [x * s, y * s, 2]
            if matched:
                used.add(bi); box = dets[bi][0]; st["gt_box_from_model"] += 1
                for f in FACE:
                    fx, fy, fc = dets[bi][2][f]
                    if fc >= CONF_SURE:
                        kp[f] = [fx, fy, 2]
            else:
                # 키는 세로면 높이, 누우면 긴 변 — 누운 사람의 여백도 "몸 길이" 대비로 준다
                L = max(e[2] - e[0], e[3] - e[1])
                box = [e[0] - mg["l"] * L, e[1] - mg["t"] * L, e[2] + mg["r"] * L, e[3] + mg["b"] * L]
                st["gt_box_calibrated"] += 1
                if bi >= 0 and iou(e, dets[bi][0]) >= 0.3:
                    used.add(bi)   # 같은 사람을 다른 박스로 잡은 검출 — 무라벨로 취급하지 않음
            lines.append((box, kp))
        for i, (b, c, k) in enumerate(dets):
            if i in used:
                continue
            if any(iou(b, [v * s for v in ext]) >= 0.1 for _, ext in gts):
                continue   # 라벨 사람과 겹치는 조각 검출
            if c >= CONF_SURE:
                lines.append((b, [[x, y, 2 if sc >= CONF_SURE else 0] for x, y, sc in k] if k else [[0.0, 0.0, 0]] * 17))
                st["pseudo_people"] += 1
            else:
                skip = True
        if skip:
            st["frames_skipped_ambiguous"] += 1
            continue
        if not lines:
            continue
        txt = []
        for box, kp in lines:
            x1, y1 = max(0.0, box[0]), max(0.0, box[1]); x2, y2 = min(float(w), box[2]), min(float(h), box[3])
            if x2 <= x1 or y2 <= y1:
                continue
            v = [0, (x1 + x2) / 2 / w, (y1 + y2) / 2 / h, (x2 - x1) / w, (y2 - y1) / h]
            for x, y, vis in kp:
                v += [x / w, y / h, int(vis)] if vis else [0.0, 0.0, 0]
            txt.append(" ".join(f"{a:.6f}" if isinstance(a, float) else str(a) for a in v))
        src = SRC / "images/train" / f"{stem}.jpg"; dst = DST / "images/train" / f"{stem}.jpg"
        if not dst.exists():
            shutil.copyfile(src, dst)   # H: 는 하드링크를 지원하지 않는 파일 시스템이라 복사
        (DST / "labels/train" / f"{stem}.txt").write_text("\n".join(txt) + "\n", encoding="utf-8")
        st["frames_written"] += 1
    aihub = sorted(p.as_posix() for p in (DST / "images/train").glob("*.jpg"))
    coco = [l for l in (ROOT / "datasets/pose_original_12000/train.txt").read_text(encoding="utf-8").split() if l]
    train = aihub + coco
    random.Random(4300).shuffle(train)
    (DST / "train.txt").write_text("\n".join(train) + "\n", encoding="utf-8")
    for name in ("pose.yaml", "heldout.yaml"):
        text = (SRC / name).read_text(encoding="utf-8").replace((SRC / "train.txt").as_posix(), (DST / "train.txt").as_posix())
        (DST / name).write_text(text, encoding="utf-8")
    audit = {"recipe": "v3 pseudo-label", "margins_over_height": mg, "stats": dict(st), "aihub_images": len(aihub),
             "coco_images": len(coco), "conf_sure": CONF_SURE, "conf_maybe": CONF_MAYBE, "seed": 4300}
    (DST / "audit.json").write_text(json.dumps(audit, ensure_ascii=False, indent=2), encoding="utf-8")
    print("[DONE]", json.dumps(audit, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    sys.exit(main())
