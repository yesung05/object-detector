#ifndef INFER_CACHE_H
#define INFER_CACHE_H

#include "yolo11.h"

#include <stddef.h>
#include <stdint.h>

/*
 * 추론 결과 기록·재생 캐시 (오프라인 평가 전용, 기본은 꺼짐).
 *
 * 왜 필요한가: 룰 임계값을 바꿔 가며 같은 영상을 여러 번 평가할 때마다 포즈 모델을 다시 돌리면
 * 클립 1,450개 × 180프레임에 설정 하나당 CPU로 수 시간이 걸린다. 프로젝트 원칙(룰 기반 우선,
 * AI 호출 최소화)에도 어긋난다. 모델 출력은 룰 파라미터와 무관하므로 클립당 한 번만 기록하고,
 * 이후에는 같은 검출 결과를 돌려주면서 디코딩·트래커·룰은 실제와 똑같이 실행한다.
 *
 * 왜 detector_run 한 곳인가: --detections CSV 는 낮은 신뢰도 박스와 회전 재검사 결과가 빠져 있어
 * 트래커·소실 룰 입력을 재현할 수 없다. 모델 호출 경계에서 저장하면 호출자가 보는 것과 같다.
 *
 * 안전 장치(틀린 결과를 조용히 재생하지 않는다):
 *   - 헤더에 모델 크기·신뢰도·IoU·후보 상한·입력 크기·sizeof(Detection)을 넣고, 다르면 열기를 거부한다.
 *   - 호출마다 순번과 프레임 지문(샘플 픽셀 해시)을 확인하고, 하나라도 다르면 그 시점부터 재생을
 *     멈춘다(miss). 호출자는 miss 면 실제 추론을 한다.
 *   - 정상 종료(close)한 파일만 재생한다. 중단된 파일은 처음부터 다시 기록한다.
 *
 * 한계: 호출 순서가 결정적이어야 한다(단일 스레드). 표면 감시처럼 다른 스레드가 같은 감지기를
 * 부르면 순번이 어긋나 miss 가 난다(결과는 여전히 정확하고 느려질 뿐). 파일은 같은 빌드·같은
 * 기계에서만 유효하다(Detection 구조체를 그대로 저장) — 버릴 수 있는 캐시다.
 */

/* 불완전 타입: 멤버는 infer_cache.c 에만 있습니다. */
typedef struct InferCache InferCache;

/* 캐시가 어떤 모델·설정의 결과인지 나타내는 지문. 하나라도 다르면 재생하지 않는다. */
typedef struct {
    uint64_t model_bytes;     /* 모델 파일 크기 */
    uint64_t max_candidates;
    uint64_t max_detections;
    float    confidence;
    float    iou;
    int32_t  input_width;
    int32_t  input_height;
} InferCacheIdentity;

typedef enum {
    INFER_CACHE_RECORD = 1,   /* 새로 기록 중: store 를 호출한다 */
    INFER_CACHE_REPLAY = 2    /* 유효한 캐시를 재생 중: lookup 을 호출한다 */
} InferCacheMode;

/*
 * path 가 없거나 비어 있으면 새로 기록(RECORD), 정상 종료된 같은 지문의 파일이면 재생(REPLAY),
 * 정상 종료되지 않은 같은 지문의 파일이면 처음부터 다시 기록한다.
 * 우리 파일이 아니거나 지문이 다르면 NULL(error 에 사유) — 알 수 없는 파일을 덮어쓰지 않는다.
 * 반환 포인터: 호출자 소유, infer_cache_close 로 해제한다(NULL 안전).
 */
InferCache *infer_cache_open(const char *path, const InferCacheIdentity *identity,
                             char *error, size_t error_size);

/* RECORD 였다면 헤더에 "정상 종료"를 표시하고 닫는다. NULL 은 무시한다. */
void infer_cache_close(InferCache *cache);

InferCacheMode infer_cache_mode(const InferCache *cache);

/*
 * REPLAY 에서 다음 호출의 기록을 out 에 채운다.
 * 반환: 1 = hit(out 을 채움), 0 = miss(순번·지문 불일치 또는 기록 끝; 이후는 계속 miss),
 *      -1 = 오류(RECORD 모드에서 호출했거나 기록 개수가 out 의 capacity 를 넘음).
 * rgb 는 빌려 쓰며 읽기만 한다(호출 동안만 유효). confidence 는 이번 호출의 현재 신뢰도로, 프레임
 * 지문에 섞는다(생성 뒤 detector_set_confidence 로 바뀔 수 있으므로 헤더 지문만으로는 부족하다).
 */
int infer_cache_lookup(InferCache *cache, const uint8_t *rgb, int width, int height,
                       int stride, float confidence, DetectionList *out);

/* RECORD 에서 이번 호출의 결과를 추가한다. 반환: 0 성공, -1 실패(RECORD 가 아니거나 쓰기 실패). */
int infer_cache_store(InferCache *cache, const uint8_t *rgb, int width, int height,
                      int stride, float confidence, const DetectionList *detections);

#endif /* INFER_CACHE_H */
