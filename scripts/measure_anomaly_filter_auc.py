"""
1차 규칙 필터(AI 없이 계산하는 단순 특징)의 분리도 측정 (개발 PC 전용, 읽기 전용)

목적: 프로젝트 원칙(룰 기반 우선, AI 최소화)에 따라, 새 이상행동을 룰로 잡을 수 있는지 구현 전에 데이터로
판정한다. 특징이 이벤트 구간과 정상 구간을 가르지 못하면 룰로 구현하지 않고 "룰로 안 풀림"으로 기록한다.

특징(프레임마다, 3fps 영상 기준):
  diff_global   회색조 160x90 에서 직전 프레임과 20 넘게 달라진 화소 비율(움직임 에너지)
  diff_person   사람 박스 합집합 안에서만 같은 값(배경 움직임·조명 변화를 줄이려는 변형)
  wrist_speed   손목(9,10) 관절의 이동 속도를 박스 높이로 나눈 값(박스 높이/초), 사람별 최댓값
  motion_combo  diff_person 과 wrist_speed 의 순위 평균(두 신호가 함께 클 때만 큼)
검출·관절은 unmanned_detector.exe 의 --detections/--keypoints CSV(프레임 번호 포함)를 쓴다. 추론 캐시를
쓰므로 같은 클립은 모델을 한 번만 돌린다.

판정(사전 고정): 정상 프레임의 95% 를 건너뛰는 임계값(정상 프레임 특징의 95분위)에서, 이벤트 구간 안에
임계값을 연속 2프레임 넘는 지점이 있는 클립 비율(TPR@5%)이 0.90 이상이어야 "go". AUC 는 참고 지표로 같이 낸다.
(문서의 1단계 규칙 필터 기준과 같다: 정상 95% 스킵 + 이벤트 90% 트리거)

평가 세트: dev 또는 pool 만(test 거부). 양성 클래스의 장면당 1클립(한 사건을 카메라 6대가 찍으므로).
정상 프레임: (A) 양성 클립의 구간 밖(앞뒤 2초 여유), (B) 다른 클래스 클립 전체.
  B 에는 다른 이상행동(폭행 등)이 섞여 있어 움직임이 크다 — 일부러 어려운 정상으로 둔다.

한계: 3fps 연출 촬영이라 프레임 간 변화량이 현장(약 15fps)과 다르다. 절대 임계값을 현장에 옮기지 말고
"룰로 풀릴 가능성이 있는가"의 판정에만 쓴다.
"""

import argparse
import csv
import json
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import cv2
import numpy as np

WRIST = (9, 10)


def run_exe(a, row, out):
    clip = row["clip"]
    det, kp = out / f"{clip}.det.csv", out / f"{clip}.kp.csv"
    cmd = [a.exe, "--model", a.model, "--provider", "cpu", "--threads", str(a.threads), "--input", row["video"],
           "--video-clock", "--motion-gate", "0", "--detect-fps", "0", "--detect-every", "1",
           "--detections", str(det), "--keypoints", str(kp), "--event-log", str(out / f"{clip}.db"),
           "--infer-cache", str(Path(a.cache_dir) / f"{clip}.bin")]
    if not (det.exists() and kp.exists() and det.stat().st_size > 0):
        with open(out / f"{clip}.log", "wb") as lf:
            subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, timeout=900)
    return row, det, kp


def load_persons(det_path, kp_path):
    """frame -> [ {box:(x1,y1,x2,y2), wrists:[(x,y,score),...]} ] (det_index 순서가 CSV 행 순서)"""
    boxes = defaultdict(list)
    if det_path.exists():
        for r in csv.reader(open(det_path, encoding="utf-8")):
            if not r or not r[0].lstrip("-").isdigit():
                continue
            boxes[int(r[0])].append({"box": tuple(map(float, r[2:6])), "wr": {}})
    if kp_path.exists():
        for r in csv.reader(open(kp_path, encoding="utf-8")):
            if not r or not r[0].lstrip("-").isdigit():
                continue
            f, i, k = int(r[0]), int(r[1]), int(r[2])
            if k in WRIST and i < len(boxes.get(f, [])):
                boxes[f][i]["wr"][k] = (float(r[3]), float(r[4]), float(r[5]))
    return boxes


