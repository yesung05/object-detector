# 확인한 관련 연구와 인용 범위

확인일: 2026-09-11. 출판사 원문과 저자 공개본을 확인했다. 아래 설명은 출처의 핵심 연결점만 요약하며, 원문의 그림·표를 복제하지 않았다. 외부 연구의 실험 결과를 우리 시스템의 결과로 전용하지 않는다.

## [1] 방치물 감시의 전체 판단 과정

Elena Luna, Juan Carlos San Miguel, Diego Ortego, José María Martínez. **Abandoned Object Detection in Video-Surveillance: Survey and Comparison.** Sensors 18(12), 4290, 2018. DOI: 10.3390/s18124290.

[출판사 원문](https://www.mdpi.com/1424-8220/18/12/4290)

- 확인 위치: 초록, 3절 Canonical Framework, 실험 방법 및 결과.
- 인용 근거: 정지 여부만이 아니라 사람이 물건을 방치했는지 판단하는 단계가 필요하다. 여러 검출 단계를 결합한 비교 실험을 포함한다.
- 우리 시스템과의 차이: 물건 소유자를 직접 연결·추적하지 않고, 표면별 이용 영역과 퇴석 유예로 일반 청소 알림을 제어한다.
- 쓰면 안 되는 주장: 이 논문이 무인 매장 위생 성능이나 우리 시스템의 45초 설정을 검증했다는 주장.

## [2] 규칙 기반 후보와 CNN의 단계적 결합

Sorina Smeureanu, Radu Tudor Ionescu. **Real-Time Deep Learning Method for Abandoned Luggage Detection in Video.** arXiv:1803.01160, 2018. 저자 공개 페이지에 EUSIPCO 2018 채택이 명시되어 있다.

[저자 공개본 및 초록](https://arxiv.org/abs/1803.01160)

- 확인 위치: 저자 공개 초록 및 서지정보.
- 인용 근거: 배경 차분·움직임 추정으로 정지 물체를 찾고 CNN 캐스케이드로 방치 수하물을 판단하는 두 단계 구조.
- 우리 시스템과의 차이: 수하물 분류가 아니라 표면별 정상 비치물·잔류 변화·이용 상태를 다루며, 규칙 알림이 AI 분류의 성공만을 전제로 하지 않는다.
- 한계: 현재는 이 논문의 상세 실험을 재현하지 않았다. ‘직접 비교에서 우수하다’고 쓸 수 없다. 학회 최종 페이지·DOI를 확인하지 않은 상태이므로 초안에서는 확인한 arXiv 공개본으로 인용한다.

## [3] 영상 변화에 따른 비싼 추론 생략

Daniel Kang, John Emmons, Firas Abuzaid, Peter Bailis, Matei Zaharia. **NoScope: Optimizing Neural Network Queries over Video at Scale.** PVLDB 10(11), 1586–1597, 2017.

[학회 공개 원문](https://www.vldb.org/pvldb/vol10/p1586-kang.pdf) · [저자 초록](https://arxiv.org/abs/1703.02529)

- 확인 위치: 초록, 1절, 차이 검출기와 모델 특화 설명.
- 인용 근거: 프레임 차이와 특화 모델의 캐스케이드로 참조 신경망 실행 비용을 줄이는 접근.
- 우리 시스템과의 차이: 특화 모델 학습·자동 비용 최적화는 구현하지 않았다. 후보별 RGB 특징 재사용과 테이블 퇴석·가림 정책을 적용한다.
- 쓰면 안 되는 주장: NoScope의 처리 속도 수치를 우리 시스템에 적용하거나, 현재 캐시가 NoScope를 완전히 재현한다는 주장.

## [4] DeepCache / [5] FrameExit

[DeepCache 저자 원문](https://xumengwei.github.io/files/MobiCom18-DeepCache.pdf)과 [FrameExit 학회 원문](https://openaccess.thecvf.com/content/CVPR2021/html/Ghodrati_FrameExit_Conditional_Early_Exiting_for_Efficient_Video_Recognition_CVPR_2021_paper.html)을 확인했다. 각각 모델 내부 결과 재사용과 학습형 조기 종료에 관한 연구이며 현재 후보 수준 캐시와는 다르다. [최적화 해설](optimization-literature.md)에서 비교한다.

## [6] TACO / [7] CDnet 2014

[TACO](https://arxiv.org/abs/2003.06975)는 쓰레기 검출·분할을 위한 실제 환경 이미지 데이터 연구다. [CDnet 2014](https://openaccess.thecvf.com/content_cvpr_workshops_2014/W12/html/Wang_CDnet_2014_An_2014_CVPR_paper.html)는 다양한 조건의 변화 검출 벤치마크다. 둘 다 보조 평가 자료로 참고할 수 있지만, 실제 매장에서 이용자가 물건을 남기고 떠나는 사건의 GT를 그대로 제공하지는 않는다. 적용 범위는 [실물 시험 안내](live-trial-guide.md)에 구분했다.

## 인용을 추가해도 남는 과제

위 7편은 잔류물 감시·선택적 추론·데이터 평가에 연결되는 출발점이다. 전체 분야의 최신성·완전성·신규성을 입증하는 포괄적 문헌 조사가 아니다. 무인 매장 청결 감시의 직접 선행연구, 실제 평가 데이터셋 및 공정한 비교군은 투고 학회와 실험 범위에 맞춰 추가 조사한다. 공개 방치 수하물 데이터는 보조 평가에 쓸 수 있지만, 실제 컵·쓰레기·오염 모사 장면을 대신할 수는 없다.
