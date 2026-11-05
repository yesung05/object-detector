#!/bin/bash
# Tier 2 를 입력 416 으로 재학습하고 평가까지 자동 수행하는 일회성 파이프라인 (개발 PC 전용).
#
# 왜 재학습인가: 재학습 없이 입력만 416 으로 키우면(docs/tier2-homeobjects-before-after-2026-10-25.md) 병·컵·의자
# AP 는 오르지만 식탁의 박스 정확도(AP50-95)가 0.371 → 0.269 로 떨어졌다. 320 으로 학습한 모델이 큰 입력의 물체
# 크기 분포에 맞지 않는 것으로 보이며, 같은 해상도로 학습하면 풀릴 가능성이 있다(미검증 — 이 실험이 확인한다).
# 레시피는 기존 A0(tier2_v3_fresh)와 같고 해상도만 다르다: yolo11n.pt 에서 시작, 120에폭, AdamW lr0 0.001.
# 단, 배치는 128 이 아니라 64 다. 416 에서 배치 128 은 GPU 메모리 7.2GB 로, 데스크톱이 쓰는 1.6GB 와 합쳐 8.2GB 를 넘겨
# 공유 메모리로 밀려 에폭당 8.3분(120에폭 약 16시간)이 걸렸다(실측, 320 학습은 에폭당 약 0.9분). 배치가 달라 레시피가
# A0 와 완전히 같지 않다(Ultralytics 가 배치에 따라 가중치 감쇠를 조정) — 비교 문서에 그 사실을 적는다.
#
# 배포 모델(models/)은 건드리지 않는다(--no-deploy-copy). 채택은 결과를 보고 따로 판단한다.
# 사전 채택 기준(결과를 보기 전에 고정): v3 val mAP50 >= 0.540(= 재학습 없이 416 으로 돌린 A0), 식탁 AP50-95 >= 0.371,
#   병·컵 AP50 이 각각 0.401·0.454 에서 0.01 이상 떨어지지 않음.
# 종료·삭제를 하지 않는다. 각 단계는 실패해도 로그에 남기고 다음으로 넘어간다.

cd "$(dirname "$0")/.." || exit 1
# Git Bash 는 /E, /MT:8 같은 윈도 명령 옵션을 경로로 바꿔 버려 robocopy 가 rc=16 으로 실패했다(실측). 변환을 끈다.
export MSYS2_ARG_CONV_EXCL="*"
PY=.venv-train/Scripts/python.exe
EV=runs/tier2-homeobjects-eval
LOG=logs/retrain_tier2_416.log
SRC='H:/DMU/Contest/hunik/object-detector/datasets/tier2_v3'
DST="$(cygpath "$LOCALAPPDATA")/hunik-train/tier2_v3"
DSTW="$(cygpath -m "$DST")"
mkdir -p logs "$EV/export" "$DST"
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

say "시작 — 학습 데이터를 NVMe 로 복사(원본 H: 는 USB 라 읽기 17MB/s 로 GPU 가 논다)"
for sub in images/train images/val labels/train labels/val; do
  robocopy "$(cygpath -w "$SRC/$sub")" "$(cygpath -w "$DST/$sub")" /E /MT:8 /NFL /NDL /NP /R:1 /W:1 > /dev/null
  rc=$?; [ $rc -ge 8 ] && { say "ERROR robocopy $sub rc=$rc"; echo FAILED > "$EV/retrain416.done"; exit 1; }
done
say "복사 완료: train $(ls "$DST/images/train" | wc -l) val $(ls "$DST/images/val" | wc -l)"
cat > "$DST/tier2_v3_nvme.yaml" <<EOF
train: $DSTW/images/train
val:   $DSTW/images/val
nc: 7
names: ['cat', 'dog', 'bottle', 'cup', 'food', 'chair', 'dining table']
EOF

say "학습 시작 (imgsz 416)"
$PY -I scripts/run_train_tier2.py --data "$DSTW/tier2_v3_nvme.yaml" --weights yolo11n.pt --imgsz 416 \
  --epochs 120 --batch 64 --device 0 --workers 8 --amp --patience 35 --optimizer AdamW --lr0 0.001 --lrf 0.01 \
  --warmup-epochs 5 --warmup-bias-lr 0.001 --weight-decay 0.0005 --project runs/tier2_v3_416 \
  --no-test --no-int8 --no-deploy-copy > logs/train_R416.log 2>&1
say "학습 종료 rc=$?"

TRAIN_DIR=runs/detect/runs/tier2_v3_416/train
[ -d "$TRAIN_DIR" ] || TRAIN_DIR=$(ls -d runs/**/tier2_v3_416/train 2>/dev/null | head -1)
BEST="$TRAIN_DIR/weights/best_fp32.onnx"
if [ ! -f "$BEST" ]; then say "ERROR best_fp32.onnx 없음: $TRAIN_DIR"; echo FAILED > "$EV/retrain416.done"; exit 1; fi
cp "$BEST" "$EV/export/R416_n_416.onnx"
$PY -I - >> "$LOG" 2>&1 <<'PYEOF'
import csv, glob
f = glob.glob("runs/**/tier2_v3_416/train/results.csv", recursive=True)[0]
r = [{k.strip(): v.strip() for k, v in x.items()} for x in csv.DictReader(open(f))]
b = max(r, key=lambda x: float(x["metrics/mAP50(B)"]))
print(f"R416 epochs={len(r)} best_epoch={b['epoch']} best_mAP50={b['metrics/mAP50(B)']} train_minutes={float(r[-1]['time'])/60:.0f}")
PYEOF

say "평가 R416 (v3 val + HomeObjects val)"
$PY -I scripts/eval_tier2_homeobjects.py --label R416 --model "$EV/export/R416_n_416.onnx" --imgsz 416 > logs/eval_R416.log 2>&1 || say "WARN 평가 실패"
say "지연 측정"
for m in models/yolo11n_tier2_fp32.onnx "$EV/export/A0_n_416.onnx" "$EV/export/R416_n_416.onnx"; do
  [ -f "$m" ] && $PY -I scripts/bench_tier2_latency.py --model "$m" > "$EV/latency_$(basename "$m" .onnx).json" 2>> "$LOG"
done
$PY -I scripts/report_tier2_tables.py --labels A0,A0-n416,R416 > "$EV/tables_R416.md" 2>> "$LOG"
say "pipeline done"
echo OK > "$EV/retrain416.done"
