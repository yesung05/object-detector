"""
폭행 2차 AI 최종 평가 — test 분할 **한 번만** (사전 등록: docs/prereg_fight_stage2.md)

전제: 모델(fight_stage2.onnx)과 임계값(model_info.json 의 threshold)은 교차검증으로 이미 고정되어 있다. 이 스크립트는 둘 다
바꾸지 않는다. test 창은 fight2_extract.py --final-test 로 만든 windows.npz(별도 폴더)를 쓴다.
결과가 나쁘더라도 임계값·모델을 다시 조정하지 않고 그대로 보고한다.
"""
import argparse
import csv
import importlib.util
import json
from pathlib import Path

import numpy as np
import onnxruntime as ort

_spec = importlib.util.spec_from_file_location("f2train", Path(__file__).with_name("fight2_train.py"))
f2 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(f2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model-dir", required=True)     # fight_stage2.onnx, model_info.json
    ap.add_argument("--test-dir", required=True)      # test windows.npz
    ap.add_argument("--index", required=True)
    a = ap.parse_args()
    info = json.load(open(Path(a.model_dir) / "model_info.json"))
    thr = info["threshold"]
    z = np.load(Path(a.test_dir) / "windows.npz", allow_pickle=False)
    assert set(np.unique(z["split"])) == {"test"}
    X, y, clip, store, w0 = z["X"], z["y"], z["clip"], z["store"], z["w0"]
    sess = ort.InferenceSession(str(Path(a.model_dir) / "fight_stage2.onnx"), providers=["CPUExecutionProvider"])
    logit = np.concatenate([sess.run(None, {"x": X[i:i + 1024]})[0] if False else
                            np.array([sess.run(None, {"x": X[j:j + 1]})[0].reshape(-1)[0] for j in range(i, min(i + 1024, len(X)))])
                            for i in range(0, len(X), 1024)])
    p = 1.0 / (1.0 + np.exp(-logit))
    rows = {r["clip"]: r for r in csv.DictReader(open(a.index, encoding="utf-8-sig"))}
    clips = {}
    for c in np.unique(clip):
        r = rows[c]
        sel = np.where(clip == c)[0]
        order = sel[np.argsort(w0[sel])]
        gt = json.loads(r["events"]) if r["class_code"] == "13" else []
        clips[c] = (r["class_code"] == "13", gt, int(r["frames"]), w0[order], r["store"], order)
    cl = {c: v[:5] for c, v in clips.items()}
    rec, fp, hrs, per = f2.evaluate(cl, lambda c: p[clips[c][5]], thr)
    nfight = sum(1 for v in clips.values() if v[0])
    out = {"threshold": thr, "fight_clips_with_windows": nfight, "clip_recall": rec, "false_alarms": fp,
           "neg_hours": hrs, "fa_per_hour": fp / hrs if hrs else None, "store_recall": per,
           "worst_store_recall": min(per.values()) if per else None,
           "windows": int(len(y)), "positive_windows": int(y.sum())}
    print(json.dumps(out, ensure_ascii=False, indent=1, default=float))
    (Path(a.model_dir) / "test_result.json").write_text(json.dumps(out, ensure_ascii=False, indent=1, default=float), encoding="utf-8")


if __name__ == "__main__":
    main()
