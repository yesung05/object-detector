#include "netaccess.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int netaccess_same_subnet(uint32_t a_be, uint32_t b_be, uint32_t mask_be) {
    return (a_be & mask_be) == (b_be & mask_be);
}

int netaccess_is_loopback(uint32_t addr_be) {
    /* 네트워크 바이트 순서에서는 첫 바이트가 최상위 옥텟입니다. */
    return ((const unsigned char *)&addr_be)[0] == 127;
}

static int ieq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}

int netaccess_extract_pin(const char *request, char *out, size_t size) {
    static const char key[] = "x-hunik-pin:";
    const char *p = request;
    size_t klen = sizeof(key) - 1;
    if (!request || !out || size == 0) return 0;
    out[0] = '\0';
    /* 헤더는 줄 시작에서만 인식합니다 — 본문이나 다른 헤더 값에 같은 문자열이
       들어 있어도 PIN 으로 오인하지 않게 하기 위함입니다. */
    while ((p = strchr(p, '\n')) != NULL) {
        size_t i;
        p++;
        for (i = 0; i < klen; i++) if (!p[i] || !ieq(p[i], key[i])) break;
        if (i != klen) continue;
        p += klen;
        while (*p == ' ' || *p == '\t') p++;
        {
            size_t n = 0;
            while (*p && *p != '\r' && *p != '\n' && *p != ' ' && *p != '\t' && n + 1 < size) {
                if (*p < 0x21 || *p > 0x7e) { out[0] = '\0'; return 0; } /* 인쇄 가능 ASCII 만 */
                out[n++] = *p++;
            }
            out[n] = '\0';
            while (*p == ' ' || *p == '\t') p++;
            if (*p && *p != '\r' && *p != '\n') { out[0] = '\0'; return 0; } /* PIN 안의 공백은 거부 */
            return n > 0;
        }
    }
    return 0;
}

const char *netaccess_tier_name(NetTier t) {
    switch (t) {
        case NET_TIER_LOCALHOST: return "localhost";
        case NET_TIER_LAN:       return "lan";
        default:                 return "denied";
    }
}

const char *netaccess_pin_reason_name(int reason) {
    switch (reason) {
        case 1:  return "pin_required";
        case 2:  return "pin_wrong";
        case 3:  return "pin_locked";
        default: return "ok";
    }
}

const char *netaccess_deny_reason_name(int reason) {
    switch (reason) {
        case 1:  return "netmask_lookup_failed";
        case 2:  return "socket_error";
        default: return "outside_subnet";
    }
}

#if defined(_WIN32)

#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

/*
 * 연결이 들어온 로컬 인터페이스 주소로 넷마스크를 찾습니다. 실패하면 0.
 *
 * GetAdaptersAddresses 대신 GetIpAddrTable 을 쓰는 이유: IPv4 주소·마스크 쌍만
 * 필요하고, 후자가 훨씬 단순합니다(구조체 한 겹, 링크드 리스트 없음).
 * 요청마다 호출하지만 표는 작고(인터페이스 수), 변경 요청은 드물어 비용이 무시됩니다.
 */
