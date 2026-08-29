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
3. 매칭 실패 시 `misses++`, `max_misses`(기본 5) 초과 시 `active=0`.
4. 미매칭 detection → 빈 슬롯에 새 트랙 생성.

`dwell_seconds`는 연속 매칭된 프레임 간격의 합입니다.

**OrderState**:
- `TRACK_UNORDERED`: 입장 후 기본 상태.
- `TRACK_ORDERED`: 키오스크 ROI를 통과했거나 `tracks_mark_ordered()`가 호출된 상태.

### RulesEngine (`include/rules.h`, `src/rules.c`)

이벤트는 트랙당 1회만 발화(latch)합니다. 조건이 해소되면 latch가 해제되어 재발 시 다시 감지합니다.

| 이벤트 | 조건 | 로그 레벨 |
|---|---|---|
| `overstay` | `dwell > dwell_limit_seconds` (기본 3600s) | WARN |
| `unordered_seated` | `order==UNORDERED AND dwell > unordered_grace_seconds` (기본 300s) | WARN |
| `person_fallen` | 수평 자세 `fall_hold_seconds`(기본 5s) 이상 지속 | ERROR |

**쓰러짐 판정 (`is_horizontal_pose`)**:
1. bbox 가로 > 세로 × 1.2 이어야 합니다.
2. keypoint가 있으면 (score ≥ 0.4인 것 기준): 머리(0), 어깨(5,6), 엉덩이(11,12)의 y좌표 표준편차 / bbox 높이 ≤ 0.25 이어야 합니다.
3. keypoint가 없으면 bbox 비율만으로 판정합니다.

**ROI 키오스크**: `roi_kiosk_x/y/w/h`를 설정하면 박스 중심이 해당 영역을 통과할 때 `TRACK_ORDERED`로 전환합니다.

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
