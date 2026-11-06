#!/usr/bin/env bash
# 보호 모드 화면 모양(privacy_style)의 CPU 비용 비교 — 개발 PC 전용
#
# 같은 매장 클립(180프레임)을 같은 exe 로 --video-clock 모드에서 처리하고, 설정만 바꿔 처리 속도와 프레임당 CPU 시간을 잰다.
#   off      보호 모드 끔(가림 없음)                                  — 기준선
#   solid    단색
#   edge_v1  엣지, 처음 구현(블러 1, 번짐 0, 농도 100)
#   edge_v2  엣지, 블러 3(번짐 0, 농도 100)                          — "덜 선명하게" 변경 전
#   edge_v3  엣지, 블러 8, 번짐 1, 농도 70(현재 기본값)               — 변경 후
# edge_* 는 privacy_style_fps 30(관찰 중 거의 매 프레임 렌더: 최악)과 5(기본)를 각각 잰다.
# 노이즈를 줄이려고 설정마다 REPS 번 돌려 처리 시간(초)의 중앙값을 쓴다. 출력 파일 인코딩이 포함된 값이라 절대값이 아니라
# 설정 간 "차이"만 본다.
#
# 사용: bash scripts/bench_privacy_style.sh <detector exe> <ffmpeg 가 있는 폴더가 PATH 에 잡힌 상태> <클립> <출력 폴더> [REPS=3]
set -u
EXE="$1"; CLIP="$2"; OUT="$3"; REPS="${4:-3}"
MODEL='models/yolo11n-pose-416x224.onnx'
mkdir -p "$OUT"
: > "$OUT/results.tsv"
run() {  # name fps json_extra
  local name="$1" fps="$2" extra="$3" i port
  for i in $(seq 1 "$REPS"); do
    port=$((19100 + RANDOM % 800))
    printf '{"stream_port":%d,"stream_privacy_style":1%s}\n' "$port" "$extra" > "$OUT/c.json"
    "$EXE" --model "$MODEL" --provider cpu --threads 2 --input "$CLIP" --video-clock --config "$OUT/c.json" \
        --stream-port "$port" --output "$OUT/o.mp4" > "$OUT/r.log" 2>&1
    # 예: "done: 180 frames, 180 inference runs, 9.67 s (18.62 fps), CPU 11.72 s (65.10 ms/frame)"
    local line; line=$(grep -a "^done:" "$OUT/r.log" | tail -1)
    local secs fpsv cpu; secs=$(echo "$line" | sed -E 's/.*runs, ([0-9.]+) s.*/\1/'); fpsv=$(echo "$line" | sed -E 's/.*\(([0-9.]+) fps\).*/\1/'); cpu=$(echo "$line" | sed -E 's/.*\(([0-9.]+) ms\/frame\).*/\1/')
    printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$fps" "$secs" "$fpsv" "$cpu" >> "$OUT/results.tsv"
  done
}
run off 0      ',"stream_privacy_mode":0'
run solid 0    ',"stream_privacy_mode":1,"stream_privacy_style":0'
for f in 30 5; do
  run edge_v1 $f ",\"stream_privacy_mode\":1,\"privacy_style_fps\":$f,\"privacy_edge_blur\":1,\"privacy_edge_soft\":0,\"privacy_edge_opacity\":100"
  run edge_v2 $f ",\"stream_privacy_mode\":1,\"privacy_style_fps\":$f,\"privacy_edge_blur\":3,\"privacy_edge_soft\":0,\"privacy_edge_opacity\":100"
  run edge_v3 $f ",\"stream_privacy_mode\":1,\"privacy_style_fps\":$f,\"privacy_edge_blur\":8,\"privacy_edge_soft\":1,\"privacy_edge_opacity\":70"
done
# 중앙값 요약
sort -t$'\t' -k1,1 -k2,2n "$OUT/results.tsv" | awk -F'\t' '
  { key=$1 "\t" $2; n[key]++; s[key,n[key]]=$3; f[key,n[key]]=$4; c[key,n[key]]=$5; if(!(key in order)){order[key]=++k; names[k]=key} }
  function med(arr,key,cnt,   i,j,t,a){ for(i=1;i<=cnt;i++)a[i]=arr[key,i]; for(i=1;i<=cnt;i++)for(j=i+1;j<=cnt;j++)if(a[j]+0<a[i]+0){t=a[i];a[i]=a[j];a[j]=t}; return a[int((cnt+1)/2)] }
  END { for(i=1;i<=k;i++){ key=names[i]; printf "%s\tn=%d\tsec=%s\tfps=%s\tcpu_ms=%s\n", key, n[key], med(s,key,n[key]), med(f,key,n[key]), med(c,key,n[key]) } }' > "$OUT/summary.tsv"
cat "$OUT/summary.tsv"
