# 이상 탐지 룰 엔진

`feature/anomaly-rules` 브랜치에서 구현된 이상 탐지 파이프라인 설명서입니다.

## 처리 흐름

```
process_frame()
  ├─ gray_buf_update()          : RGB → 그레이스케일 다운샘플 (1회/프레임)
  ├─ camera_health_update()     : 카메라 장애 감지
  ├─ 모션 게이트 판단           : 정적 장면이면 YOLO 건너뜀
  ├─ detector_run()             : YOLO 추론 (조건 충족 시만)
  ├─ tracks_update()            : IoU 그리디 매칭 → ID 안정화
  └─ rules_evaluate()           : 룰 기반 이벤트 평가 → event_log
```

## 모듈별 설명

### GrayBuf (`include/gray.h`, `src/gray.c`)

RGB 프레임을 그레이스케일로 변환한 공유 버퍼입니다.

- 다운샘플 비율 `downsample`만큼 출력 크기를 줄입니다 (기본값: 4 → 각 축 1/4 크기).
- 루마 공식: `(77R + 150G + 29B + 128) >> 8` (BT.601 근사, 정수 연산으로 FP 불필요)
- 셀 중앙 점 샘플링 방식입니다.
- camera_health와 motion gate가 같은 버퍼를 공유하므로 프레임당 1회만 변환합니다.

### CameraHealth (`include/camera_health.h`, `src/camera_health.c`)

카메라 장애 3가지를 감지합니다.

| 상태 | 조건 |
|---|---|
| `CAM_WHITEOUT` | 평균 루마 > `luma_white_threshold` (기본 240) |
| `CAM_BLACKOUT` | 평균 루마 < `luma_black_threshold` (기본 12) |
| `CAM_FROZEN`   | 변화 픽셀 < `motion_threshold` (기본 8)가 `frozen_frames_threshold` (기본 150)프레임 연속 |

상태 전환은 `anomaly_hold_frames` (기본 5)프레임 연속 이상 감지 후에만 발생합니다.  
장애 해소 시 `CAM_OK`로 즉시 복귀합니다.

### 모션 게이트

픽셀 변화 비율이 낮고 마지막 추론 이후 충분한 시간이 지나지 않았으면 YOLO 추론을 건너뜁니다.

**조건**: `changed_ratio < motion_ratio_threshold` AND `(now - last_detection_time) < idle_refresh_seconds`

- `motion_ratio_threshold`: 기본 0.004 (0.4%). 빈 매장처럼 정적인 환경일수록 높여도 됩니다.
- `idle_refresh_seconds`: 기본 10.0초. 이 주기마다 강제 추론해 새 진입자를 놓치지 않습니다.
- SAD 추적기가 실패(박스 탈출, 추적 손실)하면 adaptive run이 게이트를 우선 적용합니다.
- `--motion-gate 0`으로 비활성화할 수 있습니다.

### TrackList (`include/tracks.h`, `src/tracks.c`)

LightTracker(SAD 패치)와 달리 프레임 간 안정적 ID를 부여하고 체류 시간을 누적합니다.

**그리디 IoU 매칭 (O(M×N))**:
1. 기존 트랙마다 IoU가 가장 높은 detection을 찾습니다.
2. `iou_threshold`(기본 0.4) 이상이면 매칭 → box, dwell 갱신.
3. 매칭 실패 시 `misses++`, `max_misses` 초과 시 `active=0`. 운영 값은 150(`main.c`가 `tracks_init`에 고정 전달). `tracks_update`는 추론한 프레임에서만 호출되므로 이 횟수는 카메라 프레임이 아니라 사람 추론 횟수 기준이다.
4. 미매칭 detection → 빈 슬롯에 새 트랙 생성.

`dwell_seconds`는 연속 매칭된 프레임 간격의 합입니다.

**OrderState**:
- `TRACK_UNORDERED`: 입장 후 기본 상태.
- `TRACK_PROBABLY_ORDERED`: 키오스크 앞 근접(bbox 높이 65%·면적 20% 이상, score 0.6 이상, 상체 직립)이 30초 유지된 상태. 결제 확인은 아니지만 결제 연동이 없어 `ORDERED`에 도달할 수 없으므로, 이 상태면 `unordered_seated`를 발화하지 않는다.
- `TRACK_ORDERED`: `tracks_mark_ordered()`가 호출된 상태 (외부 결제 확인용). **현재 이 함수를 호출하는 코드가 없어 운영에서는 이 상태가 되지 않는다.**

### RulesEngine (`include/rules.h`, `src/rules.c`)

이벤트는 트랙당 1회만 발화(latch)합니다. 조건이 해소되면 latch가 해제되어 재발 시 다시 감지합니다.

