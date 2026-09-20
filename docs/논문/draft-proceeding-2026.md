<!--
학술대회 논문 초안 | proceeding_form_2026.pdf 기준 | 2026-09-12

이 주석은 제출 원고에서 삭제한다. Markdown은 아래한글의 글꼴과 2단 조판을
고정할 수 없으므로 아래 내용을 양식 원본에 옮겨 다음 서식을 적용한다.

1. 페이지: 양식 원본의 A4·2단·여백·머리말·쪽번호를 그대로 유지한다.
2. 국문 제목: HY견고딕 14 pt, 장평 100, 자간 -5, 줄간격 160%, 왼쪽 정렬.
3. 영문 제목: Arial 16.5 pt, 장평 100, 자간 0, 줄간격 130%, 왼쪽 정렬.
4. 국문 저자: HY견명조 10.5 pt. 국문 소속: 돋움 8.7 pt.
5. 영문 저자: Times New Roman 10.5 pt. 영문 소속: Arial 8.7 pt.
6. Abstract 제목: Arial 10 pt. 본문: Times New Roman 9.2 pt,
   장평 100, 자간 -5, 들여쓰기 10 pt, 줄간격 150%. 120–150 words.
7. 장 제목: HY중고딕 10.5 pt, 장평 90, 자간 0.
   절 제목: HY중고딕 10 pt, 장평 90, 자간 -6.
8. 본문: 국문 바탕, 영문 Times New Roman 9.5 pt,
   장평 90, 자간 -6, 들여쓰기 10 pt.
9. 수식은 한글 수식편집기로 입력하고 식 번호를 오른쪽에 정렬한다.
10. 그림·표 캡션: 돋움 9 pt, 장평 90, 자간 -6, 양쪽 정렬.
    그림 캡션은 그림 아래, 표 캡션은 표 위에 둔다.
11. 그림 내부 표기와 캡션은 요청에 따라 영어로 통일한다.
12. Fig. 1과 Fig. 2는 내부 글자의 가독성을 위해 두 단 전체 폭 배치를 권장한다.
13. References 제목: HY견고딕 10 pt. 본문: Times New Roman 9 pt,
    줄간격 150%. 본문에서 처음 인용한 순서대로 번호를 부여한다.
14. 저자·소속·교신저자 정보는 실제 정보로 교체한다. 지원 과제가 있을 때만
    결론과 References 사이에 Acknowledgments를 추가한다.
-->

# 무인 공간 청결 관리를 위한 표면 상태 기반 영상 감시 시스템의 설계 및 예비 평가

Surface-State Monitoring for Cleanliness in Unmanned Spaces

〈국문 저자명〉<sup>1</sup> · 〈국문 교신저자명〉<sup>2*</sup>

<sup>1</sup>〈대학교·기관명 학과명〉  
<sup>2</sup>〈대학교·기관명 학과명〉

〈English Author Name〉<sup>1</sup> · 〈English Corresponding Author Name〉<sup>2*</sup>

<sup>1</sup>〈Department, Institution, City, Postal Code, Republic of Korea〉  
<sup>2</sup>〈Department, Institution, City, Postal Code, Republic of Korea〉

## [Abstract]

This paper presents a video monitoring system designed to support cleanliness inspection in unmanned spaces. The system registers a reference image for each surface, detects persistent changes, and combines the resulting candidates with occupancy and occlusion states. Object detection is selectively applied to supplement candidate information, while recent results can be reused for visually similar candidates. A preliminary evaluation used 21 development videos recorded at a single office table. Among 18 parameter combinations, the selected configuration achieved an F1 score of 66.7%, compared with 58.8% for the initial configuration, while retaining two false positives. Because the same videos were used for parameter selection and reporting, the results do not estimate performance in independent environments. Error analysis identified interference near the table and chair boundary and insufficient occupancy observations as key design issues requiring further evaluation.

*Corresponding Author

## I. 서 론

무인점포에서는 관리자가 상주하지 않기 때문에 이용 후 남겨진 물건이나 정리가 필요한 공간을 지속적으로 확인하기 어렵다. 매장의 청결성과 편리성이 소비자의 부정적 감정을 낮추는 요인으로 보고된 점을 고려하면 [1], 청소가 필요한 상황을 관리자에게 알리는 기능은 무인점포의 운영을 지원하는 과제가 될 수 있다. 그러나 영상에서 컵을 검출했다는 사실만으로 청소 필요 여부를 판단하기는 어렵다. 같은 컵이라도 정상 비치물일 수 있고, 이용자가 사용 중이거나 자리를 떠나며 남긴 물건일 수도 있기 때문이다.

