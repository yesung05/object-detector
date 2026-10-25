"""
AI-Hub 「실내(편의점, 매장) 사람 이상행동 데이터」 압축 해제 (개발 PC 전용, 원본은 읽기만)

설계 이유
  - 파이썬 zipfile: mp4 는 이미 압축된 형식이라 zip 의 deflate 이득이 없다(실측 비율 1.0).
    해제는 순수 I/O 이고, zipfile 은 항목을 끝까지 읽을 때 CRC 를 자동 검증하므로 무결성 점검을 겸한다.
  - .part 로 쓰고 끝나면 이름 변경: 중단·정전 때 반쪽 파일이 완성본처럼 남는 것을 막는다.
  - 이미 있고 크기가 같은 파일은 건너뜀: 중단 후 다시 실행하면 이어서 진행(재개 가능).
  - zip 을 한 번에 하나씩 순차 처리: 원본(D:)이 HDD 라 여러 zip 을 동시에 읽으면 탐색이 늘어 오히려 느리다.
  - 항목 이름은 basename 만 쓴다: zip 항목이 '/C_3_7_...xml' 처럼 선행 슬래시를 가져서, 이름을 그대로 경로로
    쓰면 대상 밖으로 나갈 수 있다(zip-slip). 클래스 폴더 아래 평면 구조로 둔다.
  - 실패는 목록으로 남기고 계속한다: 272GB 를 풀다가 파일 하나 때문에 전체를 다시 하지 않도록.
  - *.zip 만 대상이라 .irx465(다운로드 임시 파일로 보이는 것)는 건드리지 않는다.

순서(--plan): 라벨 전부 → 전도·교통약자 영상 → 나머지 영상. 우선순위 1 평가를 일찍 시작할 수 있게 한다.

사용:
  python -I scripts\\extract_anomaly_data.py --src "D:\\...\\01-1.정식개방데이터" --dest E:\\hunik-anomaly
"""

import argparse
import json
import os
import re
import shutil
import sys
import time
import zipfile
from pathlib import Path

CHUNK = 8 * 1024 * 1024  # 8MB: HDD 순차 쓰기에 충분히 크고 메모리는 가볍다


def classify(zip_path: Path):
    """파일명 TS_03.이상행동_07.전도.zip → (split, kind, class). split 은 Training/Validation."""
    name = zip_path.name
    split = "Training" if "Training" in zip_path.parts else "Validation"
    kind = "원천데이터" if name[1] == "S" else "라벨링데이터"
    m = re.match(r"[TV][SL]_03\.이상행동_(\d+)\.(.+)\.zip$", name)
    cls = f"{m.group(1)}.{m.group(2)}" if m else name
    return split, kind, cls


def order_key(zip_path: Path):
    split, kind, cls = classify(zip_path)
    if kind == "라벨링데이터":
        group = 0
    elif cls.startswith(("07.", "14.")):
        group = 1  # 전도·교통약자: 우선순위 1 평가용
    else:
        group = 2
    return (group, split != "Validation", cls)  # 같은 그룹에서는 Validation 먼저(작아서 빨리 확인됨)


def safe_name(entry: str) -> str:
    base = os.path.basename(entry.replace("\\", "/"))
    if not base or base in (".", "..") or ":" in base:
        raise ValueError(f"안전하지 않은 항목 이름: {entry!r}")
    return base


