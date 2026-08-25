# YOLO11n detector (C)

카메라, 이미지 또는 동영상을 한 프레임씩 읽어 YOLO11n으로 사람(`COCO class 0`)만
감지하는 C11 프로그램입니다. FFmpeg로 영상을 처리하고 ONNX Runtime C API로
추론합니다. OpenCV와 C++ 런타임은 사용하지 않습니다.

## 처리 흐름

```text
카메라/파일 → FFmpeg 디코딩 → RGB24 변환 → letterbox/NCHW 전처리
            → ONNX 추론 → person 필터 및 NMS → 박스 그리기 → 화면 표시/파일 저장
```

프레임, 모델 입출력 텐서와 검출 버퍼를 재사용하므로 처리한 영상을 메모리에
누적하지 않습니다.

## 요구 사항

- C11 컴파일러
- FFmpeg 개발 라이브러리: `libavformat`, `libavcodec`, `libavutil`,
  `libswscale`, `libavdevice`
- ONNX Runtime C API
- 실시간 화면 표시 시 `ffplay`

실행 모델 `yolo11n-416.onnx`는 저장소에 포함되어 있습니다. 이 모델은 batch 1,
고정 416×416, float32 입력 모델입니다. 416은 YOLO11n의 최대 해상도가 아니라
현재 포함된 모델의 입력 크기입니다.

## 빌드

### macOS

저장소에 포함된 ONNX Runtime은 macOS arm64용입니다.

```sh
brew install ffmpeg
make
```

다른 ONNX Runtime을 사용하려면 `make ORT_ROOT=/path/to/onnxruntime`으로
지정합니다.

### Linux

```sh
sudo apt install build-essential pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libavdevice-dev
make ORT_ROOT=/path/to/onnxruntime
```

### Windows (Visual Studio 2022, x64)

FFmpeg 개발 패키지, `Microsoft.ML.OnnxRuntime.DirectML`과
`Microsoft.AI.DirectML` 패키지가 필요합니다.

```powershell
cmake -S . -B build-windows -G "Visual Studio 17 2022" -A x64 `
  -DORT_ROOT=C:\deps\Microsoft.ML.OnnxRuntime.DirectML `
  -DDIRECTML_ROOT=C:\deps\Microsoft.AI.DirectML `
  -DFFMPEG_ROOT=C:\deps\ffmpeg
cmake --build build-windows --config Release
```

CMake는 `/arch:AVX2`를 적용하고 찾은 런타임 DLL을 실행 파일 옆에 복사합니다.
DirectML 사용에는 DirectX 12를 지원하는 Intel 그래픽 드라이버가 필요합니다.

## 실행

이미지와 동영상:

```sh
./yolo11-person --model yolo11n-416.onnx \
  --input input.mp4 --output output.mp4
```

카메라 실시간 표시(녹화하지 않음):

```sh
./yolo11-person --model yolo11n-416.onnx --camera --preview
```

`--preview`에서 `--output`을 생략하면 인코딩과 파일 저장을 하지 않습니다.
녹화가 필요할 때만 `--output camera.mp4`를 추가합니다. `Ctrl+C`로 정상 종료합니다.

기본 카메라 입력은 다음과 같습니다.

| 운영체제 | FFmpeg 입력 | 기본 장치 |
|---|---|---|
| macOS | AVFoundation | `0:none` |
| Linux | V4L2 | `/dev/video0` |
| Windows | DirectShow | `video=Integrated Camera` |

Windows에서는 실제 카메라 이름을 확인한 뒤 지정할 수 있습니다.

```powershell
ffmpeg -list_devices true -f dshow -i dummy
.\build-windows\Release\yolo11-person.exe -m yolo11n-416.onnx --camera `
  --camera-device "video=Integrated Camera" --preview --provider directml
```

## 주요 기본값과 옵션

