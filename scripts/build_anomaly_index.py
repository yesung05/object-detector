"""
이상행동 데이터 라벨 인덱스 생성 (개발 PC 전용, 라벨 XML 만 읽음)

라벨 XML(CVAT 1.1)을 한 파일씩 파싱해 클립당 한 줄의 CSV 로 만든다. 모든 XML 을 한 번에 메모리에
올리지 않는다(파일 하나 약 0.5MB, 한 번에 하나만 처리).

정답 구간(events):
  - 전도·파손·방화·흡연·유기·절도·폭행은 `<이름>_start` / `<이름>_end` 트랙이 구간 마커다. 같은 이름의
    start 를 프레임 순으로 정렬해, 각각 자기 프레임 이후 처음 나오는 end 와 짝지은다.
  - 교통약자는 마커가 아니라 `weak pedestrian` 트랙의 프레임 범위(박스가 있는 구간)가 정답이다.
  표본 조사에서 마커는 `fall_start` 프레임 143 의 박스(outside=0)와 다음 프레임(outside=1)으로 이뤄져 있어
  구간 시작은 outside=0 박스의 최소 프레임으로 잡는다.

장면 키: (상점, 날짜, 시:분). 한 사건을 카메라 6대(CA~CF)가 동시에 찍어서 클립 단위로 학습/평가를 나누면
같은 사건이 양쪽에 들어간다. 카메라마다 시작 초가 1~3초 어긋나므로 초는 키에서 뺀다.

분할:
  - test  = 제공된 Validation 전부(최종 확인 전용, 튜닝에 쓰지 않는다)
  - dev   = Training 장면의 약 25%(장면 키 해시로 결정 — 같은 장면은 항상 같은 분할)
  - pool  = Training 나머지(분석·특징 측정용)
  - excluded_overlap = 장면 키가 Validation 과 겹치는 Training 클립(dev 오염 방지로 제외)

ASCII 경로(video 열): unmanned_detector.exe 는 Windows 에서 비ASCII 명령줄 인자를 FFmpeg 가 기대하는
UTF-8 로 넘기지 못해 '원천데이터\\07.전도' 같은 경로를 열지 못한다(avformat_open_input: Invalid argument,
실측). 영상을 복사하지 않고 ASCII 이름의 디렉터리 정션(<root>\\v\\<split>\\<분류코드>)을 만들어 그 경로를
video 열에 쓴다. 정션은 관리자 권한이 필요 없고 원본을 그대로 가리킨다(데이터 복사 없음).

사용:
  python -I scripts\\build_anomaly_index.py --root E:\\hunik-anomaly
"""

import argparse
import csv
import hashlib
import json
import re
import statistics
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path
import xml.etree.ElementTree as ET

# 라벨 XML 의 관절 이름(공백이 두 번인 'Right  hip' 은 원본 그대로).
JOINTS = {"Pelvis", "Left hip", "Left knee", "Left foot", "Right  hip", "Right knee", "Right foot",
          "Spine naval", "Spine chest", "Neck base", "Center head", "Right shoulder", "Right elbow",
          "Right hand", "Left shoulder", "Left elbow", "Left hand"}
EVENT_PREFIX = {"07": "fall", "08": "broken", "09": "fire", "10": "smoke", "11": "abandon",
                "12": "theft", "13": "fight"}
# 구분자가 '-' 인 이름과 '_' 인 이름(13.폭행 DYA 08_20_13_41_07)이 섞여 있어 둘 다 받고 '-' 로 정규화한다.
NAME_RE = re.compile(r"_(?P<store>[A-Z]{3})_(?P<date>\d\d[-_]\d\d)_(?P<time>\d\d[-_]\d\d[-_]\d\d)_(?P<cam>[A-Za-z]+)_")
DEV_FRACTION = 0.25


def frames_of(track, tags=("box", "points"), only_visible=True):
    out = []
    for tag in tags:
        for e in track.findall(tag):
            if only_visible and e.get("outside") == "1":
                continue
            out.append(int(e.get("frame")))
    return out


def pair_markers(starts, ends):
    """start 프레임들을 정렬해 각각 자기 이후의 가장 이른 미사용 end 와 짝짓는다. 못 찾으면 end=None."""
    ends = sorted(ends)
    used = [False] * len(ends)
    pairs = []
    for s in sorted(starts):
        e = None
        for i, v in enumerate(ends):
            if not used[i] and v >= s:
                used[i] = True
                e = v
                break
        pairs.append([s, e])
    return pairs


def parse_clip(xml_path: Path, class_code: str):
    root = ET.parse(xml_path).getroot()
    frames = int(root.find("./meta/task/size").text)
    width = int(root.find("./meta/task/original_size/width").text)
    height = int(root.find("./meta/task/original_size/height").text)
    by_label = defaultdict(list)
    joint_frames = set()
    ids = set()
    obj_boxes = Counter()
    for t in root.iter("track"):
        lab = t.get("label")
        if lab in JOINTS:
            joint_frames.update(frames_of(t, ("points",)))
            continue
        fr = frames_of(t, ("box",))
        if fr:
            by_label[lab].append((min(fr), max(fr)))
        if lab.startswith("object") or lab in ("wheelchair", "crutches", "cane"):
            obj_boxes[lab] += 1
        for b in t.findall("box"):
            for a in b.findall("attribute"):
                if a.get("name") == "ID" and a.text:
                    ids.add(a.text.strip())
    events = []
    if class_code == "14":
        # 교통약자: weak pedestrian 박스가 있는 구간들이 곧 정답 구간.
        events = [[s, e] for s, e in sorted(by_label.get("weak pedestrian", []))]
        kind = "weak_pedestrian"
    else:
        kind = EVENT_PREFIX[class_code]
        events = pair_markers([s for s, _ in by_label.get(kind + "_start", [])],
                              [s for s, _ in by_label.get(kind + "_end", [])])
    return {"frames": frames, "width": width, "height": height, "event_kind": kind,
            "events": events, "n_persons": len(ids), "joint_label_frames": len(joint_frames),
            "object_tracks": dict(obj_boxes)}


