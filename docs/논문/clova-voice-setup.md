# CLOVA Voice 설정과 실제 시험 테이블

> 이전 구현 참고 문서: 사용자 요청에 따라 촬영 웹은 브라우저 기본 한국어 TTS로 복귀했다. 아래 CLOVA 생성기는 보존하지만 현재 웹에서 사용하지 않는다. 현재 촬영에는 키 설정이나 음성 생성이 필요 없다.

## 사진 기준 환경

시험 대상은 제공된 사진에서 화면 앞쪽에 가장 크게 보이는 흰색 테이블이다. 뒤쪽 다른 책상은 제외한다. 모니터·칸막이·키보드·케이블 등 기본 비치물은 유지하고 카메라에서 보이는 빈 상판을 실험 위치로 쓴다. 모니터를 들거나 실제 음료를 전자제품 근처에 흘리지 않는다. 컵은 비어 있는 컵, 오염은 마른 종이나 안전한 모형으로 재현한다.

사람 한 명이 같은 의자·테이블을 사용하며 카메라와 화각을 고정한다. 사진에는 배경 이용자도 보이므로 촬영 때는 가능하면 다른 사람이 없는 시간에 진행한다. 배경 이용자가 남으면 단일 참여자 통제 실험의 혼입 조건으로 기록하고, 동의 및 얼굴 비식별을 확인한다. 제공 사진만으로 ROI·이용 구역을 자동 변경하지 않았다. 테이블 상판, 앉은 사람의 이용 구역, 비치물 설정은 실제 영상에서 별도 확인해야 한다.

## 필요한 인증 정보

네이버 클라우드 콘솔에서 CLOVA Voice 사용이 가능한 Application을 만들고 **Client ID / Client Secret**을 확인한다. 일반 계정 비밀번호나 다른 서비스의 Access Key를 넣는 것이 아니다. 서비스 활성화·요금·생성 음성 보관 및 이용 조건을 계정에서 확인한다.

키는 채팅·스크린샷·브라우저 코드·저장소에 넣지 않는다. 음성 생성 프로세스가 읽을 수 있도록 로컬 환경변수 `CLOVA_CLIENT_ID`, `CLOVA_CLIENT_SECRET`을 설정한다. 환경변수 설정 후에는 새 터미널을 사용한다. 현재 구현은 `.env` 파일을 자동으로 읽지 않는다.

PowerShell에서 값이 명령 기록에 남지 않게 입력하려면 다음처럼 대화형 입력을 사용할 수 있다. 생성 프로세스 실행 후 아래 finally 구문은 그 터미널에서 두 변수를 제거한다.

```powershell
$env:CLOVA_CLIENT_ID = Read-Host 'CLOVA Client ID'
$voiceSecret = Read-Host 'CLOVA Client Secret' -AsSecureString
$voiceCredential = New-Object System.Net.NetworkCredential('', $voiceSecret)
$env:CLOVA_CLIENT_SECRET = $voiceCredential.Password
try {
    node scripts/clova_voice.js
    # 위의 누락 음성 수·문자 수와 이용 요금을 확인한 뒤 실행
    node scripts/clova_voice.js --generate
} finally {
    Remove-Item Env:CLOVA_CLIENT_ID -ErrorAction SilentlyContinue
    Remove-Item Env:CLOVA_CLIENT_SECRET -ErrorAction SilentlyContinue
    $voiceCredential = $null
    $voiceSecret = $null
}
```

`node`를 찾지 못하면 `& 'C:\Program Files\nodejs\node.exe'`로 대체한다. 위 예시는 설명용이며 인증 정보나 API 호출을 이번 개발 중 실행하지 않았다. 환경변수가 이미 준비되어 있으면 `prepare-clova-voice.bat`를 실행하고 생성 여부에 동의해도 된다.

## 생성·재생 방식

공식 CLOVA Voice TTS Premium API의 `nara`, 속도 0, MP3를 사용한다. 목소리 취향에 대한 청취 검증은 아직 하지 않았다. 생성기는 준비·행동·공통 단계 안내를 모아 중복 문장은 한 번만 생성한다. 텍스트와 음성 설정의 해시를 파일명으로 사용하며 변경된 안내만 새로 생성한다.

`runs/scenario-voice/`에 MP3를 보관하고 Git에서는 제외한다. 생성 실패 시 이미 받은 파일은 유지하며 다음 실행에서 재사용한다. API 요청은 순차 실행하고 오류 응답 본문이나 키를 출력하지 않는다. 인증 정보가 없으면 명확한 오류로 종료한다. 임의의 문장을 웹에서 유료 생성하는 API는 제공하지 않는다.

`run-scenario-recorder.bat`를 실행하고 **CLOVA 음성 확인·미리듣기**로 확인한다. 음성이 준비되지 않았으면 녹화 시작 시 안내하고 멈춘다. 음성 안내를 직접 끄면 무음 촬영은 가능하다. 브라우저 기본 TTS로 몰래 대체하지 않는다.

촬영 웹은 로컬 MP3만 재생하므로 촬영 중 네이버 API를 호출하지 않는다. 네이버로 전송되는 것은 음성 생성 시 정해진 안내 텍스트뿐이며 사진·영상·카메라 화면은 전송하지 않는다. 로컬 웹 서버는 켜져 있어야 한다.

## 안내와 타이머

이제 **설명 재생 완료 → 준비 카운트다운 → 녹화 시작** 순서다. 녹화 중에도 단계 설명이 끝난 다음 해당 단계의 카운트다운을 시작한다. 따라서 기존 기본 190초보다 영상이 길어지며, 안내 길이는 기본 단계 시간에 포함되지 않는다. 실제 행동은 설명을 끝까지 들은 다음 수행한다.

단계 로그에는 안내 시작 `prompt_elapsed_s`와 카운트다운 시작 `countdown_started_elapsed_s`를 구분해 남긴다. 두 값 모두 정답 행동 시각이 아니며, 실제 배치·퇴석 시점은 영상으로 확인한다. 음성 재생 실패·브라우저 차단은 촬영을 중단하고 부분 저장을 시도한다. 먼저 미리듣기로 재생 권한과 스피커를 확인한다. 안내 음성은 촬영 영상에 녹음하지 않는다.

## 검증 및 제한

자동 테스트에서 인증 누락, 캐시 재사용, API HTTP 실패, 음성 종료 대기·취소·실패 처리를 검증한다. 실제 CLOVA 합성·한국어 청취 품질은 유효한 사용자 인증 정보로 생성 후 확인해야 한다. 요금은 코드에서 임의 계산하지 않고 생성할 문자 수만 보여준다.

공식 근거: [CLOVA Voice 공통 인증](https://api.ncloud-docs.com/docs/ai-naver-clovavoice), [TTS Premium 요청 파라미터](https://api.ncloud-docs.com/docs/ai-naver-clovavoice-ttspremium).
