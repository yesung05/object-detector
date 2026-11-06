#include "privacy_style.h"

#include <stdlib.h>
#include <string.h>

/*
 * 가려진 화면 렌더러. 순수 정수 연산이고 외부 라이브러리가 없다.
 *
 * 왜 OpenCV 등을 쓰지 않는가: 배포 기기에 의존성을 늘리지 않는다(FFmpeg·ORT 외 라이브러리 없음). 필요한 연산이
 * 박스 평균 축소, 3x3/5탭 가우시안, Sobel, 비최대 억제뿐이라 직접 짜는 쪽이 작고, 해상도마다 한 번만 할당한
 * 버퍼를 프레임마다 재사용할 수 있다(프로젝트의 "버퍼는 한 번만 할당" 원칙).
 *
 * 처리 순서(개발 PC 실험에서 정한 스타일):
 *   1) 그레이스케일
 *   2) 배경 = 칸(cell) 단위 박스 평균(정보 제거) → 축소 이미지에서 3x3 가우시안 → 쌍선형 확대
 *      축소 이미지에서 블러하는 이유: 원본 해상도에서 시그마 0.7*cell(수십 px)의 가우시안을 거는 것과 거의 같은
 *      결과를 수백 배 적은 연산으로 얻는다(i5-4200U 2코어 예산).
 *   3) 엣지 = 약한 가우시안(5탭을 edge_blur 번, 시그마≈√n) → Sobel(|gx|+|gy|) → 기울기 방향 비최대 억제 → 이력 임계
 *      Canny 와 같은 구성이지만 이력 추적을 한 번의 이웃 검사로 줄였다(강한 엣지에 닿은 약한 엣지만 남김).
 *      체인을 끝까지 따라가는 재귀 추적은 프레임당 시간이 들쭉날쭉해지고, 한 단계 차이는 시각적으로 구분되지 않는다.
 *   4) 합성: 엣지 위치(2x2로 두껍게)는 흰색, 나머지는 배경. R=G=B 라서 색 정보가 전혀 남지 않는다.
 */

struct PrivacyScratch {
    int w, h;                 /* 마지막으로 할당한 해상도 */
    int sw, sh, cell;         /* 축소 이미지 크기와 칸 크기 */
    uint8_t *gray;            /* w*h 그레이스케일 */
    uint8_t *tmp;             /* w*h 작업용(가로 블러 → 이후 NMS 결과) */
    uint8_t *blur;            /* w*h 약한 가우시안 결과 */
    uint8_t *mag;             /* w*h Sobel 크기(255 로 자름) → 이후 확정 엣지 0/1 */
    uint8_t *dir;             /* w*h 기울기 방향 0~3 */
    uint8_t *small_a, *small_b; /* sw*sh 축소 이미지와 블러 임시 */
    int32_t *col_x0;          /* w 확대용 열 색인 */
    uint8_t *col_wx;          /* w 확대용 열 가중치(0~255) */
    uint8_t alpha_lut[256];   /* 엣지 알파 × edge_opacity 환산표(프레임마다 갱신) */
};

static void scratch_free_buffers(PrivacyScratch *s) {
    free(s->gray); free(s->tmp); free(s->blur); free(s->mag); free(s->dir);
    free(s->small_a); free(s->small_b); free(s->col_x0); free(s->col_wx);
    s->gray = s->tmp = s->blur = s->mag = s->dir = s->small_a = s->small_b = NULL;
    s->col_x0 = NULL;
    s->col_wx = NULL;
    s->w = s->h = s->sw = s->sh = s->cell = 0;
}

PrivacyScratch *privacy_scratch_create(void) {
    return (PrivacyScratch *)calloc(1, sizeof(PrivacyScratch));
}

void privacy_scratch_destroy(PrivacyScratch *scratch) {
    if (!scratch) return;
    scratch_free_buffers(scratch);
    free(scratch);
}

PrivacyParams privacy_params_default(void) {
    PrivacyParams p;
    p.style = PRIVACY_STYLE_SOLID;
    p.solid_gray = 0x24;
    p.cell = 24;
    p.edge_low = 40;
    p.edge_high = 110;
    p.edge_blur = 8;
    p.edge_soft = 1;
    p.edge_opacity = 70;
    return p;
}

