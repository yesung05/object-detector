# 기능 체크리스트

> 구현 완료 / 진행 예정 기능을 한눈에 파악하기 위한 문서.

---

## ✅ 현재 구현된 기능

### 영상 파이프라인
- [x] FFmpeg 기반 파일(MP4, JPEG 등) 입력·출력
- [x] 카메라 실시간 입력 (macOS AVFoundation / Linux V4L2 / Windows DirectShow)
- [x] ffplay 파이프 연결 로컬 미리보기 (`--preview`)
- [x] 프레임별 메모리 재사용 (고정 상한, 누적 없음)
- [x] Ctrl+C 정상 종료 (SIGINT/SIGTERM)

### 감지·추론
- [x] YOLO11n ONNX 모델 로딩 (고정 형상 [1,3,H,W] float32)
- [x] YOLO11n-pose 17개 관절 keypoint 디코딩 + 스켈레톤 렌더링
- [x] Letterbox 전처리 — 비율 유지 리사이즈 + 여백 채움
- [x] fast / reference 두 가지 전처리 모드
- [x] ONNX Runtime C API 추론 (CPU / Windows DirectML)
- [x] person(COCO class 0) 필터링
- [x] NMS (Non-Maximum Suppression), IoU 0.45 기본값
- [x] `[1,84,N]` / `[1,N,84]` / `[1,N,6]`(embedded NMS) / `[1,56,N]`(pose) 출력 형태 자동 판별
- [x] INT8 static 양자화 스크립트 (`scripts/quantize.py`, Haswell 기대치 1.5~2.5×)

### 추론 주기 최적화
- [x] `--detect-every N`: N프레임마다 한 번만 YOLO 실행
- [x] SAD 패치 기반 경량 추적기 (`--track`): 중간 프레임 박스 이동 보정
- [x] Adaptive 재추론: 박스 밖 예상치 못한 움직임 감지 시 즉시 YOLO 재실행

### 시각화
- [x] 바운딩 박스 + "PERSON XX%" 라벨 직접 그리기 (외부 폰트 라이브러리 없음)

### 계측·로깅
- [x] 단계별 처리 시간 측정 (전처리 / 추론 / 후처리 / 추적 / 그리기 / 출력)
- [x] 추론 지연 분포 통계 — p50/p95/max (`inference_p50_ms` 등, metrics JSON 포함)
- [x] 성능 지표 JSON 저장 (`--metrics`)
- [x] 프레임별 검출 결과 CSV 저장 (`--detections`)
- [x] 구조화 이벤트 로그 (`--event-log`, ISO 8601 타임스탬프 + key=value 근거)
- [x] 매장별 설정 파일 (`--config`, key=value 파서)

### 빌드·플랫폼
- [x] macOS (Makefile + 번들 ONNX Runtime arm64)
- [x] Linux (Makefile + 외부 ORT)
- [x] Windows (CMake + DirectML)
- [x] i5-4200U 전용 최적화 빌드 (`make i5-4200u`, Haswell AVX2)
- [x] ASan/UBSan 디버그 빌드 (`make debug`)

---

## 🔲 단기 목표 — 안정성·보안 기반

> 배포 전 반드시 해결해야 할 항목.

- [ ] **출력 경로 로컬 파일 검증**: `--output`에 `rtmp://` 등 URL 전달 시 외부 스트리밍 가능한 구조 — 경로가 로컬 파일인지 확인하는 검증 함수 추가
- [ ] **입력 경로 제한**: 카메라 모드가 아닌 경우 네트워크 URL 입력 차단
- [x] **카메라 장애 감지**: 백화·암전·프리즈 감지 + 이벤트 로그 (`camera_health.c`)
- [x] **통합 로깅 시스템**: ISO 8601 타임스탬프·심각도·key=value 근거 포함 이벤트 로그 (`log.c`)

---

## 🔲 중기 목표 — 비정상 상황 감지 (룰 기반)

> AI 추론 없이 조건문으로 처리 가능한 항목 우선.

- [x] **동일인 장기 추적 (Re-ID)**: IoU 그리디 매칭으로 안정적 트랙 ID 부여 (`tracks.c`)
- [x] **체류 시간 측정**: 트랙 ID 기반 `dwell_seconds` 누적, 설정 초과 시 이벤트 발생
- [x] **미주문 장기 착석 감지**: UNORDERED 상태 + `dwell > unordered_grace_seconds` (`rules.c`)
- [x] **초과 장기 체류**: `dwell > dwell_limit_seconds`, 트랙당 1회 latch 발화 (`rules.c`)
- [ ] **1인 1메뉴 미구입**: 인원 수 vs 주문 수 불일치 (결제 DB 연동 전제)
- [ ] **출입문 개방 감지**: 카메라 영역에서 문 상태 변화 감지
- [ ] **인원 초과**: 설정된 수용 인원 초과 시 이벤트
- [x] **매장 크기별 설정 분리**: `key = value` 설정 파일로 매장별 임계값 외부 주입 (`config.c`)

---

## 🔲 중기 목표 — 비정상 상황 감지 (모델 추가 필요)

> 현재 person bounding box만으로는 판단 불가 — 추가 모델 또는 포즈 추정 필요.

- [x] **응급상황 감지 (쓰러짐)**: bbox 가로/세로 비율 + YOLO11n-pose keypoint y 분산으로 판정, `fall_hold_seconds` 유지 시 이벤트 (`rules.c`)
- [ ] **반려동물 의자 착석**: 개/고양이가 사람용 의자에 올라간 경우 (동물 감지 모델)
- [ ] **외부 음식 반입·취식**: 매장 외 음식물 테이블 위 감지 (객체 감지 모델 확장)
- [ ] **쓰레기 투기**: 쓰레기를 바닥에 버리는 행위
- [ ] **시럽 과다 복용**: 특정 행동 패턴 반복 감지
- [ ] **구토**: 이상 행동 감지
- [ ] **노키즈존 — 어린이 감지**: 얼굴 기반 나이 추정 경량 모델 (C 환경 동작 필수)
- [ ] **기물 파손**: 갑작스러운 장면 변화 + 이상 동작 감지

---

## 🔲 장기 목표 — 시스템 통합

- [ ] **결제 DB 연동**: POS 승인 내역과 영상 타임라인 크로스체크 ("14:00 입장 → 미결제 체류 30분")
- [ ] **WebSocket 이벤트 서버**: 감지 이벤트를 점주 앱/서버로 실시간 전달
- [ ] **점주 설정 UI 연동**: 기능별 ON/OFF 체크박스, 매장별 임계값 설정값 수신
- [ ] **외부 스피커 안내방송 연동**: 미결제 이탈 등 특정 이벤트 발생 시 경고음 송출
- [ ] **멀티스레드 파이프라인**: 디코딩 / 추론 / 이벤트 전송을 스레드 분리 (i5-4200U 기준 3스레드 활용)
- [ ] **캘리브레이션**: 매장 조명·구조에 맞게 감지 기준값 영점 조정 기능
- [ ] **J1900 대응 모드**: 입력 해상도 축소 or `detect_every` 자동 조정으로 저사양 기기 실시간성 확보
