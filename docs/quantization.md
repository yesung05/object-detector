# INT8 양자화 가이드

## 목적

이 문서는 YOLO11n-pose FP32 모델을 INT8로 양자화하는 절차와 주의사항, i5-4200U에서의 현실적인 기대치를 설명합니다.

---

## 왜 static 양자화인가

| 방식 | 특성 | 적합 모델 |
|------|------|-----------|
| **static** | 캘리브레이션 데이터로 activation 범위를 미리 계산 → QDQ 노드 삽입 | CNN (YOLO 등) |
| dynamic | 추론 시 MatMul 가중치만 양자화 | RNN, Transformer |

CNN은 대부분의 비용이 Conv에서 발생합니다. dynamic 양자화는 MatMul만 처리하므로 **CNN에는 거의 효과가 없습니다.** YOLO11n-pose에는 static을 사용합니다.

---

## 사전 요구 사항

```sh
pip install onnxruntime onnxruntime-tools onnx Pillow
```

캘리브레이션 이미지 디렉터리(`samples/`)에 JPG/PNG 파일을 준비합니다. 실제 매장 영상 프레임 100~300장이 이상적입니다. 장면 다양성이 중요하므로 조명 조건·밀도를 다양하게 포함합니다.

---

## 변환 절차

### 1단계 — 모델 내보내기 (포즈 모델)

```sh
python scripts/export_pose.py
# 출력: yolo11n-pose-416.onnx
```

### 2단계 — INT8 static 양자화

```sh
python scripts/quantize.py \
    --model yolo11n-pose-416.onnx \
    --calib-dir samples/ \
    --mode static
# 출력: yolo11n-pose-416-int8.onnx
```

스크립트 내부 처리 순서:
1. `quant_pre_process()` — shape inference + 그래프 최적화 적용 (생략 시 ORT가 경고 후 정확도 저하)
2. `YoloCalibReader` — C 코드와 동일한 letterbox 전처리(114 패딩, 중앙 정렬, /255.0, NCHW)로 변환
3. `quantize_static()` — QDQ 노드 삽입, 가중치 QInt8 변환

### 3단계 — 성능 비교

```powershell
# Windows
.\scripts\bench.ps1 -Input test.mp4 -Fp32Model yolo11n-pose-416.onnx -Int8Model yolo11n-pose-416-int8.onnx
```

```sh
# Linux / macOS
./scripts/bench.sh --input test.mp4 --fp32 yolo11n-pose-416.onnx --int8 yolo11n-pose-416-int8.onnx
```

---

## 기대 성능 — i5-4200U (Haswell)

| 항목 | 내용 |
|------|------|
| ISA | AVX2 (VNNI 없음) |
| INT8 경로 | `u8s8s32` AVX2 루틴 |
| 기대 속도 향상 | **1.5~2.5×** |

> **주의**: 자료에 따라 "2~4×"로 표기된 수치는 VNNI가 있는 Cascade Lake+ 기준입니다. Haswell은 VNNI가 없으므로 `u8s8s32` AVX2 경로를 타며 이론 최대치가 낮습니다. 실측 향상이 1.5× 미만이면 모델 구조나 메모리 대역폭이 병목일 수 있습니다.

---

## DirectML + INT8 비호환

Intel HD 4400 (배포 대상 기기)의 DirectML은 QDQ INT8을 올바르게 가속하지 못합니다. INT8 모델을 DirectML로 실행하면 FP32보다 느려질 수 있습니다.

- `--provider cpu`로 실행하면 AVX2 INT8 경로가 활성화됩니다.
- 프로그램은 INT8 모델 파일명을 감지하면 자동으로 경고를 출력합니다.

---

## 세션 최적화 옵션

빌드 시 자동 적용되는 ORT 세션 설정:

| 설정 | 효과 |
|------|------|
| `session.set_denormals_as_zero=1` | Haswell FP32 denormal 연산 (~수백 사이클) 제거 |
| `ORT_SEQUENTIAL` 실행 모드 | 저전력 모바일 CPU에서 스케줄러 오버헤드 감소 |
| intra-op spinning 비활성화 | 비추론 구간 CPU 점유율 억제 |

### 웜업(`--warmup N`)

시작 시 더미 추론을 N회 실행하여 JIT 컴파일·메모리 매핑 초기화를 서비스 시작 전에 소진합니다. 첫 실제 프레임의 지연 스파이크가 사라집니다. 권장값: 2~3회.

```sh
./yolo11-person --model yolo11n-pose-416-int8.onnx --warmup 3 ...
```

---

## 지연 통계 확인

`--metrics out.json`으로 실행하면 다음 필드가 포함됩니다:

```json
{
  "inference_p50_ms": 42.3,
  "inference_p95_ms": 61.8,
  "inference_max_ms": 87.2,
  ...
}
```

p95가 평균(p50)의 2배를 넘으면 서멀 스로틀링이 의심됩니다. 이 경우 `--detect-every` 값을 높이거나 입력 해상도를 낮춥니다.
