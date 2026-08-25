# ?= 는 사용자가 CC를 지정하지 않았을 때만 기본값을 넣는 Make 문법입니다.
CC ?= cc
CFLAGS ?= -O3 -DNDEBUG
CPPFLAGS += -Iinclude
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow

# FFmpeg와 ONNX Runtime의 헤더/라이브러리 위치를 pkg-config에서 찾습니다.
FFMPEG_PACKAGES := libavformat libavcodec libavutil libswscale libavdevice
FFMPEG_CFLAGS := $(shell pkg-config --cflags $(FFMPEG_PACKAGES) 2>/dev/null)
FFMPEG_LIBS := $(shell pkg-config --libs $(FFMPEG_PACKAGES) 2>/dev/null)
ORT_CFLAGS := $(shell pkg-config --cflags onnxruntime 2>/dev/null)
ORT_LIBS := $(shell pkg-config --libs onnxruntime 2>/dev/null)
BREW_FFMPEG := $(shell brew --prefix ffmpeg 2>/dev/null)
BUNDLED_ORT := $(CURDIR)/third_party/onnxruntime

# 프로젝트에 내려받은 ONNX Runtime이 있으면 별도 ORT_ROOT 지정 없이 사용합니다.
ifeq ($(strip $(ORT_ROOT)),)
ifneq ($(wildcard $(BUNDLED_ORT)/include/onnxruntime_c_api.h),)
ORT_ROOT := $(BUNDLED_ORT)
endif
endif

# pkg-config가 FFmpeg를 못 찾은 macOS에서는 Homebrew 설치 경로를 대신 사용합니다.
ifeq ($(strip $(FFMPEG_LIBS)),)
ifneq ($(wildcard $(BREW_FFMPEG)/include/libavformat/avformat.h),)
FFMPEG_CFLAGS := -isystem $(BREW_FFMPEG)/include
FFMPEG_LIBS := -L$(BREW_FFMPEG)/lib -lavformat -lavcodec -lavutil -lswscale -lavdevice
endif
endif

# make ORT_ROOT=/경로 로 지정했다면 해당 ONNX Runtime을 빌드와 실행에 연결합니다.
ifneq ($(strip $(ORT_ROOT)),)
ORT_CFLAGS += -I$(ORT_ROOT)/include
ORT_LIBS += -L$(ORT_ROOT)/lib -lonnxruntime
ifeq ($(shell uname -s),Darwin)
ORT_LIBS += -Wl,-rpath,$(ORT_ROOT)/lib
endif
endif

# 각 .c 파일을 .o(목적 파일)로 컴파일한 뒤 마지막에 실행 파일로 연결합니다.
SOURCES := src/main.c src/postprocess.c src/draw.c src/detector_ort.c \
           src/media_ffmpeg.c src/tracker.c src/platform.c
OBJECTS := $(SOURCES:.c=.o)
TARGET := yolo11-person

.PHONY: all clean test test-media check-deps check-ffmpeg debug i5-4200u

all: check-deps $(TARGET)

# @를 붙이면 검사에 사용한 셸 명령 자체는 화면에 출력하지 않습니다.
check-ffmpeg:
	@test -n "$(FFMPEG_LIBS)" || (echo "FFmpeg development packages not found (pkg-config: $(FFMPEG_PACKAGES))" >&2; exit 1)

check-deps: check-ffmpeg
	@test -n "$(ORT_LIBS)" || (echo "ONNX Runtime not found; set ORT_ROOT or install onnxruntime.pc" >&2; exit 1)

# 아래 규칙에서 $^는 필요한 모든 .o 파일, $@는 만들 대상 yolo11-person입니다.
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(WARNINGS) $^ $(FFMPEG_LIBS) $(ORT_LIBS) -lm -o $@

# 패턴 규칙: src/name.c가 바뀌면 해당 src/name.o만 다시 컴파일합니다.
%.o: %.c
	$(CC) $(CPPFLAGS) $(FFMPEG_CFLAGS) $(ORT_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -c $< -o $@

test: build/test_core
	./build/test_core

# 테스트 실행 파일은 제품 실행 파일과 분리하여 build/ 아래에 만듭니다.
test-media: check-ffmpeg build/test_media
	@echo "Run build/test_media INPUT OUTPUT to exercise FFmpeg streaming I/O."

build/test_core: tests/test_core.c src/postprocess.c src/draw.c src/tracker.c \
                 include/yolo11.h include/tracker.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARNINGS) -std=c11 \
		tests/test_core.c src/postprocess.c src/draw.c src/tracker.c -lm -o $@

build/test_media: tests/test_media.c src/media_ffmpeg.c src/platform.c \
                  include/media.h include/platform.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(FFMPEG_CFLAGS) -O1 -g $(WARNINGS) -std=c11 \
		tests/test_media.c src/media_ffmpeg.c src/platform.c $(FFMPEG_LIBS) -o $@

debug: CFLAGS := -O1 -g3 -fsanitize=address,undefined
debug: clean all

# Run this target on the deployment machine. Haswell enables the i5-4200U AVX2
# instruction set for application-side preprocessing and drawing code.
i5-4200u:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O3 -DNDEBUG -march=haswell -mtune=haswell"

# 자동 생성 파일만 삭제하고 사람이 작성한 소스와 입력/결과 파일은 유지합니다.
clean:
	rm -f $(OBJECTS) $(TARGET) build/test_core build/test_media
	rm -rf build/test_core.dSYM build/test_media.dSYM
