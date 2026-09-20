<!--
2쪽 학술대회 초안 ver3 | proceeding_form_2026.pdf 기준

양식 원본에 옮길 때 A4·2단·여백·머리말을 유지한다. 국문 제목 HY견고딕
14 pt, 영문 제목 Arial 16.5 pt, 본문 국문 바탕·영문 Times New Roman
9.5 pt, 장평 90, 자간 -6, 들여쓰기 10 pt를 적용한다. 장 제목은 HY중고딕
10.5 pt, 절 제목은 HY중고딕 10 pt이다. Abstract는 Times New Roman
9.2 pt이며 120–150 words로 작성한다. 그림·표 캡션은 돋움 9 pt로 하고,
그림은 아래, 표는 위에 캡션을 둔다. 그림 내부 표기와 캡션은 영어로 유지한다.
Fig. 1은 가독성을 위해 두 단 전체 폭으로 배치한다. 저자·소속은 실제 정보로
교체하고, 지원 과제가 있을 때만 Acknowledgments를 추가한다.
굵게+밑줄 표시한 문장은 이번 참고문헌 보강에서 추가한 부분이므로 검토 후
일반 서식으로 되돌린다.
-->

> 편집 안내 (2026-09-15): `논문초안_2차.pdf`와 제목·구성·기존 평가 수치는 일치하지만 문구, 저자·소속·사사 및 참고문헌 수는 다르다. 이 파일을 기준으로 ~~삭제 제안~~과 **추가 제안**을 표시한다. 기존 `<strong><u>…</u></strong>`는 이전 참고문헌 보강 표시이며 이번 추가와 구별한다. 제출 시 삭제 제안을 제거하고 추가 문장을 일반 서식으로 바꾼다. 비교용으로 두 내용이 함께 보이므로 현재 표시본 자체는 2쪽 분량이 아니다. 기존 그림은 유지했으며, AI가 최종 알림을 승인하는 구조로 오해되지 않는지 조판 시 확인한다.

# 무인 공간 청결 관리를 위한 표면 상태 기반 영상 감시 시스템의 설계 및 예비 평가

Surface-State Monitoring for Cleanliness in Unmanned Spaces

〈국문 저자명〉<sup>1</sup> · 〈국문 교신저자명〉<sup>2*</sup>

<sup>1</sup>〈대학교·기관명 학과명〉  
<sup>2</sup>〈대학교·기관명 학과명〉

〈English Author Name〉<sup>1</sup> · 〈English Corresponding Author Name〉<sup>2*</sup>

<sup>1</sup>〈Department, Institution, City, Postal Code, Republic of Korea〉  
<sup>2</sup>〈Department, Institution, City, Postal Code, Republic of Korea〉

## [Abstract]

~~This paper presents a video monitoring system for supporting cleanliness inspection in unmanned spaces. The system registers a reference image for each surface, detects persistent changes, and combines change candidates with occupancy and occlusion states. Object detection is selectively applied to supplement candidate information. A preliminary evaluation used 21 development videos recorded at a single office table. Among 18 parameter combinations, the selected configuration achieved an F1 score of 66.7%, compared with 58.8% for the initial configuration, while retaining two false positives. Because the same videos were used for parameter selection and reporting, the results do not represent performance in independent environments. Error analysis found change candidates near the table and chair boundary and insufficient occupancy observations. The results demonstrate the implemented workflow and identify region configuration and occupancy detection as priorities for further evaluation.~~

**This paper presents a surface-state monitoring system for cleanliness inspection in unmanned spaces. The system detects persistent changes against a reference image and combines them with occupancy and occlusion states. Object detection is selectively applied to supplement candidate information, while similar candidates reuse recent results to reduce repeated inference. An evaluation on 49 development videos recorded at a single office table compared 18 parameter combinations. The maximum-F1 configuration achieved 70.4% precision, 82.6% recall, and an F1 score of 76.0%. A recall-oriented configuration achieved 91.3% recall, with 12 false positives and two false negatives. These results illustrate the trade-off between false alarms and missed residues and motivate high-recall candidate generation followed by AI verification. Because the same videos were used for parameter selection and evaluation, independent testing and final AI-based contamination verification remain future work.**

