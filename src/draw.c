#include "yolo11.h"

#include <stdio.h>
#include <string.h>

/* 정수 좌표가 이미지 범위를 벗어나지 않게 제한합니다. */
static int clampi(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static void set_pixel(uint8_t *rgb, int width, int height, int stride,
                      int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *pixel;
    if (x < 0 || y < 0 || x >= width || y >= height) return;

    /*
     * RGB24에서는 한 픽셀이 R,G,B 세 바이트입니다.
     * y * stride로 행을 찾고 x * 3으로 그 행 안의 픽셀을 찾습니다.
     */
    pixel = rgb + y * stride + x * 3;
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
}

/*
 * 태그가 잘 보이도록 기존 픽셀을 완전히 덮지 않고 어둡게 섞습니다.
 * 별도 반투명 레이어를 만들지 않고 원본 RGB 버퍼를 제자리에서 수정합니다.
 */
static void fill_blended(uint8_t *rgb, int width, int height, int stride,
                         int x1, int y1, int x2, int y2) {
    x1 = clampi(x1, 0, width);
    y1 = clampi(y1, 0, height);
    x2 = clampi(x2, 0, width);
    y2 = clampi(y2, 0, height);
    for (int y = y1; y < y2; ++y) {
        uint8_t *pixel = rgb + y * stride + x1 * 3;
        for (int x = x1; x < x2; ++x, pixel += 3) {
            pixel[0] = (uint8_t)(pixel[0] / 4 + 4);
            pixel[1] = (uint8_t)(pixel[1] / 4 + 9);
            pixel[2] = (uint8_t)(pixel[2] / 4 + 7);
        }
    }
}

/*
 * 외부 폰트 라이브러리를 쓰지 않기 위한 5x7 비트맵 글꼴입니다.
 * 각 숫자의 하위 5비트가 한 줄의 켜진 점을 나타냅니다. 예를 들어 30은
 * 이진수 11110이므로 해당 줄의 네 점을 켭니다.
 *
 * "PERSON 87%"에 필요한 문자만 넣어 실행 파일과 메모리 사용량을 줄였습니다.
 */
static const uint8_t *glyph(char c) {
    static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t p[7] = {30, 17, 17, 30, 16, 16, 16};
    static const uint8_t e[7] = {31, 16, 16, 30, 16, 16, 31};
    static const uint8_t r[7] = {30, 17, 17, 30, 20, 18, 17};
    static const uint8_t s[7] = {15, 16, 16, 14, 1, 1, 30};
    static const uint8_t o[7] = {14, 17, 17, 17, 17, 17, 14};
    static const uint8_t n[7] = {17, 25, 21, 19, 17, 17, 17};
    static const uint8_t percent[7] = {25, 26, 4, 4, 11, 19, 0};
    static const uint8_t digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14},
        {4, 12, 4, 4, 4, 4, 14},
        {14, 17, 1, 2, 4, 8, 31},
        {30, 1, 1, 14, 1, 1, 30},
        {2, 6, 10, 18, 31, 2, 2},
        {31, 16, 16, 30, 1, 1, 30},
        {14, 16, 16, 30, 17, 17, 14},
        {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14},
        {14, 17, 17, 15, 1, 1, 14}
    };
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
        case 'P': return p;
        case 'E': return e;
        case 'R': return r;
        case 'S': return s;
        case 'O': return o;
        case 'N': return n;
        case '%': return percent;
        default: return blank;
    }
}

/* 5x7 글자를 scale배 확대해서 흰색 픽셀로 하나씩 그립니다. */
static void draw_text(uint8_t *rgb, int width, int height, int stride,
                      int x, int y, const char *text, int scale) {
    for (size_t i = 0; text[i] != '\0'; ++i) {
        const uint8_t *rows = glyph(text[i]);
        for (int gy = 0; gy < 7; ++gy) {
            for (int gx = 0; gx < 5; ++gx) {
                /* 비트 AND 결과가 0이 아니면 글자 모양에 포함된 점입니다. */
                if (rows[gy] & (1u << (4 - gx))) {
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            set_pixel(rgb, width, height, stride,
                                      x + (int)i * 6 * scale + gx * scale + sx,
                                      y + gy * scale + sy,
                                      245, 255, 248);
                        }
                    }
                }
            }
        }
    }
}

