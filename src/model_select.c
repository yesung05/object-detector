#include "model_select.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  define PATH_SEP "\\"
#else
#  include <dirent.h>
#  define PATH_SEP "/"
#endif

/*
 * 파일명 끝에서 "-WIDTHxHEIGHT.onnx" 패턴을 찾아 w, h를 꺼냅니다.
 * 예) "yolo11n-416x224.onnx" → w=416, h=224
 * 반환: 1 = 파싱 성공, 0 = 패턴 없음
 */
static int parse_model_size(const char *name, int *w, int *h) {
    size_t len = strlen(name);
    /* ".onnx" 확인 */
    if (len < 10 || strcmp(name + len - 5, ".onnx") != 0) return 0;

    const char *p = name + len - 5; /* ".onnx" 바로 앞 */

    /* 숫자 (height) 역방향 탐색 */
    const char *hend = p;
    while (p > name && p[-1] >= '0' && p[-1] <= '9') --p;
    if (p == hend || p == name || p[-1] != 'x') return 0;
    int height = atoi(p);
    if (height <= 0) return 0;
    --p; /* 'x' 건너뜀 */

    /* 숫자 (width) 역방향 탐색 */
    const char *wend = p;
    while (p > name && p[-1] >= '0' && p[-1] <= '9') --p;
    if (p == wend || p == name || p[-1] != '-') return 0;
    int width = atoi(p);
    if (width <= 0) return 0;

    *w = width;
    *h = height;
    return 1;
}

/*
 * cam_w×cam_h 카메라를 mw×mh 모델에 letterbox로 넣을 때
 * 낭비되는 픽셀의 비율을 반환합니다.
 *
 * 0.0 = 낭비 없음(비율이 정확히 일치)
 * 1.0 = 전부 낭비(이론상 불가능하지만 상한)
 *
 * 계산 방법:
 *   scale = min(mw/cw, mh/ch)  — 모델 안에 꽉 차게 축소
 *   content = cw*scale × ch*scale — 실제로 그림이 들어가는 픽셀 수
 *   waste_ratio = (mw*mh - content) / (mw*mh)
 */
static double waste_ratio(int mw, int mh, int cw, int ch) {
    double sw = (double)mw / cw;
    double sh = (double)mh / ch;
    double scale = sw < sh ? sw : sh;
    double content = (double)cw * scale * (double)ch * scale;
    double total   = (double)mw * mh;
    return (total - content) / total;
}

int model_select(const char *model_dir, int cam_w, int cam_h,
                 char *out_path, size_t out_size) {
    double best_waste = 2.0; /* 1.0 초과로 시작해 첫 후보가 무조건 대체 */
    int    best_mw = 0, best_mh = 0;
    char   best_name[512] = {0};
    int    candidates = 0;

#if defined(_WIN32)
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*.onnx", model_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "model-select: 디렉터리를 열 수 없습니다: %s\n", model_dir);
        return 0;
    }
    do {
        int mw, mh;
        if (!parse_model_size(fd.cFileName, &mw, &mh)) continue;
        ++candidates;
        double w = waste_ratio(mw, mh, cam_w, cam_h);
        fprintf(stderr, "model-select: 후보 %s (%dx%d) — letterbox %.1f%%\n",
                fd.cFileName, mw, mh, w * 100.0);
        if (w < best_waste) {
            best_waste = w;
            best_mw = mw;
            best_mh = mh;
            strncpy(best_name, fd.cFileName, sizeof(best_name) - 1);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(model_dir);
    if (!d) {
        fprintf(stderr, "model-select: 디렉터리를 열 수 없습니다: %s\n", model_dir);
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        int mw, mh;
        if (!parse_model_size(ent->d_name, &mw, &mh)) continue;
        ++candidates;
        double w = waste_ratio(mw, mh, cam_w, cam_h);
        fprintf(stderr, "model-select: 후보 %s (%dx%d) — letterbox %.1f%%\n",
                ent->d_name, mw, mh, w * 100.0);
        if (w < best_waste) {
            best_waste = w;
            best_mw = mw;
            best_mh = mh;
            strncpy(best_name, ent->d_name, sizeof(best_name) - 1);
        }
    }
    closedir(d);
#endif

    if (!best_name[0]) {
        fprintf(stderr,
                "model-select: %s 에서 *-WxH.onnx 패턴의 파일을 찾지 못했습니다 "
                "(후보 %d개 스캔)\n",
                model_dir, candidates);
        return 0;
    }

    snprintf(out_path, out_size, "%s" PATH_SEP "%s", model_dir, best_name);
    fprintf(stderr,
            "model-select: 카메라 %dx%d → %s (%dx%d, letterbox %.1f%%) 선택\n",
            cam_w, cam_h, best_name, best_mw, best_mh, best_waste * 100.0);
    return 1;
}