*Corresponding Author

## I. 서 론

무인점포의 청결성과 편리성은 소비자의 부정적 감정을 낮추는 요인으로 보고되었다 [1]. 그러나 영상에서 컵을 검출한 사실만으로는 정상 비치물, 이용 중인 물건과 퇴장 후 잔류물을 구분하기 어렵다. 기존 무인매장 영상 연구는 주로 이상행동 탐지를 다루며 [2], 방치물 검출은 전경·정지 물체·사람·방치 여부를 단계적으로 판단한다 [3]. <strong><u>배경 차분과 움직임 추정으로 정지 후보를 생성한 뒤 CNN으로 확인하는 2단계 방식도 제안되었다 [4].</u></strong> 본 연구는 정상 표면 기준과 이용·가림 상태를 결합하여 지속 변화를 청소 확인 대상으로 전환하고, 규칙 기반 후보에 선택적으로 인공지능 (AI; artificial intelligence) 검사를 적용하는 시스템을 구현한다. 또한 개발 영상의 설정 탐색과 실패 사례 분석을 통해 후속 설계 요구사항을 도출한다.

## II. 시스템 설계 및 구현

### 1. 처리 구조

사용자는 정상 비치물을 포함한 표면을 다각형 관심 영역 (ROI; region of interest)으로 지정하고, 가림 없는 관측 5회의 평균을 기준 영상으로 등록한다. Fig. 1과 같이 현재 표면과 기준의 차이로 변화 후보를 생성한 뒤 이용·가림·지속시간 조건을 적용한다. ~~AI는 후보 정보를 보조하며, 위치와 모습이 유사한 후보에는 최근 성공한 결과를 최대 60 s 재사용한다.~~ **현재 일반 오염 의심 알림은 규칙으로 발생하며 AI는 후보 정보를 보조한다. AI에 의한 최종 오염 판정은 미구현이다. 위치와 모습이 유사한 후보에는 최근 성공한 AI 결과를 최대 60 s 재사용한다.** 연속 영상에서 가벼운 변화 필터로 신경망 실행 대상을 선별하는 방식은 반복 추론을 줄이기 위한 기존 접근과 관련된다 [5].

![Monitoring workflow](figures/ver2/monitoring-workflow.jpg)

Fig. 1. Monitoring workflow combining surface changes, occupancy and occlusion states, and selective AI inspection.

### 2. 변화 및 상태 판단

기준과 현재 표면을 96×96 RGB 표본으로 변환한다. 비교 가능한 셀의 변화 마스크는 식 (1)로 정의한다.

$$
M_t(p)=\mathbb{I}\!\left[\max_{c\in\{R,G,B\}}\left|I_t^c(p)-B^c(p)-\Delta_t\right|>\tau\right],\quad p\in V_t.\tag{1}
$$

여기서 $I_t^c(p)$와 $B^c(p)$는 현재와 기준 영상의 채널값, $\tau$는 임계값, $V_t$는 가림 등으로 제외되지 않은 ROI 셀 집합이다. $\mathbb{I}[\cdot]$는 조건이 참이면 1을 반환한다. 밝기 보정값 $\Delta_t$는 RGB 평균 차이의 절댓값이 40 미만인 표본의 중앙값으로 계산하고 $[-20,20]$으로 제한한다. 변화 셀은 연결영역으로 묶어 최소 면적과 지속시간을 확인한다.

테이블 이용 중에는 일반 청소 알림을 보류하고, 퇴장 후 45 s의 유예와 ~~15 s의 잔류 확인을 적용한다.~~ **설정된 잔류 확인시간을 적용한다(초기 15 s, 탐색 범위 5–15 s).** 사람 영역과 겹치는 ROI 셀의 비율이 30%를 초과하면 관측 불충분으로 표시한다. 가려진 후보의 소실은 물건 제거로 판단하지 않고 재노출 후 확인한다.

## III. 예비 평가 및 고찰

