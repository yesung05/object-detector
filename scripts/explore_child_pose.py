"""
탐색(구현 아님): 배포 포즈 모델의 관절 비율로 어린이와 어른을 가를 수 있는가 — 노키즈존(출입 제한 대상 진입) 가능성 진단.

근거 라벨: AI-Hub 이상행동 데이터의 이벤트 행위자 `*_start` 박스에 붙은 `age group`(child / young people / middle age /
advanced age). 행위자를 같은 프레임의 관절 GT 사람과 잇고, 그 사람과 IoU 로 맞는 배포 모델 검출(diag_pose_gt.py 가 남긴
det/kp CSV)의 관절로 비율 특징을 만든다. 카메라 거리에 덜 민감하도록 길이의 비율만 쓴다:
  head_ratio  = 귀-귀(없으면 눈-눈×2) 거리 / 몸 높이(코 → 발목 평균)   — 어린이는 머리가 몸에 비해 크다
  leg_ratio   = 엉덩이→발목 / 어깨→엉덩이                             — 어린이는 다리가 상대적으로 짧다
  shoulder_ratio = 어깨 너비 / 몸 높이
선 자세(GT 관절 외곽 세로 > 가로) 프레임만, 관절 점수 0.4 이상만 쓰고, 행위자-클립마다 중앙값 하나로 줄인다.
같은 장면을 카메라 여러 대가 찍으므로 독립 표본 수는 장면 수로 따로 보고한다.

판단 기준(계획 문서): 가장 좋은 특징의 AUC < 0.8 이면 이 방향은 중단.
프레임 이미지는 열지 않는다(README 의 AI-Hub 이용 규칙) — XML·CSV 수치만.

사용: python -I scripts/explore_child_pose.py --index E:/hunik-anomaly/anomaly_index.csv --runs E:/hunik-anomaly/_work/pose_gt/dev_base/runs
"""
import argparse
import csv
import json
import statistics
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from diag_pose_gt import load_gt, load_pred, iou  # noqa: E402  같은 GT 파서·CSV 로더를 그대로 쓴다

KP = 0.4


def actors(xml_path):
    """[(frame, (x1,y1,x2,y2), age)] — *_start 박스의 행위자."""
    out = []
    for tr in ET.parse(xml_path).getroot().iter("track"):
        if not tr.get("label", "").endswith("_start"):
            continue
        for b in tr.iter("box"):
            if b.get("outside") == "1":
                continue
            age = next((a.text for a in b.iter("attribute") if a.get("name") == "age group"), None)
            if age:
                out.append((int(b.get("frame")), tuple(float(b.get(k)) for k in ("xtl", "ytl", "xbr", "ybr")), age.strip()))
    return out


def features(k):
    def p(i):
        v = k.get(i)
        return (v[0], v[1]) if v and v[2] >= KP else None

    def dist(a, b):
        return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5

    nose, la, ra = p(0), p(15), p(16)
    if not nose or not (la or ra):
        return None
    ank = ((la[0] + ra[0]) / 2, (la[1] + ra[1]) / 2) if la and ra else (la or ra)
    height = dist(nose, ank)
    if height < 40:
        return None
    f = {}
    if p(3) and p(4):
        f["head_ratio"] = dist(p(3), p(4)) / height
    elif p(1) and p(2):
        f["head_ratio"] = 2 * dist(p(1), p(2)) / height
    sh = [x for x in (p(5), p(6)) if x]
    hp = [x for x in (p(11), p(12)) if x]
    if sh and hp:
        s = (sum(x[0] for x in sh) / len(sh), sum(x[1] for x in sh) / len(sh))
        h = (sum(x[0] for x in hp) / len(hp), sum(x[1] for x in hp) / len(hp))
        torso = dist(s, h)
        if torso > 5:
            f["leg_ratio"] = dist(h, ank) / torso
    if p(5) and p(6):
        f["shoulder_ratio"] = dist(p(5), p(6)) / height
    return f


def auc(pos, neg):
    """Mann-Whitney U / (n1 n2). 0.5 = 무작위. 방향을 모르므로 max(auc, 1-auc) 도 함께 본다."""
    if not pos or not neg:
        return None
    wins = sum((1.0 if a > b else 0.5 if a == b else 0.0) for a in pos for b in neg)
    return wins / (len(pos) * len(neg))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--index", required=True)
    ap.add_argument("--runs", required=True, help="diag_pose_gt.py 의 runs 폴더(클립별 det.csv/kp.csv)")
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    rows = {}
    with open(a.index, encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            rows[r["clip"]] = r
    root = Path(a.index).parent
    samples = []   # (is_child, scene_key, {feature: median})
    for d in sorted(Path(a.runs).iterdir()):
        r = rows.get(d.name)
        if not r or not (d / "kp.csv").exists():
            continue
        if r["split"] == "test":
            continue
        sub = "Training" if r["split_orig"] == "Training" else "Validation"
        xml = next((root / sub / "라벨링데이터").glob(f"{r['class_code']}.*/{d.name}.xml"), None)
        if not xml:
            continue
        gt = load_gt(xml)
        boxes, kps = load_pred(d)
        for frame0, abox, age in actors(xml):
            # 행위자 박스 ↔ 같은 프레임 관절 GT 사람(관절 외곽 IoU 최대)
            best, pid = 0.0, None
            for (fr, p), j in gt.items():
                if fr != frame0 or len(j) < 6:
                    continue
                xs = [v[0] for v in j.values()]; ys = [v[1] for v in j.values()]
                v = iou(abox, (min(xs), min(ys), max(xs), max(ys)))
                if v > best:
                    best, pid = v, p
            if pid is None or best < 0.2:
                continue
            feats = defaultdict(list)
            for (fr, p), j in gt.items():
                if p != pid or len(j) < 6:
                    continue
                xs = [v[0] for v in j.values()]; ys = [v[1] for v in j.values()]
                g = (min(xs), min(ys), max(xs), max(ys))
                if (g[2] - g[0]) >= (g[3] - g[1]):
                    continue   # 선 자세만
                bi, bv = -1, 0.3
                for i, b in enumerate(boxes.get(fr, [])):
                    v = iou(g, b)
                    if v > bv:
                        bi, bv = i, v
                if bi < 0:
                    continue
                f = features(kps.get((fr, bi), {}))
                for k, v in (f or {}).items():
                    feats[k].append(v)
            med = {k: statistics.median(v) for k, v in feats.items() if len(v) >= 3}
            if med:
                samples.append((age == "child", r["scene_key"], med))
    res = {"actor_clips": len(samples), "child": sum(1 for s in samples if s[0]),
           "child_scenes": len({s[1] for s in samples if s[0]}), "adult_scenes": len({s[1] for s in samples if not s[0]}),
           "features": {}}
    for k in ("head_ratio", "leg_ratio", "shoulder_ratio"):
        pos = [s[2][k] for s in samples if s[0] and k in s[2]]
        neg = [s[2][k] for s in samples if not s[0] and k in s[2]]
        v = auc(pos, neg)
        res["features"][k] = {"n_child": len(pos), "n_adult": len(neg),
                              "auc": round(v, 3) if v is not None else None,
                              "auc_best_direction": round(max(v, 1 - v), 3) if v is not None else None,
                              "child_median": round(statistics.median(pos), 3) if pos else None,
                              "adult_median": round(statistics.median(neg), 3) if neg else None}
    text = json.dumps(res, ensure_ascii=False, indent=2)
    if a.out:
        Path(a.out).write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    sys.exit(main())
