#include "infer_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "HKIC" 을 little-endian 으로 읽은 값. 다른 파일을 캐시로 오인해 덮어쓰지 않으려는 표식이다. */
#define CACHE_MAGIC   0x43494B48u
#define CACHE_VERSION 1u

/* 파일 맨 앞. memset(0) 후 채워 패딩 바이트까지 결정적으로 만든다(같은 입력 → 같은 파일). */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t detection_bytes;  /* sizeof(Detection): 구조체가 바뀐 빌드의 캐시를 거부 */
    uint32_t complete;         /* 0 = 기록 중/중단, 1 = close 까지 정상 종료 */
    uint64_t calls;            /* complete==1 일 때 기록된 호출 수 */
    InferCacheIdentity identity;
} CacheHeader;

/* 호출 한 번의 머리. uint32 3개라 패딩이 없다. */
typedef struct {
    uint32_t index;   /* 0부터 시작하는 호출 순번 */
    uint32_t key;     /* 프레임 지문 */
    uint32_t count;   /* 뒤따르는 Detection 개수 */
} RecordHead;

struct InferCache {
    FILE *fp;                 /* InferCache 소유, infer_cache_close 에서 fclose */
    InferCacheMode mode;
    uint64_t next_index;
    int diverged;             /* REPLAY 에서 한 번이라도 miss 가 나면 1 — 이후는 모두 miss */
    CacheHeader header;       /* RECORD 종료 시 갱신해서 파일 맨 앞에 다시 쓴다 */
};

static void set_error(char *error, size_t error_size, const char *msg) {
    if (error && error_size) snprintf(error, error_size, "%s", msg);
}

static int identity_equal(const InferCacheIdentity *a, const InferCacheIdentity *b) {
    /* 구조체를 memcmp 하지 않고 필드별로 비교한다: 패딩 바이트 값에 기대지 않기 위해서다. */
    return a->model_bytes == b->model_bytes && a->max_candidates == b->max_candidates &&
           a->max_detections == b->max_detections && a->confidence == b->confidence &&
           a->iou == b->iou && a->input_width == b->input_width &&
           a->input_height == b->input_height;
}

/*
 * 프레임 지문: 전체 픽셀이 아니라 격자(최대 64x64)로 뽑은 픽셀의 FNV-1a 해시.
 * 전체를 해시하면 1080p 프레임마다 6MB 를 읽어야 해서 추론을 아낀 의미가 줄고,
 * 같은 디코더가 같은 영상을 풀면 프레임이 바이트 단위로 같으므로 표본으로 다른 프레임을
 * 가려내는 데 충분하다. 크기·stride 도 섞어 해상도가 다른 입력을 구분한다.
 */
static uint32_t frame_key(const uint8_t *rgb, int width, int height, int stride,
                          float confidence) {
    uint32_t h = 2166136261u;
    int sy = height / 64 > 0 ? height / 64 : 1;
    int sx = width / 64 > 0 ? width / 64 : 1;
    int x, y;
    uint32_t dims[4];
    const uint8_t *dp = (const uint8_t *)dims;
    size_t i;
    dims[0] = (uint32_t)width; dims[1] = (uint32_t)height; dims[2] = (uint32_t)stride;
    /* 신뢰도: detector_set_confidence 가 생성 뒤에 값을 바꿀 수 있어 헤더 지문만으로는 부족하다.
     * 호출마다 현재 값을 섞어, 기록 때와 다른 신뢰도로 재생하면 miss 가 되게 한다. */
    memcpy(&dims[3], &confidence, sizeof(confidence));
    for (i = 0; i < sizeof(dims); ++i) { h ^= dp[i]; h *= 16777619u; }
    for (y = 0; y < height; y += sy) {
        const uint8_t *row = rgb + (size_t)y * (size_t)stride;
        for (x = 0; x < width; x += sx) {
            const uint8_t *p = row + (size_t)x * 3;
            h ^= p[0]; h *= 16777619u;
            h ^= p[1]; h *= 16777619u;
            h ^= p[2]; h *= 16777619u;
        }
    }
    return h;
}

static int write_header(FILE *fp, const CacheHeader *h) {
    return fseek(fp, 0, SEEK_SET) == 0 && fwrite(h, sizeof(*h), 1, fp) == 1 &&
           fflush(fp) == 0;
}

