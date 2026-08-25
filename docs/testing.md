# 테스트 모듈 가이드

> 현재 구현된 기능(letterbox, decode, NMS, draw, tracker, platform)의 동작을 보증하는 테스트 모음.
> pose 브랜치 분기 전 기반 코드의 건전성을 확인하고, 이후 기능 추가 시 회귀를 잡는 것이 목적.

---

## 파일 구조

```
tests/
  test_runner.h      — 매크로 기반 테스트 프레임워크 (헤더 전용, 표준 라이브러리만)
  test_core.c        — 단위 테스트 22개 (ORT/FFmpeg 의존 없음)
  test_detector.c    — ORT 통합 테스트 5개 (모델 파일 필요)
  test_media.c       — FFmpeg 파이프라인 테스트 (실제 동영상 파일 필요)
```

---

## 실행 방법

### Windows (Visual Studio 2022)

```powershell
# 빌드
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target test_core

# 실행
.\build\Release\test_core.exe
```

ORT/FFmpeg가 없어도 `test_core`는 빌드됩니다. 메인 실행 파일(`yolo11-person`)과 달리 외부 의존이 없습니다.

### Linux / macOS (GNU Make)

```sh
make test-fast        # 단위 테스트 (외부 의존 없음)
make test-detector    # ORT 통합 테스트 (모델 필요)
make test-all         # 위 둘 모두
```

`make test`는 `test-fast`의 별칭입니다(하위 호환).

### Docker (make가 없는 Windows)

```powershell
docker run --rm -v "${PWD}:/app" -w /app gcc:14 make test-fast
```

Docker Desktop이 있으면 의존 설치 없이 바로 실행됩니다.

### CTest (CMake 기반 CI용)

```powershell
# 단위 테스트만
ctest --test-dir build -L unit -V -C Release

# ORT 통합 테스트 포함 (ORT_ROOT 지정 시)
ctest --test-dir build -V -C Release
```

---

## 기대 출력

```
=== core_unit_tests ===
  PASS  test_letterbox
  PASS  test_fast_letterbox_matches_reference
  ...
  PASS  test_platform_cpu_count
--- 22 passed, 0 failed ---
```

---

## 테스트 목록

### `test_core.c` — 단위 테스트 (22개)

#### Letterbox 전처리

| 테스트 | 검증 내용 |
|---|---|
| `test_letterbox` | 2×1 이미지를 4×4 모델로 변환, scale·pad·여백 픽셀 확인 |
| `test_fast_letterbox_matches_reference` | fast와 reference 모드가 동일 결과 생성 |
| `test_letterbox_wide_image` | 10×3→4×4, scale=0.4, pad_x=0, pad_y=1 |
| `test_letterbox_tall_image` | 3×10→4×4, scale=0.4, pad_x=1, pad_y=0 |
| `test_letterbox_invalid_args` | NULL 인수, stride 부족 → -1 반환 |

#### YOLO 출력 디코딩 / NMS

| 테스트 | 검증 내용 |
|---|---|
| `test_decode_and_nms` | [1,5,7] 출력에서 겹치는 두 박스가 NMS 후 1개로 |
| `test_decode_channel_first` | [1,5,100] shape (channel_first) 파싱 |
| `test_decode_channel_last` | [1,100,5] shape (channel_last) 파싱 |
| `test_decode_embedded_nms` | [1,2,6] shape, class≠0 필터링 확인 |
| `test_decode_invalid_args` | NULL 인수, rank≠3, 지원 안 하는 shape → -1 |
| `test_nms_two_overlapping_boxes` | IoU=1.0인 두 박스 → 고점수만 생존 |
| `test_nms_two_nonoverlapping_boxes` | 완전히 떨어진 두 박스 → 둘 다 생존 |

#### 그리기

| 테스트 | 검증 내용 |
|---|---|
| `test_draw_bounds` | guard byte 패턴으로 버퍼 초과 쓰기 검출 |
| `test_draw_partial_box` | 박스가 이미지 경계 밖 → guard byte 무결 |
| `test_draw_empty_detections` | count=0 → 버퍼 변형 없음 |

#### DetectionList 수명

| 테스트 | 검증 내용 |
|---|---|
| `test_detection_list_lifecycle` | NULL/0 인수 → -1, 정상 init, 이중 destroy 안전 |

#### 추적기

| 테스트 | 검증 내용 |
|---|---|
| `test_light_tracker_translation` | 4픽셀 이동 후 박스 좌표 갱신 |
| `test_tracker_invalid_create` | NULL/0 옵션 → NULL 반환 |
| `test_tracker_null_args` | reset/update에 NULL 전달 → -1 반환 |
| `test_tracker_stationary` | 동일 프레임 두 번 → request_detection=0 |

#### 플랫폼

| 테스트 | 검증 내용 |
|---|---|
| `test_platform_timer_advances` | 타이머 단조 증가, 양수 확인 |
| `test_platform_cpu_count` | CPU 수 ≥ 1 |

---

### `test_detector.c` — ORT 통합 테스트 (5개)

모델 경로를 `argv[1]`로 받아, 파일이 없으면 해당 테스트를 `SKIP`합니다.

```powershell
# 모델이 있을 때
cmake --build build --config Release --target test_detector
.\build\Release\test_detector.exe yolo11n-416.onnx
```

| 테스트 | 검증 내용 |
|---|---|
| `test_detector_null_destroy` | `detector_destroy(NULL)` crash 없음 |
| `test_detector_invalid_model` | 없는 경로 → NULL, error 문자열 채워짐 |
| `test_detector_lifecycle` | create → input 크기 416×416 확인 → destroy |
| `test_detector_blank_image` | 회색 416×416 → rc=0, inference_seconds>0 |
| `test_detector_fast_preprocess` | ref/fast 모드 동일 입력 → count 동일 |

---

## 테스트 프레임워크 (`test_runner.h`)

외부 라이브러리 없이 `<setjmp.h>` + `<stdio.h>`만 사용합니다.

```c
// 치명적: 실패 시 현재 테스트를 즉시 중단 (longjmp)
ASSERT_TRUE(cond)
ASSERT_INT_EQ(a, b)

// 비치명적: 실패를 기록하고 테스트 계속 진행
EXPECT_TRUE(cond)
EXPECT_INT_EQ(a, b)
EXPECT_FLOAT_NEAR(a, b, eps)
```

실패 시 출력 예시:
```
  FAIL  test_decode_and_nms
    expect list.count==1: 0!=1 at tests/test_core.c:107
```

### 새 테스트 추가하는 법

```c
// 1. 테스트 함수 작성
static void test_my_feature(void) {
    ASSERT_INT_EQ(my_function(input), expected_output);
    EXPECT_FLOAT_NEAR(my_float, 1.23f, 0.001f);
}

// 2. main()의 RUN_TEST 목록에 추가
RUN_TEST(test_my_feature);
```

외부 의존이 없으면 `test_core.c`에, ORT가 필요하면 `test_detector.c`에 추가합니다.

---

## 메모리 오류 탐지 (Linux)

```sh
make CFLAGS="-O1 -g3 -fsanitize=address,undefined" build/test_core
./build/test_core
```
