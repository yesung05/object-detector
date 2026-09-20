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
#  include <sys/stat.h>
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

/*
 * letterbox 낭비가 같을 때 어느 파일을 고를지 정합니다 — 파일이 큰 쪽입니다.
 *
 * models\ 에는 원본 FP32 와 증류·양자화 변종이 함께 들어 있고, 입력 크기가 같으면
 * (예: yolo11n-pose-416x224 와 yolo11n-pose-distilled-416x224) 낭비율이 완전히 동일합니다.
 * 예전에는 디렉터리 열거 순서가 승자를 정했기 때문에, 폴더에 파일을 하나 넣는 것만으로
 * 조용히 다른 모델이 실행될 수 있었습니다. 크기 우선 규칙은 "원본 FP32 우선"을 뜻하며
 * 파일 이름 규칙에 기대지 않습니다(증류·양자화 모델은 언제나 더 작습니다).
 */
static int prefer_candidate(double waste, double best_waste,
                            unsigned long long size, unsigned long long best_size) {
    const double eps = 1e-9;
    if (waste < best_waste - eps) return 1;
    if (waste > best_waste + eps) return 0;
    return size > best_size;
}

int model_select(const char *model_dir, int cam_w, int cam_h,
                 char *out_path, size_t out_size) {
    double best_waste = 2.0; /* 1.0 초과로 시작해 첫 후보가 무조건 대체 */
    int    best_mw = 0, best_mh = 0;
    char   best_name[512] = {0};
    int    candidates = 0;
    unsigned long long best_size = 0;

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
        unsigned long long size;
        if (!parse_model_size(fd.cFileName, &mw, &mh)) continue;
        ++candidates;
        size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        double w = waste_ratio(mw, mh, cam_w, cam_h);
        fprintf(stderr, "model-select: 후보 %s (%dx%d) — letterbox %.1f%%, %.1f MB\n",
                fd.cFileName, mw, mh, w * 100.0, (double)size / (1024.0 * 1024.0));
        if (prefer_candidate(w, best_waste, size, best_size)) {
            best_waste = w;
            best_mw = mw;
            best_mh = mh;
            best_size = size;
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
        unsigned long long size = 0;
        char full[1024];
        struct stat st;
        if (!parse_model_size(ent->d_name, &mw, &mh)) continue;
        ++candidates;
        snprintf(full, sizeof(full), "%s/%s", model_dir, ent->d_name);
        if (stat(full, &st) == 0) size = (unsigned long long)st.st_size;
        double w = waste_ratio(mw, mh, cam_w, cam_h);
        fprintf(stderr, "model-select: 후보 %s (%dx%d) — letterbox %.1f%%, %.1f MB\n",
                ent->d_name, mw, mh, w * 100.0, (double)size / (1024.0 * 1024.0));
        if (prefer_candidate(w, best_waste, size, best_size)) {
            best_waste = w;
            best_mw = mw;
            best_mh = mh;
            best_size = size;
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
            "model-select: 카메라 %dx%d → %s (%dx%d, letterbox %.1f%%, %.1f MB) 선택\n",
            cam_w, cam_h, best_name, best_mw, best_mh, best_waste * 100.0,
            (double)best_size / (1024.0 * 1024.0));
    return 1;
}
