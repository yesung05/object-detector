"""
알림 품질 자동 지표 (읽기 전용) — 사람 검토 없이 "켜도 되는 알림인가"를 가리는 1차 근거

현장에서 사람이 알림을 일일이 검토하기 어렵다는 전제로, 로그만으로 계산할 수 있는 신호를 모은다.
  - 알림 종류별 건수와 시간당 건수, 그림자 모드(mode=shadow) 건수, 같은 트랙의 반복 비율(재알림)
  - 무인 시간대 발화: 사람이 있어야 성립하는 알림(쓰러짐·체류·음료 반입 등)이 활성 사람이 0명일 때 발화하면
    오탐이 확정이다. 이벤트 시각은 replay_events.mono(룰이 본 시계)로 리플레이 좌표 스냅샷과 연결한다
  - 날짜별 WARN/ERROR 합계: 점주가 감당할 알림량(권고 상한 10건/일)과 비교

한계: 사람이 있는 오탐(정상 행동을 이상으로 판단)은 잡지 못한다 — 그건 sample_events_for_review.py 의 표본 검토와
연출 시험의 몫이다. replay_events 에 기록되지 않는 이벤트는 시각 연결이 안 되어 "연결 불가"로 센다.

사용: python -I scripts/analyze_alert_quality.py <로그 폴더> [--out report.md]
"""

import argparse
import glob
import json
import os
import re
import sqlite3
import sys
from collections import Counter, defaultdict
from datetime import datetime

# 사람이 있어야 성립하는 알림. 이 알림이 활성 사람 0명일 때 나오면 오탐이다.
# 잔류물·문 개방·카메라는 사람이 없을 때 나오는 것이 정상이라 제외한다.
NEEDS_PERSON = {
    "person_fallen", "overstay", "unordered_seated", "external_drink", "external_food",
    "no_cup_seated", "animal_on_chair", "animal_on_table", "person_motionless", "occupancy_exceeded",
}
ALERT_MODULES = ("rules", "door", "slot_monitor", "residue", "surface")
SHADOW_SUFFIX = " mode=shadow"


def ro(path):
    return sqlite3.connect("file:" + path.replace(chr(92), "/") + "?mode=ro", uri=True)


def token_of(msg):
    return re.split(r"[ :=]", msg, maxsplit=1)[0]


def event_dbs(folder):
    return sorted(f for f in glob.glob(os.path.join(folder, "*.db")) if not f.endswith("_perf.db"))


def active_people_near(con, session, mono, window):
    """mono 주변 window 초의 스냅샷 중 활성(misses==0) 사람이 가장 많았던 값. 스냅샷이 없으면 None."""
    rows = con.execute(
        "SELECT payload FROM replay_frames WHERE session=? AND mono BETWEEN ? AND ? ORDER BY abs(mono-?) LIMIT 4",
        (session, mono - window, mono + window, mono)).fetchall()
    if not rows:
        return None
    best = 0
    for (payload,) in rows:
        try:
            people = json.loads(payload).get("people", [])
        except ValueError:
            continue
        best = max(best, sum(1 for p in people if p.get("misses", 1) == 0))
    return best


