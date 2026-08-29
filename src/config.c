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

    /* 모든 줄을 하나의 버퍼에 모아 두고, 그 안의 위치를 keys/values가 가리킵니다. */
    while (fgets(line, (int)sizeof(line), f)) {
        char *eq, *k, *v, *comment;
        /* 주석 이후 잘라냄 */
        comment = strchr(line, '#');
        if (comment) *comment = '\0';
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        k = trim(line);
        v = trim(eq + 1);
        if (*k == '\0') continue;

        /* k\0v\0 형태로 buf 에 추가 */
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

    /* buf が確定した後にポインタを再設定する */
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
