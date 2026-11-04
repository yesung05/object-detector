"""
전도 클립에서 쓰러짐 룰이 어느 조건에서 막히는지 프레임 단위로 집계 (개발 PC 전용, 진단)

eval_anomaly_dataset.py 는 "잡혔나/못 잡았나"만 알려 준다. 못 잡은 이유를 알아야 룰을 고칠 수 있어서,
unmanned_detector.exe 의 --fall-trace CSV(프레임·트랙별 게이트)를 정답 구간 안에서 집계한다.

게이트 해석(rules.c): 0 직립 / 1 신뢰도 부족 / 2 관측 끊김(misses) / 3 자세 판정 불가 또는 수평 아님 /
4 직립 이력 없음 / 5 수평 유지 누적 중 / 6 발화. 3 은 dbg_pose, dbg_horizontal 로 둘로 나눈다.

프레임 분류(정답 구간 [start, end] 의 프레임마다, 그 프레임에 있는 활성 트랙 중 가장 "멀리 간" 것):
  no_track        활성 트랙이 없음(검출 실패 또는 트랙 소실)
  stale           gate 2
  low_conf        gate 1 (박스·매칭 점수 < 0.5)
  pose_unknown    gate 3 이면서 몸통 자세 판정 불가(dbg_pose==0): 어깨·엉덩이 점수 <0.5, 박스 밖, 대각선
  upright         몸통이 직립으로 판정(gate 0): 아직 안 쓰러진 것으로 본다
  not_horizontal  몸통은 수평인데 박스 비율·관절 분산 조건을 못 넘음(dbg_horizontal==0)
  no_upright_hist 수평인데 직립 이력이 없음(gate 4) — 트랙이 새로 생겼거나 직립 판정이 3샘플·1초 미만
  holding         누적 중(gate 5)
  fired           발화(gate 6)
클립 단계 = 정답 구간 안에서 도달한 가장 높은 단계(위 순서). 발화가 아닌 클립의 "막힌 곳"이 룰 수정의 근거다.

주의: 이 진단은 dev 분할에서만 쓴다(test 는 최종 확인 전용이라 원인 분석에도 쓰지 않는다).
"""

import argparse
import csv
import json
import subprocess
import sys
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

STAGES = ["no_track", "stale", "low_conf", "pose_unknown", "upright", "not_horizontal",
          "no_upright_hist", "holding", "fired"]


def classify(row):
    gate = int(row["gate"])
    pose = int(row["pose"])
    horiz = int(row["horizontal"])
    if gate == 6 or int(row["latched"]) == 1:
        return "fired"
    if gate == 5:
        return "holding"
    if gate == 4:
        return "no_upright_hist"
    if gate == 2:
        return "stale"
    if gate == 1:
        return "low_conf"
    if gate == 3:
        return "pose_unknown" if pose == 0 else "not_horizontal"
    if gate == 0:
        return "upright"
    return "no_track"


def run(a, row, out_dir):
    clip = row["clip"]
    trace = out_dir / f"{clip}.csv"
    log = out_dir / f"{clip}.log"
    cmd = [a.exe, "--model", a.model, "--provider", "cpu", "--threads", str(a.threads),
           "--input", row["video"], "--video-clock", "--motion-gate", "0", "--detect-fps", "0",
           "--detect-every", "1", "--fall-trace", str(trace),
           "--event-log", str(out_dir / f"{clip}.db"),
           "--infer-cache", str(Path(a.cache_dir) / f"{clip}.bin")]
    if a.config:
        cmd += ["--config", a.config]
    with open(log, "wb") as lf:
        r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, timeout=900)
    return row, trace, r.returncode


def analyze(row, trace, fps, pre, post):
    gt = json.loads(row["events"])
    frames = {}
    if trace.exists():
        for t in csv.DictReader(open(trace, encoding="utf-8")):
            f = int(t["frame"])
            st = classify(t)
            if f not in frames or STAGES.index(st) > STAGES.index(frames[f]):
                frames[f] = st
    per_frame = Counter()
    best = "no_track"
    for s, e in gt:
        for f in range(s, (e if e is not None else s) + 1):
            st = frames.get(f, "no_track")
            per_frame[st] += 1
            if STAGES.index(st) > STAGES.index(best):
                best = st
    # 정답 구간 앞(직립 이력이 쌓이는 곳)의 사정도 같이 본다: 구간 시작 전 30초 안에서 'upright' 프레임이 있었는지
    s0 = gt[0][0] if gt else 0
    pre_upright = sum(1 for f in range(max(0, s0 - int(30 * fps)), s0) if frames.get(f) == "upright")
    return {"clip": row["clip"], "scene": row["scene_key"], "gt": gt, "best_stage": best,
            "window_frames": sum(per_frame.values()), "per_frame": dict(per_frame),
            "pre_upright_frames": pre_upright}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--split", default="dev")
    ap.add_argument("--class-code", default="07")
    ap.add_argument("--per-scene", type=int, default=1)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--fps", type=float, default=3.0)
    ap.add_argument("--config", default="")
    a = ap.parse_args()
    if a.split == "test":
        sys.exit("test 분할은 진단에 쓰지 않습니다(최종 확인 전용).")
    rows = [r for r in csv.DictReader(open(a.index, encoding="utf-8-sig"))
            if r["split"] == a.split and r["class_code"] == a.class_code]
    rows.sort(key=lambda r: (r["scene_key"], r["clip"]))
    seen, picked = Counter(), []
    for r in rows:
        if seen[r["scene_key"]] < a.per_scene:
            seen[r["scene_key"]] += 1
            picked.append(r)
    if a.limit:
        step = max(1, len(picked) // a.limit)
        picked = picked[::step][:a.limit]
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    Path(a.cache_dir).mkdir(parents=True, exist_ok=True)
    print(f"[diag] {len(picked)}클립 (split={a.split}, class={a.class_code}, per-scene={a.per_scene})", flush=True)
    t0 = time.time()
    results = []
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = [ex.submit(run, a, r, out) for r in picked]
        for i, f in enumerate(as_completed(futs), 1):
            row, trace, rc = f.result()
            if rc != 0:
                print(f"  WARN {row['clip']} exit={rc}", flush=True)
            results.append(analyze(row, trace, a.fps, 0, 0))
            if i % 10 == 0 or i == len(futs):
                print(f"  {i}/{len(futs)} {time.time() - t0:.0f}s", flush=True)
    results.sort(key=lambda r: r["clip"])
    clip_stage = Counter(r["best_stage"] for r in results)
    frame_tot = Counter()
    for r in results:
        frame_tot.update(r["per_frame"])
    summary = {"clips": len(results), "clip_best_stage": {s: clip_stage.get(s, 0) for s in STAGES},
               "window_frame_totals": {s: frame_tot.get(s, 0) for s in STAGES},
               "clips_with_pre_upright": sum(1 for r in results if r["pre_upright_frames"] >= 3)}
    (out / "summary.json").write_text(json.dumps({"summary": summary, "clips": results},
                                                 ensure_ascii=False, indent=2), encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
