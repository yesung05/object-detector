"""
사람 쌍·손-얼굴 특징의 분리도 측정 (개발 PC 전용, 읽기 전용) — 폭행·흡연·절도 등의 1차 룰 가능성 판정

왜 새 스크립트인가: measure_anomaly_filter_auc.py 는 영상 화소 차분과 손목 속도만 본다. 폭행은 "두 사람이 가깝고
팔다리가 빠르게 움직이는가", 흡연은 "손이 입 근처에 머무는가"라서 사람 사이 관계·관절 위치 특징이 필요하다. 영상을
다시 디코딩해 화소를 볼 필요가 없고(프라이버시·속도 모두 이득), --detections/--keypoints CSV 만으로 계산한다.
추론 캐시(--infer-cache)가 있으면 모델은 다시 돌지 않는다.

특징(프레임마다, 값이 클수록 이상 행동답다):
  n_persons      활성 사람 수
  pair_close     가장 가까운 두 사람의 근접도 = max(0, 1 - 중심거리 / (1.5 × 평균 박스 높이))
  pair_iou       두 사람 박스의 최대 IoU
  pair_approach  가까운 쌍의 중심거리 감소 속도(박스 높이/초, 접근할 때만 양수)
  limb_speed     팔·다리 관절 이동 속도의 최댓값(박스 높이/초). 직전 프레임의 가장 가까운 사람과 짝지어 계산
  pair_limb      가까운 쌍(거리 < 1.2 × 높이)에 속한 사람의 limb_speed 최댓값, 쌍이 없으면 0
  fight_score    pair_close × limb_speed (두 신호가 함께 클 때만 큼 — 단순 곱을 고른 이유: 학습 없이 해석 가능)
  hand_mouth     손목이 코에 가까운 정도 = max(0, 1 - 거리 / (0.25 × 박스 높이)), 사람별 최댓값
  hand_mouth_sust  최근 3프레임(3fps 에서 1초) 동안의 hand_mouth 최솟값 — 흡연은 잠깐 스치는 동작이 아니므로

판정(사전 고정, measure_anomaly_filter_auc.py 와 같은 기준): 정상 프레임의 95분위를 임계값으로 하여, 이벤트 구간 안에
임계값을 연속 2프레임 넘는 지점이 있는 클립 비율(TPR@5%)이 0.90 이상이면 go. AUC 는 참고 지표.
AUC < 0.75 이면 2단계(2차 AI) 없이 종료하는 조기 중단 기준(계획서)을 함께 표시한다.

평가 세트: dev 또는 pool 만(test 거부). 양성 클래스의 장면당 1클립, 음성은 다른 클래스의 장면당 1클립 + 양성 클립의
구간 밖 프레임. --cached-only(기본)이면 추론 캐시가 있는 클립만 써서 HDD 읽기와 재추론을 피한다.

한계: 3fps 연출 촬영(배우 1~2명, 매장 7곳) — 절대 임계값을 현장에 옮기지 말고 "룰로 풀릴 가능성"의 판정에만 쓴다.
"""

import argparse
import csv
import importlib.util
import json
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import numpy as np

# measure_anomaly_filter_auc.py 의 run_exe/auc 를 재사용한다. -I(격리 모드)에서는 같은 폴더 import 가 안 되므로
# 파일 경로로 직접 읽는다 — 같은 로직을 복사하면 한쪽만 고쳐지는 사고가 나기 쉽다.
_spec = importlib.util.spec_from_file_location("auc_base", Path(__file__).with_name("measure_anomaly_filter_auc.py"))
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)

NOSE = 0
ARMS = (5, 6, 7, 8, 9, 10)        # 어깨·팔꿈치·손목
LEGS = (13, 14, 15, 16)           # 무릎·발목
LIMBS = ARMS + LEGS
WRISTS = (9, 10)
MIN_KP = 0.4                      # 관절 신뢰도 하한 — 낮은 점수 관절의 지터가 속도 특징을 부풀리지 않게
FEATURES = ["n_persons", "pair_close", "pair_iou", "pair_approach", "limb_speed", "pair_limb",
            "fight_score", "hand_mouth", "hand_mouth_sust"]


def load_all(det_path, kp_path):
    """frame -> [ {box, kp:{idx:(x,y,s)}} ]  (det_index 순서가 CSV 행 순서; 관절 전체를 읽는다)"""
    frames = defaultdict(list)
    if det_path.exists():
        for r in csv.reader(open(det_path, encoding="utf-8")):
            if r and r[0].lstrip("-").isdigit():
                frames[int(r[0])].append({"box": tuple(map(float, r[2:6])), "kp": {}})
    if kp_path.exists():
        for r in csv.reader(open(kp_path, encoding="utf-8")):
            if r and r[0].lstrip("-").isdigit():
                f, i, k = int(r[0]), int(r[1]), int(r[2])
                if i < len(frames.get(f, [])):
                    frames[f][i]["kp"][k] = (float(r[3]), float(r[4]), float(r[5]))
    return frames


