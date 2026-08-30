#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s) {
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' ||
                       *end == '\r' || *end == '\n')) *end-- = '\0';
    return s;
}

static int grow(Config *c, char *error, size_t error_size) {
    size_t new_cap;
    char **nk, **nv;
    if (c->count < c->capacity) return 0;
    new_cap = c->capacity == 0 ? 16 : c->capacity * 2;
    nk = (char **)realloc(c->keys,   new_cap * sizeof(*nk));
    nv = (char **)realloc(c->values, new_cap * sizeof(*nv));
    if (!nk || !nv) {
        free(nk); free(nv);
        if (error) snprintf(error, error_size, "config: out of memory");
        return -1;
    }
    c->keys     = nk;
    c->values   = nv;
    c->capacity = new_cap;
    return 0;
}

/* buf에 key\0value\0 쌍을 추가하는 내부 헬퍼 */
static int append_kv(Config *c, char **all, size_t *all_len, size_t *all_cap,
                     const char *k, size_t klen, const char *v, size_t vlen,
                     char *error, size_t error_size) {
    size_t need = *all_len + klen + 1 + vlen + 1;
    if (need > *all_cap) {
        size_t nc = *all_cap ? *all_cap * 2 : 1024;
        if (nc < need) nc = need * 2;
        char *nb = (char *)realloc(*all, nc);
        if (!nb) { if (error) snprintf(error, error_size, "config: out of memory"); return -1; }
        *all = nb; *all_cap = nc;
    }
    memcpy(*all + *all_len, k, klen); (*all)[*all_len + klen] = '\0';
    memcpy(*all + *all_len + klen + 1, v, vlen); (*all)[*all_len + klen + 1 + vlen] = '\0';
    *all_len += klen + 1 + vlen + 1;
    return grow(c, error, error_size);
}

/* 포인터 배열을 buf 내 위치로 재설정합니다. */
static void repoint(Config *c) {
    size_t pos = 0, i;
    for (i = 0; i < c->count; ++i) {
        c->keys[i]   = c->buf + pos;
        pos += strlen(c->keys[i]) + 1;
        c->values[i] = c->buf + pos;
        pos += strlen(c->values[i]) + 1;
    }
}

/* ── flat JSON 파서 ──────────────────────────────────────────────────────────
 * { "key": 1.23, "key2": true, "key3": "str" } 형식만 지원합니다.
 * 중첩 객체·배열은 무시됩니다. true/false → "1"/"0" 으로 변환해 저장합니다. */
static int config_load_json(Config *c, FILE *f, char *error, size_t error_size) {
    /* 파일 전체 읽기 */
    char *raw = NULL;
    size_t raw_len = 0, raw_cap = 0;
    char chunk[1024];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (raw_len + got + 1 > raw_cap) {
            size_t nc = raw_cap ? raw_cap * 2 : 4096;
            if (nc < raw_len + got + 1) nc = (raw_len + got + 1) * 2;
            char *nb = (char *)realloc(raw, nc);
            if (!nb) { free(raw); if (error) snprintf(error, error_size, "config: oom"); return -1; }
            raw = nb; raw_cap = nc;
        }
        memcpy(raw + raw_len, chunk, got);
        raw_len += got;
    }
    if (!raw) return 0;
    raw[raw_len] = '\0';

    char *all = NULL;
    size_t all_len = 0, all_cap = 0;
    const char *p = raw;

    while (*p) {
        /* key: " 찾기 */
        while (*p && *p != '"' && *p != '}') p++;
        if (!*p || *p == '}') break;
        p++; /* 여는 " */
        const char *ks = p;
        while (*p && *p != '"') p++;
        if (!*p) break;
        size_t klen = (size_t)(p - ks);
        p++; /* 닫는 " */
        if (klen == 0) continue;

        /* : 건너뜀 */
        while (*p && *p != ':' && *p != '{' && *p != '}') p++;
        if (!*p || *p != ':') { if (*p == '}') break; continue; }
        p++;
        while (*p == ' ' || *p == '\t') p++;

        /* value 파싱 */
        char vbuf[256];
        int vlen = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && vlen < 255) vbuf[vlen++] = *p++;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ',' && *p != '}' && *p != '\n' && *p != '\r' && vlen < 255)
                vbuf[vlen++] = *p++;
            while (vlen > 0 && (vbuf[vlen-1] == ' ' || vbuf[vlen-1] == '\t')) vlen--;
        }
        vbuf[vlen] = '\0';
        if (vlen == 0) continue;

        /* true/false → 1/0 */
        if (strcmp(vbuf, "true")  == 0) { vbuf[0]='1'; vbuf[1]='\0'; vlen=1; }
        if (strcmp(vbuf, "false") == 0) { vbuf[0]='0'; vbuf[1]='\0'; vlen=1; }

        if (append_kv(c, &all, &all_len, &all_cap,
                      ks, klen, vbuf, (size_t)vlen, error, error_size) != 0) {
            free(all); free(raw); config_destroy(c); return -1;
        }
        c->count++;
    }
    free(raw);
    c->buf = all;
    if (all) repoint(c);
    return 0;
}

