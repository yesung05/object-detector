"""
AI-Hub 이상행동 데이터의 관절 라벨(GT)로 배포 포즈 모델이 사람·관절을 얼마나 잡는지 잰다 (학습 없음, 진단 전용).

왜 필요한가: 쓰러짐 재현율의 병목 중 "저신뢰 20% + 몸통 자세 판정 불가 15%"(docs/anomaly-dataset-improvements-2026-11-05.md)는
포즈 모델이 누운 사람에서 무너진다는 뜻으로 읽히지만, 지금까지는 룰 게이트로만 간접 측정했다. 라벨 XML 에는 이벤트 구간의
17관절(Azure Kinect 골격)이 프레임마다 있으므로 그걸 정답으로 직접 잰다. 전도(07)와 서 있는 사람 위주 클래스(13 폭행·08 파손)를
같은 방식으로 재서 "누운 자세 때문인가"를 대조한다.

측정 경로: 배포 실행 파일(unmanned_detector.exe)을 클립마다 매 프레임 추론(--detect-every 1, 추적·게이트 끔)으로 돌려
--detections/--keypoints CSV 를 받는다. 파이썬 재구현이 아니라 배포 전처리·후처리를 그대로 재는 것이 목적이다.
프레임 이미지는 이 스크립트도 사람도 열지 않는다(README 의 AI-Hub 이용 규칙) — CSV·XML 수치만 다룬다.

골격 매핑(Kinect → COCO 17): 어깨·팔꿈치·엉덩이·무릎은 같은 관절, 손(hand)→손목, 발(foot)→발목은 근사.
머리(Center head)는 COCO 코와 위치가 달라 평가에서 뺀다. 좌우는 둘 다 "사람 기준" 으로 가정하고, 반대일 가능성에 대비해
좌우를 뒤집은 PCK 도 함께 보고한다(둘 중 큰 쪽이 맞는 규약).

사용:
  python -I scripts/diag_pose_gt.py --exe build/Release/unmanned_detector.exe --model models/yolo11n-pose-416x224.onnx \
      --index E:/hunik-anomaly/anomaly_index.csv --split dev --classes 07,13,08 --work E:/hunik-anomaly/_work/pose_gt_dev
"""
import argparse
import csv
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# Kinect 라벨 이름 → COCO 인덱스. "Right  hip" 은 원본 라벨에 공백이 두 개다.
KINECT_TO_COCO = {
    "Left shoulder": 5, "Right shoulder": 6, "Left elbow": 7, "Right elbow": 8,
    "Left hand": 9, "Right hand": 10, "Left hip": 11, "Right  hip": 12, "Right hip": 12,
    "Left knee": 13, "Right knee": 14, "Left foot": 15, "Right foot": 16,
}
COCO_FLIP = {5: 6, 6: 5, 7: 8, 8: 7, 9: 10, 10: 9, 11: 12, 12: 11, 13: 14, 14: 13, 15: 16, 16: 15}
EVAL_JOINTS = sorted(set(KINECT_TO_COCO.values()))
KP_VISIBLE = 0.4   # rules.c KP_SCORE_THRESH 와 같은 기준 — 룰이 "보인다"고 치는 관절
PCK_ALPHA = 0.1    # 정답 박스 대각선의 10% 이내면 맞음(누운 자세는 몸통 길이 정규화가 불안정해 대각선 사용)


def load_gt(xml_path):
    """반환: {(frame, person_id): {coco_idx: (x, y)}} — 이벤트 구간 관절만."""
    out = defaultdict(dict)
    root = ET.parse(xml_path).getroot()
    for tr in root.iter("track"):
        idx = KINECT_TO_COCO.get(tr.get("label"))
        if idx is None:
            continue
        for p in tr.iter("points"):
            if p.get("outside") == "1":
                continue
            pid = "0"
            for a in p.iter("attribute"):
                if a.get("name") == "ID":
                    pid = (a.text or "0").strip()
            x, y = (float(v) for v in p.get("points").split(";")[0].split(","))
            out[(int(p.get("frame")), pid)][idx] = (x, y)
    return out


def run_clip(a, row):
    d = Path(a.work) / "runs" / row["clip"]
    det, kp, done = d / "det.csv", d / "kp.csv", d / "done"
    if done.exists():
        return d
    d.mkdir(parents=True, exist_ok=True)
    # --config 를 주지 않으면 데이터 디렉터리가 실행 위치 기준이 되어 클립끼리 상태를 공유한다 — 빈 설정 파일로 격리.
    (d / "empty.ini").write_text("# diag_pose_gt\n", encoding="utf-8")
    cmd = [a.exe, "--model", a.model, "--provider", "cpu", "--threads", str(a.threads), "--input", row["video"],
           "--video-clock", "--motion-gate", "0", "--detect-fps", "0", "--detect-every", "1", "--no-track",
           "--confidence", "0.1", "--config", str(d / "empty.ini"), "--event-log", str(d / "ev.db"),
           "--detections", str(det), "--keypoints", str(kp)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1800)
    except subprocess.TimeoutExpired:
        print(f"[skip] {row['clip']}: timeout", flush=True)
        return None
    if r.returncode != 0:
        print(f"[skip] {row['clip']}: rc={r.returncode} {r.stderr[-200:]}", flush=True)
        return None
    done.write_text("ok", encoding="utf-8")   # 끝까지 돈 클립만 표시 — 중간에 죽은 CSV 는 다시 돈다
    return d