def center(b):
    return (b[0] + b[2]) / 2, (b[1] + b[3]) / 2


def height(b):
    return max(b[3] - b[1], 1.0)


def iou(a, b):
    ix = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    u = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / u if u > 0 else 0.0


def match_prev(p, prev_people):
    """직전 프레임에서 가장 가까운 사람(중심 거리 < 0.6 × 높이). 3fps 라 한 프레임 사이 이동이 크다."""
    cx, cy = center(p["box"])
    h = height(p["box"])
    best, bd = None, 1e18
    for q in prev_people:
        qx, qy = center(q["box"])
        d = (cx - qx) ** 2 + (cy - qy) ** 2
        if d < bd:
            best, bd = q, d
    return best if best is not None and bd <= (0.6 * h) ** 2 else None


def limb_speed_of(p, q, fps):
    h = height(p["box"])
    sp = 0.0
    for k in LIMBS:
        if k in p["kp"] and k in q["kp"] and p["kp"][k][2] >= MIN_KP and q["kp"][k][2] >= MIN_KP:
            v = np.hypot(p["kp"][k][0] - q["kp"][k][0], p["kp"][k][1] - q["kp"][k][1]) / h * fps
            sp = max(sp, float(v))
    return sp


def frame_feats(people, prev_people, fps):
    f = dict.fromkeys(FEATURES, 0.0)
    f["n_persons"] = float(len(people))
    speeds = [limb_speed_of(p, m, fps) if (m := match_prev(p, prev_people)) is not None else 0.0 for p in people]
    f["limb_speed"] = max(speeds, default=0.0)
    best_close, near = 0.0, set()
    for i in range(len(people)):
        for j in range(i + 1, len(people)):
            a, b = people[i], people[j]
            (ax, ay), (bx, by) = center(a["box"]), center(b["box"])
            h = (height(a["box"]) + height(b["box"])) / 2
            d = float(np.hypot(ax - bx, ay - by))
            f["pair_iou"] = max(f["pair_iou"], iou(a["box"], b["box"]))
            best_close = max(best_close, max(0.0, 1.0 - d / (1.5 * h)))
            if d < 1.2 * h:
                near.update((i, j))
            # 접근 속도: 두 사람의 직전 프레임 중심 거리와 비교(둘 다 직전 프레임에 짝이 있을 때만)
            ma, mb = match_prev(a, prev_people), match_prev(b, prev_people)
            if ma is not None and mb is not None and ma is not mb:
                (cx, cy), (dx, dy) = center(ma["box"]), center(mb["box"])
                dprev = float(np.hypot(cx - dx, cy - dy))
                f["pair_approach"] = max(f["pair_approach"], (dprev - d) / h * fps if d < 1.5 * h else 0.0)
    f["pair_close"] = best_close
    f["pair_limb"] = max((speeds[i] for i in near), default=0.0)
    f["fight_score"] = best_close * f["limb_speed"]
    hm = 0.0
    for p in people:
        if NOSE in p["kp"] and p["kp"][NOSE][2] >= MIN_KP:
            nx, ny = p["kp"][NOSE][0], p["kp"][NOSE][1]
            for w in WRISTS:
                if w in p["kp"] and p["kp"][w][2] >= MIN_KP:
                    d = np.hypot(p["kp"][w][0] - nx, p["kp"][w][1] - ny) / height(p["box"])
                    hm = max(hm, max(0.0, 1.0 - float(d) / 0.25))
    f["hand_mouth"] = hm
    return f


def clip_features(frames, n_frames, fps):
    feats = [frame_feats(frames.get(t, []), frames.get(t - 1, []) if t else [], fps) for t in range(n_frames)]
    for t, f in enumerate(feats):
        f["hand_mouth_sust"] = min(x["hand_mouth"] for x in feats[max(0, t - 2):t + 1])
    return feats