int config_load(Config *c, const char *path, char *error, size_t error_size) {
    FILE *f;
    char line[512];
    char *all = NULL;
    size_t all_len = 0;
    size_t all_cap = 0;

    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    if (!path) return 0;  /* 빈 Config — 기본값만 사용 */

    f = fopen(path, "r");
    if (!f) {
        if (error) snprintf(error, error_size, "config: cannot open %s", path);
        return -1;
    }

    /* .json 파일이면 JSON 파서로 처리합니다. */
    {
        const char *ext = strrchr(path, '.');
        if (ext && strcmp(ext, ".json") == 0) {
            int r = config_load_json(c, f, error, error_size);
            fclose(f);
            return r;
        }
    }

    /* ── INI (key = value) 파서 ────────────────────────────────────────────── */
    while (fgets(line, (int)sizeof(line), f)) {
        char *eq, *k, *v, *comment;
        comment = strchr(line, '#');
        if (comment) *comment = '\0';
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        k = trim(line);
        v = trim(eq + 1);
        if (*k == '\0') continue;

        {
            size_t klen = strlen(k) + 1;
            size_t vlen = strlen(v) + 1;
            size_t need = all_len + klen + vlen;
            if (need > all_cap) {
                size_t new_cap = all_cap == 0 ? 1024 : all_cap * 2;
                if (new_cap < need) new_cap = need * 2;
                char *nb = (char *)realloc(all, new_cap);
                if (!nb) { fclose(f); free(all); config_destroy(c);
                    if (error) snprintf(error, error_size, "config: out of memory");
                    return -1; }
                all     = nb;
                all_cap = new_cap;
            }
            memcpy(all + all_len, k, klen);
            memcpy(all + all_len + klen, v, vlen);
            all_len += klen + vlen;
        }

        if (grow(c, error, error_size) != 0) {
            fclose(f); free(all); config_destroy(c); return -1;
        }
        c->count++;
    }
    fclose(f);
    c->buf = all;

    {
        size_t pos = 0, i;
        for (i = 0; i < c->count; ++i) {
            c->keys[i]   = c->buf + pos;
            pos += strlen(c->keys[i]) + 1;
            c->values[i] = c->buf + pos;
            pos += strlen(c->values[i]) + 1;
        }
    }
    return 0;
}

void config_destroy(Config *c) {
    if (!c) return;
    free(c->buf);
    free(c->keys);
    free(c->values);
    memset(c, 0, sizeof(*c));
}

int config_get(const Config *c, const char *key, const char **value_out) {
    size_t i;
    if (!c || !key) return -1;
    for (i = 0; i < c->count; ++i) {
        if (c->keys[i] && strcmp(c->keys[i], key) == 0) {
            if (value_out) *value_out = c->values[i];
            return 0;
        }
    }
    return -1;
}

