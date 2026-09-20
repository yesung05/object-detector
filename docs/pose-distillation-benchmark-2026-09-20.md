# FP32 비율별 증류 모델 성능·CPU 부하 테스트

측정일: 2026-09-20

> 정정: 후속 코드 점검에서 이 실험의 학습 전 loss monkey patch가 학습기가 새로 생성한 모델에 전달되지 않는 문제가 확인됐다. 아래 파일 크기·CPU 측정값은 유효하지만, 이 실험은 정상적인 지식 증류 효과의 근거로 사용할 수 없다. 낮은 정확도를 학습 횟수만의 문제로 해석해서도 안 된다. 후속 `scripts/distill_pose_2m.py`는 학습기의 native feature distillation을 사용하며 실제 학습 모델에서 KD loss와 student gradient를 검사한다.

## 결론

FP32 증류 모델은 기존 `yolo11n-pose` FP32 모델보다 훨씬 작고 빠르다. 현재 i7-8700의 ONNX Runtime CPU 측정에서는 해상도별 평균 지연시간이 약 47~49% 줄고, 추론 1회당 CPU 시간이 약 50~55% 줄었다.

다만 이번 5 epoch 파일럿 학습의 정확도는 원본 teacher보다 크게 낮다. 따라서 현재 생성된 증류 ONNX는 속도 실험용이며, 배포 모델로 사용하면 안 된다. 실사용을 위해서는 더 큰 학습 데이터와 긴 학습, 그리고 pose head에 맞는 distillation loss 재설계가 필요하다.

## 모델 구성

| 구분 | 내용 |
|---|---|
| Teacher | `yolo11n-pose.pt` |
| Student | YOLO11 pose 기반 축소 구조, 약 66.7만 parameter |
| Student 학습 | COCO person-keypoint train 4,000장 / val 500장 |
| Distillation | teacher의 box·class·keypoint raw output에 Smooth L1 loss, weight `0.05` |
| 학습 | 5 epoch, batch 16, RTX 3060 Ti |
| 배포 형식 | FP32 ONNX, 입력 폭 416 고정 |
| 비율 | `416x224`(16:9), `416x288`(4:3·16:10), `416x416`(1:1) |

학습 시 Ultralytics의 rectangular training 제약 때문에 각 비율의 짧은 변을 기준으로 정사각형(`224`, `288`, `416`)으로 학습한 뒤, 배포용 ONNX를 `[height, 416]`으로 export했다. 따라서 정확도 비교 수치는 정사각형 validation 결과이고, CPU 성능 수치는 최종 rectangular ONNX 결과다.

## CPU 성능

측정 조건은 현재 개발 PC의 ONNX Runtime CPU Execution Provider, intra-op thread 2, inter-op thread 1, warmup 10회, 본 측정 100회, 고정 random float32 입력이다. 전처리·후처리·카메라 I/O는 제외했다.

| 입력 | 모델 | 크기 | 평균 | 중앙값 | p95 | CPU 시간/추론 |
|---|---|---:|---:|---:|---:|---:|
| 416×224 | 기존 FP32 | 11.106 MiB | 13.823 ms | 12.819 ms | 17.162 ms | 25.000 ms |
| 416×224 | 증류 FP32 | 2.714 MiB | 7.313 ms | 6.627 ms | 10.146 ms | 11.250 ms |
| 416×288 | 기존 FP32 | 11.123 MiB | 17.555 ms | 17.184 ms | 21.809 ms | 31.250 ms |
| 416×288 | 증류 FP32 | 2.731 MiB | 9.193 ms | 8.621 ms | 12.039 ms | 15.469 ms |
| 416×416 | 기존 FP32 | 11.156 MiB | 24.529 ms | 24.954 ms | 28.314 ms | 43.750 ms |
| 416×416 | 증류 FP32 | 2.764 MiB | 12.968 ms | 11.896 ms | 16.423 ms | 22.188 ms |

### 기존 모델 대비 감소율

| 입력 | 파일 크기 | 평균 지연시간 | CPU 시간/추론 |
|---|---:|---:|---:|
| 416×224 | -75.57% | -47.10% | -55.00% |
| 416×288 | -75.46% | -47.64% | -50.50% |
| 416×416 | -75.22% | -47.13% | -49.29% |

기존 C 실행 경로에서 자동 비율 선택을 사용하므로, 모델 파일명도 기존 규칙에 맞춰 저장했다.

- [증류 416×224 ONNX](../models/yolo11n-pose-distilled-416x224.onnx)
- [증류 416×288 ONNX](../models/yolo11n-pose-distilled-416x288.onnx)
- [증류 416×416 ONNX](../models/yolo11n-pose-distilled-416x416.onnx)

## 정확도 검증

동일한 500장 validation subset에서 teacher와 student의 pose mAP를 비교했다.

| 입력 | 모델 | Pose mAP50 | Pose mAP50-95 | Box mAP50 | Box mAP50-95 |
|---|---|---:|---:|---:|---:|
| 224 | Teacher | 0.5056 | 0.2422 | 0.7395 | 0.4856 |
| 224 | 증류 Student | 0.0051 | 0.0010 | 0.1746 | 0.0521 |
| 288 | Teacher | 0.6285 | 0.3203 | 0.8317 | 0.5708 |
| 288 | 증류 Student | 0.0005 | 0.0001 | 0.1706 | 0.0531 |
| 416 | Teacher | 0.7321 | 0.4118 | 0.8963 | 0.6515 |
| 416 | 증류 Student | 0.0012 | 0.0001 | 0.1769 | 0.0573 |

현재 결과는 “작은 FP32 구조가 CPU에서 빠르다”는 점은 확인하지만, “teacher 성능을 유지하는 증류가 완료됐다”는 뜻은 아니다. 특히 pose mAP가 거의 0에 가까워 현재 student를 제품에 넣을 수 없다.

## 4200U에 대한 해석

이번 숫자는 i7-8700에서 직접 측정한 값이므로 i5-4200U의 절대 지연시간을 보장하지 않는다. 그래도 student가 연산량과 parameter를 크게 줄였기 때문에 4200U에서도 기존 FP32보다 유리할 가능성이 높다. 다만 4200U는 Haswell 2코어/4스레드이고 메모리·열 상태의 영향도 크므로, 최종 선택은 실제 장치에서 `threads=1`과 `threads=2`를 각각 측정해야 한다.

현재 배포 권장 순서는 다음과 같다.

1. 정확도가 필요한 경우: 기존 비율별 FP32 모델을 사용한다.
2. CPU 부하 실험이 필요한 경우: 증류 모델을 사용하되 결과가 정확도 미달임을 전제로 한다.
3. 제품용 student가 필요하면 4,000장보다 큰 train set, 최소 수십 epoch, pose-aware teacher target/feature distillation을 적용해 다시 학습한다.

학습 스크립트: [`scripts/distill_pose_fp32.py`](../scripts/distill_pose_fp32.py)