def pick_rows(rows, cls, per_scene_limit, limit):
    out, seen = [], Counter()
    for r in sorted((r for r in rows if r["class_code"] == cls), key=lambda r: (r["scene_key"], r["clip"])):
        if seen[r["scene_key"]] < per_scene_limit:
            seen[r["scene_key"]] += 1
            out.append(r)
    return out[::max(1, len(out) // limit)][:limit] if limit else out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--split", default="dev")
    ap.add_argument("--pos-class", required=True)
    ap.add_argument("--neg-classes", default="")
    ap.add_argument("--neg-per-class", type=int, default=20)
    ap.add_argument("--pos-limit", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--fps", type=float, default=3.0)
    ap.add_argument("--margin-seconds", type=float, default=2.0)
    ap.add_argument("--no-cached-only", action="store_true")
    a = ap.parse_args()
    if a.split == "test":
        sys.exit("test 분할은 측정에 쓰지 않습니다(최종 확인 전용).")
    allrows = [r for r in csv.DictReader(open(a.index, encoding="utf-8-sig")) if r["split"] == a.split]
    if not a.no_cached_only:
        cached = {p.stem for p in Path(a.cache_dir).glob("*.bin")}
        allrows = [r for r in allrows if r["clip"] in cached]
    all_classes = sorted({r["class_code"] for r in allrows})
    negs = [c for c in (a.neg_classes.split(",") if a.neg_classes else all_classes) if c and c != a.pos_class]
    pos_rows = pick_rows(allrows, a.pos_class, 1, a.pos_limit)
    neg_rows = [r for c in negs for r in pick_rows(allrows, c, 1, a.neg_per_class)]
    out = Path(a.work)
    out.mkdir(parents=True, exist_ok=True)
    print(f"[pair] 양성 {len(pos_rows)}({a.pos_class}) 음성 {len(neg_rows)}({','.join(negs)}) split={a.split}", flush=True)
    clips = []
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(base.run_exe, a, r, out): (r, True) for r in pos_rows}
        futs.update({ex.submit(base.run_exe, a, r, out): (r, False) for r in neg_rows})
        for i, f in enumerate(as_completed(futs), 1):
            r, pos = futs[f]
            _, det, kp = f.result()
            frames = load_all(det, kp)
            clips.append({"clip": r["clip"], "pos": pos, "cls": r["class_code"], "gt": json.loads(r["events"]) if pos else [],
                          "feats": clip_features(frames, int(r["frames"]), a.fps)})
            if i % 20 == 0 or i == len(futs):
                print(f"  {i}/{len(futs)}", flush=True)
    margin = int(a.margin_seconds * a.fps)
    result = {"pos_class": a.pos_class, "split": a.split, "pos_clips": len(pos_rows), "neg_clips": len(neg_rows),
              "neg_classes": negs, "features": {}}
    for k in FEATURES:
        ev, norm_a, norm_b = [], [], []
        for c in clips:
            v = [f[k] for f in c["feats"]]
            if c["pos"]:
                inwin, near = set(), set()
                for s, e in c["gt"]:
                    e = s if e is None else e
                    inwin.update(range(s, e + 1))
                    near.update(range(s - margin, e + margin + 1))
                for t, x in enumerate(v):
                    if t in inwin:
                        ev.append(x)
                    elif t not in near:
                        norm_a.append(x)
            else:
                norm_b += v
        norm = np.array(norm_a + norm_b)
        thr = float(np.percentile(norm, 95)) if len(norm) else None
        # 클립 내부 정상 구간만으로 정한 임계값(엄격 판정): 다른 클래스 클립과 "사람 수가 다르다"는 사실만으로
        # 분리되는 착시(폭행 클립은 거의 항상 2인 이상)를 걸러낸다. Go 는 두 판정을 모두 통과해야 한다.
        thr_in = float(np.percentile(np.array(norm_a), 95)) if len(norm_a) else None

        def clip_tpr(th):
            hit = tot = 0
            for c in clips:
                if not c["pos"] or th is None:
                    continue
                tot += 1
                v = [f[k] for f in c["feats"]]
                ok = False
                for s, e in c["gt"]:
                    seg = v[s:(s if e is None else e) + 1]
                    if any(seg[i] > th and seg[i + 1] > th for i in range(len(seg) - 1)):
                        ok = True
                hit += ok
            return round(hit / tot, 4) if tot else None
        tpr_all, tpr_in = clip_tpr(thr), clip_tpr(thr_in)
        result["features"][k] = {
            "auc_vs_normal_in_clip": base.auc(np.array(ev), np.array(norm_a)),
            "auc_vs_other_class_clips": base.auc(np.array(ev), np.array(norm_b)),
            "auc_vs_all_normal": base.auc(np.array(ev), norm),
            "threshold_p95_normal": thr, "clip_tpr_at_5pct": tpr_all,
            "threshold_p95_in_clip_normal": thr_in, "clip_tpr_in_clip_normal": tpr_in,
            "strict_tpr": min(tpr_all or 0, tpr_in or 0),
            "event_frames": len(ev), "normal_frames": int(len(norm))}
    best = max(result["features"].items(), key=lambda kv: (kv[1]["strict_tpr"], kv[1]["auc_vs_normal_in_clip"] or 0))
    bauc = max((v["auc_vs_all_normal"] or 0) for v in result["features"].values())
    result["verdict"] = {"criterion": "clip_tpr_at_5pct >= 0.90 그리고 clip_tpr_in_clip_normal >= 0.90", "best_feature": best[0],
                         "best_tpr": best[1]["clip_tpr_at_5pct"], "best_strict_tpr": best[1]["strict_tpr"],
                         "best_auc_any_feature": round(bauc, 4),
                         "go": bool(best[1]["strict_tpr"] >= 0.90),
                         "early_stop_auc_lt_0.75": bool(bauc < 0.75)}
    (out / f"pair_{a.pos_class}_{a.split}.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
