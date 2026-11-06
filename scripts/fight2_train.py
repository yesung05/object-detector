"""
폭행 2차 AI 학습·교차검증 (개발 PC 전용). 사전 등록: docs/prereg_fight_stage2.md

- 입력: scripts/fight2_extract.py 가 만든 windows.npz (dev+pool 만, test 는 이 스크립트가 보지 않는다).
- 후보 3개(1D-CNN / GRU / MLP 풀링)를 고정 하이퍼파라미터로 매장 단위 leave-one-store-out 교차검증하고, 7개 폴드 예측을 합쳐
  PR-AUC 가 가장 높은 모델을 고른다(탐색 없음).
- 클립 단위 결정 규칙(연속 4개 창 구간에서 임계값 이상 창 2개 이상 → 발화)을 교차검증 예측에 적용해, 음성 시간당 오탐 ≤ 2.0 을
  만족하는 가장 낮은 임계값을 정하고 재현율·매장별 최저 재현율·1차 필터 단독 기준선과 비교한다.
- 선택한 모델을 전체 데이터로 다시 학습해 ONNX(FP32)로 내보내고 크기·지연을 잰다.
결과는 <work>/cv_result.json 과 표준 출력에 남긴다.
"""

import argparse
import csv
import json
import time
from collections import defaultdict
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

T, D = 9, 106
STRIDE = 3
FPS = 3.0
PRE, POST = 3, 9                  # 정답 구간 앞 1초, 뒤 3초(프레임)
FA_TARGET = 2.0                   # 음성 시간당 오탐 상한(사전 등록)
SEED = 0


class CNN(nn.Module):
    def __init__(self):
        super().__init__()
        self.c1 = nn.Conv1d(D, 48, 3, padding=1)
        self.c2 = nn.Conv1d(48, 48, 3, padding=1)
        self.fc = nn.Linear(96, 1)

    def forward(self, x):                 # x [B,T,D]
        h = torch.relu(self.c1(x.transpose(1, 2)))
        h = torch.relu(self.c2(h))
        return self.fc(torch.cat([h.mean(2), h.amax(2)], 1)).squeeze(1)


class GRU(nn.Module):
    def __init__(self):
        super().__init__()
        self.g = nn.GRU(D, 48, batch_first=True)
        self.fc = nn.Linear(48, 1)

    def forward(self, x):
        o, _ = self.g(x)
        return self.fc(o[:, -1]).squeeze(1)


class MLP(nn.Module):
    def __init__(self):
        super().__init__()
        self.net = nn.Sequential(nn.Linear(2 * D, 64), nn.ReLU(), nn.Linear(64, 1))

    def forward(self, x):
        return self.net(torch.cat([x.mean(1), x.std(1)], 1)).squeeze(1)


MODELS = {"cnn": CNN, "gru": GRU, "mlp": MLP}


def fit(name, X, y, device, epochs=40, batch=256):
    torch.manual_seed(SEED)
    np.random.seed(SEED)
    m = MODELS[name]().to(device)
    opt = torch.optim.AdamW(m.parameters(), lr=1e-3)
    pos, neg = float((y == 1).sum()), float((y == 0).sum())
    lossf = nn.BCEWithLogitsLoss(pos_weight=torch.tensor(neg / max(pos, 1.0), device=device))
    Xt = torch.tensor(X, device=device)
    yt = torch.tensor(y, dtype=torch.float32, device=device)
    n = len(Xt)
    for _ in range(epochs):
        perm = torch.randperm(n, device=device)
        m.train()
        for i in range(0, n, batch):
            idx = perm[i:i + batch]
            opt.zero_grad()
            lossf(m(Xt[idx]), yt[idx]).backward()
            opt.step()
    return m


@torch.no_grad()
def predict(m, X, device):
    m.eval()
    out = []
    Xt = torch.tensor(X, device=device)
    for i in range(0, len(Xt), 2048):
        out.append(torch.sigmoid(m(Xt[i:i + 2048])).cpu().numpy())
    return np.concatenate(out)


