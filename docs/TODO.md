# TODO — 앞으로 진행 방향

> 기준일: 2026-08-29. 완료된 기능은 `docs/checklist.md` 참조.

---

## 단기 — 배포 전 필수

### 보안: 경로 검증

CLAUDE.md에 명시된 가장 중요한 미해결 항목.

FFmpeg는 `rtmp://`, `rtsp://`, `http://` 등 네트워크 URL을 투명하게 처리하므로 현재 `--output` 또는 `--input`에 URL을 넘기면 영상이 외부로 스트리밍된다. 실제 업체 기기에 설치되는 소프트웨어이므로 이 취약점은 배포 전에 반드시 막아야 한다.

- `validate_local_path(const char *path)` 함수 구현 (`src/path_util.c`)
  - `://` 패턴 거부
  - Windows UNC `\\server\share` 거부
  - 절대 경로 + 확장자 화이트리스트 강제
- `--output` 경로에 적용
- `--input` 경로에서 카메라 모드가 아닌 경우에도 적용
- 관련 테스트: `tests/test_core.c`에 추가 가능 (FFmpeg 불필요)

---

## 중기 — 룰 기반 감지 확장

### 출입문 개방 감지

별도 모델 없이 고정 카메라에서 구현 가능.

- 매장 설정 파일에 `roi_door = x,y,w,h` 추가
- 해당 ROI에서 픽셀 변화량이 임계값을 초과하면 `door_open` 이벤트
- 장시간 개방 유지 시 `door_open_extended` 이벤트
- 모션 게이트가 닫혀 있어도 ROI 감시는 항상 실행

### 인원 초과 감지

`TrackList`의 `active` 트랙 수를 세면 된다. `config`에 `max_occupancy = N` 추가.

### 1인 1메뉴 미구입 (결제 연동 전제)

`tracks_mark_ordered(track_id)` API는 이미 구현됨. 연결 작업:

1. POS 결제 이벤트를 수신하는 소켓/파이프 추가
2. 결제 시각 ± 30초 윈도우 내 키오스크 ROI 체류 트랙에 `ORDERED` 표시
3. 입장 후 `unordered_grace_seconds` 초과한 UNORDERED 트랙이 N명이면 `unordered_seated` 이미 발화 → 추가 룰 불필요

---

## 중기 — 추가 모델 필요 항목

현재 person bounding box + pose keypoint 조합으로 판단할 수 없는 상황들. 각각 별도 모델 또는 데이터 수집 후 fine-tuning이 필요하다. 모두 i5-4200U에서 동시 실행 가능한 모델 크기(YOLO11n급)를 유지해야 한다.

| 감지 대상 | 접근 방식 |
|---|---|
| 반려동물 의자 착석 | COCO 클래스 확장(dog/cat 기본 포함) + 의자 ROI 교차 판정. 의자 ROI는 설정 파일에 `roi_chair_a = ...` 형태로 등록 |
| 외부 음식 반입·취식 | 음식 객체 감지 모델 추가. 매장 외 포장재(컵, 봉투) 클래스 학습 데이터 수집 필요 |
| 쓰레기 투기 | 바닥 영역에 이물질 객체 등장 감지. 배경 차분 + 객체 감지 조합 |
| 노키즈존 — 어린이 감지 | 얼굴 기반 나이 추정 경량 모델. C API로 동작하는 ONNX 모델이어야 함 |
| 기물 파손 | 급격한 장면 변화 + 객체 이동/소멸. 사전 학습된 배경 모델이 없으면 정탐률이 낮다 |
| 시럽 과다 복용 / 구토 | 특정 반복 동작 패턴 인식. 현재 pose keypoint 시퀀스로 시도 가능하나 학습 데이터 부재 |

---

## 장기 — 시스템 통합

### WebSocket 이벤트 서버

이벤트 로그(`log.c`)를 외부 서버에 실시간 전달. 영상은 전송하지 않고 이벤트 메타데이터만 (`track_id`, `event_type`, `timestamp`, `roi`, `근거 필드`). 로컬 파일 로그와 병렬 운용.

### 점주 설정 UI 연동

현재 `config/store.example.ini`를 서버에서 받아 파일로 저장하는 방식. 재시작 없이 설정 변경 반영이 필요하면 `inotify`(Linux) / `ReadDirectoryChangesW`(Windows) 감시를 추가하고 `config_reload()` 구현.

### 멀티스레드 파이프라인

i5-4200U 3스레드 활용:

```
스레드 1: FFmpeg 디코딩 + gray/camera_health (I/O 바운드)
스레드 2: YOLO 추론 + tracks_update + rules_evaluate (CPU 바운드)
스레드 3: 출력 인코딩 + 이벤트 로그 flush (I/O 바운드)
```

스레드 간 버퍼: 이중 버퍼(ping-pong). 락은 최소화. 현재 단일 스레드 대비 처리량 1.5~2× 기대.

### J1900 대응 모드

Celeron J1900은 싱글스레드 성능이 i5-4200U의 절반이다. `--low-power` 플래그 추가:

- 입력 해상도를 절반(416→320 또는 208)으로 자동 축소
- `detect_every` 기본값을 6 이상으로 조정
- 모션 게이트 임계값 상향

### 캘리브레이션

매장별로 조명 조건과 카메라 앵글이 다르다. `--calibrate` 모드:

- 30초간 빈 매장 영상으로 배경 luma 분포 측정
- 백화/암전 임계값 자동 조정
- 추천값을 `config/store.ini`에 기록

---

## 알려진 기술 부채

| 항목 | 위치 | 내용 |
|---|---|---|
| `set_error()` 중복 | `tracker.c`, `media_ffmpeg.c` | `log.c`로 통합 예정이나 아직 남아있음 |
| 작업일지 머지 충돌 | `docs/2026-08-29.md` | `feature/quantization --theirs` 선택으로 병합됐다가 오늘 수동으로 재작성함. 향후 동일 날짜 파일 생성 브랜치는 파일 이름에 브랜치명을 포함하거나 날짜 기준 단일 브랜치에서 작성 |
| `matched_det[1024]` 스택 배열 | `tracks.c:64` | 감지 수가 1024를 초과하면 조용히 잘림. 실제 매장 환경에서는 발생하지 않지만, 정적 분석기 경고 대상 |