def scene_key(stem):
    m = NAME_RE.search("_" + stem + "_")
    if not m:
        return None, None, None, None, None
    store, cam = m.group("store"), m.group("cam")
    date, time_ = m.group("date").replace("_", "-"), m.group("time").replace("_", "-")
    return store, date, time_, cam, f"{store}|{date}|{time_[:5]}"


def ensure_junction(link: Path, target: Path):
    """link(ASCII 경로)가 target 을 가리키는 디렉터리 정션을 만든다. 이미 있으면 그대로 둔다."""
    if link.exists():
        return
    link.parent.mkdir(parents=True, exist_ok=True)
    target.mkdir(parents=True, exist_ok=True)  # 아직 해제 전인 클래스도 정션이 끊기지 않게
    r = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f"정션 생성 실패 {link} -> {target}: {r.stdout!r} {r.stderr!r}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, help="extract_anomaly_data.py 의 --dest")
    ap.add_argument("--check-video", action="store_true", help="영상 파일 존재 여부 열을 채운다")
    ap.add_argument("--ascii-root", default="", help="ASCII 정션 위치(기본 <root>\\v)")
    a = ap.parse_args()
    root = Path(a.root)
    ascii_root = Path(a.ascii_root) if a.ascii_root else root / "v"
    rows, bad = [], []
    for split in ("Training", "Validation"):
        for cdir in sorted((root / split / "라벨링데이터").iterdir()):
            code = cdir.name.split(".")[0]
            cname = cdir.name.split(".", 1)[1]
            for xml in sorted(cdir.glob("*.xml")):
                try:
                    info = parse_clip(xml, code)
                except Exception as e:  # noqa: BLE001 — 한 파일 실패가 전체를 막지 않게
                    bad.append({"xml": str(xml), "error": f"{type(e).__name__}: {e}"})
                    continue
                store, date, time_, cam, skey = scene_key(xml.stem)
                native = root / split / "원천데이터" / cdir.name / (xml.stem + ".mp4")
                ensure_junction(ascii_root / split / code, native.parent)
                vid = ascii_root / split / code / (xml.stem + ".mp4")  # ASCII 경로(exe 가 열 수 있음)
                rows.append({"split_orig": split, "class_code": code, "class_name": cname, "clip": xml.stem,
                             "store": store, "date": date, "time": time_, "camera": cam, "scene_key": skey,
                             **info, "events": json.dumps(info["events"]),
                             "object_tracks": json.dumps(info["object_tracks"]),
                             "video_exists": (vid.exists() if a.check_video else ""),
                             "video": str(vid), "video_native": str(native)})
    # 분할 결정: Validation = test, Training 중 Validation 과 장면이 겹치면 제외, 나머지는 해시로 dev/pool.
    val_scenes = {r["scene_key"] for r in rows if r["split_orig"] == "Validation" and r["scene_key"]}
    for r in rows:
        if not r["scene_key"]:
            r["split"] = "excluded_unparsed"  # 장면을 몰라 누수를 판단할 수 없으므로 어느 분할에도 넣지 않는다
        elif r["split_orig"] == "Validation":
            r["split"] = "test"
        elif r["scene_key"] in val_scenes:
            r["split"] = "excluded_overlap"
        else:
            h = int(hashlib.sha256(r["scene_key"].encode()).hexdigest()[:8], 16) / 0xFFFFFFFF if r["scene_key"] else 1.0
            r["split"] = "dev" if h < DEV_FRACTION else "pool"
    out = root / "anomaly_index.csv"
    cols = list(rows[0].keys())
    with open(out, "w", newline="", encoding="utf-8-sig") as f:  # utf-8-sig: 엑셀에서 한글이 깨지지 않게
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)
    # 요약
    summ = {"clips": len(rows), "parse_failures": bad, "by_class": {}}
    for code in sorted({r["class_code"] for r in rows}):
        rr = [r for r in rows if r["class_code"] == code]
        evs = [json.loads(r["events"]) for r in rr]
        lens = [e[1] - e[0] + 1 for ev in evs for e in ev if e[1] is not None]
        unpaired = sum(1 for ev in evs for e in ev if e[1] is None)
        summ["by_class"][f"{code}.{rr[0]['class_name']}"] = {
            "clips": len(rr), "clips_with_event": sum(1 for ev in evs if ev),
            "events_total": sum(len(ev) for ev in evs), "unpaired_start": unpaired,
            "event_len_frames": ({"min": min(lens), "median": statistics.median(lens), "max": max(lens)} if lens else None),
            "splits": dict(Counter(r["split"] for r in rr)),
            "scenes": len({r["scene_key"] for r in rr}), "stores": sorted({r["store"] for r in rr if r["store"]}),
            "persons_median": statistics.median(r["n_persons"] for r in rr),
            "joint_frames_median": statistics.median(r["joint_label_frames"] for r in rr)}
    summ["unparsed_scene_keys"] = sum(1 for r in rows if not r["scene_key"])
    summ["split_totals"] = dict(Counter(r["split"] for r in rows))
    (root / "anomaly_index_summary.json").write_text(json.dumps(summ, ensure_ascii=False, indent=2), encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps(summ, ensure_ascii=False, indent=2))
    print("CSV:", out)


if __name__ == "__main__":
    main()
