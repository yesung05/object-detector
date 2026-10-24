"""현장 로그 폴더(events/perf/part DB, launcher 로그)를 읽기 전용으로 집계해 마크다운 리포트를 만든다.

사용: python -I scripts/analyze_field_logs.py <로그폴더> [출력.md]

표준 라이브러리만 사용 — 현장 로그 일회성 분석이라 의존성을 늘릴 이유가 없다.
DB는 mode=ro URI로만 열어 운영 중인 -wal 파일을 건드리지 않는다.
"""
import glob
import os
import re
import sqlite3
import statistics as st
import sys
from collections import Counter, defaultdict
from datetime import datetime

folder = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(folder, "..", "20261006_logs_report.md")
out = []


def p(s=""):
    out.append(s)


def ro(path):
    # URI 경로는 역슬래시가 깨지므로 슬래시로 통일
    return sqlite3.connect("file:" + path.replace("\\", "/") + "?mode=ro", uri=True)


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * q))] if xs else float("nan")


def parts(perf):
    fs = sorted(glob.glob(os.path.join(folder, "*.db")))
    return [f for f in fs if f.endswith("_perf.db") == perf]


# ---------- events 수집 ----------
events = []  # (ts, level, module, message)
for f in parts(False):
    c = ro(f)
    events += [(datetime.fromisoformat(a), b, m, msg) for a, b, m, msg in
               c.execute("select ts_iso, level, module, message from events order by ts_unix")]
    c.close()
events.sort(key=lambda e: e[0])
p(f"# 현장 로그 분석 리포트\n\n- 기간: {events[0][0]} ~ {events[-1][0]}, events {len(events)}건\n")

# ---------- B. 룰 알림 ----------
rules = [e for e in events if e[2] == "rules"]
kind = lambda msg: msg.split(" ")[0]
p("## B. 룰 알림 (시간당 건수 / 재알림)\n")
hours = max(1, (events[-1][0] - events[0][0]).total_seconds() / 3600)
p("| 룰 | 건수 | 시간당 |\n|---|---|---|")
for k, v in Counter(kind(e[3]) for e in rules).most_common():
    p(f"| {k} | {v} | {v / hours:.1f} |")

# (rule, track) 반복 횟수·간격, dwell 정체 여부
by_track = defaultdict(list)
for ts, lv, m, msg in rules:
    t = re.search(r"track=(\d+)", msg)
    d = re.search(r"dwell=(\d+)s", msg)
    if t and lv == "WARN":
        by_track[(kind(msg), int(t.group(1)))].append((ts, int(d.group(1)) if d else None))
p("\n(rule, track)별 재알림: 상위 10 (횟수 / 중앙 간격 / 첫·끝 dwell)\n")
p("| rule | track | 횟수 | 중앙 간격(초) | dwell 첫→끝 |\n|---|---|---|---|---|")
rep = sorted(by_track.items(), key=lambda kv: -len(kv[1]))[:10]
for (r, t), v in rep:
    gaps = [(b[0] - a[0]).total_seconds() for a, b in zip(v, v[1:])]
    p(f"| {r} | {t} | {len(v)} | {st.median(gaps) if gaps else 0:.0f} | {v[0][1]}→{v[-1][1]} |")
total_alerts = sum(len(v) for v in by_track.values())
dups = sum(len(v) - 1 for v in by_track.values())
p(f"\ntrack 기반 WARN {total_alerts}건 중 같은 (rule, track)의 재알림 {dups}건 ({dups / max(1, total_alerts):.0%}). "
  f"고유 (rule, track) {len(by_track)}개.")
p("\n※ 룰 이벤트 메시지에 track이 없는 것(external_drink, no_cup_seated)은 위 집계에서 빠지므로 아래 C에서 따로 본다.")

# ---------- C. 시간대 분포 ----------
p("\n## C. 룰 발생 시각 분포 (시간대별 건수)\n")
p("| 시 | " + " | ".join(sorted({kind(e[3]) for e in rules if e[1] == "WARN"})) + " |")
ks = sorted({kind(e[3]) for e in rules if e[1] == "WARN"})
p("|---|" + "---|" * len(ks))
hist = defaultdict(Counter)
for ts, lv, m, msg in rules:
    if lv == "WARN":
        hist[ts.hour][kind(msg)] += 1
for h in range(24):
    p(f"| {h:02d} | " + " | ".join(str(hist[h][k]) for k in ks) + " |")
ed = [re.search(r"cups=(\d+)", e[3]) for e in rules if kind(e[3]) == "external_drink"]
ec = Counter(m.group(1) for m in ed if m)
p(f"\nexternal_drink의 cups 값 분포: {dict(ec)} (cups=0 비율 {ec['0'] / max(1, sum(ec.values())):.0%})")

# ---------- D. perf ----------
rows = []
for f in parts(True):
    c = ro(f)
    rows += list(c.execute("select ts_iso,cpu_pct,cpu_temp,cam_fps,inf_fps,mem_kb,gray_ms,stream_ms,draw_ms,inf_runs,obj_runs from perf"))
    c.close()