def average_precision(y, s):
    order = np.argsort(-s)
    y = y[order]
    tp = np.cumsum(y)
    prec = tp / np.arange(1, len(y) + 1)
    return float((prec * y).sum() / max(y.sum(), 1))


def baseline_score(X):
    """1차 필터 단독 기준선: 창 안 최대 pair_close × 관절 변위(상자 정규화 좌표, 프레임 간 평균 절대차의 최댓값).
    fight_score(쌍 근접 × 팔다리 속도)의 정규화 좌표 근사다."""
    d = X[:, :, 102] * 4.0
    close = np.clip(1.0 - d / 1.5, 0, None).max(1)
    kp = X[:, :, :102].reshape(len(X), T, 2, 17, 3)
    xy = kp[..., :2]
    vis = (kp[..., 2] > 0.4)
    dif = np.abs(np.diff(xy, axis=1)).sum(-1)                 # [N,T-1,2,17]
    ok = vis[:, 1:] & vis[:, :-1]
    mot = (dif * ok).sum((2, 3)) / np.maximum(ok.sum((2, 3)), 1)
    return close * mot.max(1)


def clip_alerts(w0, p, thr):
    """창 시작 w0(오름차순)과 확률 p 로, 임계값 이상 창이 연속 4창 구간(w0 간격 9프레임 이내)에 2개 이상이면 발화.
    발화 구간(프레임 [a, b])의 목록을 돌려준다(가까운 발화는 하나로 합친다)."""
    hit = [(int(a), float(q)) for a, q in zip(w0, p) if q >= thr]
    fires = []
    for i, (a, _) in enumerate(hit):
        cnt = sum(1 for b, _ in hit[i:] if b - a <= 3 * STRIDE)
        if cnt >= 2:
            span = (a, a + 3 * STRIDE + T - 1)
            if fires and span[0] <= fires[-1][1]:
                fires[-1] = (fires[-1][0], span[1])
            else:
                fires.append(span)
    return fires