static uint32_t mask_for_local(uint32_t local_be) {
    ULONG size = 0;
    MIB_IPADDRTABLE *table;
    uint32_t mask = 0;
    DWORD i;
    if (GetIpAddrTable(NULL, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return 0;
    table = (MIB_IPADDRTABLE *)malloc(size); /* 이 함수 안에서만 쓰고 free */
    if (!table) return 0;
    if (GetIpAddrTable(table, &size, FALSE) == NO_ERROR) {
        for (i = 0; i < table->dwNumEntries; i++) {
            if (table->table[i].dwAddr == local_be) { mask = table->table[i].dwMask; break; }
        }
    }
    free(table);
    return mask;
}

/* ── PIN 무차별 대입 방어 표 ─────────────────────────────────────────────── */

#define PIN_SLOTS     16
#define PIN_MAX_FAILS 5
#define PIN_LOCK_MS   (10ULL * 60ULL * 1000ULL)

typedef struct {
    uint32_t  ip;
    int       fails;
    ULONGLONG locked_until;
    ULONGLONG last;
} PinSlot;

static PinSlot          g_pin[PIN_SLOTS];
static CRITICAL_SECTION g_pin_lock;
static INIT_ONCE        g_pin_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_pin_lock(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_pin_lock);
    return TRUE;
}

/* ip 슬롯을 찾고, 없으면 가장 오래 쉰 슬롯을 비워 돌려줍니다. 호출자가 락을 쥡니다. */
static PinSlot *pin_slot(uint32_t ip, int create, ULONGLONG now) {
    int i, oldest = 0;
    for (i = 0; i < PIN_SLOTS; i++) if (g_pin[i].ip == ip && g_pin[i].last) return &g_pin[i];
    if (!create) return NULL;
    for (i = 1; i < PIN_SLOTS; i++) if (g_pin[i].last < g_pin[oldest].last) oldest = i;
    memset(&g_pin[oldest], 0, sizeof(g_pin[oldest]));
    g_pin[oldest].ip   = ip;
    g_pin[oldest].last = now;
    return &g_pin[oldest];
}

void netaccess_evaluate(SOCKET s, const char *request, const char *pin_expected,
                        NetAccess *out) {
    struct sockaddr_in peer, local;
    int plen = sizeof(peer), llen = sizeof(local);
    uint32_t p, l, mask;
    char pin[64];
    ULONGLONG now;
    PinSlot *slot;

    memset(out, 0, sizeof(*out));
    strcpy(out->ip, "?");
    memset(&peer, 0, sizeof(peer));
    memset(&local, 0, sizeof(local));

    if (getpeername(s, (struct sockaddr *)&peer, &plen) != 0 ||
        getsockname(s, (struct sockaddr *)&local, &llen) != 0 ||
        peer.sin_family != AF_INET) {
        out->tier = NET_TIER_DENIED;
        out->deny_reason = 2;
        return;
    }
    inet_ntop(AF_INET, &peer.sin_addr, out->ip, sizeof(out->ip));
    p = peer.sin_addr.s_addr;
    l = local.sin_addr.s_addr;

    if (netaccess_is_loopback(p)) {
        out->tier   = NET_TIER_LOCALHOST;
        out->pin_ok = 1;
        return;
    }

    mask = mask_for_local(l);
    if (!mask) {
        /* 마스크를 모르면 판정 자체가 불가능합니다. 허용 쪽으로 기울면 방어선이
           조용히 사라지므로 거부합니다. 호출자가 deny_reason 을 로그에 남기면
           이 상황이 실제로 일어나는지 운영 중에 알 수 있습니다. */
        out->tier = NET_TIER_DENIED;
        out->deny_reason = 1;
        return;
    }
    if (!netaccess_same_subnet(p, l, mask)) {
        out->tier = NET_TIER_DENIED;
        out->deny_reason = 0;
        return;
    }

    out->tier = NET_TIER_LAN;
    if (!pin_expected || !pin_expected[0]) { out->pin_ok = 1; return; }

    InitOnceExecuteOnce(&g_pin_once, init_pin_lock, NULL, NULL);
    now = GetTickCount64();
    EnterCriticalSection(&g_pin_lock);
    slot = pin_slot(p, 0, now);
    if (slot && slot->locked_until > now) {
        out->pin_reason = 3;
        LeaveCriticalSection(&g_pin_lock);
        return;
    }
    if (!netaccess_extract_pin(request, pin, sizeof(pin))) {
        out->pin_reason = 1;
        LeaveCriticalSection(&g_pin_lock);
        return;
    }
    if (strcmp(pin, pin_expected) == 0) {
        out->pin_ok = 1;
        if (slot) { slot->fails = 0; slot->last = now; }
        LeaveCriticalSection(&g_pin_lock);
        return;
    }
    slot = pin_slot(p, 1, now);
    slot->fails++;
    slot->last = now;
    if (slot->fails >= PIN_MAX_FAILS) {
        slot->locked_until = now + PIN_LOCK_MS;
        slot->fails = 0;
        out->pin_reason = 3;
    } else {
        out->pin_reason = 2;
    }
    LeaveCriticalSection(&g_pin_lock);
}

#endif /* _WIN32 */