def collect(folder, window):
    ev = []
    for db in event_dbs(folder):
        try:
            con = ro(db)
            tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            if "events" not in tables:
                continue
            has_replay = "replay_events" in tables and "replay_frames" in tables
            marks = ",".join("?" * len(ALERT_MODULES))
            q = ("SELECT e.id, e.ts_iso, e.level, e.module, e.message" +
                 (", r.session, r.mono" if has_replay else "") +
                 " FROM events e" + (" LEFT JOIN replay_events r ON r.event_id=e.id" if has_replay else "") +
                 " WHERE e.level IN ('WARN','ERROR') AND e.module IN (" + marks + ")")
            for row in con.execute(q, ALERT_MODULES):
                eid, ts, level, module, msg = row[:5]
                session, mono = (row[5], row[6]) if has_replay else (None, None)
                tok = token_of(msg)
                people = None
                if tok in NEEDS_PERSON and session is not None and mono is not None:
                    people = active_people_near(con, session, mono, window)
                tm = re.search(r"track=(\d+)", msg)
                ev.append({"db": os.path.basename(db), "id": eid, "ts": ts, "level": level, "module": module,
                           "msg": msg, "token": tok, "shadow": msg.endswith(SHADOW_SUFFIX),
                           "track": tm.group(1) if tm else None, "people": people,
                           "linked": session is not None})
            con.close()
        except sqlite3.DatabaseError as e:
            print(f"WARN {os.path.basename(db)} 읽기 실패: {e}", file=sys.stderr)
    return ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--out", default="")
    ap.add_argument("--window", type=float, default=1.5, help="이벤트 시각 주변 스냅샷 탐색 폭(초)")
    a = ap.parse_args()
    ev = collect(a.folder, a.window)
    if not ev:
        sys.exit("알림 이벤트가 없습니다.")
    times = sorted(datetime.fromisoformat(e["ts"]) for e in ev)
    # 관측 시간은 첫~마지막 알림이 아니라 로그 전체 기간이어야 시간당 건수가 맞다. 이벤트 DB 의 첫·끝 시각을 쓴다.
    first = last = None
    for db in event_dbs(a.folder):
        try:
            con = ro(db)
            r = con.execute("SELECT min(ts_iso), max(ts_iso) FROM events").fetchone()
            con.close()
            if r and r[0]:
                f, l = datetime.fromisoformat(r[0]), datetime.fromisoformat(r[1])
                first = f if first is None or f < first else first
                last = l if last is None or l > last else last
        except sqlite3.DatabaseError:
            pass
    hours = max((last - first).total_seconds() / 3600.0, 1e-6) if first else 1.0
    out = []
    p = out.append
    p("# 알림 품질 자동 지표")
    p(f"\n- 로그: `{a.folder}` / 관측 {first} ~ {last} ({hours:.1f}시간) / 알림 이벤트(WARN·ERROR) {len(ev)}건")
    linked = sum(1 for e in ev if e["linked"])
    p(f"- 리플레이와 시각 연결 가능: {linked}/{len(ev)}건 (연결 불가는 무인 시간대 판정에서 제외)\n")
    p("## 알림 종류별\n")
    p("| 알림 | 건수 | 시간당 | 그림자 | 고유 트랙 | 트랙당 반복 | 무인 시간대 발화 | 판정 가능 |")
    p("|---|---|---|---|---|---|---|---|")
    by = defaultdict(list)
    for e in ev:
        by[e["token"]].append(e)
    for tok, es in sorted(by.items(), key=lambda kv: -len(kv[1])):
        tracks = {e["track"] for e in es if e["track"]}
        repeat = f"{len(es) / len(tracks):.1f}" if tracks else "-"
        if tok in NEEDS_PERSON:
            judged = [e for e in es if e["people"] is not None]
            un = sum(1 for e in judged if e["people"] == 0)
            unatt = f"{un} ({un / len(judged):.0%})" if judged else "-"
            jtxt = f"{len(judged)}/{len(es)}"
        else:
            unatt, jtxt = "해당 없음", "-"
        p(f"| {tok} | {len(es)} | {len(es) / hours:.2f} | {sum(1 for e in es if e['shadow'])} | "
          f"{len(tracks) if tracks else '-'} | {repeat} | {unatt} | {jtxt} |")
    p("\n## 날짜별 알림 합계 (그림자 제외 = 점주가 실제로 보는 양)\n")
    p("| 날짜 | 점주에게 보이는 WARN/ERROR | 그림자 | 합계 |")
    p("|---|---|---|---|")
    day = defaultdict(lambda: [0, 0])
    for e in ev:
        day[e["ts"][:10]][1 if e["shadow"] else 0] += 1
    for d in sorted(day):
        v, s = day[d]
        p(f"| {d} | {v} | {s} | {v + s} |")
    visible = [v for v, _ in day.values()]
    if visible:
        p(f"\n점주에게 보이는 알림: 일 평균 {sum(visible) / len(visible):.1f}건, 최대 {max(visible)}건 (권고 상한 10건/일)")
    p("\n## 시간대 분포 (점주에게 보이는 알림)\n")
    hist = Counter(int(e["ts"][11:13]) for e in ev if not e["shadow"])
    p("| 시 | " + " | ".join(f"{h:02d}" for h in range(24)) + " |")
    p("|---|" + "---|" * 24)
    p("| 건수 | " + " | ".join(str(hist.get(h, 0)) for h in range(24)) + " |")
    text = "\n".join(out)
    if a.out:
        open(a.out, "w", encoding="utf-8").write(text)
    sys.stdout.reconfigure(encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