void privacy_params_sanitize(PrivacyParams *p) {
    if (!p) return;
    if (p->style != PRIVACY_STYLE_EDGE_GRAY) p->style = PRIVACY_STYLE_SOLID;
    if (p->solid_gray <= 0 || p->solid_gray > 255) p->solid_gray = 0x24;
    if (p->cell < 8) p->cell = 8;
    if (p->cell > 64) p->cell = 64;
    if (p->edge_low < 5) p->edge_low = 5;
    if (p->edge_low > 250) p->edge_low = 250;
    if (p->edge_high < p->edge_low) p->edge_high = p->edge_low;
    if (p->edge_high > 255) p->edge_high = 255;
    if (p->edge_blur < 1) p->edge_blur = 1;
    if (p->edge_blur > 8) p->edge_blur = 8;
    if (p->edge_soft < 0) p->edge_soft = 0;
    if (p->edge_soft > 4) p->edge_soft = 4;
    if (p->edge_opacity < 10) p->edge_opacity = 10;
    if (p->edge_opacity > 100) p->edge_opacity = 100;
}

static void fill_solid(uint8_t *rgb, int width, int height, int stride, int gray) {
    int y;
    for (y = 0; y < height; ++y) memset(rgb + (size_t)y * (size_t)stride, gray, (size_t)width * 3);
}

static int scratch_prepare(PrivacyScratch *s, int w, int h, int cell) {
    size_t n = (size_t)w * (size_t)h;
    int sw, sh;
    if (s->w == w && s->h == h && s->cell == cell && s->gray) return 0;
    scratch_free_buffers(s);
    /* 칸이 너무 크면 한 변에 칸이 1개 미만이 되므로 최소 2칸이 되게 줄인다. */
    sw = w / cell; sh = h / cell;
    if (sw < 2) sw = 2;
    if (sh < 2) sh = 2;
    s->gray = (uint8_t *)malloc(n);
    s->tmp = (uint8_t *)malloc(n);
    s->blur = (uint8_t *)malloc(n);
    s->mag = (uint8_t *)malloc(n);
    s->dir = (uint8_t *)malloc(n);
    s->small_a = (uint8_t *)malloc((size_t)sw * (size_t)sh);
    s->small_b = (uint8_t *)malloc((size_t)sw * (size_t)sh);
    s->col_x0 = (int32_t *)malloc((size_t)w * sizeof(int32_t));
    s->col_wx = (uint8_t *)malloc((size_t)w);
    if (!s->gray || !s->tmp || !s->blur || !s->mag || !s->dir || !s->small_a || !s->small_b ||
        !s->col_x0 || !s->col_wx) {
        scratch_free_buffers(s);
        return -1;
    }
    s->w = w; s->h = h; s->sw = sw; s->sh = sh; s->cell = cell;
    return 0;
}

/* 5탭 가우시안 [1 4 6 4 1]/16 (시그마≈1). 가장자리는 가장 가까운 픽셀로 복제한다. */
static void gaussian5(const uint8_t *src, uint8_t *tmp, uint8_t *dst, int w, int h) {
    int x, y;
    for (y = 0; y < h; ++y) {
        const uint8_t *r = src + (size_t)y * w;
        uint8_t *o = tmp + (size_t)y * w;
        for (x = 0; x < w; ++x) {
            int xm2 = x < 2 ? 0 : x - 2, xm1 = x < 1 ? 0 : x - 1;
            int xp1 = x + 1 < w ? x + 1 : w - 1, xp2 = x + 2 < w ? x + 2 : w - 1;
            o[x] = (uint8_t)((r[xm2] + 4 * r[xm1] + 6 * r[x] + 4 * r[xp1] + r[xp2] + 8) >> 4);
        }
    }
    for (y = 0; y < h; ++y) {
        const uint8_t *a = tmp + (size_t)(y < 2 ? 0 : y - 2) * w;
        const uint8_t *b = tmp + (size_t)(y < 1 ? 0 : y - 1) * w;
        const uint8_t *c = tmp + (size_t)y * w;
        const uint8_t *d = tmp + (size_t)(y + 1 < h ? y + 1 : h - 1) * w;
        const uint8_t *e = tmp + (size_t)(y + 2 < h ? y + 2 : h - 1) * w;
        uint8_t *o = dst + (size_t)y * w;
        for (x = 0; x < w; ++x) o[x] = (uint8_t)((a[x] + 4 * b[x] + 6 * c[x] + 4 * d[x] + e[x] + 8) >> 4);
    }
}