| 이벤트 | 조건 | 로그 레벨 |
|---|---|---|
| `overstay` | `dwell > dwell_limit_seconds` (기본 3600s) | WARN |
| `unordered_seated` | `order == UNORDERED AND dwell > unordered_grace_seconds` (기본 300s). `PROBABLY_ORDERED`면 발화하지 않고, 발화 후 `PROBABLY_ORDERED`가 되면 래치 해제 | WARN |
| `person_fallen` | 직립 이력 뒤 수평 자세가 `fall_hold_seconds`(기본 5s) 이상 지속 (아래 참조) | ERROR |
| `occupancy_exceeded` | 활성 트랙 수 > `max_occupancy`가 `max_occupancy_hold_seconds`(기본 10s) 유지. `max_occupancy=0`이면 꺼짐 | WARN |
| `person_motionless` | 서 있거나 누운 사람이 기준 자세 대비 거의 움직이지 않고 `still_seconds`(기본 300s) 유지. 앉은 사람 제외. 0이면 꺼짐 | WARN |

Tier 2 물체 이벤트(`external_drink`, `external_food`, `animal_on_chair`, `animal_on_table`, `no_cup_seated`)는 최근 `object_confirm_window`(기본 5)번 관측 중 `object_confirm_count`(기본 3)번 이상일 때만 발화한다. 상세는 `detection-status.md`.

**쓰러짐 판정** (`rules_evaluate`의 fall 게이트, `fall_torso_pose`, `is_horizontal_pose`):
1. 관측 품질: 박스·매칭 score ≥ 0.5, 마지막 관측이 2초 이내인 **새 관측**일 때만 평가한다. 아니면 후보를 초기화한다.
2. 몸통 자세: 어깨(5,6)·엉덩이(11,12)가 모두 score ≥ 0.5이고 박스 안(±10%)일 때 몸통 벡터로 직립/수평을 나눈다. 판정 불가면 후보를 초기화한다.
3. 직립 이력: 직립이 3회·1초 이상 관측된 뒤 30초 이내여야 한다. 처음부터 누워 있던 사람은 이 경로로 발화하지 않는다.
4. 수평 판정(`is_horizontal_pose`): 박스 세로 ≥ 30px, score ≥ 0.30. 박스가 화면 경계에 닿으면 비율을 믿지 않고 코-어깨 관계(상체 수평 여부)만 쓴다. 그 밖에는
   - keypoint 충분: 가로 > 세로 × `fall_aspect_ratio_kp` **그리고** 코·어깨·엉덩이 y 표준편차/박스 높이 ≤ 0.20
   - keypoint 부족·엉덩이 없음·keypoint 없음: 가로 > 세로 × `fall_aspect_ratio_nokp`
5. 수평 샘플 3개 이상이 `fall_hold_seconds` 유지되면 `person_fallen`(ERROR)을 1회 발화한다.

비율 기본값은 코드 안에서도 둘이다: `rules.c`의 기본 구조체는 1.3/1.6, `main.c`가 설정 키가 없을 때 쓰는 값은 1.8/2.2. 운영 `config.json`은 1.3/1.6이다. 머리 SAD 하강 속도(`fall_sudden`)로 즉시 발화하던 경로는 2026-09-26에 제거되어 지금은 매번 0으로 지워진다.

**키오스크 근접**: `roi_kiosk`는 설정을 읽지만 주문 판정에는 쓰이지 않는다(보호 영역 계산에만 사용). 키오스크 근접 판정은 위 `PROBABLY_ORDERED` 조건(bbox 크기·자세)을 쓴다.

### EventLog (`include/log.h`, `src/log.c`)

```
2026-08-29T14:03:11 WARN  rules  overstay track=7 dwell=4821s limit=3600s
2026-08-29T14:05:22 ERROR rules  person_fallen track=3 hold=5.2s
2026-08-29T14:07:01 WARN  camera state=FROZEN
```

- 기본 출력: stderr. `--event-log PATH`로 파일에 append합니다.
- `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` 3단계.

### Config (`include/config.h`, `src/config.c`)

```ini
# config/store.example.ini 참조
dwell_limit_seconds = 3600
unordered_grace_seconds = 300
fall_hold_seconds = 5
motion_ratio_threshold = 0.004
idle_refresh_seconds = 10
```

`--config PATH`로 지정합니다. 없으면 코드 기본값을 사용합니다.

## 실행 예시

```powershell
# 기본 실행 (모션 게이트 활성, 이벤트는 stderr)
yolo11-person --model yolo11n-416.onnx --camera --event-log events.log

# 설정 파일 지정, 모션 게이트 비활성
yolo11-person --model yolo11n-pose.onnx --camera \
  --config config/store.ini --motion-gate 0 --event-log events.log

# 비디오 파일로 테스트
yolo11-person --model yolo11n-pose.onnx --input test.mp4 \
  --output out.mp4 --config config/store.ini --metrics metrics.json
```

## 메트릭 추가 항목

`--metrics`로 출력되는 JSON에 추가된 필드:

| 필드 | 설명 |
|---|---|
| `gated_frames` | 모션 게이트로 건너뛴 프레임 수 |
| `motion_gate` | 모션 게이트 활성화 여부 |
