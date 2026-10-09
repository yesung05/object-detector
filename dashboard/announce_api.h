/* announce_api.h — 매장 음성 안내 재생 (server.c 가 #include; 단독 컴파일 단위 아님)
 *
 *   GET  /api/announce        → {"clips":["welcome.mp3",...],"playing":bool}
 *   POST /api/announce/play?file=NAME|random   announce\ 폴더의 mp3/wav 재생 (random = 폴더에서 무작위 1개)
 *   POST /api/announce/tts    본문(UTF-8 평문) 을 Windows SAPI 로 읽기
 *   POST /api/announce/stop   재생 중단
 *
 * [설계 이유]
 * - 재생은 숨김 PowerShell 자식 프로세스로 합니다. MCI 는 장치를 연 스레드에 묶이는데 이 서버의
 *   client_thread 는 요청마다 끝나므로 장치가 같이 닫힐 위험이 있고, 자식 프로세스는 Stop 으로
 *   TerminateProcess 하기만 하면 되어 수명 관리가 가장 단순합니다. 새 DLL 의존성도 없습니다.
 * - TTS 는 오프라인 SAPI(System.Speech)만 씁니다. 클라우드 TTS 는 "외부 전송 금지" 원칙 위반입니다.
 * - 사용자 입력(TTS 문구)은 명령줄에 끼우지 않고 환경변수로 넘깁니다 — 스크립트는 고정 문자열이라
 *   문구에 따옴표·세미콜론이 있어도 명령 주입이 불가능합니다.
 * - 파일명은 safe_filename() + 확장자 화이트리스트로 announce\ 하위로만 제한합니다. (ASCII 이름만 허용:
 *   한글 파일명은 URL 인코딩·코드페이지 문제가 커서 의도적으로 제외)
 * - POST 는 client_thread 의 공통 접근 제어(서브넷 + PIN)를 이미 통과한 요청입니다.
 */
#define ANNOUNCE_TEXT_MAX 300   /* UTF-8 바이트. 한글 약 100자 — 안내 문구로 충분하고 SAPI 를 오래 붙잡지 않음 */

static SRWLOCK g_ann_lock = SRWLOCK_INIT;
/* g_ann_proc: 이 모듈 소유 핸들. 수명 = 재생 시작 ~ Stop/다음 재생/종료 감지.
   해제 책임: ann_stop_locked() 만 CloseHandle 한다 (g_ann_lock 보유 중에만 접근). */
static HANDLE g_ann_proc = NULL;

static void ann_reply(SOCKET s, int code, const char *json) {
    send_header(s, code, "application/json", (int64_t)strlen(json));
    send_all(s, json, (int)strlen(json));
}

/* 재생 중인 자식이 있으면 종료. g_ann_lock 보유 상태에서 호출. */
static void ann_stop_locked(void) {
    if (!g_ann_proc) return;
    TerminateProcess(g_ann_proc, 0);
    CloseHandle(g_ann_proc);
    g_ann_proc = NULL;
}

/* 이미 끝난 자식이면 핸들을 정리하고 0, 아직 재생 중이면 1. g_ann_lock 보유 상태에서 호출. */
static int ann_is_playing_locked(void) {
    if (g_ann_proc && WaitForSingleObject(g_ann_proc, 0) != WAIT_TIMEOUT) {
        CloseHandle(g_ann_proc);
        g_ann_proc = NULL;
    }
    return g_ann_proc != NULL;
}

static int ann_has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && _stricmp(name + n - e, ext) == 0;
}

static int ann_is_clip(const char *name) {
    return safe_filename(name) && (ann_has_ext(name, ".mp3") || ann_has_ext(name, ".wav"));
}

/* announce\ 의 클립 이름을 names[] 에 채우고 개수를 돌려줍니다 (최대 max). */
static int ann_list(char names[][64], int max) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    int count = 0;
    snprintf(pattern, sizeof(pattern), "%s\\announce\\*", g_data_root);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (strlen(fd.cFileName) >= 64 || !ann_is_clip(fd.cFileName)) continue;
        if (count < max) strcpy(names[count++], fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}

