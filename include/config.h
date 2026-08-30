#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

/*
 * 매장별 설정을 외부 파일에서 주입하기 위한 key=value 파서입니다.
 * 외부 라이브러리 없이 표준 C만 사용합니다.
 *
 * 파일 형식:
 *   key = value   # 등호 양쪽 공백 무시
 *   # 주석 (줄 전체 또는 값 뒤)
 */
typedef struct {
    char  *buf;      /* 파싱한 문자열 전체를 담는 버퍼, Config 소유 */
    char **keys;     /* buf 안의 key 포인터 배열, Config 소유 */
    char **values;   /* buf 안의 value 포인터 배열, Config 소유 */
    size_t count;
    size_t capacity;
} Config;

/* path == NULL 이면 빈 Config(기본값만 사용)를 반환합니다. */
int  config_load(Config *c, const char *path, char *error, size_t error_size);
void config_destroy(Config *c);

/* key 를 찾으면 0, 없으면 -1. value_out 이 NULL 이어도 존재 확인만 가능합니다. */
int config_get(const Config *c, const char *key, const char **value_out);

/* 범위 밖이거나 키가 없으면 def를 반환합니다. */
long  config_long (const Config *c, const char *key,
                   long  def, long  lo, long  hi);
float config_float(const Config *c, const char *key,
                   float def, float lo, float hi);

/*
 * 사각형 ROI 를 읽습니다. 값 형식은 "x,y,w,h" 입니다.
 *
 *   "roi_kiosk": "820,120,300,420"
 *
 * 값 배열이 아니라 문자열을 쓰는 이유: 이 파일의 flat JSON 파서는 배열을
 * 지원하지 않고, 파서를 확장하는 비용보다 쉼표 파싱이 훨씬 쌉니다.
 * 나중에 폴리곤이 필요해지면 그때 형식을 바꿉니다.
 *
 * 반환 1 = out 을 채움, 0 = 키가 없거나 형식이 잘못됨(out 은 건드리지 않음).
 * 0 을 오류가 아니라 "미설정"으로 다루는 이유: ROI 는 매장마다 선택 사항이고,
 * 미설정 상태를 호출자가 기능 비활성으로 해석해야 하기 때문입니다.
 */
typedef struct { float x, y, w, h; } ConfigRect;

int config_rect(const Config *c, const char *key, ConfigRect *out);

/*
 * prefix_1, prefix_2 ... 순서로 사각형을 읽어 배열을 채웁니다.
 * 번호가 끊기면 거기서 멈춥니다. 반환값은 읽은 개수입니다.
 *
 *   "ignore_roi_1": "0,0,320,180"
 *   "ignore_roi_2": "900,0,380,200"
 */
int config_rect_list(const Config *c, const char *prefix,
                     ConfigRect *out, int max_count);

/*
 * "HH:MM-HH:MM" 형식의 시간 범위를 자정 기준 분으로 읽습니다.
 *
 * 자정을 넘는 범위(22:00-02:00)도 유효하며, 이 경우 start > end 로 반환됩니다.
 * 호출자는 start <= end 인지에 따라 포함 판정을 달리해야 합니다.
 *
 * 반환 1 = 성공, 0 = 키 없음 또는 형식 오류.
 */
int config_time_range(const Config *c, const char *key,
                      int *start_minute, int *end_minute);

/* now_minute 이 [start, end) 안에 있는지 판정합니다. 자정 넘김을 처리합니다. */
int config_time_in_range(int now_minute, int start_minute, int end_minute);

#endif /* CONFIG_H */
