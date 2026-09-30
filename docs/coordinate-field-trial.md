# 좌표 기반 현장 시험 기록

대시보드의 설정 → 자동 관리 → 현장 시험 기록, 또는 로그 → 좌표 기록 재생·오탐 검토에서 `/replay`로 이동합니다.

## 기록 내용

새 기능 적용 후 실행한 감지기는 기존 이벤트 SQLite DB에 `replay_sessions`, `replay_frames`, `replay_events`, `replay_geometry`를 추가합니다. 별도의 이미지·영상 파일을 이 기능에서 생성하지 않습니다. 기존 문·표면 기준 이미지는 감지 기능용으로 별도 유지됩니다. 좌표 기록이 켜져 있는 동안 쓰러짐 사건의 기존 JPEG 캡처는 생략합니다.

- 평소: 10초마다 좌표 스냅샷. 최근 10초는 메모리 링 버퍼에 최대 5Hz로 유지합니다.
- 알림: rules/door/surface/camera의 WARN·ERROR와 일부 복구·주문 추정 이벤트에 대해 이전 10초와 이후 10초를 저장합니다. 겹치는 구간은 중복 저장하지 않습니다.
- 좌표: 0~1 정규화 박스, 관절별 점수, 추적 ID, 미검출 횟수, 최근 매칭 점수와 유지 점수, 당시 해상도·추론 경과 시간.
- 판정: 이벤트 메시지, 쓰러짐 지속시간·박스 비율·머리 위치, 문 단계, 문·테이블 영역 버전, 당시 임계값.
- 성능: 단일 코어 기준 CPU 사용량, 메모리, 입력/추론 FPS, Tier별 누적 평균 처리시간, 추론 지연 지표, 감속·카메라 상태, 직전 기록 처리시간.
- 모델: 모델 파일 FNV-1a 식별값(보안 해시 아님), 입력 크기, 실행 장치 종류·스레드 수, 빌드 시각. 설정 전체나 접근 PIN은 기록하지 않습니다.

사람은 최대 16개, 물체는 최대 24개, 낮은 점수의 사람 후보는 최대 8개까지 한 스냅샷에 남깁니다. 메모리 버퍼는 약 1.7 MiB입니다. 샘플링 시점을 포함한 최초/종료 구간은 10초보다 짧을 수 있고, 프로그램이 꺼지거나 카메라가 끊긴 시간은 재구성하지 않습니다.

## 검토와 한계

원본을 요청하지 않는 빈 배경에서 좌표를 재생합니다. 추론 결과와 추적·재사용 좌표를 구분하며 중간 위치를 보간하지 않습니다. 좌표만으로 침대인지 사람인지 실제 정답은 확정할 수 없습니다. 검토는 미검토 / 정상 감지로 판단 / 오탐 의심 / 판단 불가와 메모로 남기며, 이는 사람의 판단이지 검증된 정답이나 학습 데이터 라벨은 아닙니다. 미탐은 별도 실제 상황 기록 없이 산출할 수 없습니다.

## 저장량과 가져오기

파일 하나당 좌표 JSON 누적 1 GiB가 상한입니다(전체 SQLite 파일·다른 로그 용량의 상한은 아닙니다). 도달하면 좌표 저장을 중단하고 대시보드에 표시합니다. 기존 기록을 자동 삭제하지 않으며 여러 실행 파일의 총량 제한도 별도로 두지 않습니다. 2주 운영 중에는 남은 저장 공간과 기록 오류·용량 표시를 확인하세요. 이벤트가 잦을수록 저장량이 증가합니다.

한 사건은 재생 화면의 ‘선택 기록 내보내기’로 JSON을 받습니다. 전체 기록은 프로젝트 폴더에서 다음과 같이 내보냅니다.

```powershell
python scripts/export-coordinate-trial.py --logs logs --out trial-export
```

출력 폴더는 새 폴더여야 합니다. 이미지 폴더를 읽지 않고 좌표·이벤트·검토·영역·실행 정보만 JSONL로 저장합니다. DB별 읽기 트랜잭션으로 일관된 내용을 내보내므로 활성 DB 파일과 WAL 파일을 임의로 복사하는 방식보다 안전합니다. 내보내기 중 새로 발생한 데이터는 해당 DB의 다음 내보내기에 포함됩니다.

