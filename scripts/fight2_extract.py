"""
폭행 2차 AI용 키포인트 창(window) 데이터 만들기 (개발 PC 전용, 읽기 전용)

사전 등록(docs/prereg_fight_stage2.md)의 입력·게이트·라벨 규칙을 그대로 구현한다.
  1) 클립마다 unmanned_detector.exe 를 --video-clock 으로 돌려 사람 검출·관절 CSV 를 얻는다(영상 화소는 이 스크립트가 읽지 않는다).
  2) 프레임마다 가장 가까운 두 사람을 골라 쌍 외접 박스 기준으로 정규화한 관절 + 쌍 기하를 특징으로 만든다.
  3) 9프레임 창(이동 3)에서 게이트(2인 이상 5프레임 이상 + pair_close>=0.2 가 3프레임 이상)를 통과한 창만 저장한다.
  4) 라벨: 폭행 정답 구간과 50% 이상 겹치면 양성, 폭행이 아닌 클립이거나 폭행 클립의 구간 밖(앞뒤 2초)이면 음성, 그 사이는 제외.
dev+pool 만 쓴다(test 거부). 정답 관절이 아니라 모델이 낸 검출 관절만 쓴다.

출력: <work>/windows.npz  (X[N,9,106], y, clip, store, scene, split, cls, w0, gate_close(창 안 최대 pair_close), fscore(창 안 최대 fight_score))
"""

import argparse
import csv
import importlib.util
import json
import random
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import numpy as np

# 같은 폴더의 측정 스크립트에서 exe 실행 함수를 재사용한다(-I 모드는 같은 폴더 import 가 안 되어 경로로 읽는다).
_spec = importlib.util.spec_from_file_location("auc_base", Path(__file__).with_name("measure_anomaly_filter_auc.py"))
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)

T = 9            # 창 길이(프레임)
STRIDE = 3
PERSON_MIN = 0.25
FPS = 3.0
MARGIN = int(2.0 * FPS)


def load_clip(det_path, kp_path):
    """frame -> [(box(4), kp[17,3])] (점수 >= PERSON_MIN 인 사람만)"""
    boxes = defaultdict(list)
    for r in csv.reader(open(det_path, encoding="utf-8")):
        if r and r[0].lstrip("-").isdigit() and float(r[6]) >= PERSON_MIN:
            boxes[int(r[0])].append(np.array(r[2:6], dtype=np.float32))
    # kp CSV 의 det_index 는 "그 프레임의 검출 순번"(점수 필터 전)이라 det CSV 의 같은 프레임 행 순서와 대응한다.
    allboxes = defaultdict(list)
    scores = defaultdict(list)
    for r in csv.reader(open(det_path, encoding="utf-8")):
        if r and r[0].lstrip("-").isdigit():
            allboxes[int(r[0])].append(np.array(r[2:6], dtype=np.float32))
            scores[int(r[0])].append(float(r[6]))
    kps = defaultdict(lambda: defaultdict(lambda: np.zeros((17, 3), np.float32)))
    for r in csv.reader(open(kp_path, encoding="utf-8")):
        if r and r[0].lstrip("-").isdigit():
            f, i, k = int(r[0]), int(r[1]), int(r[2])
            if k < 17:
                kps[f][i][k] = (float(r[3]), float(r[4]), float(r[5]))
    out = {}
    for f, bl in allboxes.items():
        people = [(bl[i], kps[f][i]) for i in range(len(bl)) if scores[f][i] >= PERSON_MIN]
        out[f] = people
    return out


def frame_vec(people, w, h):
    """(벡터[106], valid, pair_close, fight_score_placeholder) — 가장 가까운 두 사람 기준"""
    v = np.zeros(106, np.float32)
    n = len(people)
    v[105] = min(n, 4) / 4.0
    if n < 2:
        return v, 0, 0.0
    best = None
    for i in range(n):
        for j in range(i + 1, n):
            a, b = people[i][0], people[j][0]
            ca, cb = np.array([(a[0] + a[2]) / 2, (a[1] + a[3]) / 2]), np.array([(b[0] + b[2]) / 2, (b[1] + b[3]) / 2])
            ha, hb = max(a[3] - a[1], 1.0), max(b[3] - b[1], 1.0)
            d = float(np.hypot(*(ca - cb))) / ((ha + hb) / 2)
            if best is None or d < best[0]:
                best = (d, i, j, ha, hb)
    d, i, j, ha, hb = best
    pair = sorted([people[i], people[j]], key=lambda p: (p[0][0] + p[0][2]) / 2)
    ux1, uy1 = min(pair[0][0][0], pair[1][0][0]), min(pair[0][0][1], pair[1][0][1])
    ux2, uy2 = max(pair[0][0][2], pair[1][0][2]), max(pair[0][0][3], pair[1][0][3])
    uw, uh = max(ux2 - ux1, 1.0), max(uy2 - uy1, 1.0)
    for s, (box, kp) in enumerate(pair):
        base_i = s * 51
        for k in range(17):
            v[base_i + k * 3] = np.clip((kp[k, 0] - ux1) / uw, -0.5, 1.5)
            v[base_i + k * 3 + 1] = np.clip((kp[k, 1] - uy1) / uh, -0.5, 1.5)
            v[base_i + k * 3 + 2] = kp[k, 2]
    a, b = pair[0][0], pair[1][0]
    ix = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    v[102] = min(d, 4.0) / 4.0
    v[103] = inter / union if union > 0 else 0.0
    v[104] = np.clip(np.log(ha / hb), -1.5, 1.5) / 1.5
    close = max(0.0, 1.0 - d / 1.5)
    return v, 1, close