long config_long(const Config *c, const char *key,
                 long def, long lo, long hi) {
    const char *v;
    char *end;
    long parsed;
    if (config_get(c, key, &v) != 0) return def;
    errno = 0;
    parsed = strtol(v, &end, 10);
    if (errno || !end || *end != '\0' || parsed < lo || parsed > hi) return def;
    return parsed;
}

float config_float(const Config *c, const char *key,
                   float def, float lo, float hi) {
    const char *v;
    char *end;
    float parsed;
    if (config_get(c, key, &v) != 0) return def;
    errno = 0;
    parsed = strtof(v, &end);
    if (errno || !end || *end != '\0' || parsed < lo || parsed > hi) return def;
    return parsed;
}

int config_rect(const Config *c, const char *key, ConfigRect *out) {
    const char *v;
    double vals[4];
    const char *p;
    int i;

    if (!c || !key || !out) return 0;
    if (config_get(c, key, &v) != 0 || !v || !*v) return 0;

    p = v;
    for (i = 0; i < 4; ++i) {
        char *end = NULL;
        errno = 0;
        vals[i] = strtod(p, &end);
        if (errno || end == p) return 0;
        p = end;
        while (*p == ' ' || *p == '\t') ++p;
        if (i < 3) {
            if (*p != ',') return 0;
            ++p;
        }
    }
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '\0') return 0;              /* 뒤에 잡다한 문자가 남으면 거부 */
    if (vals[2] <= 0.0 || vals[3] <= 0.0) return 0;  /* 폭·높이는 양수여야 함 */

    out->x = (float)vals[0];
    out->y = (float)vals[1];
    out->w = (float)vals[2];
    out->h = (float)vals[3];
    return 1;
}

int config_rect_list(const Config *c, const char *prefix,
                     ConfigRect *out, int max_count) {
    int n = 0;
    if (!c || !prefix || !out || max_count <= 0) return 0;
    while (n < max_count) {
        char key[128];
        snprintf(key, sizeof(key), "%s_%d", prefix, n + 1);
        if (!config_rect(c, key, &out[n])) break;   /* 번호가 끊기면 종료 */
        ++n;
    }
    return n;
}

/* "HH:MM" 을 자정 기준 분으로. 실패하면 -1. */
static int parse_hhmm(const char *s, const char **end_out) {
    int hh = 0, mm = 0, digits = 0;
    const char *p = s;

    while (*p >= '0' && *p <= '9' && digits < 2) { hh = hh * 10 + (*p++ - '0'); ++digits; }
    if (digits == 0 || *p != ':') return -1;
    ++p;
    digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 2) { mm = mm * 10 + (*p++ - '0'); ++digits; }
    if (digits == 0) return -1;
    if (hh > 23 || mm > 59) return -1;
    if (end_out) *end_out = p;
    return hh * 60 + mm;
}

int config_time_range(const Config *c, const char *key,
                      int *start_minute, int *end_minute) {
    const char *v;
    const char *p = NULL;
    int start, end;

    if (!c || !key || !start_minute || !end_minute) return 0;
    if (config_get(c, key, &v) != 0 || !v || !*v) return 0;

    start = parse_hhmm(v, &p);
    if (start < 0 || !p || *p != '-') return 0;
    end = parse_hhmm(p + 1, &p);
    if (end < 0 || !p) return 0;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '\0') return 0;
    if (start == end) return 0;   /* 빈 구간은 설정 실수로 보고 거부합니다 */

    *start_minute = start;
    *end_minute   = end;
    return 1;
}

int config_time_in_range(int now_minute, int start_minute, int end_minute) {
    if (start_minute <= end_minute)
        return now_minute >= start_minute && now_minute < end_minute;
    /* 자정을 넘는 구간: 22:00-02:00 이면 22:00~23:59 또는 00:00~01:59 */
    return now_minute >= start_minute || now_minute < end_minute;
}
