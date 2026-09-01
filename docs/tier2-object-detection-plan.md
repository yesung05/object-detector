# Tier 2 물체 감지 구현 계획

## 배경

현재 시스템은 **Tier 1 — yolo11n-pose** 단일 모델로 사람만 감지한다.
동물 착석·외부 음식·미구매 착석 등을 추가로 감지하려면 물체 감지 모델이 별도로 필요하다.

## 아키텍처

```
Tier 1 — yolo11n-pose  (기존, ~6fps)
  → 사람 추적 / 쓰러짐 / 체류 시간 / keypoint 분석

Tier 2 — yolo11n COCO  (추가, ~0.3~0.5fps)
  → 동물 / 가구 / 음식 / 컵 감지 → 룰 기반 판정
```

두 모델은 **같은 프레임에서 동시에 실행되지 않는다.**
Tier 2는 `detect_every_obj` 프레임(기본 90)마다 한 번만 실행한다.

## 모델 사양

| 항목 | 값 |
|---|---|
| 베이스 | YOLO11n (Ultralytics) |
| 입력 해상도 | **320×320** |
| 정밀도 | **INT8** (가속기 없는 i5-4200U 기준 최적) |
| 학습 데이터 | COCO 80클래스 (추가 학습 불필요) |
| 실행 빈도 | 90프레임(3초)마다 1회 |

### 예상 성능 (i5-4200U, CPU only)

| 조건 | 추론 시간 | 프레임당 평균 부하 |
|---|---|---|
| 320×320 FP32 | ~50ms | ~0.6ms |
| 320×320 INT8 | ~35ms | **~0.4ms** |

Tier 1 부하(~30ms/프레임) 대비 **1.3% 추가** 수준.

## 감지 대상 클래스

추가 학습 없이 COCO 사전학습 가중치로 바로 사용 가능하다.

```c
/* Tier 2 필터 클래스 ID (COCO) */
static const int OBJ_CLASSES[] = {
    15,  /* cat          */
    16,  /* dog          */
    39,  /* bottle       */
    41,  /* cup          */
    46,  /* banana       */
    47,  /* apple        */
    48,  /* sandwich     */
    49,  /* orange       */
    50,  /* broccoli     */
    51,  /* carrot       */
    52,  /* hot dog      */
    53,  /* pizza        */
    54,  /* donut        */
    55,  /* cake         */
    56,  /* chair        */
    60,  /* dining table */
};
```

## 룰 기반 판정

| 입력 조합 | 판정 | 경보 |
|---|---|---|
| cat 또는 dog bbox ∩ chair 또는 dining table (IoU ≥ 0.15) | 반려동물 가구 위 착석 | `animal_on_furniture` |
| pizza / sandwich / hot dog / donut / cake / apple / banana 등 감지 | 외부 음식 반입 | `external_food` |
| bottle 감지 | 외부 음료 반입 | `external_drink` |
| 활성 사람 수 > cup 수 + 여유분(1) | 미구매 착석 의심 | `no_cup_seated` (보조) |

> IoU 임계값을 0.15로 낮게 잡는 이유: 동물이 의자 위에 있으면 bbox 하단부만 겹치는 경우가 많음.

## 구현 범위 (나중에)

1. **`detector_ort.c`** — 두 번째 `Detector *` 인스턴스 추가, 클래스 필터 파라미터
2. **`postprocess.c`** — 80클래스 argmax 디코딩 추가, `Detection.class_id` 필드
3. **`main.c`** — `AppContext`에 `Detector *obj_detector` 추가, 90프레임 주기 실행
4. **`rules.c`** — `rules_evaluate_objects()` 추가 (물체 감지 기반 룰)
5. **`draw.c`** — Tier 2 감지 결과 시각화 (반투명 박스, 클래스 레이블)

## 모델 준비 (나중에)

Python + Ultralytics 환경에서:

```python
from ultralytics import YOLO
YOLO("yolo11n.pt").export(format="onnx", imgsz=320, int8=True)
# → yolo11n_int8.onnx
```

> 이 PC에는 GPU 가속기가 없으므로 INT8 export 및 추론 모두 CPU 기준.
> export 시 `device="cpu"`가 기본값이므로 별도 옵션 불필요.

생성된 `yolo11n_int8.onnx`를 프로젝트 `models/` 또는 루트에 배치하면 된다.

## 미결 사항

- [ ] `yolo11n_int8.onnx` (320×320) export
- [ ] `Detection` 구조체에 `class_id` 필드 추가
- [ ] `postprocess.c` 80클래스 디코딩 확장
- [ ] Tier 2 실행 파이프라인 구현
- [ ] `rules_evaluate_objects()` 구현
- [ ] `no_cup_seated` 규칙 임계값 결정 (점주 정책 따라 다름)