def load_pred(d):
    boxes = defaultdict(list)   # frame -> [(x1,y1,x2,y2,score)]  (det_index = 목록 순서)
    with open(d / "det.csv", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["kind"] not in ("inference", "adaptive"):
                continue
            boxes[int(r["frame"])].append(tuple(float(r[k]) for k in ("x1", "y1", "x2", "y2", "score")))
    kps = defaultdict(dict)     # (frame, det_index) -> {kp: (x,y,score)}
    with open(d / "kp.csv", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            kps[(int(r["frame"]), int(r["det_index"]))][int(r["kp_index"])] = (float(r["x"]), float(r["y"]), float(r["score"]))
    return boxes, kps


def iou(a, b):
    ix = max(0.0, min(a[2], b[2]) - max(a[0], b[0])); iy = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    u = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / u if u > 0 else 0.0


def evaluate(gt, boxes, kps, acc):
    for (frame, _pid), joints in gt.items():
        if len(joints) < 6:
            continue
        xs = [p[0] for p in joints.values()]; ys = [p[1] for p in joints.values()]
        w, h = max(xs) - min(xs), max(ys) - min(ys)
        m = 0.15 * max(w, h, 20.0)   # 관절 외곽은 실제 몸보다 작다 — 여백
        g = (min(xs) - m, min(ys) - m, max(xs) + m, max(ys) + m)
        diag = ((g[2] - g[0]) ** 2 + (g[3] - g[1]) ** 2) ** 0.5
        pose = "horizontal" if (g[2] - g[0]) > (g[3] - g[1]) else "vertical"
        b = acc[pose]
        b["n"] += 1
        best, best_i = 0.0, -1
        for i, box in enumerate(boxes.get(frame, [])):
            v = iou(g, box)
            if v > best:
                best, best_i = v, i
        if best < 0.3:
            b["miss"] += 1
            continue
        score = boxes[frame][best_i][4]
        b["scores"].append(round(score, 3))
        for t in (0.25, 0.5):
            if score >= t:
                b[f"det@{t}"] += 1
        pk = kps.get((frame, best_i), {})
        for flip in (0, 1):
            hit = tot = 0
            for j, (x, y) in joints.items():
                pj = pk.get(COCO_FLIP[j] if flip else j)
                tot += 1
                if pj and ((pj[0] - x) ** 2 + (pj[1] - y) ** 2) ** 0.5 <= PCK_ALPHA * diag:
                    hit += 1
            b[f"pck_hit_{flip}"] += hit; b[f"pck_tot_{flip}"] += tot
        b["vis_hit"] += sum(1 for j in joints if pk.get(j) and pk[j][2] >= KP_VISIBLE)
        b["vis_tot"] += len(joints)
        hips = sum(1 for j in (11, 12) if pk.get(j) and pk[j][2] >= KP_VISIBLE)
        b["hips_any"] += hips > 0   # rules.c 몸통 판정은 엉덩이 1개 이상이 필요


def summarize(acc):
    out = {}
    for pose, b in acc.items():
        n = b["n"]
        matched = n - b["miss"]
        s = sorted(b["scores"])
        out[pose] = {
            "person_frames": n,
            "matched_iou0.3": round(matched / n, 3) if n else None,
            "det@0.25": round(b["det@0.25"] / n, 3) if n else None,
            "det@0.5": round(b["det@0.5"] / n, 3) if n else None,
            "score_p50": s[len(s) // 2] if s else None,
            "pck@0.1_as_is": round(b["pck_hit_0"] / b["pck_tot_0"], 3) if b["pck_tot_0"] else None,
            "pck@0.1_flipped": round(b["pck_hit_1"] / b["pck_tot_1"], 3) if b["pck_tot_1"] else None,
            "kp_visible@0.4": round(b["vis_hit"] / b["vis_tot"], 3) if b["vis_tot"] else None,
            "hip_visible_any": round(b["hips_any"] / matched, 3) if matched else None,
        }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--split", default="dev", choices=["dev", "pool"])  # test 는 진단에 쓰지 않는다
    ap.add_argument("--classes", default="07,13,08")
    ap.add_argument("--work", required=True)
    ap.add_argument("--limit", type=int, default=0, help="클래스당 클립 수(0=전부)")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--threads", type=int, default=2)
    a = ap.parse_args()
    classes = a.classes.split(",")
    rows = defaultdict(list)
    with open(a.index, encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            if r["split"] == a.split and r["class_code"] in classes and r["video"]:
                rows[r["class_code"]].append(r)
    todo = [r for c in classes for r in (rows[c][: a.limit] if a.limit else rows[c])]
    print(f"clips={len(todo)} " + " ".join(f"{c}:{len(rows[c][: a.limit] if a.limit else rows[c])}" for c in classes), flush=True)
    xml_dir = Path(a.index).parent
    with ThreadPoolExecutor(a.jobs) as ex:
        dirs = list(ex.map(lambda r: run_clip(a, r), todo))
    result = {}
    for c in classes:
        acc = defaultdict(lambda: defaultdict(lambda: 0, scores=[]))
        for r, d in zip(todo, dirs):
            if r["class_code"] != c or d is None:
                continue
            sub = "Training" if r["split_orig"] == "Training" else "Validation"
            xml = next((xml_dir / sub / "라벨링데이터").glob(f"{c}.*/{r['clip']}.xml"))
            boxes, kps = load_pred(d)
            evaluate(load_gt(xml), boxes, kps, acc)
        result[c] = summarize(acc)
    out = Path(a.work) / "result.json"
    out.write_text(json.dumps({"args": vars(a), "result": result}, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    sys.exit(main())
