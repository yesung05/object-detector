# Windows 설치본 빌드

`scripts/build-installer.ps1`은 전체 Release 재빌드, 전체 CTest, 실행 파일/모델/
DLL 스테이징, SHA256 명세 생성, Inno Setup 컴파일을 수행한다.
결과는 `dist/unmanned_detector-Setup-0.1.0-alpha.1-x64.exe`와 `.exe.sha256`이다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-installer.ps1
```

빌드 기기에 Visual Studio C++ Build Tools, CMake, FFmpeg shared x64 개발 패키지,
ONNX Runtime x64 개발 패키지, Inno Setup 6이 필요하다. 경로가 기본 검색 위치와
다르면 `-FfmpegRoot`, `-OrtRoot`, `-VcRuntimeDir`, `-Iscc`로 지정한다.
`-Version 0.1.0-alpha.2`로 다음 설치본을 만든다. 기본 명령은 항상 전체 재빌드하며
이미 검증한 빌드를 패키징할 때만 `-SkipBuild`를 사용한다(CTest는 계속 실행).

설치 대상은 Windows 10/11 x64, CPU 추론이다. CRT를 앱 폴더에 포함하여
관리자 권한이나 별도 런타임 설치 없이 사용자 단위로 설치한다.
Python은 운영에 필요 없고 별도 내보내기 스크립트에만 필요하다.
설치 마법사는 한국어/영어를 지원한다. 카메라 1개는 자동 선택하고 여러 대면 GUI에서 선택한다.
X/최소화는 트레이 숨기기이며 감시를 유지한다. 완전 종료로 기록 정리를 요청하고
10초 후에도 종료되지 않으면 해당 런처 Job Object의 프로세스만 정리한다.
Windows 서비스/부팅 자동 실행은 포함하지 않는다.

설치/업데이트/제거는 프로그램 파일만 다룬다. `%LOCALAPPDATA%/unmanned_detector`의
현장 설정 및 기록은 보존된다. 개발 설정/원본 이미지/로그/실험 모델/테스트
실행 파일/백업 바이너리는 패키징하지 않는다. 문/테이블/안정화는 신규 데이터
폴더에서 새로 학습한다. 설치 후 기존 개발 서비스를 먼저 종료하고 실행한다.

검증 시 설치 폴더에 공백을 넣고, 별도 포트/사용자 데이터 경로로 대시보드가
페이지와 로그 목록을 제공하는지 확인한다. 빌드 기기의 실행 검증만으로 실제
대상 PC의 드라이버/CPU 호환성을 보증하지 않으므로 현장 기기에서 최종 확인한다.
코드 서명 인증서는 설정하지 않았다. 외부 배포 전 서명 및 포함된 각 구성요소의
배포 조건/소스 제공 자료는 배포 범위에 맞게 준비한다.