무인매장 영상 연구에는 사람의 행동과 주변 객체를 결합하여 이상행동을 탐지하는 접근이 있다 [2]. 방치물 검출 분야에서는 전경 분리, 정지 물체 검출, 사람 검출 및 방치 확인을 단계적으로 수행하며 [3], 규칙으로 추출한 정지 후보를 신경망으로 확인하는 방식도 제안되었다 [4]. 한편 NoScope는 연속 영상의 시간적 중복을 활용하여 반복적인 신경망 실행을 줄였다 [5]. 이러한 연구는 변화 후보를 먼저 선별하고 장면의 맥락에 따라 추가 검사를 수행하는 설계에 참고가 된다.

본 연구에서는 정상 표면의 모습과 이용 상태를 함께 고려하는 영상 감시 시스템을 설계하고 구현한다. 정상 비치물을 포함한 기준 영상을 등록한 뒤 지속적인 변화를 찾고, 이용 중이거나 표면이 가려진 경우에는 알림 판단을 보류한다. 인공지능 (AI; artificial intelligence) 검사는 후보의 객체 정보를 보완하며, 유사한 후보에는 최근 결과를 재사용하도록 구성한다. 본 연구의 기여는 정상 기준과 이용·가림 상태를 결합한 청소 알림 구조를 제시하고, 규칙 기반 후보와 선택적 AI 검사를 하나의 흐름으로 구현하며, 실제 영상의 설정 탐색과 실패 사례 분석을 통해 영역 설정과 상태 판정의 개선 요구사항을 도출한 데 있다. 여기서 청결 관리는 영상에 나타난 잔류물 의심 상황의 확인을 지원하는 범위로 한정한다.

## II. 시스템 설계 및 구현

### 1. 설계 요구사항과 전체 구조

청소 확인을 위한 감시 시스템은 물체의 존재뿐 아니라 해당 공간의 이용 맥락을 반영해야 한다. 이를 위해 정상 비치물을 기준 상태에 포함하고, 이용 중에는 일반 청소 알림을 보류한다. 사람이 표면을 가린 상황은 실제로 물건이 제거된 상황과 구분하며, 변화가 거의 없는 후보에 대한 반복 AI 검사도 제한한다.

Fig. 1은 이러한 요구를 반영한 처리 흐름이다. 입력 영상은 표면 비교와 사람 위치 분석에 함께 사용된다. 표면 비교에서 생성한 변화 후보에 이용·가림 상태와 지속시간 조건을 적용하고, 그 결과를 관리자 대시보드에 표시한다. 선택적 AI 검사는 객체 정보를 보조하며 객체 분류의 성공만을 청소 알림의 전제조건으로 두지 않는다.

![Monitoring workflow](figures/ver2/monitoring-workflow.jpg)

Fig. 1. Monitoring workflow combining surface changes, occupancy and occlusion states, and selective AI inspection.

### 2. 정상 기준과 변화 후보 생성

사용자는 감시할 표면을 다각형 관심 영역 (ROI; region of interest)으로 지정한다. 모니터와 같은 정상 비치물을 평소 위치에 둔 상태에서 가림 없는 관측 5회의 평균으로 기준 영상을 구성한다. 이 방식은 비치물마다 종류와 경계를 지정하는 부담을 줄이지만, 등록 이후 비치물의 위치가 바뀌면 변화 후보가 될 수 있다.

기준 영상과 현재 표면 영상은 96×96 RGB 표본으로 변환한다. 밝기 차이를 보정한 뒤 채널별 차이를 비교하며, 비교 가능한 셀의 변화 마스크는 식 (1)과 같이 정의한다.

$$
M_t(p)=\mathbb{I}\!\left(\max_{c\in\{R,G,B\}}\left|I_t^c(p)-B^c(p)-\Delta_t\right|>\tau\right),\quad p\in V_t.\tag{1}
$$

여기서 $I_t^c(p)$는 시각 $t$에서 셀 $p$의 채널 $c$ 값, $B^c(p)$는 정상 기준값, $\Delta_t$는 밝기 보정값, $\tau$는 변화 임계값이다. $V_t$는 ROI 내부에서 가림 등으로 제외되지 않은 비교 가능 셀의 집합이며, $\mathbb{I}(\cdot)$는 조건을 만족하면 1을 반환한다.

