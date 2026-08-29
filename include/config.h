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

#endif /* CONFIG_H */
