# 논문 PDF 보관 폴더

공개된 출판사·학회·저자 버전 PDF를 개인 연구용으로 보관한다. 공개 다운로드가 가능하다고 재배포 권한이 자동으로 부여되는 것은 아니다. 원문을 수정하거나 저자·소속·저작권 표시를 지우지 않는다. 회사명을 제외한다는 대시보드 요구사항은 타인의 논문 원문을 변조한다는 의미가 아니다.

PDF는 Git에서 제외하고, 서지정보·공개 원문 링크·해설만 버전 관리한다. 저장소를 공개할 때 각 PDF의 재배포 조건을 별도 확인해야 한다.

| 번호 | 로컬 PDF | 원문 |
|---|---|---|
| 1 | [방치물 감시 비교](01_luna_2018_abandoned_object_survey.pdf) | [Sensors](https://www.mdpi.com/1424-8220/18/12/4290) |
| 2 | [방치 수하물 두 단계 감지](02_smeureanu_2018_abandoned_luggage.pdf) | [arXiv](https://arxiv.org/abs/1803.01160) |
| 3 | [NoScope](03_kang_2017_noscope.pdf) | [PVLDB PDF](https://www.vldb.org/pvldb/vol10/p1586-kang.pdf) |
| 4 | [DeepCache](04_xu_2018_deepcache.pdf) | [저자 PDF](https://xumengwei.github.io/files/MobiCom18-DeepCache.pdf) |
| 5 | [FrameExit](05_ghodrati_2021_frameexit.pdf) | [CVF](https://openaccess.thecvf.com/content/CVPR2021/html/Ghodrati_FrameExit_Conditional_Early_Exiting_for_Efficient_Video_Recognition_CVPR_2021_paper.html) |
| 6 | [TACO](06_proenca_2020_taco.pdf) | [arXiv](https://arxiv.org/abs/2003.06975) |
| 7 | [CDnet 2014](07_wang_2014_cdnet.pdf) | [CVF](https://openaccess.thecvf.com/content_cvpr_workshops_2014/W12/html/Wang_CDnet_2014_An_2014_CVPR_paper.html) |

다시 받으려면 저장소 루트에서 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/download_paper_references.ps1`을 실행한다. 기존 정상 PDF는 건너뛰고, 새 파일은 `%PDF-` 헤더를 확인한 후 보관한다. 네트워크 실패나 잘못된 응답은 다운로드 완료로 처리하지 않는다. `.download` 임시 파일은 오류 분석을 위해 남을 수 있다.