$\Delta_t$는 관측마다 계산하는 전역 밝기 보정값이다. 비교 가능한 셀에서 현재 영상과 기준 영상의 RGB 평균 차이를 구하고, 절댓값이 40 미만인 표본이 ROI 셀의 3분의 1을 초과할 때 그 중앙값을 사용한다. 큰 변화가 보정값을 좌우하지 않도록 나머지 표본은 제외하고, 최종 값은 $[-20,20]$ 범위로 제한한다. 변화 셀은 상하좌우로 인접한 연결영역으로 묶고 최소 면적과 지속시간 조건을 적용한다.

### 3. 이용·가림 상태와 선택적 AI 검사

테이블의 이용 여부는 사람 위치와 사전에 지정한 이용 영역의 관계로 판단한다. 이용 중에는 일반 청소 알림을 보류하고, 사람이 떠난 뒤에는 45 s의 퇴석 유예와 15 s의 잔류 확인을 거쳐 알림을 허용한다.

사람 검출 영역과 겹치는 표본은 비교에서 제외한다. ROI 셀 집합을 $R$, 시각 $t$에서 사람 영역과 겹치는 셀 집합을 $H_t$라 할 때 가림 비율은 $O_t=|R\cap H_t|/|R|$로 계산한다. $O_t>0.3$이면 관측 불충분 상태로 표시하며, 가려진 후보의 소실을 물건 제거로 판단하지 않고 다시 보일 때 변화 해소를 확인한다.

AI 검사는 변화 후보의 객체 정보를 보완한다. 후보의 위치와 모습이 이전 검사와 유사하고 관측이 유효하면 최근 성공한 결과를 최대 60 s 재사용한다. 후보 모습의 변경, 결과 만료 또는 퇴석 확정 시에는 다시 검사한다. 대시보드는 정상, 확인 필요, 이용 중 및 가림 상태를 구분하여 표시한다.

## III. 예비 평가 및 설계 고찰

### 1. 평가 조건과 설정 탐색

구현 동작을 확인하기 위해 사무실의 동일 테이블에서 주 실험자 1명이 행동한 영상을 사용하였다. 촬영 영상 23개 중 초기 상태 또는 잔류물 정답이 불명확한 2개를 평가 전에 제외했으며, 최종 자료는 잔류물 있음 10개와 없음 11개로 구성하였다. 1280×720 영상을 2 Hz로 표본화하고 실제 사람·객체 ONNX 모델과 표면 감지 코드를 실행하였다. 각 영상은 별도의 상태로 시작하고 영상 시각 15 s 이후 가림 없는 관측 5회로 기준을 등록하였다.

각 영상의 관찰 후반에 한 평가 시점을 지정하고, 육안으로 판정한 잔류물 유무를 가장 가까운 표본의 알림 유무와 비교하였다. 변화 임계값 {16, 24, 32}, 최소 면적 비율 {0.001, 0.003}, 확인시간 {5, 10, 15} s의 18개 조합을 탐색하였다. ROI와 모델은 고정하고, 거짓양성 (FP; false positive)이 초기 설정의 2건 이하이며 품질 유효 비율이 95% 이상인 조합 중 F1이 가장 높은 설정을 선택하였다. 평가기는 사람 추론을 매 표본에 수행하고 객체 검사를 동기 실행하므로 운영 시스템의 추적과 비동기 처리 전체를 재현하지 않는다.

Table 1. Checkpoint evaluation on the same 21 development videos.

| Metric | Initial configuration | Selected configuration |
|---|---:|---:|
| Threshold / minimum area ratio / confirmation time (s) | 24 / 0.003 / 15 | 32 / 0.003 / 15 |
| TP / FP / TN / FN | 5 / 2 / 9 / 5 | 6 / 2 / 9 / 4 |
| Precision / recall (%) | 71.4 / 50.0 | 75.0 / 60.0 |
| F1 score (%) | 58.8 | 66.7 |

### 2. 설정 비교와 검출 사례

선정 설정은 변화 임계값을 24에서 32로 높이고 최소 면적과 확인시간을 유지한 조합이다. 거짓양성은 2건으로 같았으며, 한 영상의 거짓음성이 참양성으로 바뀌면서 F1이 58.8%에서 66.7%로 증가하였다. 영상 수가 21개이므로 이 한 건만으로 F1이 7.8%p 달라졌다. 따라서 관찰된 차이는 개선 가능성을 보여주는 예비 결과이며 안정적인 성능 향상을 입증하지 않는다. 모든 조합의 품질 유효 비율은 20/21(95.2%)이었다. 가림 상태였던 한 영상도 실제 알림 전달 평가에서는 거짓음성으로 포함하였다.

