# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 프로젝트 목표

무인 카페 키오스크에 설치되는 컴퓨터비전 기반 관리 소프트웨어. 카메라 영상을 분석하여 매장 내 비정상 상황을 감지하는 것이 목적이며, 실제 업체 기기에 배포됩니다.

감지 대상 비정상 상황:
- 시럽 과다 복용 / 구토
- 물건 구입 없이 체류 / 미주문 착석 / 1인 1메뉴 미구입
- 쓰레기 투기 / 외부 음식 반입·취식
- 출입문 개방 / 초과 장기 체류
- 반려동물 의자 착석 (바닥이 아닌 의자)
- 출입 제한 대상 진입 (노키즈존 등)
- 응급상황 (실신, 쓰러짐) / 기물 파손
- 카메라 장애

**영상 외부 전송 금지.** 처리된 영상과 감지 데이터는 로컬에서만 사용해야 합니다.

## 운영 환경 및 하드웨어 제약

- **배포 대상 기기**: Intel i5-4200U (Haswell, **2코어 4스레드**, 1.6~2.6GHz, TDP 15W 모바일 CPU)
- **대상 매장 규모**: 7~10평, 감지 유효 거리 10m 이내 무인 카페
- **스레드 배분 원칙**: 2코어 4스레드 기준, 1스레드는 시스템/OS 예비로 비워두고 나머지 3스레드를 파이프라인에 활용
- **리소스 목표**: 현재 CPU 점유율 기준으로 1/4 수준 이하 감축. YOLO 추론 자체 부하가 크므로 `detect_every` + tracker 조합이 핵심 레버
- **발열**: 모바일 저전력 CPU라 장시간 연속 추론 시 서멀 스로틀링 주의. 스로틀링 발생 시 프레임이 끊겨 보이는 현상으로 나타남

> **참고 (차순위 기기)**: Celeron J1900(Bay Trail, 10W, 4C/4T, DDR3L 1333)이 설치될 수도 있음. 싱글스레드 성능이 i5-4200U 대비 약 절반 수준이라 `detect_every` 값을 높이거나 입력 해상도를 낮춰야 실시간성을 유지할 수 있음. 팬리스 운용은 가능.

## 아키텍처 방향 (구현 예정 포함)

### 감지 판단 원칙: 룰 기반 우선, AI는 최소화

리소스 절감을 위해 모든 상황을 AI로 판단하지 않는다. 1차로 룰 기반 필터(체류 시간, 인원 수, 위치 등 조건문)를 최대한 적용하고, 룰로 명확히 판단되지 않는 예외 상황(실신, 이상 행동 등)에만 AI 추론을 위임한다.

### 로깅 요구사항

카메라 단절·네트워크 장애·프레임 드롭 등 문제 발생 시 스텝 단위 디버깅 없이 원인을 즉각 파악할 수 있어야 한다. 새 기능을 추가할 때 해당 기능의 정상/비정상 상태를 명확히 구분하는 로그를 함께 작성한다.

### 매장 환경별 설정 분리

매장 규모(5평/20평 등)와 점주 정책에 따라 감지 임계값·체류 허용 시간·활성화 기능이 달라야 한다. 하드코딩 대신 매장별 설정값(신뢰도, 체류 한도 등)을 외부에서 주입할 수 있는 구조로 설계한다.

## 코딩 규칙

### 사유 명시 원칙
라이브러리, API, 알고리즘, 자료구조 선택에는 선택 이유를 주석으로 남겨야 합니다. "무엇을 하는지"가 아니라 "왜 이 방법을 선택했는지"를 기록합니다. 기존 코드에서 긴 주석이 설계 결정을 설명하는 방식을 따릅니다.

### 포인터 주석 규칙
포인터를 사용할 때는 반드시 아래 사항을 주석으로 표기합니다:
- **소유권**: 이 포인터가 메모리를 소유하는지, 빌려 쓰는지 (`/* ORT 소유 — free 금지 */`, `/* Pipeline 소유, 재사용 */`)
- **수명**: 언제 유효하고 언제 무효화되는지
- **해제 책임**: 누가, 어떤 함수로 해제하는지 (`allocator->Free` vs `free()` vs ORT `ReleaseXxx`)

기존 코드 예시:
```c
/* 이름 문자열은 ORT allocator가 만들었으므로 allocator->Free로,
   float 배열은 우리가 malloc했으므로 free로 해제한다는 차이가 중요합니다. */
```

## Build

### macOS
```sh
brew install ffmpeg
make
```
번들된 `third_party/onnxruntime` (macOS arm64)이 자동으로 사용됩니다.

### Linux
```sh
sudo apt install build-essential pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libavdevice-dev
make ORT_ROOT=/path/to/onnxruntime
```

