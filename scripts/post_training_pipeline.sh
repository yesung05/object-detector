#!/bin/bash
# A1 학습이 끝나면 평가·지연 측정·표 생성까지 자동 수행하는 일회성 파이프라인.
#
# 왜 스크립트로 묶었나: 학습이 끝날 때까지 사람(또는 에이전트)이 계속 지켜보지 않아도 되게 하려는 것.
# 이 스크립트는 종료(shutdown)나 데이터 삭제를 하지 않는다 — 결과 문서를 쓰는 데 판단이 필요하고,
# 컴퓨터를 끄면 그 작업도 함께 끝나기 때문에 그건 문서 작성 뒤에 따로 한다.
# 각 단계는 실패해도 다음 단계로 넘어가고 로그에 남긴다(set -e 를 쓰지 않는 이유).
#
# 사용: bash scripts/post_training_pipeline.sh   (리포 루트에서)

cd "$(dirname "$0")/.." || exit 1
PY=.venv-train/Scripts/python.exe
EV=runs/tier2-homeobjects-eval
LOG=logs/post_training_pipeline.log
TRAIN_DIR=runs/detect/runs/tier2_v4_n320/train
mkdir -p "$EV/export" logs
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

say "pipeline start — 학습 종료 대기"
# 학습 프로세스(run_train_tier2.py)가 사라질 때까지 30초 간격으로 확인한다.
# Name 을 python.exe 로 제한하는 이유: 이 검사를 하는 powershell 자신의 명령줄에도 같은 문자열이
# 들어 있어서, 제한하지 않으면 자기 자신을 찾아 영원히 대기한다(실제로 50분 멈춤).
while powershell -NoProfile -Command "if (Get-CimInstance Win32_Process | Where-Object { \$_.Name -eq 'python.exe' -and \$_.CommandLine -like '*run_train_tier2.py*' }) { exit 0 } else { exit 1 }"; do
  sleep 30
done
say "학습 프로세스 종료 감지"
tail -c 600 logs/train_A1_v4_n320.log | tr '\r' '\n' | grep -a -v '^$' | tail -3 >> "$LOG"

# 1) A1 가중치 → ONNX(FP32) 320 / 416. 학습 스크립트가 만든 best_fp32.onnx 가 있으면 재사용한다.
BEST_PT="$TRAIN_DIR/weights/best.pt"
if [ ! -f "$BEST_PT" ]; then say "ERROR best.pt 없음: $BEST_PT"; echo FAILED > "$EV/pipeline.done"; exit 1; fi
cp "$BEST_PT" "$EV/export/A1_best.pt"
cat > "$EV/export/export_a1.py" <<'PYEOF'
import sys
from pathlib import Path
from ultralytics import YOLO
src = Path(sys.argv[1])
for sz in (320, 416):
    out = YOLO(str(src)).export(format="onnx", imgsz=sz, simplify=True, device="cpu")
    dst = src.parent / f"A1_n_{sz}.onnx"
    Path(out).replace(dst)
    print("exported", dst)
PYEOF
say "A1 ONNX 내보내기(320/416)"
$PY -I "$EV/export/export_a1.py" "$EV/export/A1_best.pt" >> "$LOG" 2>&1 || say "WARN export 실패"

# 2) 평가: A1@320, A1@416 (같은 두 세트)
for spec in "A1:320" "A1-n416:416"; do
  label="${spec%%:*}"; sz="${spec##*:}"
  model="$EV/export/A1_n_${sz}.onnx"
  if [ -f "$model" ]; then
    say "평가 $label (imgsz $sz)"
    $PY -I scripts/eval_tier2_homeobjects.py --label "$label" --model "$model" --imgsz "$sz" >> "logs/eval_${label}.log" 2>&1 || say "WARN 평가 실패 $label"
  else
    say "WARN 모델 없음 $model"
  fi
done

# 3) 지연: 평가 때문에 CPU가 붐비지 않도록 평가 뒤에 순서대로 잰다(학습도 끝난 상태).
say "지연 측정"
for m in models/yolo11n_tier2_fp32.onnx "$EV/export/A0_n_416.onnx" "$EV/export/A0_n_480.onnx" \
         "$EV/export/A1_n_320.onnx" "$EV/export/A1_n_416.onnx"; do
  if [ -f "$m" ]; then
    name=$(basename "$m" .onnx)
    $PY -I scripts/bench_tier2_latency.py --model "$m" > "$EV/latency_${name}.json" 2>> "$LOG" || say "WARN 지연 측정 실패 $m"
  fi
done

# 4) 표 생성
say "표 생성"
$PY -I scripts/report_tier2_tables.py --labels A0,A0-n416,A0-n480,A1,A1-n416 > "$EV/tables.md" 2>> "$LOG" || say "WARN 표 생성 실패"

# 5) 학습 곡선 요약(A1) — 조기 종료·best 에폭을 문서에 쓰기 위한 사실
$PY -I - >> "$LOG" 2>&1 <<'PYEOF'
import csv
r = [{k.strip(): v.strip() for k, v in x.items()} for x in csv.DictReader(open("runs/detect/runs/tier2_v4_n320/train/results.csv"))]
best = max(r, key=lambda x: float(x["metrics/mAP50(B)"]))
print(f"A1 epochs={len(r)} best_epoch={best['epoch']} best_mAP50={best['metrics/mAP50(B)']} last_mAP50={r[-1]['metrics/mAP50(B)']} train_minutes={float(r[-1]['time'])/60:.0f}")
PYEOF

say "pipeline done"
echo OK > "$EV/pipeline.done"
