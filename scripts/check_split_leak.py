"""
분할 누수 검사 (읽기 전용): dev/pool/test 사이에 같은 장면이 걸쳐 있지 않은지, 매장·카메라가 얼마나 겹치는지 보고한다.

왜 필요한가: 한 사건을 카메라 6대가 동시에 찍으므로 클립 단위로 나누면 같은 사건이 학습과 평가 양쪽에 들어가
성능이 부풀려진다. 장면 키(상점|날짜|시:분)는 분할 사이에서 겹치면 안 된다(겹치면 종료 코드 1).
매장은 7곳뿐이라 겹치는 것이 정상이지만, 2차 AI 학습은 매장 단위 hold-out 으로 한 번 더 검증해야 하므로 겹침 정도를 표로 낸다.

사용: python -I scripts/check_split_leak.py E:\hunik-anomaly\anomaly_index.csv
"""
import csv
import sys
from collections import defaultdict


def main():
    rows = list(csv.DictReader(open(sys.argv[1], encoding="utf-8-sig")))
    scenes, stores = defaultdict(set), defaultdict(lambda: defaultdict(int))
    for r in rows:
        sp = r["split"]
        if sp == "excluded_overlap":
            continue
        scenes[sp].add(r["scene_key"])
        stores[sp][r["store"]] += 1
    bad = 0
    sys.stdout.reconfigure(encoding="utf-8")
    names = sorted(scenes)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            inter = scenes[a] & scenes[b]
            print(f"장면 겹침 {a}∩{b}: {len(inter)}")
            bad += len(inter)
    print("\n매장별 클립 수")
    allst = sorted({s for sp in stores.values() for s in sp})
    print("split | " + " | ".join(allst))
    for sp in names:
        print(f"{sp} | " + " | ".join(str(stores[sp].get(s, 0)) for s in allst))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