/*
 * 사각형 전체를 채우지 않고 위·아래·왼쪽·오른쪽 테두리만 thickness만큼 그립니다.
 * set_pixel이 범위를 검사하므로 영상 가장자리의 박스도 안전합니다.
 */
static void draw_rectangle(uint8_t *rgb, int width, int height, int stride,
                           int x1, int y1, int x2, int y2, int thickness,
                           uint8_t r, uint8_t g, uint8_t b) {
    for (int t = 0; t < thickness; ++t) {
        for (int x = x1; x <= x2; ++x) {
            set_pixel(rgb, width, height, stride, x, y1 + t, r, g, b);
            set_pixel(rgb, width, height, stride, x, y2 - t, r, g, b);
        }
        for (int y = y1; y <= y2; ++y) {
            set_pixel(rgb, width, height, stride, x1 + t, y, r, g, b);
            set_pixel(rgb, width, height, stride, x2 - t, y, r, g, b);
        }
    }
}

void draw_detections(uint8_t *rgb, int width, int height, int stride,
                     const DetectionList *detections) {
    int min_dimension;
    int thickness;
    int font_scale;

    if (!rgb || !detections || width <= 0 || height <= 0 ||
        stride < width * 3) return;

    /* 해상도가 커지면 선과 글씨도 조금 키우되, 선은 1~4픽셀로 제한합니다. */
    min_dimension = width < height ? width : height;
    thickness = min_dimension / 320;
    if (thickness < 1) thickness = 1;
    if (thickness > 4) thickness = 4;
    font_scale = min_dimension >= 600 ? 2 : 1;

    for (size_t i = 0; i < detections->count; ++i) {
        const Detection *d = &detections->items[i];
        char label[24];
        int x1 = clampi((int)(d->x1 + 0.5f), 0, width - 1);
        int y1 = clampi((int)(d->y1 + 0.5f), 0, height - 1);
        int x2 = clampi((int)(d->x2 + 0.5f), 0, width - 1);
        int y2 = clampi((int)(d->y2 + 0.5f), 0, height - 1);
        int label_height = 7 * font_scale + 6;
        int label_y;
        int label_width;
        int confidence = (int)(d->score * 100.0f + 0.5f);

        /* float 좌표와 점수를 사람이 보기 좋은 정수 픽셀/백분율로 바꿉니다. */
        if (confidence > 100) confidence = 100;
        snprintf(label, sizeof(label), "PERSON %d%%", confidence);
        label_width = (int)strlen(label) * 6 * font_scale + 6;
        if (label_width > width) label_width = width;
        label_y = y1 >= label_height ? y1 - label_height : y1;

        /* 별도의 그림자나 보조 외곽선 없이 녹색 한 가지 색으로만 그립니다. */
        draw_rectangle(rgb, width, height, stride, x1, y1, x2, y2, thickness,
                       0, 224, 96);
        fill_blended(rgb, width, height, stride, x1, label_y,
                     x1 + label_width, label_y + label_height);

        /* 태그 위쪽의 얇은 녹색 막대와 "PERSON {percent}%" 글자를 추가합니다. */
        for (int bar = 0; bar < 2; ++bar) {
            for (int x = x1; x < x1 + label_width && x < width; ++x) {
                set_pixel(rgb, width, height, stride, x, label_y + bar,
                          0, 224, 96);
            }
        }
        draw_text(rgb, width, height, stride, x1 + 3, label_y + 4,
                  label, font_scale);
    }
}