def frame_features(video, persons, n_frames):
    cap = cv2.VideoCapture(video)
    prev, feats = None, []
    ok, frame = cap.read()
    t = 0
    while ok and t < n_frames:
        g = cv2.resize(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY), (160, 90), interpolation=cv2.INTER_AREA)
        if prev is None:
            feats.append({"diff_global": 0.0, "diff_person": 0.0, "wrist_speed": 0.0})
        else:
            d = (cv2.absdiff(g, prev) > 20)
            h0, w0 = frame.shape[:2]
            mask = np.zeros_like(d)
            for p in persons.get(t, []):
                x1, y1, x2, y2 = p["box"]
                mask[max(0, int(y1 / h0 * 90)):int(y2 / h0 * 90) + 1, max(0, int(x1 / w0 * 160)):int(x2 / w0 * 160) + 1] = True
            area = mask.sum()
            # 직전 프레임의 사람과 가장 가까운 중심끼리 짝지어 손목 속도를 구한다
            sp = 0.0
            for p in persons.get(t, []):
                cx, cy = (p["box"][0] + p["box"][2]) / 2, (p["box"][1] + p["box"][3]) / 2
                bh = max(p["box"][3] - p["box"][1], 1.0)
                best, bd = None, 1e18
                for q in persons.get(t - 1, []):
                    qx, qy = (q["box"][0] + q["box"][2]) / 2, (q["box"][1] + q["box"][3]) / 2
                    dd = (cx - qx) ** 2 + (cy - qy) ** 2
                    if dd < bd:
                        best, bd = q, dd
                if best is None or bd > (0.6 * bh) ** 2:
                    continue
                for k in WRIST:
                    if k in p["wr"] and k in best["wr"] and p["wr"][k][2] >= 0.4 and best["wr"][k][2] >= 0.4:
                        v = np.hypot(p["wr"][k][0] - best["wr"][k][0], p["wr"][k][1] - best["wr"][k][1]) / bh * 3.0
                        sp = max(sp, float(v))
            feats.append({"diff_global": float(d.mean()),
                          "diff_person": float((d & mask).sum() / area) if area else 0.0,
                          "wrist_speed": sp})
        prev = g
        ok, frame = cap.read()
        t += 1
    cap.release()
    return feats


def auc(pos, neg):
    """Mann-Whitney U 기반 AUC (동점은 0.5). 표본이 비면 None."""
    if not len(pos) or not len(neg):
        return None
    allv = np.concatenate([pos, neg])
    ranks = allv.argsort().argsort().astype(float) + 1
    # 동점 평균 순위
    _, inv, cnt = np.unique(allv, return_inverse=True, return_counts=True)
    sums = np.bincount(inv, weights=ranks)
    ranks = (sums / cnt)[inv]
    return float((ranks[:len(pos)].sum() - len(pos) * (len(pos) + 1) / 2) / (len(pos) * len(neg)))


