# 무인 매장 환경·위생 감시 논문 자료

이 폴더는 2페이지 시스템 논문 작성용 참고자료다. 회사명·로고는 넣지 않았으며, 실측하지 않은 정확도·CPU·FPS를 채우지 않았다. 주제는 **무인 매장 환경 및 위생(오염) 감지 시스템**이고, 중복 AI 검사 억제는 시스템의 효율화 요소다.

## 자료 목록

- [의자 경계 가림 구현·CPU 병렬 탐색 기록](chair-boundary-implementation-2026-09-15.md)
- [다른 PC 인수인계: 의자 처리 설계·50개 Grid Search 계획](next-pc-chair-registration-and-grid-search.md)
- [50개 전체 초기 설정 평가 결과·S36 등록 비교](all50-initial-results-2026-09-14.md) — 50+1개 실행 완료, 정확도/F1 60.0%
- [50개 평가 방법·정답 기준·재현 명령](all50-initial-evaluation-method.md)
- [S36 평가용 컵 비치물 등록·기준 생성 확인](S36-fixture-registration.md)
- [문제·누락 항목 재촬영, 설명 건너뛰기, 시작 알림음](retake-recorder-guide.md)
- [Grid Search 최종 결과·18개 전체 점수·다음 작업 인수인계](grid-search-final-results-2026-09-11.md) — 탐색 완료, 추천값 운영 미적용
- [초기값 보존 및 Grid Search 실행 방법](surface-grid-search.md)
- [실제 영상 21개: 표면 감지 모듈 성능 결과](video-evaluation-results-2026-09-11.md)
- [영상 기반 성능 평가 방법·제한](video-evaluation-method-2026-09-11.md)
- [촬영 완료 자료 23개: 1차 적합성 평가](records-first-assessment-2026-09-11.md)
- [이전 CLOVA Voice 설정·사진 기준 시험 테이블 참고](clova-voice-setup.md) — 현재 웹은 브라우저 기본 TTS 사용
- [1인·동일 테이블 랜덤 자동 녹화 웹 사용법](scenario-recorder-guide.md)
- [50개 시나리오별 구체적인 행동 프롬프트](50-action-prompts.md)
- [일반 실내 50개 시나리오·간편 촬영·박스 강조 설계](50-scenarios-recording-plan.md)
- [자동 화면 기록과 영상 기반 평가 안내](automatic-evidence-guide.md)
- [IMRaD 논문 초안](draft-imrad.md)
- [초안 편집·제출 전 확인 사항](draft-submission-notes.md)
- [다운로드한 논문 7편](references/README.md)
- [최적화 관련 논문 해설](optimization-literature.md)
- [관련 연구와 인용 범위](related-work.md)
- [참고문헌 BibTeX](references.bib)
- [실물 배치 시험 및 기록기 사용법](live-trial-guide.md)
- [별도 대시보드 사용 및 캡처](dashboard-capture.md)
- [수치와 그림 사용 가이드](figures-and-numbers.md)
- [추천 실험 및 평가 방법](recommended-tests.md)
- [실험 원자료 CSV 양식](experiment-template.csv)
- [이미 검증된 합성 수치 CSV](synthetic-results.csv)
- [전체 구조도 SVG](architecture.svg)
- [테이블 이용·가림 정책 SVG](table-policy.svg)

## 2페이지 구성 제안

1. 서론: 무인 운영에서 상시 청소 확인이 어려운 문제, 단순 물체 검출의 한계.
2. 시스템 설계: 표면별 기준 비교, 정상 비치물, 이용·가림 판단, 선택적 AI, 관리자 알림. 구조도 1개.
3. 실험: 실제 영상의 정상/오염 의심/이용/가림 장면과 OFF/ON 효율 비교. 결과 표 1개.
4. 결론: 관리 지원 가능성과 한계. 참고문헌은 투고 양식에 맞춰 추가.

‘위생’은 영상에서 보이는 변화·잔류물에 대한 청소 확인 지원으로 정의한다. 세균 측정, 위생 안전 인증, 토사물·배설물의 정확한 세부 분류를 주장하지 않는다. 기능 테스트 통과가 검출 정확도 100%를 의미하지 않는다.

2026-09-11 기준 검증된 핵심 수치는 특정 50초 합성 장면의 후보 AI 요청 9→1회다. 실제 매장 성능은 추천 실험 후 기록해야 한다. 구현 상세는 [재검사 억제 기록](../2026-09-11-ai-reinspection-reduction.md)을 참고한다.
