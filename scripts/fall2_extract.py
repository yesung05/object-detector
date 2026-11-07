"""
쓰러짐 2차 AI용 키포인트 창 데이터 만들기 (개발 PC 전용). 사전 등록: docs/prereg_fall_stage2.md

폭행 2차(fight2_extract.py)의 실행·CSV 로더를 그대로 쓰고, 특징·게이트·라벨만 쓰러짐용으로 바꾼다.
  - 프레임마다 "가장 가로로 누운 사람" 한 명의 박스 기준 정규화 관절(51) + 몸 모양(4) + 유효(1) = 56차원
  - 게이트: 9프레임 창에서 사람 있는 프레임 ≥ 5 이고 (가로/세로 ≥ 1.0 프레임 ≥ 3 또는 창 안 최소 키 ≤ 최대 키 × 0.6)
  - 라벨: 전도(07) 정답 구간과 50% 이상 겹치면 양성, 다른 클래스이거나 구간 앞뒤 2초 밖이면 음성, 그 사이는 제외
dev+pool 만 쓴다(전도 test 는 룰 평가에 이미 소진). 정답 관절이 아니라 배포 모델이 낸 검출 관절만 쓴다.
영상 화소는 이 스크립트가 읽지 않는다(실행 파일이 낸 CSV 만).

출력: <work>/windows.npz  (X[N,9,56], y, clip, store, scene, split, cls, w0, close=창 안 최대 가로/세로)
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

_here = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("fight2", _here / "fight2_extract.py")
fight2 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fight2)          # load_clip, base.run_exe 재사용(-I 모드라 경로로 읽는다)

T, STRIDE, FPS = 9, 3, 3.0
MARGIN = int(2.0 * FPS)
D = 56
POSITIVE = "07"


def frame_vec(people, w, h):
    """(벡터[56], valid, aspect, height) — 가장 가로로 누운 사람 기준. 키(height)는 창 단위 정규화를 위해 따로 돌려준다."""
    v = np.zeros(D, np.float32)
    v[54] = min(len(people), 4) / 4.0
    if not people:
        return v, 0, 0.0, 0.0
    box, kp = max(people, key=lambda p: (p[0][2] - p[0][0]) / max(p[0][3] - p[0][1], 1.0))
    bw, bh = max(box[2] - box[0], 1.0), max(box[3] - box[1], 1.0)
    for k in range(17):
        v[k * 3] = np.clip((kp[k, 0] - box[0]) / bw, -0.5, 1.5)
        v[k * 3 + 1] = np.clip((kp[k, 1] - box[1]) / bh, -0.5, 1.5)
        v[k * 3 + 2] = kp[k, 2]
    aspect = bw / bh
    v[51] = np.clip(np.log(aspect), -2.0, 2.0) / 2.0
    v[53] = ((box[1] + box[3]) / 2) / h
    v[55] = 1.0
    return v, 1, aspect, bh   # v[52](창 안 최대 키 대비 키)는 windows_of 에서 채운다


def windows_of(clip, n_frames, width, height, gt, is_pos):
    out = []
    if not clip:
        return out
    vecs, valid, aspect, hgt = {}, {}, {}, {}
    for f in range(n_frames):
        vecs[f], valid[f], aspect[f], hgt[f] = frame_vec(clip.get(f, []), width, height)
    inwin, near = set(), set()
    if is_pos:
        for s, e in gt:
            e = s if e is None else e
            inwin.update(range(s, e + 1))
            near.update(range(s - MARGIN, e + MARGIN + 1))
    for w0 in range(0, n_frames - T + 1, STRIDE):
        idx = list(range(w0, w0 + T))
        hs = [hgt[f] for f in idx if valid[f]]
        if len(hs) < 5:
            continue
        hmax = max(hs)
        wide = sum(1 for f in idx if valid[f] and aspect[f] >= 1.0)
        if wide < 3 and min(hs) > 0.6 * hmax:
            continue                                  # 게이트 미통과: 2차 AI 를 부르지 않는 구간
        frac = sum(1 for f in idx if f in inwin) / T
        if is_pos:
            if frac >= 0.5:
                label = 1
            elif not any(f in near for f in idx):
                label = 0
            else:
                continue                              # 경계 모호
        else:
            label = 0
        X = np.stack([vecs[f] for f in idx]).copy()
        for t, f in enumerate(idx):
            X[t, 52] = hgt[f] / hmax if valid[f] else 0.0
        out.append((X, label, w0, max(aspect[f] for f in idx)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--neg-per-class", type=int, default=80)
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--fps", type=float, default=3.0)
    a = ap.parse_args()
    rows = [r for r in csv.DictReader(open(a.index, encoding="utf-8-sig")) if r["split"] in ("dev", "pool")]
    pos = [r for r in rows if r["class_code"] == POSITIVE]
    rng = random.Random(0)
    negs = []
    for c in sorted({r["class_code"] for r in rows} - {POSITIVE}):
        by_scene = defaultdict(list)
        for r in rows:
            if r["class_code"] == c:
                by_scene[r["scene_key"]].append(r)
        scenes = sorted(by_scene)
        rng.shuffle(scenes)
        negs += [sorted(by_scene[s], key=lambda r: r["clip"])[0] for s in scenes[:a.neg_per_class]]
    todo = [(r, True) for r in pos] + [(r, False) for r in negs]
    out = Path(a.work)
    out.mkdir(parents=True, exist_ok=True)
    Path(a.cache_dir).mkdir(parents=True, exist_ok=True)
    print(f"[fall2] 전도 {len(pos)}클립 + 음성 {len(negs)}클립", flush=True)
    X, Y, meta = [], [], []
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(fight2.base.run_exe, a, r, out): (r, ip) for r, ip in todo}
        for i, f in enumerate(as_completed(futs), 1):
            r, ip = futs[f]
            try:
                _, det, kp = f.result()
                clip = fight2.load_clip(det, kp)
                gt = json.loads(r["events"]) if ip else []
                for x, y, w0, asp in windows_of(clip, int(r["frames"]), int(r["width"]), int(r["height"]), gt, ip):
                    X.append(x); Y.append(y)
                    meta.append((r["clip"], r["store"], r["scene_key"], r["split"], r["class_code"], w0, asp))
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
    print(f"[fall2] 창 {len(Y)}개 (양성 {int(ys.sum())}, 음성 {int((ys == 0).sum())}) → {out / 'windows.npz'}")


if __name__ == "__main__":
    main()