def add_combo(clips):
    allv = {k: np.concatenate([[f[k] for f in c["feats"]] for c in clips]) for k in ("diff_person", "wrist_speed")}
    def rk(k, v):
        return np.searchsorted(np.sort(allv[k]), v, side="right") / len(allv[k])
    for c in clips:
        for f in c["feats"]:
            f["motion_combo"] = (rk("diff_person", f["diff_person"]) + rk("wrist_speed", f["wrist_speed"])) / 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--split", default="dev")
    ap.add_argument("--pos-class", required=True, help="양성 분류 코드(예: 08)")
    ap.add_argument("--neg-classes", default="07,09,10,11,12,13,14")
    ap.add_argument("--neg-per-class", type=int, default=12)
    ap.add_argument("--pos-limit", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--fps", type=float, default=3.0)
    ap.add_argument("--margin-seconds", type=float, default=2.0)
    ap.add_argument("--cached-only", action="store_true", help="추론 캐시가 있는 클립만 사용(HDD 재추론 방지)")
    a = ap.parse_args()
    if a.split == "test":
        sys.exit("test 분할은 측정에 쓰지 않습니다(최종 확인 전용).")
    rows = [r for r in csv.DictReader(open(a.index, encoding="utf-8-sig")) if r["split"] == a.split]
    if a.cached_only:
        have = {p.stem for p in Path(a.cache_dir).glob("*.bin")}
        rows = [r for r in rows if r["clip"] in have]
    pos_rows, seen = [], Counter()
    for r in sorted((r for r in rows if r["class_code"] == a.pos_class), key=lambda r: (r["scene_key"], r["clip"])):
        if seen[r["scene_key"]] < 1:
            seen[r["scene_key"]] += 1
            pos_rows.append(r)
    if a.pos_limit:
        pos_rows = pos_rows[::max(1, len(pos_rows) // a.pos_limit)][:a.pos_limit]
    neg_rows = []
    for c in a.neg_classes.split(","):
        rr = sorted((r for r in rows if r["class_code"] == c), key=lambda r: (r["scene_key"], r["clip"]))
        neg_rows += rr[::max(1, len(rr) // a.neg_per_class)][:a.neg_per_class]
    out = Path(a.work)
    out.mkdir(parents=True, exist_ok=True)
    Path(a.cache_dir).mkdir(parents=True, exist_ok=True)
    todo = [(r, True) for r in pos_rows] + [(r, False) for r in neg_rows]
    print(f"[auc] 양성 {len(pos_rows)}클립({a.pos_class}) 음성 {len(neg_rows)}클립 split={a.split}", flush=True)
    clips = []
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(run_exe, a, r, out): (r, pos) for r, pos in todo}
        for i, f in enumerate(as_completed(futs), 1):
            r, pos = futs[f]
            _, det, kp = f.result()
            persons = load_persons(det, kp)
            feats = frame_features(r["video"], persons, int(r["frames"]))
            clips.append({"clip": r["clip"], "pos": pos, "gt": json.loads(r["events"]) if pos else [],
                          "cls": r["class_code"], "feats": feats})
            if i % 20 == 0 or i == len(futs):
                print(f"  {i}/{len(futs)}", flush=True)
    add_combo(clips)
    margin = int(a.margin_seconds * a.fps)
    names = ["diff_global", "diff_person", "wrist_speed", "motion_combo"]
    result = {"pos_class": a.pos_class, "split": a.split, "pos_clips": len(pos_rows), "neg_clips": len(neg_rows), "features": {}}
    for k in names:
        ev, norm_a, norm_b = [], [], []
        for c in clips:
            v = [f[k] for f in c["feats"]]
            if c["pos"]:
                inwin = set()
                for s, e in c["gt"]:
                    inwin.update(range(s, (e if e is not None else s) + 1))
                near = set()
                for s, e in c["gt"]:
                    near.update(range(s - margin, (e if e is not None else s) + margin + 1))
                for t, x in enumerate(v):
                    if t in inwin:
                        ev.append(x)
                    elif t not in near:
                        norm_a.append(x)
            else:
                norm_b += v
        norm = np.array(norm_a + norm_b)
        thr = float(np.percentile(norm, 95)) if len(norm) else None
        hit = tot = 0
        for c in clips:
            if not c["pos"]:
                continue
            tot += 1
            v = [f[k] for f in c["feats"]]
            ok = False
            for s, e in c["gt"]:
                seg = v[s:(e if e is not None else s) + 1]
                if any(seg[i] > thr and seg[i + 1] > thr for i in range(len(seg) - 1)):
                    ok = True
            hit += ok
        result["features"][k] = {
            "auc_vs_normal_in_clip": auc(np.array(ev), np.array(norm_a)),
            "auc_vs_other_class_clips": auc(np.array(ev), np.array(norm_b)),
            "auc_vs_all_normal": auc(np.array(ev), norm),
            "threshold_p95_normal": thr, "clip_tpr_at_5pct": round(hit / tot, 4) if tot else None,
            "event_frames": len(ev), "normal_frames": len(norm)}
    best = max(result["features"].items(), key=lambda kv: kv[1]["clip_tpr_at_5pct"] or 0)
    result["verdict"] = {"criterion": "clip_tpr_at_5pct >= 0.90 (정상 95% 스킵)", "best_feature": best[0],
                         "best_tpr": best[1]["clip_tpr_at_5pct"], "go": bool((best[1]["clip_tpr_at_5pct"] or 0) >= 0.90)}
    (out / f"auc_{a.pos_class}.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
