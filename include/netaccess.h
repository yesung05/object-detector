#ifndef NETACCESS_H
#define NETACCESS_H

#include <stddef.h>
#include <stdint.h>

/*
 * 대시보드(8080)와 스트림(8081) 두 HTTP 서버가 공유하는 접근 제어입니다.
 *
 * 두 서버에 따로 두지 않고 한 모듈로 묶은 이유: 두 파일의 헤더 주석은
 * "127.0.0.1 전용"이라고 말하는데 코드는 INADDR_ANY 였습니다. 한쪽만 고치면
 * 다른 포트가 그대로 열려 있게 되므로, 판정 로직은 한 곳에만 있어야 합니다.
 *
 * 계층:
 *   LOCALHOST  127.0.0.0/8 — 키오스크 자체(개발 환경). PIN 없이 모든 조작 허용.
 *   LAN        연결이 들어온 인터페이스와 같은 서브넷. 읽기 허용, 변경은 PIN.
 *   DENIED     그 밖 전부. 403.
 *
 * "같은 서브넷"을 사설 대역(192.168.x) 검사로 대신하지 않는 이유: 게스트 Wi-Fi 가
 * 다른 서브넷(192.168.50.x)이면 사설 대역 검사는 통과시키고 서브넷 검사는
 * 막습니다. 무인 매장은 손님이 매장 Wi-Fi 에 붙어 있을 수 있어 이 차이가
 * 실제 방어선입니다. 넷마스크는 OS 인터페이스 테이블에서 읽습니다.
 *
 * 한계(문서화): 전부 평문 HTTP 라 PIN 이 LAN 위를 그대로 지납니다. 같은 LAN 의
 * 스니핑까지 막으려면 HTTPS 가 필요하지만 키오스크에 인증서를 배포·갱신하는
 * 운영 부담이 커서 이 위협 모델에서는 제외했습니다.
 */
typedef enum { NET_TIER_DENIED = 0, NET_TIER_LAN = 1, NET_TIER_LOCALHOST = 2 } NetTier;

typedef struct {
    NetTier tier;
    int     pin_ok;       /* 1이면 변경 요청 허용: localhost, PIN 비활성, 또는 PIN 일치 */
    int     pin_reason;   /* pin_ok=0 일 때 0=해당없음 1=미제출 2=불일치 3=잠금 */
    int     deny_reason;  /* tier=DENIED 일 때 0=서브넷 밖 1=넷마스크 조회 실패 2=소켓 오류 */
    char    ip[48];       /* 로그용 피어 주소 문자열 */
} NetAccess;

/* 순수 함수 — 단위 테스트용. 인자는 모두 네트워크 바이트 순서입니다. */
int netaccess_same_subnet(uint32_t a_be, uint32_t b_be, uint32_t mask_be);
int netaccess_is_loopback(uint32_t addr_be);

/* 요청 헤더에서 "X-Hunik-Pin: ..." 값을 꺼냅니다. 없으면 0. 대소문자 무시. */
int netaccess_extract_pin(const char *request, char *out, size_t size);

const char *netaccess_tier_name(NetTier t);
const char *netaccess_pin_reason_name(int reason);
const char *netaccess_deny_reason_name(int reason);

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
/*
 * 소켓 하나에 대해 계층 판정 + PIN 검사를 한 번에 수행합니다.
 * pin_expected: config.json 의 access_pin. NULL 또는 "" 이면 PIN 비활성(LAN 도 변경 허용).
 * 어떤 경로로 실패해도 out 은 항상 채워지며 tier 는 DENIED 가 됩니다.
 *
 * PIN 무차별 대입 방어: IP 당 5회 불일치 → 10분 잠금. 표는 프로세스 로컬이라
 * 두 서버가 각자 셉니다(공격자는 두 포트에서 각 5회 — 은행이 아니라 키오스크라
 * 이 정도로 충분합니다). "미제출"은 실패로 세지 않습니다 — 브라우저의 첫 요청은
 * 항상 PIN 없이 오고, 그걸 세면 정상 사용자가 잠깁니다.
 */
void netaccess_evaluate(SOCKET s, const char *request, const char *pin_expected,
                        NetAccess *out);
#endif

#endif /* NETACCESS_H */
