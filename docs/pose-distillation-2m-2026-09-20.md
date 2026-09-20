# 약 200만 파라미터 포즈 모델: 증류 연결 수정 및 재학습

## 결과

학습 모델 2,024,690개 파라미터, 추론용 fusion 후 2,018,192개로 재학습을 완료했다. Teacher는 기존 `yolo11n-pose.pt`이고, 사람 1개 클래스·17개 관절 출력은 유지했다.

증류 loss와 student gradient 연결은 실제 학습 모델에서 검증했다. 세 FP32 ONNX의 평균 CPU 지연시간은 원본보다 약 4~10%, CPU 시간/추론은 약 3~10% 감소했다. 파일 크기는 약 29% 감소했다.

그러나 최종 ONNX의 pose mAP50 손실은 15.04~20.72%p다. 정확도 손실을 1~2%p 이내로 유지하려는 목표에는 미달한다. 이번 모델은 추가 실험용이며 기본 배포 모델로 전환하지 않았다.

## 수정 내용과 학습 조건

- 이전 구현은 학습 전 모델 인스턴스에 loss를 붙였지만, 학습기가 모델을 다시 만들면서 연결이 유지되지 않았다. 이전 결과는 정상적인 증류의 효과로 해석할 수 없다.
- 이번 구현은 설치된 Ultralytics 8.4.147의 `distill_model` 경로를 사용한다. Teacher 신뢰도로 가중한 다중 스케일 neck feature L2 loss와 정답 기반 pose loss를 함께 학습한다. 별도의 관절 출력 증류는 사용하지 않았다.
- 기존 스크립트 `distill_pose_fp32.py`도 native 경로와 연결 검사로 수정했다.
- Student 구조는 depth=0.5, width=0.25, max_channels=672다. 앞쪽 구조를 유지하고 넓은 뒤쪽 채널을 줄였다. 사전학습 가중치는 형상이 일치하는 392/541 항목이 전달됐으며, 모든 가중치가 계승된 것은 아니다.
- 학습 데이터: 기존 COCO person-keypoint 변환 train 4,000장, val 500장. 검증 세트의 사람 인스턴스는 1,581개다. 전체 COCO 공식 평가 결과가 아니다.
- 공통 student 1개를 20 epoch 학습했다. `imgsz=416, rect=True`로 종횡비별 배치를 사용했고, 같은 best checkpoint를 세 고정 입력 크기로 export했다. 비율마다 별도 미세조정한 모델은 아니다.
- Batch=16, AdamW, 초기 learning rate=0.001, 최종 비율=0.05, warmup=2 epoch, seed=4200, feature distillation weight=0.5, workers=0.
- Mosaic와 mixup은 끄고 scale=0.25, translate=0.1을 사용했다. 학습에는 AMP를 사용했으며 추론용 ONNX는 FP32다.
- RTX 3060 Ti 8GB에서 학습 약 28.2분. 학습 로그의 GPU 메모리는 후반 약 1.45GB였고, 중간에 확인한 장치 전체 사용량은 3,228/8,192MiB였다. 두 값은 측정 범위가 다르다.
- Best는 학습기 기본 fitness(box mAP50–95 + pose mAP50–95) 기준으로 선택했다.

### 연결 검증

실제 학습기의 `on_train_start`에서 첫 학습 배치를 넣고 아래 조건을 검사했다. 실패하면 학습을 중단한다.

| 검사 | 결과 |
|---|---:|
| 학습 모델에 DistillationModel 설치 | 통과 |
| Teacher eval 및 모든 파라미터 고정 | 통과 |
| 첫 배치 증류 loss | 0.846080 |
| 증류 항만으로 계산한 student gradient norm | 22.178628 |
| 기울기 유한값 및 teacher gradient 없음 | 통과 |
| Feature 입력 레이어 | 16, 19, 22 |
| Teacher score 출력 레이어 | 23 |

[연결 검사 원본](../runs/pose/pose-distill-2m/distillation-audit.json), [학습 로그 CSV](../runs/pose/pose-distill-2m/results.csv), [학습 설정](../runs/pose/pose-distill-2m/args.yaml).

학습 최종 검증에서 pose mAP50은 약 0.523, mAP50–95는 약 0.197이었다. 아래 표는 별도로 실시한 최종 고정 크기 ONNX 평가다.

## 최종 ONNX 정확도

각 모델에 동일한 500장과 동일한 letterbox·NMS·OKS/AP 계산을 적용했다. 값은 0~1 범위다. NMS confidence=0.001, IoU=0.7, max_det=300.

| 입력 W×H | 모델 | Pose mAP50 | Pose mAP50–95 | Box mAP50 | Box mAP50–95 |
|---|---|---:|---:|---:|---:|
| 416×224 | 원본 | 0.6126 | 0.3248 | 0.8206 | 0.5634 |
| 416×224 | 2M 증류 | 0.4622 | 0.1795 | 0.7528 | 0.4654 |
| 416×288 | 원본 | 0.6895 | 0.3787 | 0.8453 | 0.6011 |
| 416×288 | 2M 증류 | 0.5098 | 0.2001 | 0.7910 | 0.5020 |
| 416×416 | 원본 | 0.7296 | 0.4111 | 0.8747 | 0.6334 |
| 416×416 | 2M 증류 | 0.5223 | 0.1966 | 0.7988 | 0.5116 |

