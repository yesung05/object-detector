"""
알림 종류별 무작위 표본 목록 (읽기 전용) — 사람이 검토해야 하는 양을 최소화하기 위한 도구

현장에서는 알림을 전수 검토할 수 없으므로, 종류별로 N건(기본 10건)만 뽑아 "정상 감지 / 오탐 의심 / 판단 불가"를
대시보드 리플레이 화면(/replay)에서 표시하게 한다. 시드를 고정해 같은 입력이면 같은 표본이 나온다.
  - 쓰러짐(person_fallen)처럼 드물고 중요한 알림은 --full 로 전수 포함한다(기본 포함).
  - 그림자 모드 알림도 표본에 들어간다(켤지 판단하려면 검토가 필요하다).
리플레이 화면은 파일을 드롭다운에서 고르고 이벤트를 목록에서 고르는 방식이라 주소로 바로 열 수 없다.
그래서 표본 목록에 파일명, 이벤트 id, 시각을 적는다.

사용: python -I scripts/sample_events_for_review.py <로그 폴더> [--per-kind 10] [--out sample.md]
"""

import argparse
import glob
import os
import random
import re
import sqlite3
import sys
from collections import defaultdict

ALERT_MODULES = ("rules", "door", "slot_monitor", "residue", "surface")
FULL_TOKENS = {"person_fallen"}   # 드물고 중요한 알림은 전수
SHADOW_SUFFIX = " mode=shadow"


def ro(path):
    return sqlite3.connect("file:" + path.replace(chr(92), "/") + "?mode=ro", uri=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--per-kind", type=int, default=10)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    by = defaultdict(list)
    for db in sorted(f for f in glob.glob(os.path.join(a.folder, "*.db")) if not f.endswith("_perf.db")):
        try:
            con = ro(db)
            tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            if "events" not in tables or "replay_events" not in tables:
                con.close()
                continue
            marks = ",".join("?" * len(ALERT_MODULES))
            # replay_events 에 있는 이벤트만: 리플레이 화면에서 열어 볼 수 있는 것만 표본이 된다
            q = ("SELECT e.id, e.ts_iso, e.level, e.message FROM events e JOIN replay_events r ON r.event_id=e.id"
                 " WHERE e.level IN ('WARN','ERROR') AND e.module IN (" + marks + ")")
            for eid, ts, level, msg in con.execute(q, ALERT_MODULES):
                tok = re.split(r"[ :=]", msg, maxsplit=1)[0]
                by[tok].append((os.path.basename(db), eid, ts, level, msg))
            con.close()
        except sqlite3.DatabaseError as e:
            print("WARN", os.path.basename(db), e, file=sys.stderr)
    if not by:
        sys.exit("리플레이와 연결된 알림 이벤트가 없습니다.")
    rng = random.Random(a.seed)
    out = ["# 알림 검토 표본", "",
           "대시보드 `/replay` 에서 파일을 고르고 이벤트를 선택해 `정상 감지 / 오탐 의심 / 판단 불가`를 표시한다.", ""]
    for tok in sorted(by, key=lambda t: -len(by[t])):
        rows = by[tok]
        pick = rows if tok in FULL_TOKENS or len(rows) <= a.per_kind else rng.sample(rows, a.per_kind)
        pick.sort(key=lambda r: (r[2], r[1]))
        full = " (전수)" if len(pick) == len(rows) else ""
        out.append(f"## {tok} — {len(pick)}/{len(rows)}건{full}")
        out.append("")
        out.append("| 파일 | 이벤트 id | 시각 | 레벨 | 모드 | 내용 | 판단 |")
        out.append("|---|---|---|---|---|---|---|")
        for f, eid, ts, level, msg in pick:
            shadow = "shadow" if msg.endswith(SHADOW_SUFFIX) else "on"
            body = msg[:-len(SHADOW_SUFFIX)] if shadow == "shadow" else msg
            out.append(f"| {f} | {eid} | {ts} | {level} | {shadow} | {body[:90].replace('|', '/')} | |")
        out.append("")
    text = "\n".join(out)
    if a.out:
        open(a.out, "w", encoding="utf-8").write(text)
    sys.stdout.reconfigure(encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