/* 고정 스크립트 + 환경변수 값으로 숨김 PowerShell 을 띄웁니다. 0=성공, -1=실패.
   1.5초 안에 비정상 종료하면(음성 엔진 없음·파일 열기 실패) 실패로 돌려 UI 가 알 수 있게 합니다. */
static int ann_spawn(const wchar_t *script, const wchar_t *value) {
    wchar_t sysdir[MAX_PATH], exe[MAX_PATH], cmd[1024];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    GetSystemDirectoryW(sysdir, MAX_PATH);
    _snwprintf(exe, MAX_PATH, L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe", sysdir);
    _snwprintf(cmd, 1024, L"\"%ls\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"%ls\"", exe, script);
    AcquireSRWLockExclusive(&g_ann_lock);
    ann_stop_locked();
    /* 환경변수는 프로세스 전역이라 생성 직전~직후만 잡고 바로 비웁니다(락으로 이 모듈 내 경합 차단). */
    SetEnvironmentVariableW(L"HUNIK_ANN", value);
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    SetEnvironmentVariableW(L"HUNIK_ANN", NULL);
    if (!ok) { ReleaseSRWLockExclusive(&g_ann_lock); return -1; }
    CloseHandle(pi.hThread);
    g_ann_proc = pi.hProcess;
    if (WaitForSingleObject(g_ann_proc, 1500) == WAIT_OBJECT_0) {
        GetExitCodeProcess(g_ann_proc, &code);
        CloseHandle(g_ann_proc);
        g_ann_proc = NULL;
        if (code != 0) { ReleaseSRWLockExclusive(&g_ann_lock); return -1; }
    }
    ReleaseSRWLockExclusive(&g_ann_lock);
    return 0;
}

/* POST /api/announce/upload?name=NAME  본문 = WAV(RIFF/WAVE). announce\NAME.wav 로 저장.
 *
 * client_thread 의 요청 버퍼(스택 64KB)에는 녹음이 들어가지 않으므로 이 경로만 본문을 직접 받습니다.
 * 호출 전에 접근 제어(서브넷+PIN)를 통과했어야 하며, 그 전에는 본문을 소비하지 않습니다.
 * - 크기 상한 5MB: 16kHz 모노 16bit 기준 약 160초. 인증된 요청이라도 메모리를 무한정 쓰지 못하게 함.
 * - 브라우저가 WAV 로 변환해서 보냅니다(webm/m4a 는 서버 MediaPlayer 가 못 읽을 수 있음) — 서버는
 *   RIFF/WAVE 헤더만 확인합니다. 그 이상의 오디오 검증은 하지 않습니다.
 * - 임시 파일에 쓴 뒤 MoveFile 로 확정해, 중간에 끊긴 업로드가 목록에 불완전한 클립으로 보이지 않게 함.
 * have/first: 이미 요청 버퍼에 들어온 본문 앞부분(빌려 쓰는 포인터, 이 함수 안에서만 유효). */
#define ANNOUNCE_UPLOAD_MAX (5 * 1024 * 1024)
static void serve_announce_upload(SOCKET s, const char *query, const char *first, int have,
                                  int total, const NetAccess *acc) {
    char name[64], path[MAX_PATH], tmp[MAX_PATH], dir[MAX_PATH], note[160];
    const char *who = acc ? acc->ip : "?";
    query_get(query, "name", name, sizeof(name));
    size_t nl = strlen(name);
    if (nl == 0 || nl > 40) { ann_reply(s, 400, "{\"ok\":false,\"error\":\"bad_name\"}"); return; }
    for (size_t i = 0; i < nl; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            { ann_reply(s, 400, "{\"ok\":false,\"error\":\"bad_name\"}"); return; }
    }
    if (total < 44) { ann_reply(s, 400, "{\"ok\":false,\"error\":\"bad_audio\"}"); return; }
    if (total > ANNOUNCE_UPLOAD_MAX) { ann_reply(s, 400, "{\"ok\":false,\"error\":\"too_large\"}"); return; }

    /* data: 이 함수가 malloc 소유. 모든 반환 경로에서 free(data) — 아래 done 한 곳으로 모음. */
    char *data = (char *)malloc((size_t)total);
    int got = 0, code = 500;
    const char *err = "server_error";
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!data) goto done;
    if (have > total) have = total;
    if (have > 0) { memcpy(data, first, (size_t)have); got = have; }
    while (got < total) {
        int r = recv(s, data + got, total - got, 0);
        if (r <= 0) { code = 400; err = "incomplete"; goto done; }
        got += r;
    }
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) { code = 400; err = "bad_audio"; goto done; }

    snprintf(dir, sizeof(dir), "%s\\announce", g_data_root);
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s\\%s.wav", dir, name);
    snprintf(tmp, sizeof(tmp), "%s\\%s.wav.tmp", dir, name);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { code = 409; err = "exists"; goto done; }
    h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) goto done;
    {
        DWORD wrote = 0;
        BOOL ok = WriteFile(h, data, (DWORD)total, &wrote, NULL) && wrote == (DWORD)total;
        CloseHandle(h); h = INVALID_HANDLE_VALUE;
        if (!ok || !MoveFileA(tmp, path)) { DeleteFileA(tmp); goto done; }
    }
    snprintf(note, sizeof(note), "announce upload file=%s.wav bytes=%d by %s", name, total, who);
    access_log_write(0, note);
    free(data);
    {char r[128]; snprintf(r, sizeof(r), "{\"ok\":true,\"file\":\"%s.wav\"}", name); ann_reply(s, 200, r);}
    return;