def windows_of(clip, n_frames, width, height, gt, is_fight):
    out = []
    frames = sorted(clip)
    if not frames:
        return out
    vecs, valid, close = {}, {}, {}
    for f in range(n_frames):
        vecs[f], valid[f], close[f] = frame_vec(clip.get(f, []), width, height)
    inwin = set()
    near = set()
    if is_fight:
        for s, e in gt:
            e = s if e is None else e
            inwin.update(range(s, e + 1))
            near.update(range(s - MARGIN, e + MARGIN + 1))
    for w0 in range(0, n_frames - T + 1, STRIDE):
        idx = range(w0, w0 + T)
        if sum(valid[f] for f in idx) < 5 or sum(1 for f in idx if close[f] >= 0.2) < 3:
            continue                                  # 게이트 미통과: 2차 AI 를 부르지 않는 구간
        frac = sum(1 for f in idx if f in inwin) / T
        if is_fight:
            if frac >= 0.5:
                label = 1
            elif not any(f in near for f in idx):
                label = 0
            else:
                continue                              # 경계 모호
        else:
            label = 0
        out.append((np.stack([vecs[f] for f in idx]), label, w0, max(close[f] for f in idx)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--neg-per-class", type=int, default=80)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--fps", type=float, default=3.0)
    a = ap.parse_args()
    rows = [r for r in csv.DictReader(open(a.index, encoding="utf-8-sig")) if r["split"] in ("dev", "pool")]
    fights = [r for r in rows if r["class_code"] == "13"]
    rng = random.Random(0)
    negs = []
    for c in sorted({r["class_code"] for r in rows} - {"13"}):
        by_scene = defaultdict(list)
        for r in rows:
            if r["class_code"] == c:
                by_scene[r["scene_key"]].append(r)
        scenes = sorted(by_scene)
        rng.shuffle(scenes)
        negs += [sorted(by_scene[s], key=lambda r: r["clip"])[0] for s in scenes[:a.neg_per_class]]
    todo = [(r, True) for r in fights] + [(r, False) for r in negs]
    out = Path(a.work)
    out.mkdir(parents=True, exist_ok=True)
    Path(a.cache_dir).mkdir(parents=True, exist_ok=True)
    print(f"[fight2] 폭행 {len(fights)}클립 + 음성 {len(negs)}클립", flush=True)
    X, Y, meta = [], [], []
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(base.run_exe, a, r, out): (r, isf) for r, isf in todo}
        for i, f in enumerate(as_completed(futs), 1):
            r, isf = futs[f]
            try:
                _, det, kp = f.result()
                clip = load_clip(det, kp)
                gt = json.loads(r["events"]) if isf else []
                for x, y, w0, close in windows_of(clip, int(r["frames"]), int(r["width"]), int(r["height"]), gt, isf):
                    X.append(x); Y.append(y)
                    meta.append((r["clip"], r["store"], r["scene_key"], r["split"], r["class_code"], w0, close))
            except Exception as e:                    # 한 클립 실패가 전체를 멈추지 않게 기록만 하고 넘어간다
                print("WARN", r["clip"], e, file=sys.stderr, flush=True)
            if i % 50 == 0 or i == len(futs):
                print(f"  {i}/{len(futs)} windows={len(X)}", flush=True)
    m = np.array(meta, dtype=object)
    np.savez_compressed(out / "windows.npz", X=np.stack(X).astype(np.float32), y=np.array(Y, np.int8),
                        clip=m[:, 0].astype(str), store=m[:, 1].astype(str), scene=m[:, 2].astype(str),
                        split=m[:, 3].astype(str), cls=m[:, 4].astype(str), w0=m[:, 5].astype(int),
                        close=m[:, 6].astype(np.float32))
    ys = np.array(Y)
    print(f"[fight2] 창 {len(Y)}개 (양성 {int(ys.sum())}, 음성 {int((ys == 0).sum())}) → {out / 'windows.npz'}")


if __name__ == "__main__":
    main()