| 항목 | 기본값/설명 |
|---|---|
| 추론 주기 | `--detect-every 1`: 모든 프레임에서 추론 |
| 추적 | 꺼짐 |
| 신뢰도 | `--confidence 0.25` |
| NMS IoU | 0.45 |
| 추론 스레드 | `--threads 1` (1 또는 2) |
| 전처리 | `--preprocess fast` |
| ORT 그래프 최적화 | `--graph-opt all` |
| worker busy-wait | `--allow-spinning 0` |
| 실행 장치 | Windows는 DirectML, macOS/Linux는 CPU |
| 카메라 | 640×480, 30fps |

CPU 사용량을 더 줄여야 할 때만 다음 선택적 정책을 사용할 수 있습니다.

```sh
./yolo11-person -m yolo11n-416.onnx -i input.mp4 -o output.mp4 \
  --detect-every 3 --track
```

이 모드는 3프레임마다 YOLO를 실행하고 중간 프레임에서는 1/4 크기 흑백 영상으로
박스를 추적합니다. 추적 신뢰도가 낮거나 새로운 움직임이 감지되면 즉시 YOLO를
다시 실행합니다. 추론 횟수를 줄이는 정확도·반응성 절충 정책이며, 동일 작업량의
코드 최적화와는 구분해야 합니다.

전체 옵션은 다음 명령으로 확인합니다.

```sh
./yolo11-person --help
```

## 성능 측정

```sh
./yolo11-person -m yolo11n-416.onnx -i input.mp4 -o output.mp4 \
  --metrics metrics.json --detections detections.csv
```

`metrics.json`에는 프레임 수, 추론·추적 횟수, CPU/실행 시간과 전처리·추론·후처리·
그리기·출력 단계별 시간이 기록됩니다. `detections.csv`에는 프레임별 검출 결과가
기록됩니다. 정답 라벨 없이 두 CSV를 비교한 값은 실제 정확도(mAP)가 아니라 설정
사이의 검출 일치도입니다.

## 테스트

```sh
make test
make test-media
build/test_media input.mp4 /tmp/test-output.mp4
```

`make test`는 전처리, 출력 해석, NMS, 그리기와 추적 경계를 검사합니다.
`test_media`는 FFmpeg 스트리밍 입출력을 검사합니다.

## 소스 구조

- `src/main.c`: CLI, 실행 흐름과 추론 주기 정책
- `src/media_ffmpeg.c`: 파일/카메라 디코딩, RGB 변환과 출력
- `src/detector_ort.c`: ONNX Runtime 세션과 추론
- `src/postprocess.c`: letterbox 전처리, person 필터와 NMS
- `src/draw.c`: RGB 프레임에 박스와 태그 직접 그리기
- `src/tracker.c`: 선택적 저해상도 블록 추적과 조기 재추론 요청
- `src/platform.c`: Windows/POSIX 시간과 CPU 측정
- `include/`: 공개 구조체와 함수 선언
- `tests/`: 핵심 알고리즘 및 미디어 입출력 테스트

## 제한 사항

- detection 모델의 `person` 클래스만 처리하며 segmentation, pose, OBB는 지원하지
  않습니다.
- 오디오는 저장하지 않습니다.
- 추적은 박스의 평행 이동만 보정하고 크기는 다음 전체 추론에서 갱신합니다.
- `yolo11n.pt` 원본 가중치는 포함되어 있지 않습니다. 다른 입력 크기로 모델을
  내보내려면 원본 가중치를 별도로 준비해야 합니다.
- 저장소의 ONNX Runtime은 macOS 전용입니다. Windows/Linux용 런타임은 각
  플랫폼에서 별도로 준비해야 합니다.
- Windows DirectML 성능은 실제 i5-4200U 대상 장치에서 CPU provider와 같은
  조건으로 측정해야 합니다.
- 배포 전 Ultralytics 모델/코드와 FFmpeg 빌드 구성의 라이선스를 확인해야 합니다.
