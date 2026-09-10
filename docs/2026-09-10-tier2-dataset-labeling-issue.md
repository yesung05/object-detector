# Tier 2 데이터셋 레이블 오매핑 문제

작성일: 2026-09-10

## 요약

`datasets/tier2/labels/` 의 YOLO 레이블 파일 전체가 **잘못된 클래스 ID**로 저장되어 있다.
현재 `yolo11n_tier2_*.onnx` 모델은 bottle이라는 이름으로 야구 방망이를 학습하는 등,
클래스 이름과 실제 이미지 내용이 완전히 어긋난 상태로 학습됐다.

## 근거

`datasets/tier2/labels/train/` 전체(35,049개)에서 클래스 ID별 인스턴스 수를 집계한 결과:

| 데이터셋 클래스 ID | 이름(yaml) | 이 데이터셋 수 | COCO 2017에서 같은 수를 가진 클래스 |
|---|---|---|---|
| 0 | cat | 9,838 | bench |
| 1 | dog | 10,806 | bird |
| 2 | bottle | 3,276 | baseball bat |
| 3 | cup | 5,543 | skateboard |
| 4 | banana | 7,913 | wine glass |
| 5 | apple | 20,650 | cup |
| 6 | sandwich | 5,479 | fork |
| 7 | orange | 7,770 | knife |
| 8 | broccoli | 6,165 | spoon |
| 9 | carrot | 14,358 | bowl |
| 10 | hot dog | 9,458 | banana |
| 11 | pizza | 5,851 | apple |
| 12 | donut | 4,373 | sandwich |
| 13 | cake | 6,399 | orange |
| 14 | chair | 7,308 | broccoli |
| 15 | dining_table | 7,179 | carrot |

COCO 원본의 bottle은 약 24,342개여야 하는데 이 데이터셋에서는 3,276개에 불과하다.
반대로 apple(COCO 5,851개)이 이 데이터셋에서 20,650개로 가장 많다.
집계 수치가 COCO 2017의 다른 클래스 수치와 일대일 대응되어, 레이블 변환 시 클래스 ID 매핑이
통째로 밀렸음을 강하게 시사한다.

## 원인 추정

COCO 80클래스를 이 시스템의 16클래스로 리매핑하는 변환 스크립트에서 오류가 발생한 것으로 보인다.
COCO 원본 category_id는 연속적이지 않고 1~90 사이에 빈 번호가 있다.
이 빈 번호를 처리하지 않고 순서대로 0-indexed 리매핑을 하면 클래스가 밀린다.

예: COCO cat=17, dog=18이지만 리매핑 배열 인덱스를 잘못 계산하면
    실제로는 bench(15번)=9,838개가 0번 슬롯에 들어갈 수 있다.

## 영향

- 모델 mAP50 = 29.8% — 낮은 성능의 직접 원인
- bottle/cup/chair/table 실제 감지 불가 (학습한 시각 특징이 완전히 다른 물체)
- SlotMonitor, 외부 음료 감지 등 Tier 2 의존 기능 전부 무효

## 해결 방법

1. **COCO annotations JSON에서 직접 재변환**
   - `annotations/instances_train2017.json` 의 category_id를 정확히 매핑하는 스크립트 재작성
   - 대상 category_id 목록: cat=17, dog=18, bottle=44, cup=47, banana=52, apple=53,
     sandwich=54, orange=55, broccoli=56, carrot=57, hot_dog=58, pizza=59,
     donut=60, cake=61, chair=62, dining_table=67
   - 이미지는 재사용 가능, 레이블 txt 파일만 재생성

2. **재학습 설정 개선**
   - `epochs: 100`, `patience: 30` (현재 21 epoch에서 early stopping으로 중단됨)
   - bottle 클래스 focal loss 가중치 상향 또는 오버샘플링 검토 (현재 3,276개로 최소)

3. **검증**
   - 재변환 후 샘플 이미지를 열어 레이블과 실제 객체가 일치하는지 육안 확인
   - 재학습 후 mAP50 ≥ 50% 목표

## 현황

- [ ] 변환 스크립트 오류 위치 확인 및 수정
- [ ] 레이블 재생성 (train 35,049 + val 1,451개)
- [ ] 재학습 (epochs=100, patience=30)
- [ ] 재학습 후 성능 지표 확인 (mAP50, 클래스별 AP)
- [ ] 실제 카메라 영상에서 bottle/chair/table 감지 테스트