done:
    free(data);
    snprintf(note, sizeof(note), "announce upload failed name=%s reason=%s by %s", name, err, who);
    access_log_write(1, note);
    {char r[96]; snprintf(r, sizeof(r), "{\"ok\":false,\"error\":\"%s\"}", err); ann_reply(s, code, r);}
}

static void serve_announce(SOCKET s, const char *method, const char *path, const char *query,
                           const char *body, int body_len, const NetAccess *acc) {
    char note[160];
    const char *who = acc ? acc->ip : "?";

    if (strcmp(method, "GET") == 0) {
        char names[64][64];   /* 4KB — 스택. client_thread 가 동시에 여러 개 돌아 static 은 레이스가 됨 */
        char json[64 * 72 + 64];
        int n = ann_list(names, 64), len = 0;
        AcquireSRWLockExclusive(&g_ann_lock);
        int playing = ann_is_playing_locked();
        len = snprintf(json, sizeof(json), "{\"clips\":[");
        for (int i = 0; i < n; i++)
            len += snprintf(json + len, sizeof(json) - len, "%s\"%s\"", i ? "," : "", names[i]);
        snprintf(json + len, sizeof(json) - len, "],\"playing\":%s}", playing ? "true" : "false");
        ReleaseSRWLockExclusive(&g_ann_lock);
        ann_reply(s, 200, json);
        return;
    }

    if (strcmp(path, "/api/announce/stop") == 0) {
        AcquireSRWLockExclusive(&g_ann_lock);
        ann_stop_locked();
        ReleaseSRWLockExclusive(&g_ann_lock);
        ann_reply(s, 200, "{\"ok\":true}");
        return;
    }

    if (strcmp(path, "/api/announce/delete") == 0) {
        char file[256], full[MAX_PATH];
        query_get(query, "file", file, sizeof(file));
        if (!ann_is_clip(file)) { ann_reply(s, 400, "{\"ok\":false,\"error\":\"bad_file\"}"); return; }
        /* 재생 중인 파일을 지우려는 경우를 위해 먼저 중지: MediaPlayer 가 파일을 잡고 있으면 삭제 실패 */
        AcquireSRWLockExclusive(&g_ann_lock);
        ann_stop_locked();
        ReleaseSRWLockExclusive(&g_ann_lock);
        snprintf(full, sizeof(full), "%s\\announce\\%s", g_data_root, file);
        if (!DeleteFileA(full)) { ann_reply(s, 404, "{\"ok\":false,\"error\":\"not_found\"}"); return; }
        snprintf(note, sizeof(note), "announce delete file=%s by %s", file, who);
        access_log_write(0, note);
        ann_reply(s, 200, "{\"ok\":true}");
        return;
    }

    if (strcmp(path, "/api/announce/play") == 0) {
        char names[64][64];
        char file[256], full[MAX_PATH];
        wchar_t wfull[MAX_PATH];
        query_get(query, "file", file, sizeof(file));
        if (strcmp(file, "random") == 0) {
            int n = ann_list(names, 64);
            if (n == 0) { ann_reply(s, 404, "{\"ok\":false,\"error\":\"no_clips\"}"); return; }
            srand((unsigned)GetTickCount());
            strcpy(file, names[rand() % n]);
        }
        if (!ann_is_clip(file)) { ann_reply(s, 400, "{\"ok\":false,\"error\":\"bad_file\"}"); return; }
        snprintf(full, sizeof(full), "%s\\announce\\%s", g_data_root, file);
        if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) { ann_reply(s, 404, "{\"ok\":false,\"error\":\"not_found\"}"); return; }
        MultiByteToWideChar(CP_ACP, 0, full, -1, wfull, MAX_PATH);
        /* MediaPlayer 는 mp3/wav 모두 처리. NaturalDuration 은 비동기로 채워지므로 최대 5초 기다리고,
           끝까지 못 읽으면(손상 파일) 비정상 종료해 위 1.5초 감시가 아니라 로그로 남도록 한다. */
        if (ann_spawn(L"Add-Type -AssemblyName presentationCore;$p=New-Object System.Windows.Media.MediaPlayer;"
                      L"$p.Open([uri]$env:HUNIK_ANN);$p.Play();$t=0;"
                      L"while(-not $p.NaturalDuration.HasTimeSpan -and $t -lt 50){Start-Sleep -Milliseconds 100;$t++};"
                      L"if(-not $p.NaturalDuration.HasTimeSpan){exit 2};"
                      L"Start-Sleep -Milliseconds ([int]($p.NaturalDuration.TimeSpan.TotalMilliseconds+300))", wfull) != 0) {
            snprintf(note, sizeof(note), "announce play failed file=%s by %s", file, who);
            access_log_write(1, note);
            ann_reply(s, 500, "{\"ok\":false,\"error\":\"play_failed\"}");
            return;
        }
        snprintf(note, sizeof(note), "announce play file=%s by %s", file, who);
        access_log_write(0, note);
        {char r[128]; snprintf(r, sizeof(r), "{\"ok\":true,\"file\":\"%s\"}", file); ann_reply(s, 200, r);}
        return;
    }

    if (strcmp(path, "/api/announce/tts") == 0) {
        wchar_t wtext[ANNOUNCE_TEXT_MAX + 1];
        if (!body || body_len <= 0 || body_len > ANNOUNCE_TEXT_MAX) {
            ann_reply(s, 400, "{\"ok\":false,\"error\":\"bad_text\"}");
            return;
        }
        int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, body_len, wtext, ANNOUNCE_TEXT_MAX);
        if (w <= 0) { ann_reply(s, 400, "{\"ok\":false,\"error\":\"bad_text\"}"); return; }
        wtext[w] = 0;
        if (ann_spawn(L"Add-Type -AssemblyName System.Speech;$v=New-Object System.Speech.Synthesis.SpeechSynthesizer;"
                      L"$v.Speak($env:HUNIK_ANN)", wtext) != 0) {
            snprintf(note, sizeof(note), "announce tts failed (%d chars) by %s", w, who);
            access_log_write(1, note);
            ann_reply(s, 500, "{\"ok\":false,\"error\":\"tts_failed\"}");
            return;
        }
        /* 문구 내용은 로그에 넣지 않습니다: 개행·긴 문자열로 로그를 오염시킬 수 있고 길이만으로 추적에 충분. */
        snprintf(note, sizeof(note), "announce tts (%d chars) by %s", w, who);
        access_log_write(0, note);
        ann_reply(s, 200, "{\"ok\":true}");
        return;
    }

    ann_reply(s, 404, "{\"ok\":false}");
}