조회 및 검토 API는 기존 대시보드 네트워크 접근 제한을 따르며, LAN에서 검토를 변경할 때는 설정된 PIN이 필요합니다. 외부 프록시·터널이 로컬 연결로 전달되는 경우를 구분하는 별도 인증 정책은 이 기능에서 추가하지 않습니다.

## 0.1.0-alpha.1 쓰러짐 판정

`fall_gate`: 0=대기, 1=신뢰도 부족, 2=새 관측 부족, 3=관절 근거 부족,
4=서 있는 자세 이력 미확인, 5=수평 유지 중, 6=확정.
머리 이동 즉시 발화를 제거했다. 양어깨/양엉덩이의 공간 배치와 검출·매칭 점수
0.5 이상을 요구한다. 1초/3회 이상 서 있는 자세를 관측한 뒤 30초 안에 수평
자세를 기본 5초/3회 이상 확인해야 한다. 2초를 넘는 관측 공백/카메라 단절은
유지를 초기화한다. 이미 누운 채로 등장하거나 관절이 가려진 경우 확정이
어려울 수 있다. 모델 자체 재학습이나 검증된 오탐률 개선 수치를 의미하지 않는다.


## 2026-09-27: hourly log segments

- Event/coordinate and performance databases commit while the application is running; shutdown is not required to save them. WAL sidecars (`-wal`, `-shm`) may exist until connections close. Do not copy only the active `.db` file; use the coordinate exporter or a SQLite backup.
- With `--event-log`, the initial filename is retained. After 3,600 seconds of monotonic runtime, the next processed frame switches to `<original>_part000001.db` and `<original>_part000001_perf.db`, then increasing part numbers. Detection/tracking and camera capture are not restarted. With no camera frames there are no frame-driven rotations; switching happens on the next frame.
- All three new stores (events, replay, performance) must open successfully before the previous stores close. On failure, the previous files remain active and rotation retries after 60 seconds. Failed attempts can leave incomplete empty segment files; stderr reports the failure.
- The replay ring and geometry carry across the boundary. Event IDs belong to their own segment. A pre-boundary event's post-window can span both files; the event-specific viewer does not automatically merge segments. The exporter includes all segments.
- Event and performance connections use WAL plus `synchronous=FULL`. Each successful commit requests disk synchronization, improving power-loss durability at additional storage latency. Hardware/OS failures can still prevent successful writes. Ordinary replay samples remain every 10 seconds; temporary uncommitted ring samples are not guaranteed after a crash.
- The 1 GiB coordinate quota is per segment, not a total retention limit. Old logs are not automatically deleted. Launcher console and supervisor restart text files remain per launch/append logs; hourly segmentation applies to event, coordinate and performance databases.
- Verified with automated hour-boundary, separate-reader live visibility, abrupt child-process exit/recovery, replay carry-over and failed-rotation fallback tests. These tests do not simulate physical power failure.


## 2026-09-27 Alpha 3: retention policy (supersedes no-pruning behavior above)

Windows installations now check retention on the first camera frame and every 60 seconds of frame processing. Generated timestamp-named event databases and matching performance/WAL/SHM files are grouped; launcher diagnostic logs and restarts.log are also managed. Only direct files in the active log directory are scanned. Custom filenames, subdirectories, images/videos and reparse-point entries are excluded.

Completed groups whose latest file modification is at least 30 days old are deleted. Then the oldest eligible groups are deleted until managed logs fit `log_max_total_mb` (MiB; default 10240 = 10 GiB; 0 disables only the size cap; maximum 1048576). This can delete records younger than 30 days. The setting is under Settings → Automatic management and reloads without restarting.

The current event/performance group is explicitly protected. All files in each candidate group must permit exclusive access before deletion; open launcher logs and databases in use by other processes are skipped. Active/locked files can keep usage above the limit; this is a cleanup target, not a hard filesystem quota. Cleanup failures/remaining over-limit status are written to the event log. The existing per-segment coordinate JSON limit of 1 GiB still applies. No cleanup runs while the app is stopped or waiting indefinitely for camera frames.