~~사무실의 동일 테이블에서 촬영한 23개 영상 중 초기 상태 또는 정답이 불명확한 2개를 사전에 제외하였다. 잔류물 있음 10개와 없음 11개를 2 Hz로 처리하고, 영상별 후반의 한 시점에서 육안 정답과 알림 유무를 비교하였다. 변화 임계값 {16, 24, 32}, 최소 면적 비율 {0.001, 0.003}, 확인시간 {5, 10, 15} s의 18개 조합을 탐색하였다. FP가 초기값의 2건 이하이고 품질 유효 비율이 95% 이상인 조합 중 F1이 가장 높은 설정을 선택하였다.~~

**연구실의 동일 테이블에서 촬영한 개발 영상 49개로 평가하였다. 잔류물 있음 23개와 없음 26개를 2 Hz로 처리하고, 영상별 후반 한 시점의 정답과 알림 유무를 비교하였다. 변화 임계값 {16, 24, 32}, 최소 면적 비율 {0.001, 0.003}, 확인시간 {5, 10, 15} s의 18개 조합을 탐색하였다. Table 1은 F1이 가장 높은 설정과 재현율을 우선한 설정의 결과다.**

~~Table 1. Checkpoint evaluation on the same 21 development videos.~~

| ~~Metric~~ | ~~Initial~~ | ~~Selected~~ |
|---|---:|---:|
| ~~Threshold / area / time (s)~~ | ~~24 / 0.003 / 15~~ | ~~32 / 0.003 / 15~~ |
| ~~TP / FP / TN / FN~~ | ~~5 / 2 / 9 / 5~~ | ~~6 / 2 / 9 / 4~~ |
| ~~Precision / recall (%)~~ | ~~71.4 / 50.0~~ | ~~75.0 / 60.0~~ |
| ~~F1 score (%)~~ | ~~58.8~~ | ~~66.7~~ |

**Table 1. Checkpoint evaluation on 49 development videos.**

| **Setting** | **FP / FN** | **P / R (%)** | **F1 (%)** |
|---|---:|---:|---:|
| **Maximum F1** | **8 / 4** | **70.4 / 82.6** | **76.0** |
| **Recall-oriented** | **12 / 2** | **63.6 / 91.3** | **75.0** |

**Threshold / area / time: 32 / 0.001 / 5 s (maximum F1); 24 / 0.001 / 5 s (recall-oriented). P: precision; R: recall.**

~~선정 설정은 FP를 늘리지 않고 한 건의 FN을 줄여 F1이 58.8%에서 66.7%로 변했다. 표본이 21개이므로 한 건이 7.8%p 차이를 만든 결과이며, 동일 자료를 설정 선택에도 사용했으므로 일반화 성능을 의미하지 않는다. 오탐 2건은 테이블 왼쪽의 의자 인접 경계에서 관찰되었다. 그림자와 정지 물체의 움직임은 변화 검출의 알려진 난제이므로 [6], 의자·그림자·ROI 외부 픽셀의 영향을 구분한 뒤 실제 상판에 맞게 ROI를 점검해야 한다.~~ <strong><u>실제 환경의 쓰레기는 작거나 변형되고, 가려지거나 배경과 유사할 수 있어 검출이 어렵다는 점도 보고되었다 [7].</u></strong> ~~따라서 경계 오탐을 줄이는 동시에 작은 잔류물의 누락 여부를 확인해야 한다. 또한 이용 중 상태가 기록된 영상은 2개뿐이어서 이용 영역과 사람 위치의 대응을 추가 검증할 필요가 있다.~~

**F1 최대 설정은 정밀도 70.4%, 재현율 82.6%, F1 76.0%를 보였다. 재현율 우선 설정은 미탐을 4건에서 2건으로 줄였으나 오탐은 8건에서 12건으로 늘었다. 그림자와 정지 물체의 움직임은 변화 검출의 난제다 [6]. 1차에서 누락된 대상은 후속 AI가 확인할 수 없으므로, 후보 단계에서는 재현율을 우선하고 AI로 오탐을 검증하는 방향을 고려할 수 있다. 다만 AI 최종 오염 판정의 효과와 추가 검사 비용은 후속 검증이 필요하다.**

