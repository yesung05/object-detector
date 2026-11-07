"""현장 로그 폴더의 실제 이벤트 메시지로, 로깅 v2 의 events 증가분을 추정한다(사용: python -I scripts/estimate_log_growth.py <로그폴더>).
src/log.c 의 derive_fields 규칙(허용 모듈만 data, 400바이트 상한, 숫자 단위 제거)을 그대로 따라 한다. 읽기 전용."""
import glob
import re
import sqlite3

import sys
root = sys.argv[1] if len(sys.argv) > 1 else 'docs/분석_로그/2026-10-06_logs'
dbs = [f for f in sorted(glob.glob(root + '/**/*.db', recursive=True)) if not f.endswith('_perf.db')]
ALLOW = {'rules', 'door', 'slot_monitor', 'residue', 'surface', 'camera', 'summary', 'config'}
NUM = re.compile(r'^-?(0|[1-9]\d*)(\.\d+)?(s|ms|px|%)?$')


def derive(module, msg):
    m = re.match(r'[^ :=]*', msg)
    kind = m.group(0)
    if msg[len(kind):len(kind) + 1] == '=':
        kind, rest = module, msg
    else:
        rest = msg[len(kind):]
    if module not in ALLOW:
        return kind, 0
    out, seen = 1, set()
    for tok in rest.split(' '):
        if '=' not in tok:
            continue
        k, _, v = tok.partition('=')
        if not k or len(k) > 32 or not re.fullmatch(r'\w+', k) or not v or len(v) > 64 or k in seen:
            continue
        isnum = bool(NUM.match(v))
        piece = len(k) + 3 + (len(re.sub(r'(ms|px|s|%)$', '', v)) if isnum else len(v) + 2)
        if out + piece + 2 > 400:
            break
        out += piece + (1 if out > 1 else 0)
        seen.add(k)
    return kind, (out + 1 if out > 1 else 0)


n = extra = base = alert = 0
for f in dbs:
    try:
        c = sqlite3.connect('file:' + f.replace('\\', '/') + '?mode=ro&immutable=1', uri=True)
        rows = c.execute('select module,message from events').fetchall()
    except Exception:
        continue
    for mod, msg in rows:
        k, d = derive(mod, msg)
        n += 1
        extra += len(k) + d + 20          # kind + data + 고정 열(run 6, mono 8, frame 2, track 1 + 헤더)
        base += len(msg) + len(mod) + 40  # ts_unix 8 + ts_iso 19 + level 5 + 헤더
        alert += mod in ALLOW
print(f'event DB {len(dbs)}개, 이벤트 {n:,}건(알림성 모듈 {alert:,}건)')
print(f'기존 행 페이로드 약 {base / 1e6:.1f} MB, 증가분 약 {extra / 1e6:.2f} MB (+{100 * extra / max(base, 1):.0f}%), 이벤트당 평균 {extra / max(n, 1):.0f} B')
hours = float(sys.argv[2]) if len(sys.argv) > 2 else 143.5   # 로그가 덮은 시간(h)
print(f'시간당 증가분 약 {extra / hours / 1024:.1f} KB')
