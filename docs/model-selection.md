# 모델 선택 분석: YOLO11n vs YOLO11n-pose

## 모델 차이

| | YOLO11n | YOLO11n-pose |
|---|---|---|
| 출력 | bbox + class | bbox + class + **17 keypoints** |
| 파라미터 | 2.6M | 2.9M |
| ONNX 크기 (FP32) | ~11MB | ~13MB |
| CPU 추론 속도 | 기준 | 약 15–25% 느림 |
| 추가 정보 | 없음 | 관절 좌표 17개 (어깨·무릎·머리 등) |

YOLO11n-pose는 detection의 상위 집합이다. keypoint를 쓰지 않는 상황에서도 bbox는 동일하게 사용할 수 있다.

---

## 감지 목표별 적합성

| 감지 대상 | detection | pose | 판단 |
|---|---|---|---|
| 미주문 착석 / 초과 체류 | bbox + 시간 카운팅 | 불필요 | detection 충분 |
| 출입문 개방 | 문 영역 교차 | 불필요 | detection 충분 |
| 반려동물 의자 착석 | dog/cat bbox + 의자 영역 | 불필요 | detection 충분 |
| 외부 음식 반입 | cup/bottle bbox | 불필요 | detection 충분 |
| **실신·쓰러짐** | bbox만으로 눕기 판단 어려움 | 머리·어깨·엉덩이 y좌표로 판단 | **pose 필수** |
| 구토 / 이상 행동 | 어려움 | 몸통 굽힘 각도로 보조 | pose가 약간 유리 |
| 기물 파손 | 거의 불가 | 팔 동작 패턴으로 보조 | 둘 다 어려움 |

---

## 결론

**YOLO11n-pose가 더 적합하다.**

1. **쓰러짐/실신이 핵심 요구사항** — bbox만으로는 앉은 사람과 쓰러진 사람을 구분하기 어렵다. keypoint로 머리-어깨-엉덩이의 높이 관계를 보면 룰 기반으로 판단 가능하다.
2. **속도 차이는 상쇄 가능** — 15–25% 느린 것은 `detect_every` 값을 1 높이면 보완된다.
3. **두 모델 동시 운용은 무리** — i5-4200U 단일 기기에서는 하나를 선택해야 한다.
4. **교체 비용이 낮다** — 파이프라인 구조는 그대로이고, 출력 파싱(`yolo11_decode`)과 keypoint 처리 로직만 추가하면 된다.

### 개발 순서 제안

체류·착석 판단을 먼저 개발한다면 YOLO11n으로 시작해도 무방하다. 쓰러짐 감지 단계에 진입할 때 pose 모델로 교체한다.

---

## 양자화 참고

현재 사용 중인 `yolo11n-416.onnx`는 **FP32(비양자화)** 버전이다 (11MB = 2.6M params × 4바이트).

| 방식 | 크기 | CPU 속도 | 정확도 손실 |
|---|---|---|---|
| FP32 (현재) | 11MB | 기준 | 없음 |
| FP16 | 5MB | CPU에서 큰 차이 없음* | 거의 없음 |
| INT8 | 3MB | 약 2–4× 빠름 | 소폭 (1–2% mAP) |

\* x86 CPU는 FP16을 FP32로 올려서 계산하므로 속도 이점이 없다.

i5-4200U 배포 기기에서는 INT8 양자화가 가장 큰 성능 레버다.

```python
# onnxruntime으로 INT8 동적 양자화
from onnxruntime.quantization import quantize_dynamic, QuantType

quantize_dynamic(
    "yolo11n-pose-416.onnx",
    "yolo11n-pose-416-int8.onnx",
    weight_type=QuantType.QUInt8
)
```
