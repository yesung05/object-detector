"""
이상행동 데이터로 룰 이벤트를 평가 (개발 PC 전용)

클립마다 unmanned_detector.exe 를 영상 시간 모드(--video-clock)로 돌리고, 이벤트 DB 의 룰 이벤트를
라벨의 정답 구간과 대조한다. 모델 추론은 --cache-dir 를 주면 클립당 한 번만 하고(첫 실행이 기록),
이후 설정을 바꾼 재실행은 같은 검출을 재생한다(AI 호출 최소화 원칙).

이벤트의 영상 시간: 이벤트 로그의 ts_unix 는 벽시계라 프레임과 연결되지 않는다. 대신 replay_events 테이블이
이벤트마다 mono(= process_frame 의 now)를 기록하고, --video-clock 에서는 그것이 영상 시간(1000 + 프레임/fps)이다.
events.id 로 조인해 프레임 번호 = round((mono - 1000) * fps) 로 복원한다. 이 오프셋(1000)은
platform.c 의 PLATFORM_VIDEO_CLOCK_EPOCH 와 같아야 한다.

평가 대상 이벤트(--event, 기본 person_fallen):
  - 정답 구간 [start - pre, end + post] 안에 이벤트가 있으면 해당 클립은 검출(TP), 지연 = 첫 이벤트 - start.
  - 구간 밖 이벤트는 오탐. 정답 구간이 없는 클래스의 클립(--negative-classes)에서는 모든 이벤트가 오탐이다.

한계(문서에도 적는다): 영상은 3fps 연출 촬영이라 현장(약 15fps)의 트래커 동작을 그대로 대표하지 않는다.
"""

import argparse
import csv
import json
import os
import sqlite3
import statistics
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

EPOCH = 1000.0  # src/platform.c 의 PLATFORM_VIDEO_CLOCK_EPOCH


def load_index(path, split, classes, limit, per_scene):
    rows = [r for r in csv.DictReader(open(path, encoding="utf-8-sig"))
            if r["split"] == split and r["class_code"] in classes]
    rows.sort(key=lambda r: (r["class_code"], r["scene_key"], r["clip"]))
    if per_scene > 0:
        seen, kept = {}, []
        for r in rows:
            k = (r["class_code"], r["scene_key"])
            if seen.get(k, 0) < per_scene:
                seen[k] = seen.get(k, 0) + 1
                kept.append(r)
        rows = kept
    if limit > 0:
        # 앞에서 자르면 한 상점·날짜에 쏠리므로 클래스별로 균등 간격으로 뽑는다.
        out = []
        for c in sorted({r["class_code"] for r in rows}):
            rr = [r for r in rows if r["class_code"] == c]
            step = max(1, len(rr) // limit)
            out += rr[::step][:limit]
        rows = out
    return rows


def run_clip(a, row, label_dir, cache_dir):
    clip = row["clip"]
    db = label_dir / f"{clip}.db"
    log = label_dir / f"{clip}.log"
    if a.resume and db.exists() and db.stat().st_size > 0:
        return row, db, 0.0, "resumed"
    for f in (db, Path(str(db) + "-wal"), Path(str(db) + "-shm")):
        if f.exists():
            f.unlink()
    cmd = [a.exe, "--model", a.model, "--provider", "cpu", "--threads", str(a.threads),
           "--input", row["video"], "--video-clock", "--motion-gate", "0", "--detect-fps", "0",
           "--detect-every", "1", "--event-log", str(db)]
    if a.config:
        cmd += ["--config", a.config]
    if cache_dir:
        cmd += ["--infer-cache", str(cache_dir / f"{clip}.bin")]
    t0 = time.time()
    with open(log, "wb") as lf:
        try:
            r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, timeout=a.timeout)
            status = "ok" if r.returncode == 0 else f"exit={r.returncode}"
        except subprocess.TimeoutExpired:
            status = "timeout"
    return row, db, time.time() - t0, status


def read_events(db, fps):
    """룰 이벤트를 (프레임, 레벨, 토큰, 메시지) 로 돌려준다. 영상 시간이 없는 이벤트는 프레임 None."""
    con = sqlite3.connect(f"file:{str(db).replace(chr(92), '/')}?mode=ro", uri=True)
    try:
        q = ("SELECT e.id, e.level, e.module, e.message, r.mono FROM events e "
             "LEFT JOIN replay_events r ON r.event_id = e.id WHERE e.module = 'rules' ORDER BY e.id")
        out = []
        for _id, level, module, msg, mono in con.execute(q):
            frame = None if mono is None else int(round((mono - EPOCH) * fps))
            out.append({"frame": frame, "level": level, "token": msg.split(" ")[0], "message": msg})
        return out
    finally:
        con.close()


