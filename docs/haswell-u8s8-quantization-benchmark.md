# i5-4200U용 U8S8 양자화 실험 및 벤치마크

> 실험일: 2026-09-12
>
> 측정 장비: Intel Core i7-8700, ONNX Runtime 1.30.0 CPU EP
>
> 목적: Haswell i5-4200U 후보 양자화 형식을 만들고 속도와 출력 유효성을 선별
>
> 주의: 4200U 자체 실측은 아니며, 최종 선택에는 대상 장비 재측정이 필요하다.

## 결론

현재 생성한 U8S8 양자화 모델 중 **속도와 출력 정확성을 모두 만족한 모델은 없었다.**

- 전체 U8S8 QOperator는 FP32보다 11% 빨랐지만 confidence가 모두 0이어서 사용할 수 없다.
- Conv만 U8S8로 바꾼 혼합 모델은 confidence가 정상적으로 나왔지만 FP32보다 16% 느렸다.
- 기존 INT8은 출력은 유지하지만 FP32보다 89% 느렸다.
- 따라서 현시점 배포 후보는 **FP32 Pose + FP32 Tier2**다.
- 활성 배포 모델은 교체하지 않았다. 신규 모델은 모두 비교용 이름으로 보존했다.

## 양자화 조건

학습 데이터 누수를 막기 위해 `datasets/tier2_v3/images/train`에서 고정 시드 4200으로 300장을 표본화했다.

| 항목 | 설정 |
|---|---|
| 방식 | 정적 post-training quantization |
| activation | UInt8 |
| weight | Int8 |
| 형식 | QOperator |
| 캘리브레이션 | MinMax, 학습 이미지 300장 |
| 전처리 | 모델 크기 letterbox, RGB, `/255`, NCHW |
| 모델 전처리 | ORT shape inference + graph optimization |

전체 그래프 양자화와 `Conv` 전용 혼합 양자화를 각각 시험했다. `per_channel + reduce_range`, full-range, QDQ도 별도로 확인했지만 전체 그래프 방식은 모두 confidence가 0이 되는 동일한 문제가 발생했다.

## 속도 결과

각 모델을 5회 워밍업한 후 100회 측정했다. ORT는 sequential execution, CPU EP, intra-op 2스레드, spinning off로 설정했다. 카메라 디코딩과 후처리는 포함하지 않은 순수 모델 시간이다.

| 형식 | Pose 평균 | Tier2 평균 | 직렬 합계 평균 | 합계 p95 | FP32 대비 |
|---|---:|---:|---:|---:|---:|
| **FP32** | **22.46ms** | **12.71ms** | **35.17ms** | **37.70ms** | 기준 |
| 기존 INT8 | 41.74ms | 24.72ms | 66.46ms | 75.27ms | 89% 느림 |
| U8S8 QOperator 전체 | 19.91ms | 11.32ms | 31.23ms | 35.74ms | 11% 빠름, **출력 불량** |
| U8S8 QOperator Conv 전용 | 26.19ms | 14.56ms | 40.75ms | 43.14ms | 16% 느림 |

전체 U8S8이 계산 성능 면에서는 효과가 있었다. 그러나 후술한 confidence 손상 때문에 이 수치를 배포 성능으로 사용할 수 없다.

## 출력 유효성 검사

Tier2 테스트 이미지 100장을 사용해 FP32와 양자화 모델의 raw output을 비교했다. 이 검사는 모델이 깨졌는지 확인하는 수치 검사이며 mAP 또는 Pose AP 평가를 대신하지 않는다.

| 후보 | 모델 | confidence MAE | 최고 confidence 차이 | 최고 anchor 일치 | 판정 |
|---|---|---:|---:|---:|---|
| 기존 INT8 | Pose | 0.000314 | 0.0218 | 62% | 출력 유지 |
| 기존 INT8 | Tier2 | 0.000427 | 0.0856 | 29% | 출력 유지, 정확도 평가 필요 |
| U8S8 전체 | Pose | 0.002915 | 0.4592 | 0% | confidence 전부 0 |
| U8S8 전체 | Tier2 | 0.001125 | 0.6774 | 0% | confidence 전부 0 |
| U8S8 Conv 전용 | Pose | 0.000658 | 0.0443 | 33% | 동작하나 기존 INT8보다 차이 큼 |
| U8S8 Conv 전용 | Tier2 | 0.000751 | 0.1301 | 29% | 동작하나 기존 INT8보다 차이 큼 |

전체 U8S8 모델은 출력 텐서 형상 자체는 원본과 같지만 실제 confidence 값이 0이었다. 파일 로드 성공만으로 모델을 채택하면 안 되는 이유다.

## 모델 크기

| 모델 | FP32 | 기존 INT8 | U8S8 전체 | U8S8 Conv 전용 |
|---|---:|---:|---:|---:|
| Pose | 11.16MiB | 3.12MiB | 3.08MiB | 3.06MiB |
| Tier2 | 10.00MiB | 2.80MiB | 2.75MiB | 2.70MiB |

U8S8은 저장 크기를 약 73% 줄였지만, 이 장비에서는 사용 가능한 Conv 전용 모델의 속도 이득이 없었다.

## 생성된 실험 모델

- `models/yolo11n_pose_416_u8s8_qop.onnx`: 빠르지만 confidence 불량
- `models/yolo11n_tier2_u8s8_qop.onnx`: 빠르지만 confidence 불량
- `models/yolo11n_pose_416_u8s8_qop_conv.onnx`: 출력 유지, FP32보다 느림
- `models/yolo11n_tier2_u8s8_qop_conv.onnx`: 출력 유지, FP32보다 느림

`fullrange`, `qdq` 이름의 추가 파일도 실패 원인 비교용이며 배포하면 안 된다.

## 재현 명령

Conv 전용 U8S8 생성:

```powershell
.\.venv-train\Scripts\python.exe scripts\quantize.py `
  --model yolo11n-pose-416.onnx `
  --calib-dir datasets\tier2_v3\images\train `
  --output models\yolo11n_pose_416_u8s8_qop_conv.onnx `
  --format qoperator --activation-type quint8 --weight-type qint8 `
  --op-types Conv --calib-limit 300 --calib-seed 4200
```

동시 직렬 벤치마크:

```powershell
.\.venv-train\Scripts\python.exe scripts\benchmark_dual_onnx.py `
  --pose models\yolo11n_pose_416_u8s8_qop_conv.onnx `
  --tier2 models\yolo11n_tier2_u8s8_qop_conv.onnx `
  --threads 2 --runs 100
```

출력 비교:

```powershell
.\.venv-train\Scripts\python.exe scripts\compare_onnx_outputs.py `
  --fp32 models\yolo11n_tier2_fp32.onnx `
  --quantized models\yolo11n_tier2_u8s8_qop_conv.onnx `
  --images datasets\tier2_v3\images\test --limit 100
```

## 4200U에서 할 최종 시험

개발 PC 결과만으로 4200U 성능을 단정할 수는 없다. 대상 장비에서 다음 두 조합만 우선 비교하면 된다.

1. FP32 Pose + FP32 Tier2
2. U8S8 Conv 전용 Pose + U8S8 Conv 전용 Tier2

4200U에서도 Conv 전용 U8S8이 FP32보다 느리면 FP32를 확정한다. 전체 U8S8 모델은 속도와 관계없이 confidence 불량으로 후보에서 제외한다.
