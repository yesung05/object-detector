"""
앱 후처리 경로에서 NMS 방식(클래스 비구분 vs 클래스별)의 전후 차이를 라벨 데이터로 측정 (읽기 전용)

왜 따로 재나: Ultralytics 검증(eval_tier2_homeobjects.py)은 처음부터 클래스별 NMS 라서, 앱의
src/postprocess.c 가 클래스를 구분하지 않던 문제(수정 전)의 영향이 감사 수치에 드러나지 않는다.
이 스크립트는 앱과 같은 경로 — ONNX 원시 출력 [1,11,N] → 앵커별 argmax(클래스 1개) →
신뢰도 컷 → NMS(IoU 0.45) — 를 파이썬으로 재현해 두 NMS 만 바꿔 같은 이미지·같은 라벨에 적용한다.

한계(보고서에 같이 적는다):
  - 전처리는 Ultralytics 식 letterbox(회색 114)이고 C 코드의 letterbox 와 픽셀이 완전히 같지 않다.
    두 NMS 변형이 같은 전처리를 쓰므로 *차이*는 공정하지만 절대 수치는 앱과 약간 다를 수 있다.
  - C 의 후보 상한(max_candidates)·최대 검출 수 제한은 재현하지 않았다.
정답 매칭은 같은 클래스·IoU≥0.5 의 탐욕 매칭이다.

사용:
  .venv-train\\Scripts\\python.exe -I scripts\\compare_nms_app_path.py --model models\\yolo11n_tier2_fp32.onnx
"""

import argparse
import json
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort

ROOT = Path(__file__).resolve().parents[1]
NAMES = ["cat", "dog", "bottle", "cup", "food", "chair", "dining table"]
SETS = {"v3val": ROOT / "datasets/tier2_v4", "homeobjects": ROOT / "datasets/homeobjects-eval"}
NMS_IOU = 0.45


def letterbox(img, size):
    h, w = img.shape[:2]
    r = min(size / h, size / w)
    nh, nw = round(h * r), round(w * r)
    px, py = (size - nw) // 2, (size - nh) // 2
    canvas = np.full((size, size, 3), 114, np.uint8)
    canvas[py:py + nh, px:px + nw] = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    return canvas, r, px, py


def iou_one_to_many(b, boxes):
    x1 = np.maximum(b[0], boxes[:, 0]); y1 = np.maximum(b[1], boxes[:, 1])
    x2 = np.minimum(b[2], boxes[:, 2]); y2 = np.minimum(b[3], boxes[:, 3])
    inter = np.clip(x2 - x1, 0, None) * np.clip(y2 - y1, 0, None)
    a = (b[2] - b[0]) * (b[3] - b[1])
    ar = (boxes[:, 2] - boxes[:, 0]) * (boxes[:, 3] - boxes[:, 1])
    return inter / np.maximum(a + ar - inter, 1e-9)


def nms(boxes, scores, classes, per_class):
    order = np.argsort(-scores)
    keep = []
    for i in order:
        suppressed = False
        for j in keep:
            if per_class and classes[i] != classes[j]:
                continue
            if iou_one_to_many(boxes[i], boxes[j][None])[0] > NMS_IOU:
                suppressed = True
                break
        if not suppressed:
            keep.append(i)
    return keep