**동일 영상으로 설정을 선택하고 평가했으므로 일반화 성능을 의미하지 않는다. 또한 영상별 한 시점의 알림 유무를 평가한 결과로, 물체 위치와 전체 사건 단위의 성능은 추가 확인이 필요하다.**

## IV. 결 론

~~본 연구는 정상 표면의 지속 변화를 이용·가림 상태와 함께 해석하고, 필요한 후보에 선택적 AI 검사를 적용하는 청결 확인 시스템을 설계·구현하였다. 이를 통해 정상 비치물을 기준 상태에 포함하면서 이용 중 알림과 가림에 따른 오판을 보류하는 처리 흐름을 구성하였다. 제한된 개발 영상에서 18개 설정을 비교한 결과, 선정 설정은 거짓양성을 2건으로 유지하면서 F1을 58.8%에서 66.7%로 높였다. 다만 동일 자료를 설정 선택과 평가에 사용한 예비 결과이므로 일반화 성능으로 해석하지 않았으며, 실패 사례를 통해 ROI 경계와 이용 상태 판정을 우선 개선 과제로 도출하였다. 향후 현재 설정을 고정한 뒤 촬영 환경과 참여자를 확대한 독립 영상에서 사건 단위 정밀도·재현율, 알림 지연 및 AI 검사 비용을 평가할 예정이다. 시스템 안정성과 현장 운영 조건을 확인한 뒤에는 실제 무인매장에서 조명 변화, 고객 이동, 좌석 배치와 장시간 운영 조건을 포함한 현장 시험을 수행하여 시스템의 일반화 가능성과 운영 적합성을 검증할 계획이다.~~

**본 연구는 표면 변화와 이용·가림 상태를 결합하고 선택적 AI 검사로 후보 정보를 보조하는 청결 확인 시스템을 구현하였다. 개발 영상 49개에서 18개 설정을 비교하여 최대 F1 76.0%를 얻었다. 재현율 우선 설정에서는 재현율 91.3%를 보였으며 오탐과 미탐 간의 절충 관계를 확인하였다. 향후 AI 최종 오염 판정을 구현하고 독립 영상에서 후보 검출과 최종 알림 성능, 처리 지연 및 AI 검사 비용을 평가할 예정이다.**

## References

[1] B. S. Kim and J. W. Yoo, “The study on customer misbehavior in unmanned stores: Focusing unmanned convenience store,” *Journal of Channel and Retailing*, Vol. 30, No. 1, pp. 53-78, Jan. 2025.

[2] S. Lee, G. Kwon, and J. Ahn, “Zero-shot model-based abnormal behavior detection fusion model in unmanned store,” *Journal of Korean Institute of Information Technology*, Vol. 23, No. 8, pp. 1-10, Aug. 2025.

[3] E. Luna, J. C. San Miguel, D. Ortego, and J. M. Martínez, “Abandoned object detection in video-surveillance: Survey and comparison,” *Sensors*, Vol. 18, No. 12, Article No. 4290, Dec. 2018.

[4] <strong><u>S. Smeureanu and R. T. Ionescu, “Real-time deep learning method for abandoned luggage detection in video,” in Proceedings of the 26th European Signal Processing Conference, Rome: Italy, pp. 1775-1779, Sep. 2018.</u></strong>

[5] D. Kang, J. Emmons, F. Abuzaid, P. Bailis, and M. Zaharia, “NoScope: Optimizing neural network queries over video at scale,” *Proceedings of the VLDB Endowment*, Vol. 10, No. 11, pp. 1586-1597, Aug. 2017.

[6] Y. Wang, P. M. Jodoin, F. Porikli, J. Konrad, Y. Benezeth, and P. Ishwar, “CDnet 2014: An expanded change detection benchmark dataset,” in *Proceedings of the IEEE Conference on Computer Vision and Pattern Recognition Workshops*, Columbus: OH, pp. 387-394, Jun. 2014.

[7] <strong><u>P. F. Proença and P. Simões, “TACO: Trash annotations in context for litter detection,” arXiv preprint arXiv:2003.06975, Mar. 2020.</u></strong>