int privacy_style_apply(PrivacyScratch *scratch, uint8_t *rgb, int width, int height, int stride,
                        const PrivacyParams *params) {
    PrivacyParams p;
    PrivacyScratch *s = scratch;
    int x, y, sx, sy;
    int hi, lo;
    if (!rgb || width <= 0 || height <= 0 || stride < width * 3) return -1;
    p = params ? *params : privacy_params_default();
    privacy_params_sanitize(&p);
    if (p.style != PRIVACY_STYLE_EDGE_GRAY) {
        fill_solid(rgb, width, height, stride, p.solid_gray);
        return 0;
    }
    /* 3x3 Sobel 과 5탭 가우시안에는 최소 크기가 필요하다. 이보다 작은 프레임은 단색으로 안전하게 처리한다. */
    if (!s || width < 16 || height < 16 || scratch_prepare(s, width, height, p.cell) != 0) {
        fill_solid(rgb, width, height, stride, p.solid_gray);
        return -1;
    }

    /* 1) 그레이스케일: ITU-R BT.601 계수를 8비트 고정소수점으로(77+150+29=256). */
    for (y = 0; y < height; ++y) {
        const uint8_t *r = rgb + (size_t)y * (size_t)stride;
        uint8_t *g = s->gray + (size_t)y * width;
        for (x = 0; x < width; ++x, r += 3) g[x] = (uint8_t)((77 * r[0] + 150 * r[1] + 29 * r[2]) >> 8);
    }

    /* 2) 배경: 칸 박스 평균 → 3x3 이항 가우시안(축소 이미지에서) */
    for (sy = 0; sy < s->sh; ++sy) {
        int y0 = (int)((long long)sy * height / s->sh), y1 = (int)((long long)(sy + 1) * height / s->sh);
        for (sx = 0; sx < s->sw; ++sx) {
            int x0 = (int)((long long)sx * width / s->sw), x1 = (int)((long long)(sx + 1) * width / s->sw);
            unsigned sum = 0, cnt = (unsigned)((y1 - y0) * (x1 - x0));
            for (y = y0; y < y1; ++y) {
                const uint8_t *g = s->gray + (size_t)y * width;
                for (x = x0; x < x1; ++x) sum += g[x];
            }
            s->small_a[(size_t)sy * s->sw + sx] = (uint8_t)(cnt ? (sum + cnt / 2) / cnt : 0);
        }
    }
    for (sy = 0; sy < s->sh; ++sy) {          /* 가로 [1 2 1]/4 */
        const uint8_t *r = s->small_a + (size_t)sy * s->sw;
        uint8_t *o = s->small_b + (size_t)sy * s->sw;
        for (sx = 0; sx < s->sw; ++sx) {
            int l = sx > 0 ? sx - 1 : 0, rr = sx + 1 < s->sw ? sx + 1 : s->sw - 1;
            o[sx] = (uint8_t)((r[l] + 2 * r[sx] + r[rr] + 2) >> 2);
        }
    }
    for (sy = 0; sy < s->sh; ++sy) {          /* 세로 [1 2 1]/4 */
        const uint8_t *a = s->small_b + (size_t)(sy > 0 ? sy - 1 : 0) * s->sw;
        const uint8_t *b = s->small_b + (size_t)sy * s->sw;
        const uint8_t *c = s->small_b + (size_t)(sy + 1 < s->sh ? sy + 1 : s->sh - 1) * s->sw;
        uint8_t *o = s->small_a + (size_t)sy * s->sw;
        for (sx = 0; sx < s->sw; ++sx) o[sx] = (uint8_t)((a[sx] + 2 * b[sx] + c[sx] + 2) >> 2);
    }
    /* 확대용 열 색인·가중치(쌍선형, 1/256 고정소수점) */
    for (x = 0; x < width; ++x) {
        long long v = ((long long)(2 * x + 1) * s->sw * 128) / width - 128;
        int x0, wx;
        if (v < 0) { x0 = 0; wx = 0; }
        else { x0 = (int)(v >> 8); wx = (int)(v & 255); if (x0 >= s->sw - 1) { x0 = s->sw - 1; wx = 0; } }
        s->col_x0[x] = x0;
        s->col_wx[x] = (uint8_t)wx;
    }

    /* 3) 엣지 */
    /* 5탭 가우시안을 여러 번 겹쳐 시그마를 키운다(√n). 큰 커널 하나보다 단순하고, 한 번씩 가로·세로 두 패스라
       n 이 작을 때(기본 3) 비용이 선형으로만 는다. 두 번째부터는 제자리(src==dst)로 돌려도 안전하다:
       가로 패스가 tmp 에 쓰고 세로 패스가 tmp 에서 읽어 dst 에 쓰므로 src 는 가로 패스 뒤 더 읽지 않는다. */
    gaussian5(s->gray, s->tmp, s->blur, width, height);
    for (x = 1; x < p.edge_blur; ++x) gaussian5(s->blur, s->tmp, s->blur, width, height);
    lo = p.edge_low; hi = p.edge_high;
    memset(s->mag, 0, (size_t)width * height);
    memset(s->dir, 0, (size_t)width * height);
    for (y = 1; y < height - 1; ++y) {
        const uint8_t *r0 = s->blur + (size_t)(y - 1) * width;
        const uint8_t *r1 = s->blur + (size_t)y * width;
        const uint8_t *r2 = s->blur + (size_t)(y + 1) * width;
        uint8_t *mo = s->mag + (size_t)y * width;
        uint8_t *dd = s->dir + (size_t)y * width;
        for (x = 1; x < width - 1; ++x) {
            int gx = (r0[x + 1] + 2 * r1[x + 1] + r2[x + 1]) - (r0[x - 1] + 2 * r1[x - 1] + r2[x - 1]);
            int gy = (r2[x - 1] + 2 * r2[x] + r2[x + 1]) - (r0[x - 1] + 2 * r0[x] + r0[x + 1]);
            int ax = gx < 0 ? -gx : gx, ay = gy < 0 ? -gy : gy, m = ax + ay;
            mo[x] = (uint8_t)(m > 255 ? 255 : m);
            /* 기울기 방향을 4방향으로 양자화(22.5° 경계: tan≈0.4). y 는 아래로 증가한다. */
            if (ay * 5 < ax * 2) dd[x] = 0;                 /* 가로 기울기 → 좌우 비교 */
            else if (ax * 5 < ay * 2) dd[x] = 2;            /* 세로 기울기 → 상하 비교 */
            else dd[x] = (uint8_t)((gx > 0) == (gy > 0) ? 1 : 3); /* 대각 */
        }
    }
    /* 비최대 억제 + 임계 분류 → tmp: 2 강한 엣지, 1 약한 엣지, 0 없음 */
    memset(s->tmp, 0, (size_t)width * height);
    for (y = 1; y < height - 1; ++y) {
        const uint8_t *mo = s->mag + (size_t)y * width;
        const uint8_t *dd = s->dir + (size_t)y * width;
        uint8_t *eo = s->tmp + (size_t)y * width;
        for (x = 1; x < width - 1; ++x) {
            int m = mo[x], n1, n2;
            if (m < lo) continue;
            switch (dd[x]) {
                case 0:  n1 = mo[x - 1]; n2 = mo[x + 1]; break;
                case 2:  n1 = mo[x - width]; n2 = mo[x + width]; break;
                case 1:  n1 = mo[x - width - 1]; n2 = mo[x + width + 1]; break;
                default: n1 = mo[x - width + 1]; n2 = mo[x + width - 1]; break;
            }
            if (m < n1 || m <= n2) continue;      /* 한쪽만 등호: 같은 크기의 평평한 능선이 두 줄로 남는 것을 막는다 */
            eo[x] = (uint8_t)(m >= hi ? 2 : 1);
        }
    }
    /* 이력: 약한 엣지는 8이웃에 강한 엣지가 있을 때만 남긴다. 결과는 mag(0/1)에 쓴다. */
    memset(s->mag, 0, (size_t)width * height);
    for (y = 1; y < height - 1; ++y) {
        const uint8_t *eo = s->tmp + (size_t)y * width;
        uint8_t *mo = s->mag + (size_t)y * width;
        for (x = 1; x < width - 1; ++x) {
            if (eo[x] == 2) { mo[x] = 1; continue; }
            if (eo[x] == 1 &&
                (eo[x - 1] == 2 || eo[x + 1] == 2 || eo[x - width - 1] == 2 || eo[x - width] == 2 ||
                 eo[x - width + 1] == 2 || eo[x + width - 1] == 2 || eo[x + width] == 2 || eo[x + width + 1] == 2))
                mo[x] = 1;
        }
    }

    /* 4) 엣지 알파: 확정 엣지를 2x2 로 두껍게 만든 0/255 마스크를 blur 버퍼에 만들고(Sobel 이 끝나 blur 는 더 쓰지 않는다),
       edge_soft 번 5탭 가우시안으로 번지게 한다(tmp 는 NMS 결과였지만 이력 단계 뒤로는 안 쓴다). 선이 번지면 가장자리가
       흐려져 덜 선명해지고, 번진 만큼 최대 밝기도 줄어든다. edge_opacity 는 그 위에 곱해 전체 선 농도를 낮춘다.
       soft=0, opacity=100 이면 예전의 "선명한 흰 선"과 비트 단위로 같다(전후 성능·모양 비교용). */
    for (y = 0; y < height; ++y) {
        const uint8_t *em = s->mag + (size_t)y * width;
        const uint8_t *ep = y > 0 ? s->mag + (size_t)(y - 1) * width : NULL;
        uint8_t *al = s->blur + (size_t)y * width;
        for (x = 0; x < width; ++x)
            al[x] = (uint8_t)((em[x] || (x > 0 && em[x - 1]) || (ep && (ep[x] || (x > 0 && ep[x - 1])))) ? 255 : 0);
    }
    for (x = 0; x < p.edge_soft; ++x) gaussian5(s->blur, s->tmp, s->blur, width, height);
    {
        int ai;
        for (ai = 0; ai < 256; ++ai) s->alpha_lut[ai] = (uint8_t)((ai * p.edge_opacity + 50) / 100);
    }

    /* 5) 합성: 쌍선형 확대한 배경 위에 엣지 알파만큼 흰색을 섞는다. 한 행씩 만들어 rgb 에 직접 쓴다. */
    for (y = 0; y < height; ++y) {
        long long v = ((long long)(2 * y + 1) * s->sh * 128) / height - 128;
        int y0, y1, wy;
        const uint8_t *ra, *rb, *al;
        uint8_t *o = rgb + (size_t)y * (size_t)stride;
        if (v < 0) { y0 = 0; y1 = 0; wy = 0; }
        else {
            y0 = (int)(v >> 8); wy = (int)(v & 255);
            if (y0 >= s->sh - 1) { y0 = s->sh - 1; y1 = y0; wy = 0; } else y1 = y0 + 1;
        }
        ra = s->small_a + (size_t)y0 * s->sw;
        rb = s->small_a + (size_t)y1 * s->sw;
        al = s->blur + (size_t)y * width;
        for (x = 0; x < width; ++x, o += 3) {
            int x0 = s->col_x0[x], x1 = x0 + 1 < s->sw ? x0 + 1 : x0, wx = s->col_wx[x];
            unsigned top = (unsigned)(ra[x0] * (256 - wx) + ra[x1] * wx);
            unsigned bot = (unsigned)(rb[x0] * (256 - wx) + rb[x1] * wx);
            uint8_t val = (uint8_t)((top * (unsigned)(256 - wy) + bot * (unsigned)wy) >> 16);
            unsigned a = s->alpha_lut[al[x]];
            if (a) val = (uint8_t)(val + (((255u - val) * a + 127u) / 255u));
            o[0] = o[1] = o[2] = val;
        }
    }
    return 0;
}