def match(pred_boxes, pred_scores, pred_cls, gt_boxes, gt_cls, stats):
    order = np.argsort(-pred_scores)
    used = set()
    for i in order:
        c = int(pred_cls[i])
        best, bj = 0.5, -1
        for j in range(len(gt_boxes)):
            if j in used or int(gt_cls[j]) != c:
                continue
            v = iou_one_to_many(pred_boxes[i], gt_boxes[j][None])[0]
            if v >= best:
                best, bj = v, j
        if bj >= 0:
            used.add(bj)
            stats[c]["tp"] += 1
        else:
            stats[c]["fp"] += 1
    for j in range(len(gt_boxes)):
        if j not in used:
            stats[int(gt_cls[j])]["fn"] += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--imgsz", type=int, default=320)
    ap.add_argument("--conf", default="0.20,0.35")
    ap.add_argument("--limit", type=int, default=0, help="디버그용 이미지 수 제한")
    ap.add_argument("--out", default="runs/tier2-homeobjects-eval/nms_app_path.json")
    a = ap.parse_args()
    confs = [float(c) for c in a.conf.split(",")]
    so = ort.SessionOptions(); so.intra_op_num_threads = 2
    sess = ort.InferenceSession(str(ROOT / a.model), so, providers=["CPUExecutionProvider"])
    inp = sess.get_inputs()[0].name
    result = {"model": a.model, "imgsz": a.imgsz, "nms_iou": NMS_IOU, "sets": {}}
    for sname, droot in SETS.items():
        imgs = sorted((droot / "images/val").iterdir())
        if a.limit:
            imgs = imgs[: a.limit]
        # stats[conf][variant][class] = tp/fp/fn
        stats = {c: {v: defaultdict(lambda: {"tp": 0, "fp": 0, "fn": 0}) for v in ("agnostic", "per_class")}
                 for c in confs}
        removed_cross = {c: 0 for c in confs}  # 클래스 비구분 NMS 만 지운 박스 수
        for n, im in enumerate(imgs):
            img = cv2.imread(str(im))
            h, w = img.shape[:2]
            lb, r, px, py = letterbox(img, a.imgsz)
            x = lb[:, :, ::-1].transpose(2, 0, 1)[None].astype(np.float32) / 255.0
            out = sess.run(None, {inp: np.ascontiguousarray(x)})[0][0].T  # [N, 4+nc]
            sc_all = out[:, 4:]
            cls_all = sc_all.argmax(1)
            score_all = sc_all.max(1)
            lab = (droot / "labels/val" / (im.stem + ".txt"))
            gts = [l.split() for l in lab.read_text().splitlines() if l.strip()] if lab.exists() else []
            gt_cls = np.array([int(g[0]) for g in gts], int)
            gt_boxes = np.array([[(float(g[1]) - float(g[3]) / 2) * w, (float(g[2]) - float(g[4]) / 2) * h,
                                  (float(g[1]) + float(g[3]) / 2) * w, (float(g[2]) + float(g[4]) / 2) * h]
                                 for g in gts]).reshape(-1, 4)
            for c in confs:
                m = score_all >= c
                if not m.any():
                    for v in ("agnostic", "per_class"):
                        for j in range(len(gt_cls)):
                            stats[c][v][int(gt_cls[j])]["fn"] += 1
                    continue
                cx, cy, bw, bh = out[m, 0], out[m, 1], out[m, 2], out[m, 3]
                boxes = np.stack([(cx - bw / 2 - px) / r, (cy - bh / 2 - py) / r,
                                  (cx + bw / 2 - px) / r, (cy + bh / 2 - py) / r], 1)
                scores, classes = score_all[m], cls_all[m]
                kept = {}
                for v, pc in (("agnostic", False), ("per_class", True)):
                    k = nms(boxes, scores, classes, pc)
                    kept[v] = k
                    match(boxes[k], scores[k], classes[k], gt_boxes, gt_cls, stats[c][v])
                removed_cross[c] += len(kept["per_class"]) - len(kept["agnostic"])
            if (n + 1) % 500 == 0:
                print(f"  {sname} {n + 1}/{len(imgs)}", flush=True)
        sres = {}
        for c in confs:
            sres[str(c)] = {"extra_boxes_kept_by_per_class_nms": removed_cross[c], "classes": {}}
            for v in ("agnostic", "per_class"):
                for ci in range(len(NAMES)):
                    s = stats[c][v].get(ci)
                    if not s or s["tp"] + s["fn"] == 0:
                        continue
                    p = s["tp"] / max(s["tp"] + s["fp"], 1)
                    rc = s["tp"] / max(s["tp"] + s["fn"], 1)
                    sres[str(c)]["classes"].setdefault(NAMES[ci], {})[v] = {
                        **s, "precision": round(p, 4), "recall": round(rc, 4)}
        result["sets"][sname] = sres
        print(f"[{sname}] done", flush=True)
    out = ROOT / a.out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    print("saved", out)


if __name__ == "__main__":
    main()
