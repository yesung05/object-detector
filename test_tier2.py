"""
Tier 2 ONNX 모델 정적 이미지 추론 테스트
- 모델 출력 형태 확인
- 실제 감지 결과(class_id, score, bbox) 출력
"""

import sys
import numpy as np
import onnxruntime as ort
from PIL import Image

CLASS_NAMES = {
    0:  "cat",
    1:  "dog",
    2:  "bottle",
    3:  "cup",
    4:  "food_0",  5: "food_1", 6: "food_2", 7: "food_3", 8: "food_4",
    9:  "food_5", 10: "food_6",11: "food_7",12: "food_8",13: "food_9",
    14: "chair",
    15: "dining_table",
}

MODEL = r"D:\DMU\Contest\hunik\object-detector\models\yolo11n_tier2_fp32.onnx"

# 이미지 인자 없으면 캐시 이미지 사용
IMG_PATH = sys.argv[1] if len(sys.argv) > 1 else \
    r"C:\Users\AILAB\.claude\image-cache\b6cfe6c1-a24b-47e1-afc0-1712851d4a48\7.png"

CONF_THRESH = 0.10   # 낮게 설정해서 약한 감지도 모두 출력

def letterbox(img, target_w, target_h):
    iw, ih = img.size
    scale = min(target_w / iw, target_h / ih)
    nw = int(iw * scale)
    nh = int(ih * scale)
    img = img.resize((nw, nh), Image.BILINEAR)
    pad = Image.new("RGB", (target_w, target_h), (114, 114, 114))
    pad.paste(img, ((target_w - nw) // 2, (target_h - nh) // 2))
    return pad, scale, (target_w - nw) // 2, (target_h - nh) // 2

sess = ort.InferenceSession(MODEL)

inp  = sess.get_inputs()[0]
outs = sess.get_outputs()
print(f"=== 모델 정보 ===")
print(f"input : {inp.name}  shape={inp.shape}")
for o in outs:
    print(f"output: {o.name}  shape={o.shape}")

_, _, in_h, in_w = inp.shape  # NCHW
print(f"\n입력 해상도: {in_w}x{in_h}")
print(f"이미지: {IMG_PATH}\n")

img_orig = Image.open(IMG_PATH).convert("RGB")
orig_w, orig_h = img_orig.size

img_lb, scale, pad_x, pad_y = letterbox(img_orig, in_w, in_h)
x = np.array(img_lb, dtype=np.float32).transpose(2, 0, 1) / 255.0
x = x[np.newaxis, ...]   # 1×3×H×W

raw = sess.run(None, {inp.name: x})[0]   # 첫 번째 출력
print(f"출력 텐서 shape: {raw.shape}")

# 출력 형태 판별
# [1, 4+nc, N]  → 전치 필요
# [1, N, 4+nc]  → 그대로
if raw.ndim == 3:
    if raw.shape[1] < raw.shape[2]:   # [1, 4+nc, N]
        pred = raw[0].T               # → [N, 4+nc]
        nc = raw.shape[1] - 4
    else:                             # [1, N, 4+nc]
        pred = raw[0]
        nc = raw.shape[2] - 4
elif raw.ndim == 2:                   # [N, 4+nc] (NMS 완료)
    pred = raw
    nc = raw.shape[1] - 4
else:
    print("알 수 없는 출력 형태:", raw.shape)
    sys.exit(1)

print(f"클래스 수(nc): {nc}")

# NMS 결과 형태 [N, 6] 인지 확인
if nc == 2 and pred.shape[1] == 6:
    # embedded NMS: cx,cy,w,h,score,class_id
    detections = []
    for row in pred:
        cx, cy, w, h, score, cid = row
        if score < CONF_THRESH: continue
        detections.append((int(cid), float(score), float(cx), float(cy), float(w), float(h)))
else:
    # 일반 YOLO 출력: cx,cy,w,h,cls0,cls1,...
    detections = []
    for row in pred:
        box  = row[:4]
        if nc == 1:
            scores = row[4:5]
        else:
            scores = row[4:4+nc]
        cid = int(np.argmax(scores))
        score = float(scores[cid])
        if score < CONF_THRESH: continue
        cx, cy, w, h = box
        detections.append((cid, score, float(cx), float(cy), float(w), float(h)))

# confidence 내림차순 정렬
detections.sort(key=lambda d: d[1], reverse=True)

print(f"\n=== 감지 결과 (conf ≥ {CONF_THRESH}) — 총 {len(detections)}개 ===")
for cid, score, cx, cy, w, h in detections[:30]:
    name = CLASS_NAMES.get(cid, f"class{cid}")
    # letterbox → 원본 좌표 역산
    x1 = int((cx - w/2 - pad_x) / scale)
    y1 = int((cy - h/2 - pad_y) / scale)
    x2 = int((cx + w/2 - pad_x) / scale)
    y2 = int((cy + h/2 - pad_y) / scale)
    print(f"  [{cid:2d}] {name:<14}  {score:.3f}  bbox=({x1},{y1},{x2},{y2})")

if not detections:
    print("  (감지 없음)")

# chair/table 전용 요약
furn = [(cid,s,cx,cy,w,h) for cid,s,cx,cy,w,h in detections if cid in (14,15)]
print(f"\n=== 가구(chair/table) 감지 — {len(furn)}개 ===")
for item in furn:
    cid, score = item[0], item[1]
    print(f"  [{cid}] {CLASS_NAMES.get(cid,'?')}  score={score:.3f}")
if not furn:
    print("  (없음 — 모델에 해당 클래스 없거나 conf 미달)")