def extract_zip(zip_path: Path, dest_root: Path, log):
    split, kind, cls = classify(zip_path)
    out_dir = dest_root / split / kind / cls
    out_dir.mkdir(parents=True, exist_ok=True)
    ok = skipped = 0
    failures = []
    nbytes = 0
    t0 = time.time()
    with zipfile.ZipFile(zip_path) as z:
        infos = [i for i in z.infolist() if not i.is_dir()]
        for info in infos:
            try:
                target = out_dir / safe_name(info.filename)
            except ValueError as e:
                failures.append({"entry": info.filename, "error": str(e)})
                continue
            if target.exists() and target.stat().st_size == info.file_size:
                skipped += 1
                continue
            part = target.with_name(target.name + ".part")
            try:
                with z.open(info) as src, open(part, "wb") as dst:
                    shutil.copyfileobj(src, dst, CHUNK)  # EOF 에서 CRC 불일치면 BadZipFile 이 난다
                if part.stat().st_size != info.file_size:
                    raise IOError(f"크기 불일치 {part.stat().st_size} != {info.file_size}")
                os.replace(part, target)
                ok += 1
                nbytes += info.file_size
            except Exception as e:  # noqa: BLE001 — 어떤 실패든 기록하고 다음 항목으로
                failures.append({"entry": info.filename, "error": f"{type(e).__name__}: {e}"})
                try:
                    part.unlink()
                except OSError:
                    pass
    secs = time.time() - t0
    rec = {"zip": zip_path.name, "extracted": ok, "skipped": skipped, "failed": len(failures),
           "bytes": nbytes, "seconds": round(secs, 1),
           "MBps": round(nbytes / 1e6 / secs, 1) if secs > 0 and nbytes else None}
    log(rec, failures)
    return ok, skipped, failures, nbytes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--dest", required=True)
    ap.add_argument("--only", default="", help="이름에 이 문자열이 있는 zip 만(예: 07.전도). 비우면 전부")
    a = ap.parse_args()
    src, dest = Path(a.src), Path(a.dest)
    zips = sorted((p for p in src.rglob("*.zip") if a.only in p.name), key=order_key)
    if not zips:
        sys.exit("zip 을 찾지 못했습니다")
    dest.mkdir(parents=True, exist_ok=True)

    # 사전 점검: 필요 용량(압축 해제 크기 합)이 대상 드라이브 여유보다 크면 시작하지 않는다.
    need = 0
    for p in zips:
        with zipfile.ZipFile(p) as z:
            need += sum(i.file_size for i in z.infolist())
    free = shutil.disk_usage(dest).free
    print(f"[preflight] zip {len(zips)}개, 해제 크기 합 {need/1e9:.1f}GB, 대상 여유 {free/1e9:.1f}GB", flush=True)
    if free < need * 1.02:
        sys.exit("대상 드라이브 여유 공간이 부족합니다")

    logf = open(dest / "_extract.log.jsonl", "a", encoding="utf-8")

    def log(rec, failures):
        rec = dict(rec, time=time.strftime("%Y-%m-%dT%H:%M:%S"))
        logf.write(json.dumps(rec, ensure_ascii=False) + "\n")
        for f in failures:
            logf.write(json.dumps({"zip": rec["zip"], **f}, ensure_ascii=False) + "\n")
        logf.flush()
        print(f"[{rec['time']}] {rec['zip']}: 추출 {rec['extracted']} 건너뜀 {rec['skipped']} "
              f"실패 {rec['failed']} {rec['bytes']/1e9:.2f}GB {rec['seconds']}s {rec['MBps']}MB/s", flush=True)

    t0 = time.time()
    total_ok = total_fail = total_bytes = 0
    all_failures = []
    for p in zips:
        ok, skipped, failures, nbytes = extract_zip(p, dest, log)
        total_ok += ok
        total_fail += len(failures)
        total_bytes += nbytes
        all_failures += [{"zip": p.name, **f} for f in failures]
    secs = time.time() - t0
    summary = {"zips": len(zips), "extracted": total_ok, "failed": total_fail,
               "bytes": total_bytes, "seconds": round(secs, 1),
               "MBps": round(total_bytes / 1e6 / secs, 1) if secs > 0 and total_bytes else None,
               "failures": all_failures}
    (dest / "_extract_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print("[done]", json.dumps({k: v for k, v in summary.items() if k != "failures"}, ensure_ascii=False), flush=True)
    sys.exit(0 if total_fail == 0 else 2)


if __name__ == "__main__":
    main()