설치된 기본 ONNX validator는 정사각형 입력을 가정한다. `evaluate_pose_onnx.py`는 데이터셋의 letterbox 목표를 ONNX의 실제 H×W로 고정하고, 표준 PoseValidator의 NMS·지표 계산을 재사용한다. 모든 입력 shape와 평가 이미지 수를 확인했다.

## CPU 성능

Intel Core i7-8700, Windows, ONNX Runtime 1.30.0 CPU EP. Intra-op=2, inter-op=1, sequential, graph optimization=all, spinning off. 고정 seed=4200 random FP32 입력, warmup=20, 측정=300회. 학습 종료 후 원본·student를 해상도별로 번갈아 측정했다. 카메라·전처리·후처리를 제외한 모델 단독 시간이다.

| 입력 | 모델 | 크기 MiB | 평균 ms | p50 ms | p95 ms | CPU ms/추론 |
|---|---|---:|---:|---:|---:|---:|
| 416×224 | 원본 | 11.106 | 14.607 | 13.817 | 18.537 | 26.146 |
| 416×224 | 2M 증류 | 7.870 | 13.120 | 12.132 | 16.625 | 23.438 |
| 416×288 | 원본 | 11.123 | 17.068 | 16.304 | 20.599 | 30.573 |
| 416×288 | 2M 증류 | 7.887 | 16.400 | 15.517 | 19.651 | 29.688 |
| 416×416 | 원본 | 11.156 | 24.927 | 24.951 | 29.797 | 45.677 |
| 416×416 | 2M 증류 | 7.920 | 23.609 | 23.588 | 27.551 | 42.969 |

| 입력 | 평균 지연시간 감소 | CPU 시간/추론 감소 | Pose mAP50 손실 |
|---|---:|---:|---:|
| 416×224 | 10.18% | 10.36% | 15.04%p |
| 416×288 | 3.91% | 2.89% | 17.98%p |
| 416×416 | 5.29% | 5.93% | 20.72%p |

파라미터 감소율과 실행시간 감소율은 같지 않다. 이번 구조는 앞쪽 고해상도 연산을 많이 유지한다. 짧은 반복 측정의 작은 차이는 시스템 상태에 영향을 받으며 i5-4200U 직접 측정값은 아니다. 현재 정확도 손실에 비해 속도 이득이 작으므로 배포 교체는 권장하지 않는다.

## 산출물 및 재현

- [Best checkpoint](../runs/pose/pose-distill-2m/weights/best.pt)
- [416×224 FP32 ONNX](../runs/pose/pose-distill-2m/yolo11n-pose-2m-416x224.onnx)
- [416×288 FP32 ONNX](../runs/pose/pose-distill-2m/yolo11n-pose-2m-416x288.onnx)
- [416×416 FP32 ONNX](../runs/pose/pose-distill-2m/yolo11n-pose-2m-416x416.onnx)
- [정확도·CPU 원본 JSON 및 모델 SHA256](../runs/pose/pose-distill-2m/onnx-evaluation.json)
- [ONNX 구조·FP32·PyTorch 수치 일치 검사](../runs/pose/pose-distill-2m/export-verification.json)
- [학습 스크립트](../scripts/distill_pose_2m.py), [구조 YAML](../scripts/yolo11n-pose-2m.yaml)

ONNX checker와 세 입력 크기의 PyTorch 출력 비교가 통과했다(rtol=0.001, atol=0.002). 최대 절대 오차는 0.000702 이하였으며 FP16 initializer와 양자화 연산이 없음을 검사했다. 실험용 파일은 자동 선택되는 기본 models 폴더에 추가하지 않았다.

```powershell
.\.venv-train\Scripts\python.exe scripts\distill_pose_2m.py --epochs 20 --batch 16 --name pose-distill-2m
.\.venv-train\Scripts\python.exe scripts\verify_pose_exports.py runs\pose\pose-distill-2m
```

같은 이름으로 다시 학습하면 기존 결과를 보존하도록 새 run 디렉터리가 생성된다. 검증 명령에는 실제 생성된 경로를 사용한다.

## 다음 실험

사용자 요청에 따라 다음 실행의 기본 batch는 32로 변경했다. 이번 실행은 16으로 완료됐다. 32부터 메모리·이미지/초를 확인하고 여유가 있으면 64를 비교한다. 메모리 사용량을 늘리는 것 자체가 정확도 향상을 의미하지는 않는다.

정확도 개선을 위해서는 데이터 확대, 더 충분한 학습, 부분적으로 초기화된 층의 회복, 관절에 직접 대응하는 증류 항 등을 비교해야 한다. 이번에는 증류 없는 동일 student 대조군을 학습하지 않았으므로 개선분을 증류만의 효과로 분리할 수 없다.

원본 모델도 실제 카메라 환경의 검수된 관절 라벨로 미세조정할 수 있다. 원본 구조와 배포 해상도를 유지한다면 연산량 증가 없이 정확도를 개선할 가능성이 있다. 단, 일반 COCO 성능 향상과 특정 현장 성능 향상은 구분해야 한다. 원본을 개선한 뒤 teacher로 활용하되, 장면·영상 단위로 분리한 테스트 세트로 과적합 여부를 확인하는 후속 실험이 적절하다. 이번 작업에서는 원본 재학습을 수행하지 않았다.