rows.sort()
p(f"\n## D. perf (1분 샘플 {len(rows)}개)\n")
p("| 지표 | p50 | p95 | max |\n|---|---|---|---|")
for i, name in [(1, "cpu_pct"), (3, "cam_fps"), (4, "inf_fps"), (6, "gray_ms"), (7, "stream_ms"), (8, "draw_ms")]:
    xs = [r[i] for r in rows if r[i] is not None]
    p(f"| {name} | {pct(xs, .5):.1f} | {pct(xs, .95):.1f} | {max(xs):.1f} |")
p(f"\ncpu_pct>100 샘플: {sum(1 for r in rows if (r[1] or 0) > 100)}개 / cpu_temp==-1: {sum(1 for r in rows if r[2] == -1)}개")
p("\n시간대별 cpu_pct p50 / inf_fps p50\n")
p("| 시 | cpu | inf_fps |\n|---|---|---|")
bh = defaultdict(list)
for r in rows:
    bh[datetime.fromisoformat(r[0]).hour].append(r)
for h in sorted(bh):
    p(f"| {h:02d} | {pct([r[1] for r in bh[h]], .5):.0f} | {pct([r[4] for r in bh[h]], .5):.1f} |")

# ---------- E. throttle ----------
th = [e for e in events if e[2] == "throttle" and e[3].startswith("throttle=") and "=on" not in e[3]]
p(f"\n## E. throttle 전환 ({len(th)}회)\n")
lv = Counter(re.match(r"throttle=(\w+)", e[3]).group(1) for e in th)
p(f"상태별 진입: {dict(lv)}")
gaps = [(b[0] - a[0]).total_seconds() for a, b in zip(th, th[1:])]
p(f"전환 간격: p50 {pct(gaps, .5):.0f}s / p10 {pct(gaps, .1):.0f}s / 10초 이하 {sum(1 for g in gaps if g <= 10)}회")

# ---------- F. camera ----------
cam = [e for e in events if e[2] == "camera"]
p(f"\n## F. camera 상태 ({len(cam)}건)\n")
p("| 시각 | 상태 |\n|---|---|")
blk_h = Counter()
for ts, l, m, msg in cam:
    if "blackout" in msg:
        blk_h[ts.hour] += 1
    if "blackout" not in msg or l == "ERROR":
        p(f"| {ts} | {l} {msg} |")
p(f"\nblackout 시간대 분포: {dict(sorted(blk_h.items()))}")
# blackout→ok 지속 시간
dur, start = [], None
for ts, l, m, msg in cam:
    if "state=blackout" in msg:
        start = ts
    elif "state=ok" in msg and start:
        dur.append((ts - start).total_seconds())
        start = None
if dur:
    p(f"blackout 지속: p50 {pct(dur, .5):.0f}s / max {max(dur):.0f}s ({len(dur)}회)")

# ---------- G. 용량 ----------
p("\n## G. 로그 용량\n")
sizes = [(os.path.getsize(f), f) for f in glob.glob(os.path.join(folder, "*.db"))]
p(f"DB 합계 {sum(s for s, _ in sizes) / 1e6:.0f} MB, part DB {len([1 for _, f in sizes if 'part' in f and not f.endswith('_perf.db')])}개")
tab = Counter()
for f in parts(False)[1:]:
    c = ro(f)
    try:
        for name, sz in c.execute("select name, sum(pgsize) from dbstat group by name"):
            tab[name] += sz
    except sqlite3.DatabaseError:
        # dbstat 가상 테이블이 없는 빌드면 payload 길이 합으로 근사
        for t in ("replay_frames", "replay_geometry", "events"):
            col = "payload" if t != "events" else "message"
            tab[t] += c.execute(f"select coalesce(sum(length({col})),0) from {t}").fetchone()[0]
    c.close()
p("\n| 객체 | MB |\n|---|---|")
for k, v in tab.most_common(6):
    p(f"| {k} | {v / 1e6:.0f} |")
days = hours / 24
p(f"\n일평균 {sum(s for s, _ in sizes) / 1e6 / days:.0f} MB/일")

# ---------- H. 메모리 ----------
p("\n## H. mem_kb 추세 (일별 p50)\n")
p("| 일 | mem_kb |\n|---|---|")
bd = defaultdict(list)
for r in rows:
    bd[r[0][:10]].append(r[5])
for d in sorted(bd):
    p(f"| {d} | {pct(bd[d], .5):.0f} |")
xs = [r[5] for r in rows]
p(f"\n첫 1시간 평균 {st.mean(xs[:60]):.0f} → 마지막 1시간 평균 {st.mean(xs[-60:]):.0f} KB")

# ---------- 기타: startup WARN / 런처 ----------
p("\n## 기타: startup/WARN·ERROR (rules 제외)\n")
for ts, l, m, msg in events:
    if m != "rules" and l in ("WARN", "ERROR") and m not in ("camera", "throttle"):
        p(f"- {ts} {l} {m} {msg}")

text = "\n".join(out)
open(out_path, "w", encoding="utf-8").write(text)
sys.stdout.reconfigure(encoding="utf-8")
print(text)