### Windows (Visual Studio 2022)
```powershell
cmake -S . -B build-windows -G "Visual Studio 17 2022" -A x64 `
  -DORT_ROOT=C:\deps\Microsoft.ML.OnnxRuntime.DirectML `
  -DDIRECTML_ROOT=C:\deps\Microsoft.AI.DirectML `
  -DFFMPEG_ROOT=C:\deps\ffmpeg
cmake --build build-windows --config Release
```

### 기타 빌드 타겟
```sh
make debug          # ASan + UBSan, 최적화 없음
make i5-4200u       # 배포 대상 기기용 (Haswell AVX2)
make clean
```

## Tests

```sh
make test                                       # postprocess, draw, tracker 단위 테스트
make test-media                                 # FFmpeg I/O 테스트 바이너리 빌드
build/test_media input.mp4 /tmp/out.mp4         # FFmpeg 스트리밍 입출력 실행
```

`test_core`는 FFmpeg·ONNX Runtime 없이 컴파일됩니다. `test_media`는 FFmpeg만 필요하고 ONNX Runtime은 필요 없습니다.

## Architecture

### 데이터 흐름

```
media_process()          — FFmpeg 디코딩 루프, 프레임마다 FrameCallback 호출
  └─ process_frame()     — 추론 주기 판단, 추적기 조율 (main.c)
       ├─ detector_run() — letterbox 전처리 → ONNX 추론 → NMS (detector_ort.c, postprocess.c)
       ├─ tracker_update/reset() — SAD 패치 추적 (tracker.c)
       └─ draw_detections() — RGB 버퍼 위에 직접 그리기 (draw.c)
```

### 핵심 설계 원칙

**모든 버퍼는 시작 시 한 번만 할당, 프레임마다 재사용.** `Detector` 내부의 `input_data`/`output_data`, `DetectionList.items`, `Pipeline`의 RGB 버퍼가 모두 이 방식입니다. 프레임 수에 무관하게 메모리 사용량이 고정됩니다.

**에러 전파는 `char *error, size_t error_size` 쌍으로.** 반환값 0 = 성공, -1 = 실패. 예외 없음.

**자원 정리는 단일 `goto done/fail` 지점으로.** C에 RAII가 없으므로 생성 중 실패한 경우도 동일한 정리 코드를 통과합니다. `destroy` 함수들은 모두 NULL을 안전하게 처리합니다.

### 모듈 경계

| 헤더 | 공개 대상 |
|---|---|
| `include/yolo11.h` | `Detection`, `DetectionList`, `Letterbox`, `Detector` (opaque), 전처리·추론·그리기 함수 |
| `include/media.h` | `RgbFrame`, `FrameCallback`, `MediaOptions`, `media_process()` |
| `include/tracker.h` | `LightTracker` (opaque), `tracker_create/reset/update/destroy` |
| `include/platform.h` | `platform_monotonic_seconds()`, `platform_process_cpu_seconds()` |

`Detector`와 `LightTracker`는 opaque 타입입니다. 내부 멤버는 각각 `detector_ort.c`와 `tracker.c`에만 노출됩니다.

### 추론 주기 정책 (`main.c: process_frame`)

- `frame.index % detect_every == 0` 인 프레임에서 YOLO 실행
- 나머지 프레임에서 `--track` 활성화 시: `tracker_update()`로 박스 이동 보정
- `tracker_update()`가 `request_detection=1`을 반환하면(박스 밖 움직임 또는 추적 실패) 즉시 YOLO 재실행 (adaptive run)

### 출력 포맷 처리 (`detector_ort.c: detector_run`)

YOLO 출력 텐서 형태가 고정(`output_value` 미리 생성)이면 재사용 버퍼에 바로 씁니다. 동적 출력(NMS 내장 모델 등)이면 `output_value = NULL`로 두고 ORT가 프레임마다 메모리를 할당합니다. `yolo11_decode()`는 `[1,84,N]`, `[1,N,84]`, `[1,N,6]`(embedded NMS) 세 가지 형태를 모두 처리합니다.

### 플랫폼별 차이

- **macOS**: `third_party/onnxruntime` 번들, AVFoundation 카메라, CPU provider
- **Windows**: DirectML provider 기본, DirectShow 카메라, CMake 빌드, `wchar_t` 경로 변환 필요 (`utf8_to_wide`)
- **Linux**: 외부 ORT 지정 필수, V4L2 카메라, CPU provider

### 보안 주의사항

`--output`과 `--input` 경로에 대한 로컬 파일 검증이 없습니다. FFmpeg는 `rtmp://`, `rtsp://`, `http://` 등 네트워크 URL을 투명하게 처리하므로, URL을 넘기면 영상이 외부로 전송될 수 있습니다. 이 소프트웨어는 실제 업체 기기에 설치되므로 경로 검증 로직 추가가 필요합니다.