Fig. 2는 가방을 가져간 뒤 종이가 남은 S34의 처리 예이다. 실제 추론에서 저장한 기준·증거 영상으로 재구성한 마스크에는 종이 위치에 대응하는 연결영역 57셀이 나타났으며 최소 면적 필터 후에도 유지되었다. 이 사례는 지속 변화가 후보로 생성되는 과정을 보여주며, 픽셀 단위 정답 마스크와의 일치도는 측정하지 않았다.

![S34 detection example](figures/ver2/s34-three-panels.jpg)

Fig. 2. S34: (a) original frame with ROI and alert candidate, (b) reference surface, and (c) final change mask.

### 3. 실패 사례와 개선 방향

설정 탐색 이후에도 거짓양성 2건과 거짓음성 4건이 남았다. 특히 오탐 후보는 테이블 왼쪽의 의자 인접 경계에 나타났다. 그림자와 정지 물체의 움직임은 기존 변화 검출 연구에서도 난제로 다뤄진다 [6]. 이 결과는 의자 이동, 그림자 또는 ROI 외부 픽셀의 영향을 구분하고 실제 상판에 맞게 ROI를 점검할 필요가 있음을 보여준다. 경계 영역을 지나치게 제외하면 가장자리의 잔류물을 놓칠 수 있으므로 수정 전후의 오탐과 미검출을 함께 비교해야 한다.

이용 상태 판정에서도 보완할 점이 확인되었다. 21개 영상 중 이용 중 상태가 기록된 영상은 2개뿐이었다. 지정한 이용 영역과 사람 위치의 대응 및 평가기와 운영 추적 방식의 차이를 점검해야 한다. 또한 모든 영상이 동일한 장소와 촬영 세션에 속하고, 같은 자료에서 설정을 선택한 뒤 결과를 보고하였다. 후속 평가에서는 설정을 고정한 독립 촬영 자료에 실제 퇴석과 물체 제거 시각을 기록하여 사건 단위 알림 성능을 측정해야 한다. 선택적 AI 검사의 효율도 재사용 활성·비활성 조건에서 실제 호출 수와 추론 시간을 비교해야 한다.

## IV. 결 론

본 연구에서는 정상 표면과의 지속적인 변화를 이용·가림 상태와 함께 해석하는 청결 확인 지원 시스템을 설계하고 구현하였다. 표면 비교로 생성한 후보를 알림 정책과 선택적 AI 검사에 연결하여 정상 비치물과 이용 중인 공간을 고려하는 처리 구조를 구성하였다. 자체 영상 21개의 예비 평가에서는 설정에 따른 판정 차이를 확인하고, 의자 인접 경계의 간섭과 이용 상태 판정의 개선 필요성을 도출하였다. 향후 실제 상판과 좌석에 맞게 영역 설정을 점검하고, 독립 촬영 자료에서 사건 단위 알림 성능과 AI 검사 비용을 평가할 예정이다.

## References

[1] B. S. Kim and J. W. Yoo, “The study on customer misbehavior in unmanned stores: Focusing unmanned convenience store,” *Journal of Channel and Retailing*, Vol. 30, No. 1, pp. 53-78, Jan. 2025.

[2] S. Lee, G. Kwon, and J. Ahn, “Zero-shot model-based abnormal behavior detection fusion model in unmanned store,” *Journal of Korean Institute of Information Technology*, Vol. 23, No. 8, pp. 1-10, Aug. 2025.

[3] E. Luna, J. C. San Miguel, D. Ortego, and J. M. Martínez, “Abandoned object detection in video-surveillance: Survey and comparison,” *Sensors*, Vol. 18, No. 12, Article No. 4290, Dec. 2018.

[4] S. Smeureanu and R. T. Ionescu, “Real-time deep learning method for abandoned luggage detection in video,” in *Proceedings of the 26th European Signal Processing Conference*, Rome: Italy, pp. 1775-1779, Sep. 2018.

[5] D. Kang, J. Emmons, F. Abuzaid, P. Bailis, and M. Zaharia, “NoScope: Optimizing neural network queries over video at scale,” *Proceedings of the VLDB Endowment*, Vol. 10, No. 11, pp. 1586-1597, Aug. 2017.

[6] Y. Wang, P. M. Jodoin, F. Porikli, J. Konrad, Y. Benezeth, and P. Ishwar, “CDnet 2014: An expanded change detection benchmark dataset,” in *Proceedings of the IEEE Conference on Computer Vision and Pattern Recognition Workshops*, Columbus: OH, pp. 387-394, Jun. 2014.
