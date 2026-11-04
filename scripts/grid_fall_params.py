"""
쓰러짐 룰 파라미터 격자 탐색 (config 값만 바꿈, 코드 변경 없음 — 매장별 설정 분리 원칙)

같은 클립·같은 모델 추론 캐시로 설정만 바꿔 eval_anomaly_dataset.py 를 반복 실행한다. 첫 설정이 추론을
기록하고 이후는 재생하므로 AI 호출은 클립당 한 번이다.

평가 구성(dev 분할만 — test 는 최종 확인 한 번 전용):
  양성: 07.전도 dev, 장면당 1클립(한 사건을 카메라 6대가 찍으므로 한 장면은 한 번만 센다)
  음성: 08~14 중 07 을 뺀 클래스 dev, 클래스당 N클립(--neg-limit). 이 클립들에는 쓰러짐이 없으므로
        person_fallen 은 전부 오탐이다. 단 폭행(13)에는 사람이 넘어지는 장면이 섞여 있을 수 있어 클래스별로 따로 보고한다.
파라미터: fall_aspect_ratio_kp(박스 가로/세로 기준), fall_aspect_ratio_nokp, fall_hold_seconds.

사용:
  python -I scripts\\grid_fall_params.py --exe <exe> --model <onnx> --index <csv> --work <dir> --cache-dir <dir>
"""

import argparse
import itertools
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def write_ini(path, kp, nokp, hold):
    path.write_text(
        "# grid_fall_params.py 가 만든 파일 — 쓰러짐 파라미터만 담는다\n"
        f"fall_aspect_ratio_kp = {kp}\nfall_aspect_ratio_nokp = {nokp}\nfall_hold_seconds = {hold}\n",
        encoding="utf-8")


def run_eval(a, label, cfg, classes, positive, limit, per_scene, sub):
    cmd = [sys.executable, "-I", str(HERE / "eval_anomaly_dataset.py"), "--exe", a.exe, "--model", a.model,
           "--index", a.index, "--work", str(Path(a.work) / sub), "--label", label, "--split", "dev",
           "--classes", classes, "--positive-classes", positive, "--jobs", str(a.jobs),
           "--threads", str(a.threads), "--cache-dir", a.cache_dir, "--resume"]
    if limit:
        cmd += ["--limit", str(limit)]
    if per_scene:
        cmd += ["--per-scene", str(per_scene)]
    if cfg:
        cmd += ["--config", str(cfg)]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print(r.stdout[-1500:], r.stderr[-1500:])
        sys.exit(f"eval 실패: {label} {sub}")
    return json.loads((Path(a.work) / sub / label / "result.json").read_text(encoding="utf-8"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--kp", default="1.0,1.15,1.3,1.8")
    ap.add_argument("--hold", default="3,5")
    ap.add_argument("--neg-limit", type=int, default=12)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--nokp-offset", type=float, default=0.3, help="nokp = kp + offset (현장 1.3/1.6 의 간격)")
    a = ap.parse_args()
    work = Path(a.work)
    (work / "cfg").mkdir(parents=True, exist_ok=True)
    kps = [float(x) for x in a.kp.split(",")]
    holds = [float(x) for x in a.hold.split(",")]
    neg_classes = "08,09,10,11,12,13,14"
    rows = []
    # 코드 기본값(--config 없음)은 기준선 평가와 같은 조건이라 비교 기준으로 먼저 둔다.
    points = [("code_default(1.8/2.2/5)", None)]
    for kp, hold in itertools.product(kps, holds):
        label = f"kp{kp}_h{int(hold) if hold == int(hold) else hold}"
        cfg = work / "cfg" / f"{label}.ini"
        write_ini(cfg, kp, round(kp + a.nokp_offset, 2), hold)
        points.append((label, cfg))
    for label, cfg in points:
        safe = label.replace("(", "_").replace(")", "").replace("/", "-")
        pos = run_eval(a, safe, cfg, "07", "07", 0, 1, "grid_pos")
        neg = run_eval(a, safe, cfg, neg_classes, "", a.neg_limit, 0, "grid_neg")
        sp, sn = pos["summary"]["07"], neg["summary"]
        neg_fa = {c: v["false_alarm_events"] for c, v in sn.items()}
        neg_hours = sum(v["clips_evaluated"] for v in sn.values()) * 60 / 3600
        row = {"label": label, "clips_pos": sp["clips_evaluated"], "tp": sp["tp"], "recall": sp["recall"],
               "latency_p50": (sp["latency_s"] or {}).get("p50"),
               "fa_in_pos_clips": sp["false_alarm_events"], "neg_clips": sum(v["clips_evaluated"] for v in sn.values()),
               "neg_fa_total": sum(neg_fa.values()), "neg_fa_by_class": neg_fa,
               "neg_fa_per_hour": round(sum(neg_fa.values()) / neg_hours, 2) if neg_hours else None}
        rows.append(row)
        print(json.dumps(row, ensure_ascii=False), flush=True)
        (work / "grid_summary.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n설정 | 재현율(TP/클립) | 지연p50 | 양성클립 내 오탐 | 음성 오탐(건, /시간)")
    for r in rows:
        print(f"{r['label']} | {r['recall']} ({r['tp']}/{r['clips_pos']}) | {r['latency_p50']} | "
              f"{r['fa_in_pos_clips']} | {r['neg_fa_total']} ({r['neg_fa_per_hour']}/h) {r['neg_fa_by_class']}")


if __name__ == "__main__":
    main()