def evaluate(a, results):
    pre, post = int(round(a.pre_seconds * a.fps)), int(round(a.post_seconds * a.fps))
    per_class = {}
    clip_rows = []
    for row, db, secs, status in results:
        cls = row["class_code"]
        rec = {"clip": row["clip"], "class": cls, "status": status, "seconds": round(secs, 1),
               "scene_key": row["scene_key"], "store": row["store"], "frames": int(row["frames"])}
        if status not in ("ok", "resumed") or not Path(db).exists():
            clip_rows.append(rec)
            continue
        evs = read_events(db, a.fps)
        target = [e for e in evs if e["token"] == a.event]
        gt = json.loads(row["events"])
        windows = [[s - pre, (e if e is not None else s) + post] for s, e in gt]
        is_positive = cls in a.positive_classes
        hits, false_alarms, unresolved = [], [], 0
        for e in target:
            if e["frame"] is None:
                unresolved += 1
                continue
            inside = is_positive and any(w0 <= e["frame"] <= w1 for w0, w1 in windows)
            (hits if inside else false_alarms).append(e["frame"])
        gt_len = sum((e - s + 1) for s, e in gt if e is not None) if is_positive else 0
        rec.update({"positive": is_positive, "gt": gt, "detected": bool(hits) if is_positive else None,
                    "first_hit_frame": min(hits) if hits else None,
                    "latency_s": (round((min(hits) - gt[0][0]) / a.fps, 2) if hits and gt else None),
                    "false_alarms": len(false_alarms), "unresolved_time": unresolved,
                    "normal_seconds": round((int(row["frames"]) - gt_len) / a.fps, 1),
                    "all_rule_events": {t: sum(1 for e in evs if e["token"] == t) for t in sorted({e["token"] for e in evs})}})
        clip_rows.append(rec)
        pc = per_class.setdefault(cls, {"clips": 0, "ok": 0, "tp": 0, "fp_events": 0, "normal_s": 0.0,
                                        "lat": [], "other_events": {}})
        pc["clips"] += 1
        pc["ok"] += 1
        pc["fp_events"] += len(false_alarms)
        pc["normal_s"] += rec["normal_seconds"]
        if is_positive and hits:
            pc["tp"] += 1
            pc["lat"].append(rec["latency_s"])
        for t, n in rec["all_rule_events"].items():
            pc["other_events"][t] = pc["other_events"].get(t, 0) + n
    summary = {}
    for cls, pc in per_class.items():
        lat = sorted(x for x in pc["lat"] if x is not None)
        pos = cls in a.positive_classes
        summary[cls] = {
            "clips_evaluated": pc["ok"],
            "recall": (round(pc["tp"] / pc["ok"], 4) if pos and pc["ok"] else None),
            "tp": pc["tp"] if pos else None,
            "latency_s": ({"p50": statistics.median(lat), "p90": lat[int(len(lat) * 0.9) - 1] if len(lat) > 1 else lat[0],
                           "min": lat[0], "max": lat[-1]} if lat else None),
            "false_alarm_events": pc["fp_events"],
            "false_alarms_per_clip": round(pc["fp_events"] / pc["ok"], 3) if pc["ok"] else None,
            "false_alarms_per_hour_of_normal": (round(pc["fp_events"] / (pc["normal_s"] / 3600.0), 2) if pc["normal_s"] else None),
            "rule_events_total": pc["other_events"]}
    failed = [r for r in clip_rows if r["status"] not in ("ok", "resumed")]
    return {"summary": summary, "failed_clips": failed, "clips": clip_rows}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--work", required=True, help="결과 폴더(라벨마다 하위 폴더)")
    ap.add_argument("--label", required=True, help="이 평가 설정의 이름(예: baseline)")
    ap.add_argument("--split", default="dev", choices=["dev", "test", "pool"])
    ap.add_argument("--classes", default="07", help="쉼표 구분 분류 코드(07=전도, 14=교통약자 ...)")
    ap.add_argument("--positive-classes", default="07", help="--event 가 정답 구간에 있어야 하는 분류")
    ap.add_argument("--event", default="person_fallen")
    ap.add_argument("--limit", type=int, default=0, help="분류별 최대 클립 수(균등 간격 추출), 0=전부")
    ap.add_argument("--per-scene", type=int, default=0, help="장면(카메라 6대 동시 촬영)당 최대 클립 수, 0=전부")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--threads", type=int, default=2, help="프로세스당 ORT 스레드(jobs×threads ≤ 물리 스레드)")
    ap.add_argument("--fps", type=float, default=3.0)
    ap.add_argument("--pre-seconds", type=float, default=1.0, help="정답 시작 전 허용(초)")
    ap.add_argument("--post-seconds", type=float, default=3.0, help="정답 종료 후 허용(초)")
    ap.add_argument("--config", default="", help="--config 로 넘길 룰 설정 파일(key=value)")
    ap.add_argument("--cache-dir", default="", help="추론 캐시 폴더(없으면 매번 추론)")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--resume", action="store_true")
    a = ap.parse_args()
    a.positive_classes = set(a.positive_classes.split(","))
    classes = set(a.classes.split(","))
    rows = load_index(a.index, a.split, classes, a.limit, a.per_scene)
    work = Path(a.work) / a.label
    work.mkdir(parents=True, exist_ok=True)
    cache_dir = Path(a.cache_dir) if a.cache_dir else None
    if cache_dir:
        cache_dir.mkdir(parents=True, exist_ok=True)
    print(f"[eval] {a.label}: {len(rows)}클립 {sorted(classes)} split={a.split} jobs={a.jobs}", flush=True)
    results = []
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = [ex.submit(run_clip, a, r, work, cache_dir) for r in rows]
        for i, f in enumerate(as_completed(futs), 1):
            res = f.result()
            results.append(res)
            if i % 10 == 0 or i == len(futs):
                print(f"  {i}/{len(futs)} {time.time() - t0:.0f}s", flush=True)
    results.sort(key=lambda x: x[0]["clip"])
    ev = evaluate(a, results)
    ev["args"] = {k: (sorted(v) if isinstance(v, set) else v) for k, v in vars(a).items()}
    ev["wall_seconds"] = round(time.time() - t0, 1)
    out = work / "result.json"
    out.write_text(json.dumps(ev, ensure_ascii=False, indent=2), encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps({"summary": ev["summary"], "failed": len(ev["failed_clips"]), "wall_seconds": ev["wall_seconds"]},
                     ensure_ascii=False, indent=2))
    print("saved", out)


if __name__ == "__main__":
    main()