def evaluate(clips, score_of, thr):
    """clips: dict clip -> (is_fight, gt, n_frames, w0[], store). 반환: (재현율, 오탐 건수, 음성 시간, 매장별 재현율)"""
    tp = fp = 0
    nfight = 0
    neg_sec = 0.0
    per_store = defaultdict(lambda: [0, 0])
    for c, (is_fight, gt, nfr, w0, store) in clips.items():
        fires = clip_alerts(w0, score_of(c), thr)
        if is_fight:
            nfight += 1
            ok_range = [(s - PRE, (e if e is not None else s) + POST) for s, e in gt]
            hit = any(a <= r1 and b >= r0 for a, b in fires for r0, r1 in ok_range)
            tp += hit
            per_store[store][0] += hit
            per_store[store][1] += 1
            fp += sum(1 for a, b in fires if not any(a <= r1 and b >= r0 for r0, r1 in ok_range))
            inside = sum((min(e if e is not None else s, nfr) - s + 1) for s, e in gt) + 2 * 2 * FPS * len(gt)
            neg_sec += max(nfr / FPS - inside / FPS, 0.0)
        else:
            fp += len(fires)
            neg_sec += nfr / FPS
    stores = {s: (a / b if b else 0.0) for s, (a, b) in per_store.items()}
    return tp / max(nfight, 1), fp, neg_sec / 3600.0, stores


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--epochs", type=int, default=40)
    a = ap.parse_args()
    work = Path(a.work)
    z = np.load(work / "windows.npz", allow_pickle=False)
    X, y, clip, store, w0 = z["X"], z["y"].astype(np.int64), z["clip"], z["store"], z["w0"]
    assert set(np.unique(z["split"])) <= {"dev", "pool"}, "test 분할이 섞여 있다"
    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"창 {len(y)}개 (양성 {int(y.sum())}), 매장 {sorted(set(store))}, device={device}", flush=True)

    rows = {r["clip"]: r for r in csv.DictReader(open(a.index, encoding="utf-8-sig"))}
    clips = {}
    for c in np.unique(clip):
        r = rows[c]
        sel = np.where(clip == c)[0]
        order = sel[np.argsort(w0[sel])]
        gt = json.loads(r["events"]) if r["class_code"] == "13" else []
        clips[c] = (r["class_code"] == "13", gt, int(r["frames"]), w0[order], r["store"], order)

    stores = sorted(set(store))
    preds = {}
    for name in MODELS:
        t0 = time.time()
        p = np.zeros(len(y), np.float32)
        for st in stores:                                           # leave-one-store-out
            tr, te = store != st, store == st
            m = fit(name, X[tr], y[tr], device, a.epochs)
            p[te] = predict(m, X[te], device)
        preds[name] = p
        print(f"[{name}] PR-AUC {average_precision(y, p):.4f}  ({time.time() - t0:.0f}s)", flush=True)
    best = max(preds, key=lambda n: average_precision(y, preds[n]))
    print("선택 모델:", best, flush=True)

    def score_fn(pred):
        return lambda c: pred[clips[c][5]]
    result = {"models": {n: {"pr_auc": average_precision(y, preds[n])} for n in preds}, "selected": best}
    base_all = baseline_score(X)
    for tag, pred in (("model", preds[best]), ("baseline", base_all / max(base_all.max(), 1e-9))):
        cl = {c: v[:5] for c, v in clips.items()}
        table = []
        for thr in np.unique(np.round(np.quantile(pred, np.linspace(0.0, 0.999, 300)), 5)):
            rec, fp, hrs, per = evaluate(cl, score_fn(pred), float(thr))
            table.append((float(thr), rec, fp, hrs, per))
        ok = [t for t in table if t[3] > 0 and t[2] / t[3] <= FA_TARGET]
        pick = min(ok, key=lambda t: t[0]) if ok else None
        result[tag] = {"fa_target": FA_TARGET, "neg_hours": table[0][3] if table else None}
        if pick:
            result[tag].update({"threshold": pick[0], "clip_recall": pick[1], "false_alarms": pick[2],
                                "fa_per_hour": pick[2] / pick[3], "store_recall": pick[4],
                                "worst_store_recall": min(pick[4].values()) if pick[4] else None})
        else:
            result[tag]["note"] = "오탐 상한을 만족하는 임계값 없음"
        # 같은 재현율에서 비교하기 위한 곡선(재현율별 최소 오탐/시간)
        result[tag]["curve"] = [{"thr": t[0], "recall": t[1], "fa_per_hour": t[2] / t[3] if t[3] else None} for t in table[::10]]
    print(json.dumps({k: v for k, v in result.items() if k in ("models", "selected", "model", "baseline")}, ensure_ascii=False,
                     indent=1, default=lambda o: o if not isinstance(o, np.floating) else float(o))[:3500], flush=True)
    (work / "cv_result.json").write_text(json.dumps(result, ensure_ascii=False, indent=1,
                                                    default=lambda o: float(o) if isinstance(o, np.floating) else str(o)), encoding="utf-8")

    # 최종 모델: 전체 데이터로 다시 학습 → ONNX
    m = fit(best, X, y, device, a.epochs)
    m.eval().cpu()
    dummy = torch.zeros(1, T, D)
    onnx_path = work / "fight_stage2.onnx"
    torch.onnx.export(m, dummy, str(onnx_path), input_names=["x"], output_names=["logit"], opset_version=13,
                      dynamic_axes=None)
    import onnxruntime as ort
    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    xs = np.random.RandomState(0).randn(1, T, D).astype(np.float32)
    for _ in range(50):
        sess.run(None, {"x": xs})
    t0 = time.perf_counter()
    for _ in range(500):
        sess.run(None, {"x": xs})
    ms = (time.perf_counter() - t0) / 500 * 1000
    n_params = sum(p.numel() for p in m.parameters())
    info = {"model": best, "params": n_params, "onnx_bytes": onnx_path.stat().st_size, "latency_ms_dev_pc_1thread": ms,
            "threshold": result["model"].get("threshold")}
    (work / "model_info.json").write_text(json.dumps(info, indent=1), encoding="utf-8")
    torch.save(m.state_dict(), work / "fight_stage2.pt")
    print("모델 정보:", info, flush=True)


if __name__ == "__main__":
    main()
