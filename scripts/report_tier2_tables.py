"""
Tier 2 전후 비교 표 생성 (읽기 전용): 평가·지연·NMS·데이터 JSON → 마크다운 표

문서의 수치를 손으로 옮기면 틀리기 쉬워서, 원시 결과(JSON)에서 표를 만들어 그대로 붙인다.
없는 JSON 은 건너뛰고 "미측정"으로 표시한다.

사용:
  .venv-train\\Scripts\\python.exe -I scripts\\report_tier2_tables.py --labels A0,A0-n416,A0-n480,A1
"""

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EVAL = ROOT / "runs/tier2-homeobjects-eval"
THS = ("0.2", "0.35", "0.5")


def load(p):
    p = Path(p)
    return json.loads(p.read_text(encoding="utf-8")) if p.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", required=True)
    a = ap.parse_args()
    labels = a.labels.split(",")
    res = {l: load(EVAL / f"{l}.json") for l in labels}
    out = []

    out.append("### 평가 결과 요약 (mAP)\n")
    out.append("| 모델 | 입력 | v3 val mAP50 | v3 val mAP50-95 | HomeObjects mAP50 | HomeObjects mAP50-95 |")
    out.append("|---|---|---|---|---|---|")
    for l in labels:
        r = res[l]
        if not r:
            out.append(f"| {l} | 미측정 | | | | |")
            continue
        v, h = r["sets"].get("v3val"), r["sets"].get("homeobjects")
        out.append(f"| {l} | {r['imgsz']} | {v['mAP50']:.4f} | {v['mAP50_95']:.4f} | "
                   f"{h['mAP50']:.4f} | {h['mAP50_95']:.4f} |" if v and h else f"| {l} | {r['imgsz']} | 미측정 | | | |")

    for sname, title in (("v3val", "v3 val 3,315장 (회귀 확인, 낙관적 세트)"),
                         ("homeobjects", "HomeObjects val 404장 (도메인 밖, chair/table 만 정답)")):
        out.append(f"\n### 클래스별 — {title}\n")
        out.append("| 클래스 | 모델 | AP50 | AP50-95 | 정밀도@0.20 | 재현율@0.20 | 정밀도@0.35 | 재현율@0.35 | 정밀도@0.50 | 재현율@0.50 |")
        out.append("|---|---|---|---|---|---|---|---|---|---|")
        names = []
        for l in labels:
            if res[l] and sname in res[l]["sets"]:
                for c in res[l]["sets"][sname]["classes"]:
                    if c["class"] not in names:
                        names.append(c["class"])
        for cn in names:
            for l in labels:
                s = res[l]["sets"].get(sname) if res[l] else None
                row = next((c for c in s["classes"] if c["class"] == cn), None) if s else None
                if not row:
                    out.append(f"| {cn} | {l} | 미측정 | | | | | | | |")
                    continue
                t = row["thresholds"]
                out.append(f"| {cn} | {l} | {row['AP50']:.3f} | {row['AP50_95']:.3f} | "
                           + " | ".join(f"{t[x]['precision']:.3f} | {t[x]['recall']:.3f}" for x in THS) + " |")

    lat = [load(p) for p in sorted(EVAL.glob("latency_*.json"))]
    out.append("\n### 추론 지연 (ORT CPU, 2스레드, FP32, 개발 PC)\n")
    if any(lat):
        out.append("| 모델 | 입력 | 크기(MB) | 평균(ms) | p50(ms) | p95(ms) |")
        out.append("|---|---|---|---|---|---|")
        for d in lat:
            if d:
                out.append(f"| {Path(d['model']).name} | {d['input'][2]}x{d['input'][3]} | {d['size_mb']} | "
                           f"{d['mean_ms']} | {d['p50_ms']} | {d['p95_ms']} |")
        out.append(f"\nCPU: {lat[0]['cpu']}, onnxruntime {lat[0]['ort']}")
    else:
        out.append("미측정")

    nms = load(EVAL / "nms_app_path.json")
    out.append("\n### NMS 방식 전후 (앱 경로 재현, 클래스 비구분 → 클래스별)\n")
    if nms:
        for sname, sv in nms["sets"].items():
            for conf, d in sv.items():
                out.append(f"\n**{sname}, conf {conf}** — 클래스별 NMS가 추가로 살린 박스 {d['extra_boxes_kept_by_per_class_nms']}개\n")
                out.append("| 클래스 | TP 전→후 | FP 전→후 | 정밀도 전→후 | 재현율 전→후 |")
                out.append("|---|---|---|---|---|")
                for cn, x in d["classes"].items():
                    b, f = x["agnostic"], x["per_class"]
                    out.append(f"| {cn} | {b['tp']}→{f['tp']} | {b['fp']}→{f['fp']} | "
                               f"{b['precision']:.3f}→{f['precision']:.3f} | {b['recall']:.3f}→{f['recall']:.3f} |")
    else:
        out.append("미측정")

    bs = load(ROOT / "datasets/tier2_v4/build_stats.json")
    out.append("\n### 데이터 가공 통계\n")
    if bs:
        out.append("```json\n" + json.dumps(bs, ensure_ascii=False, indent=2) + "\n```")
    else:
        out.append("미측정")
    sys.stdout.reconfigure(encoding="utf-8")
    print("\n".join(out))


if __name__ == "__main__":
    main()
