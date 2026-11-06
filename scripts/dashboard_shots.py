"""
대시보드 화면 스크린샷과 콘솔 오류 수집 (개발 PC 전용, 읽기 전용)

재디자인 전/후를 눈으로 비교하고, 브라우저 콘솔에 JS 오류가 없는지 확인한다.
  1. 임시 루트(<tmp>/dashboard 에 HTML 복사 + logs/ 에 알림 예시가 든 샘플 DB)를 만들고 대시보드 서버를 따로 띄운다.
     운영 중인 대시보드(포트 8080)와 로그를 건드리지 않기 위해 임시 루트·다른 포트(기본 18791)를 쓴다.
  2. Edge headless 로 화면×테마×크기별 스크린샷을 찍는다. 서버는 detector 없이도 오프라인 상태 화면이 나온다.
  3. Edge 의 콘솔 로그에서 JS 오류를 모은다(out/console.txt).
서버는 이름이 아니라 PID 로 종료한다(이름으로 종료하면 같은 이름의 운영 프로세스까지 끈다 — 실제로 그런 적이 있다).

사용: python -I scripts/dashboard_shots.py --html-dir dashboard --exe <dashboard exe> --out runs/dashboard-redesign/before
"""

import argparse
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request

EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
# (이름, 경로). index 의 고급 모드·다크는 URL 파라미터로 지정한다(새 디자인이 지원; 기존 코드는 무시하고 기본 화면).
PAGES = [("index", "/"), ("index-adv", "/?mode=advanced"), ("index-modes", "/?mode=advanced&group=modes"), ("surfaces", "/surfaces"),
         ("announce", "/announce"), ("replay", "/replay"), ("research", "/research")]
UNUSED_STREAM_PORT = 18899
SIZES = {"desktop": (1440, 1800), "mobile": (390, 2600)}


def make_root(html_dir, config_src):
    root = tempfile.mkdtemp(prefix="dashshot-")
    shutil.copytree(html_dir, os.path.join(root, "dashboard"))
    os.makedirs(os.path.join(root, "logs"))
    con = sqlite3.connect(os.path.join(root, "logs", "20261105_000000.db"))
    con.execute("CREATE TABLE events(id INTEGER PRIMARY KEY, ts_unix REAL, ts_iso TEXT, level TEXT, module TEXT, message TEXT)")
    rows = [("INFO", "startup", "alert_modes shadow=person_unaccounted,external_drink off=occupancy_exceeded"),
            ("WARN", "rules", "overstay track=2 dwell=3700s limit=3600s"),
            ("ERROR", "rules", "person_fallen track=3 hold=3.0s score=0.66 match=0.66 samples=10 evidence=upright_to_horizontal"),
            ("WARN", "door", "door_open: 30초 이상 개방 상태 지속"),
            ("INFO", "camera", "state=ok"),
            ("WARN", "rules", "unordered_seated track=5 dwell=412s grace=300s"),
            ("WARN", "rules", "overstay track=2 dwell=3800s limit=3600s"),
            ("WARN", "rules", "overstay track=2 dwell=3900s limit=3600s"),
            ("WARN", "rules", "external_drink track=7 mode=shadow"),
            ("WARN", "rules", "external_drink track=8 mode=shadow")]
    for i, (lv, mod, msg) in enumerate(rows):
        con.execute("INSERT INTO events VALUES(?,?,?,?,?,?)", (i + 1, 1.0 * i, "2026-11-05T10:00:%02d" % i, lv, mod, msg))
    con.commit()
    con.close()
    # 설정은 운영 config.json 을 복사하지 않고 만든다. 운영 설정의 stream_port(8081)로 페이지가 접속하면 운영 중인
    # detector 의 실시간 MJPEG 스트림에 붙어 (1) 라이브 카메라 영상을 받게 되고 (2) 스트림이 끝나지 않아 headless 가
    # load 이벤트를 기다리며 멈춘다(실측). 아무도 쓰지 않는 포트를 가리켜 "detector 미실행" 화면이 나오게 한다.
    cfg = os.path.join(root, "config.json")
    open(cfg, "w", encoding="utf-8").write('{"stream_port": %d}' % UNUSED_STREAM_PORT)
    return root, cfg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--html-dir", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=18791)
    ap.add_argument("--config", default="", help="무시됨(호환용). 운영 설정을 쓰지 않는다")
    ap.add_argument("--themes", default="light,dark")
    ap.add_argument("--only", default="", help="쉼표로 구분한 화면 이름만(예: index,surfaces)")
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)  # Edge 는 상대 경로 스크린샷을 자기 작업 폴더 기준으로 해석해 파일이 안 생긴다
    os.makedirs(a.out, exist_ok=True)
    root, cfg = make_root(a.html_dir, a.config)
    log = open(os.path.join(root, "server.log"), "wb")
    server = subprocess.Popen([a.exe, "--root", root, "--config", cfg, "--port", str(a.port)],
                              stdout=log, stderr=subprocess.STDOUT, creationflags=0x08000000)  # CREATE_NO_WINDOW
    console_lines, shots = [], 0
    try:
        base = f"http://127.0.0.1:{a.port}"
        for _ in range(40):
            try:
                urllib.request.urlopen(base + "/api/logs", timeout=2).read()
                break
            except Exception:
                time.sleep(0.25)
        else:
            sys.exit("대시보드 서버가 시작되지 않았습니다: " + open(os.path.join(root, "server.log"), errors="replace").read()[-300:])
        only = {s for s in a.only.split(",") if s}
        for name, path in PAGES:
            if only and name not in only:
                continue
            for theme in a.themes.split(","):
                for size, (w, h) in SIZES.items():
                    sep = "&" if "?" in path else "?"
                    url = f"{base}{path}{sep}theme={theme}"
                    png = os.path.join(a.out, f"{name}-{theme}-{size}.png")
                    prof = tempfile.mkdtemp(prefix="edgeprof-")
                    cmd = [EDGE, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                           f"--user-data-dir={prof}", f"--window-size={w},{h}", "--timeout=5000",
                           "--enable-logging=stderr", "--v=0", f"--screenshot={png}"]
                    if theme == "dark":
                        cmd.append("--blink-settings=preferredColorScheme=1")  # prefers-color-scheme: dark
                    cmd.append(url)
                    r = subprocess.run(cmd, capture_output=True, timeout=45)
                    err = (r.stderr or b"").decode("utf-8", "replace")
                    for line in err.splitlines():
                        if "CONSOLE" in line and ("Uncaught" in line or "Error" in line or "error" in line):
                            console_lines.append(f"[{name}-{theme}-{size}] {line.strip()[:240]}")
                    shutil.rmtree(prof, ignore_errors=True)
                    ok = os.path.exists(png) and os.path.getsize(png) > 2000
                    shots += ok
                    print(("OK  " if ok else "FAIL"), os.path.basename(png), (os.path.getsize(png) // 1024 if ok else 0), "KB", flush=True)
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
        log.close()
        shutil.rmtree(root, ignore_errors=True)
    open(os.path.join(a.out, "console.txt"), "w", encoding="utf-8").write("\n".join(console_lines))
    print(f"스크린샷 {shots}장, 콘솔 오류 {len(console_lines)}건 → {a.out}")
    for line in console_lines[:8]:
        print("  ", line)


if __name__ == "__main__":
    main()