InferCache *infer_cache_open(const char *path, const InferCacheIdentity *identity,
                             char *error, size_t error_size) {
    InferCache *c = NULL;
    FILE *fp = NULL;
    CacheHeader existing;
    long size = 0;

    if (!path || !path[0] || !identity) {
        set_error(error, error_size, "infer cache: invalid arguments");
        return NULL;
    }
    c = (InferCache *)calloc(1, sizeof(*c));
    if (!c) {
        set_error(error, error_size, "infer cache: out of memory");
        return NULL;
    }

    fp = fopen(path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (size > 0) {
            if (fread(&existing, sizeof(existing), 1, fp) != 1 || existing.magic != CACHE_MAGIC ||
                existing.version != CACHE_VERSION) {
                /* 우리 파일이 아니면 덮어쓰지 않는다 — 사용자의 다른 파일일 수 있다. */
                set_error(error, error_size, "infer cache: not a cache file (refusing to overwrite)");
                goto fail;
            }
            if (existing.detection_bytes != (uint32_t)sizeof(Detection) ||
                !identity_equal(&existing.identity, identity)) {
                set_error(error, error_size,
                          "infer cache: model/settings differ from the cached run "
                          "(delete the file to re-record)");
                goto fail;
            }
            if (existing.complete == 1) {
                /* 정상 종료된 같은 지문의 캐시 → 재생. fp 는 헤더 직후에 있다. */
                c->fp = fp;
                c->mode = INFER_CACHE_REPLAY;
                c->header = existing;
                return c;
            }
            /* complete==0: 기록 중 중단된 파일. 신뢰할 수 없으므로 처음부터 다시 기록한다. */
        }
        fclose(fp);
        fp = NULL;
    }

    fp = fopen(path, "wb+");
    if (!fp) {
        set_error(error, error_size, "infer cache: cannot create file");
        goto fail;
    }
    memset(&c->header, 0, sizeof(c->header));
    c->header.magic = CACHE_MAGIC;
    c->header.version = CACHE_VERSION;
    c->header.detection_bytes = (uint32_t)sizeof(Detection);
    c->header.complete = 0;
    c->header.identity = *identity;
    if (!write_header(fp, &c->header)) {
        set_error(error, error_size, "infer cache: cannot write header");
        goto fail;
    }
    c->fp = fp;
    c->mode = INFER_CACHE_RECORD;
    return c;

fail:
    if (fp) fclose(fp);
    free(c);
    return NULL;
}

void infer_cache_close(InferCache *c) {
    if (!c) return;
    if (c->fp) {
        if (c->mode == INFER_CACHE_RECORD) {
            c->header.complete = 1;
            c->header.calls = c->next_index;
            write_header(c->fp, &c->header);
        }
        fclose(c->fp);
    }
    free(c);
}

InferCacheMode infer_cache_mode(const InferCache *c) {
    return c ? c->mode : (InferCacheMode)0;
}

int infer_cache_lookup(InferCache *c, const uint8_t *rgb, int width, int height,
                       int stride, float confidence, DetectionList *out) {
    RecordHead rh;
    if (!c || c->mode != INFER_CACHE_REPLAY || !rgb || !out || !out->items) return -1;
    if (c->diverged) return 0;
    if (fread(&rh, sizeof(rh), 1, c->fp) != 1) {   /* 기록 끝: 이번 실행이 캐시보다 길다 */
        c->diverged = 1;
        return 0;
    }
    if (rh.index != (uint32_t)c->next_index || rh.key != frame_key(rgb, width, height, stride, confidence)) {
        c->diverged = 1;  /* 호출 순서나 프레임이 달라졌다 — 이후 기록은 믿을 수 없다 */
        return 0;
    }
    if (rh.count > out->capacity) return -1;
    if (rh.count && fread(out->items, sizeof(Detection), rh.count, c->fp) != rh.count) {
        c->diverged = 1;  /* 잘린 기록 */
        return 0;
    }
    out->count = rh.count;
    c->next_index++;
    return 1;
}

int infer_cache_store(InferCache *c, const uint8_t *rgb, int width, int height,
                      int stride, float confidence, const DetectionList *det) {
    RecordHead rh;
    if (!c || c->mode != INFER_CACHE_RECORD || !rgb || !det) return -1;
    rh.index = (uint32_t)c->next_index;
    rh.key = frame_key(rgb, width, height, stride, confidence);
    rh.count = (uint32_t)det->count;
    if (fwrite(&rh, sizeof(rh), 1, c->fp) != 1) return -1;
    if (det->count && fwrite(det->items, sizeof(Detection), det->count, c->fp) != det->count)
        return -1;
    c->next_index++;
    return 0;
}
