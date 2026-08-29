#!/usr/bin/env bash
# FP32 vs INT8 추론 성능 비교 스크립트 (Linux/macOS)
#
# 사용법:
#   ./scripts/bench.sh --input test.mp4 --fp32 yolo11n-pose-416.onnx [--int8 yolo11n-pose-416-int8.onnx]

set -euo pipefail

EXE="${EXE:-./build/yolo11-person}"
DETECT_EVERY=1
MAX_FRAMES=300
WARMUP=3
FP32_MODEL=""
INT8_MODEL=""
INPUT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --input)       INPUT="$2";       shift 2 ;;
        --fp32)        FP32_MODEL="$2";  shift 2 ;;
        --int8)        INT8_MODEL="$2";  shift 2 ;;
        --exe)         EXE="$2";         shift 2 ;;
        --detect-every) DETECT_EVERY="$2"; shift 2 ;;
        --max-frames)  MAX_FRAMES="$2";  shift 2 ;;
        --warmup)      WARMUP="$2";      shift 2 ;;
        *) echo "알 수 없는 옵션: $1" >&2; exit 1 ;;
    esac
done

[[ -z "$INPUT"      ]] && { echo "--input 필수" >&2; exit 1; }
[[ -z "$FP32_MODEL" ]] && { echo "--fp32 필수" >&2; exit 1; }

measure() {
    local label="$1" model="$2"
    local tmp; tmp="$(mktemp /tmp/bench_metrics_XXXXXX.json)"
    echo "측정 중: $label ..." >&2
    "$EXE" \
        --model "$model" --input "$INPUT" \
        --detect-every "$DETECT_EVERY" \
        --warmup "$WARMUP" \
        --max-frames "$MAX_FRAMES" \
        --provider cpu \
        --metrics "$tmp" 2>/dev/null || true
    if [[ ! -s "$tmp" ]]; then
        echo "$label: metrics 파일을 찾을 수 없습니다." >&2
        rm -f "$tmp"; return
    fi
    # python3 또는 jq 로 값 추출
    if command -v python3 &>/dev/null; then
        python3 - "$label" "$tmp" <<'PY'
import sys, json
label, path = sys.argv[1], sys.argv[2]
d = json.load(open(path))
print(f"{label}|{d['inference_runs']}|{d['inference_seconds']:.3f}|"
      f"{d['inference_p50_ms']:.2f}|{d['inference_p95_ms']:.2f}|"
      f"{d['inference_max_ms']:.2f}|{d['cpu_ms_per_frame']:.2f}")
PY
    elif command -v jq &>/dev/null; then
        jq -r "\"$label|\" + (.inference_runs|tostring) + \"|\" + (.inference_seconds|tostring) + \"|\" + (.inference_p50_ms|tostring) + \"|\" + (.inference_p95_ms|tostring) + \"|\" + (.inference_max_ms|tostring) + \"|\" + (.cpu_ms_per_frame|tostring)" "$tmp"
    else
        echo "$label|N/A|N/A|N/A|N/A|N/A|N/A"
    fi
    rm -f "$tmp"
}

printf '\n%s\n' "$(printf '=%.0s' {1..70})"
printf '%-8s %8s %12s %8s %8s %8s %12s\n' \
    "모델" "추론횟수" "총추론(s)" "p50(ms)" "p95(ms)" "max(ms)" "CPU ms/frame"
printf '%s\n' "$(printf -- '-%.0s' {1..70})"

fp32_inf=""
int8_inf=""

while IFS='|' read -r label runs inf_s p50 p95 maxms cpu_ms; do
    printf '%-8s %8s %12s %8s %8s %8s %12s\n' \
        "$label" "$runs" "$inf_s" "$p50" "$p95" "$maxms" "$cpu_ms"
    [[ "$label" == "FP32" ]] && fp32_inf="$inf_s"
    [[ "$label" == "INT8" ]] && int8_inf="$inf_s"
done < <(
    measure "FP32" "$FP32_MODEL"
    [[ -n "$INT8_MODEL" ]] && measure "INT8" "$INT8_MODEL"
)

printf '%s\n' "$(printf '=%.0s' {1..70})"

if [[ -n "$fp32_inf" && -n "$int8_inf" ]] && command -v python3 &>/dev/null; then
    python3 -c "
fp32=$fp32_inf; int8=$int8_inf
if int8 > 0:
    print(f'\nINT8 속도 향상 (총 추론 시간): {fp32/int8:.2f}x')
    print('참고: i5-4200U (AVX2, VNNI 없음) 기대치는 1.5~2.5x 입니다.')
"
fi
