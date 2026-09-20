"""Replay S18 with candidate-17 and export a paper-ready mask pipeline figure."""

from __future__ import annotations

import hashlib
import json
import subprocess
from collections import deque
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "docs" / "논문" / "figures" / "s18-mask-replay-2026-09-12-r2"
SCRATCH = OUT / "replay"
FFMPEG = Path("C:/dev/ffmpeg-master-latest-win64-gpl-shared/bin/ffmpeg.exe")
EVALUATOR = ROOT / "build-windows" / "surface-validation" / "evaluate_surface_video.exe"
CONFIG = ROOT / "runs" / "surface-search" / "search-KqZcln" / "candidate-17" / "config.json"
AUDIT = ROOT / "runs" / "record-audit-6l5Wca" / "timestamp-audit.json"
PERSON_MODEL = ROOT / "models" / "yolo11n-pose-416x224.onnx"
OBJECT_MODEL = ROOT / "models" / "yolo11n_tier2_fp32.onnx"
GRID = 96


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def point_in_polygon(points: list[list[float]], x: float, y: float) -> bool:
    inside = False
    j = len(points) - 1
    for i, (xi, yi) in enumerate(points):
        xj, yj = points[j]
        if ((yi > y) != (yj > y)) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def read_surface_image(path: Path) -> Image.Image:
    data = path.read_bytes()
    if len(data) != 16 + GRID * GRID * 3 or data[:4] != b"1FRS":
        raise RuntimeError(f"Unexpected surface evidence format: {path}")
    return Image.frombytes("RGB", (GRID, GRID), data[16:])


def reconstruct_masks(
    baseline: Image.Image,
    current: Image.Image,
    polygon: list[list[float]],
    threshold: int,
    min_area: float,
) -> tuple[list[bool], list[bool], list[bool], int, list[int]]:
    xs = [p[0] for p in polygon]
    ys = [p[1] for p in polygon]
    x1, x2, y1, y2 = min(xs), max(xs), min(ys), max(ys)
    roi = []
    for gy in range(GRID):
        for gx in range(GRID):
            nx = x1 + (gx + 0.5) / GRID * (x2 - x1)
            ny = y1 + (gy + 0.5) / GRID * (y2 - y1)
            roi.append(point_in_polygon(polygon, nx, ny))

    base = list(baseline.getdata())
    now = list(current.getdata())
    histogram = [0] * 511
    samples = 0
    for index, visible in enumerate(roi):
        if not visible:
            continue
        delta = int((sum(now[index]) - sum(base[index])) / 3)
        if abs(delta) < 40:
            histogram[delta + 255] += 1
            samples += 1
    offset = 0
    if samples > sum(roi) // 3:
        cumulative = 0
        for index, count in enumerate(histogram):
            cumulative += count
            if cumulative >= samples // 2:
                offset = index - 255
                break
    offset = max(-20, min(20, offset))

    raw = [False] * (GRID * GRID)
    for index, visible in enumerate(roi):
        if visible and max(abs(now[index][c] - base[index][c] - offset) for c in range(3)) > threshold:
            raw[index] = True

    filtered = [False] * len(raw)
    visited = [False] * len(raw)
    accepted_sizes: list[int] = []
    minimum = sum(roi) * min_area
    for start, changed in enumerate(raw):
        if not changed or visited[start]:
            continue
        queue = deque([start])
        visited[start] = True
        component: list[int] = []
        while queue:
            index = queue.popleft()
            component.append(index)
            x, y = index % GRID, index // GRID
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                neighbor = ny * GRID + nx
                if 0 <= nx < GRID and 0 <= ny < GRID and raw[neighbor] and not visited[neighbor]:
                    visited[neighbor] = True
                    queue.append(neighbor)
        if len(component) >= 3 and len(component) >= minimum:
            accepted_sizes.append(len(component))
            for index in component:
                filtered[index] = True
    return roi, raw, filtered, offset, sorted(accepted_sizes, reverse=True)


def masked_surface(image: Image.Image, roi: list[bool]) -> Image.Image:
    pixels = list(image.getdata())
    for index, valid in enumerate(roi):
        if not valid:
            pixels[index] = (210, 210, 210)
    result = Image.new("RGB", image.size)
    result.putdata(pixels)
    return result


def overlay_mask(image: Image.Image, roi: list[bool], mask: list[bool]) -> Image.Image:
    base = masked_surface(image, roi).convert("RGBA")
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    pixels = list(layer.getdata())
    for index, enabled in enumerate(mask):
        if enabled:
            pixels[index] = (255, 30, 30, 185)
    layer.putdata(pixels)
    return Image.alpha_composite(base, layer).convert("RGB")


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    name = "malgunbd.ttf" if bold else "malgun.ttf"
    return ImageFont.truetype(str(Path("C:/Windows/Fonts") / name), size)


def panel(image: Image.Image, title: str, note: str, square: bool = False) -> Image.Image:
    canvas = Image.new("RGB", (800, 450), "white")
    draw = ImageDraw.Draw(canvas)
    target = (340, 340) if square else (760, 340)
    fitted = image.resize(target, Image.Resampling.NEAREST) if square else image.copy()
    if not square:
        fitted.thumbnail(target, Image.Resampling.LANCZOS)
    x = (800 - fitted.width) // 2
    y = 54 + (340 - fitted.height) // 2
    canvas.paste(fitted, (x, y))
    draw.text((20, 12), title, fill=(20, 20, 20), font=font(25, True))
    draw.text((20, 408), note, fill=(45, 45, 45), font=font(18))
    return canvas


def run_replay(video: Path) -> tuple[list[dict], str]:
    OUT.mkdir(parents=True, exist_ok=True)
    SCRATCH.mkdir(exist_ok=True)
    predictions = OUT / "predictions.jsonl"
    process_log = OUT / "process.log"
    if predictions.exists() and predictions.stat().st_size and process_log.exists():
        rows = [json.loads(line) for line in predictions.read_text(encoding="utf-8").splitlines() if line]
        return rows, process_log.read_text(encoding="utf-8", errors="replace")
    ffmpeg_cmd = [
        str(FFMPEG), "-hide_banner", "-loglevel", "error", "-threads", "1",
        "-i", str(video), "-an", "-vf", "fps=fps=2:start_time=0",
        "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1",
    ]
    evaluator_cmd = [
        str(EVALUATOR), str(CONFIG), str(SCRATCH), str(PERSON_MODEL), str(OBJECT_MODEL), "1"
    ]
    with predictions.open("wb") as output, process_log.open("wb") as errors:
        ffmpeg = subprocess.Popen(ffmpeg_cmd, stdout=subprocess.PIPE, stderr=errors)
        evaluator = subprocess.Popen(evaluator_cmd, stdin=ffmpeg.stdout, stdout=output, stderr=errors)
        assert ffmpeg.stdout is not None
        ffmpeg.stdout.close()
        evaluator_code = evaluator.wait()
        ffmpeg_code = ffmpeg.wait()
    if evaluator_code or ffmpeg_code not in (0, 4294967294):
        raise RuntimeError(f"Replay failed: ffmpeg={ffmpeg_code}, evaluator={evaluator_code}")
    rows = [json.loads(line) for line in predictions.read_text(encoding="utf-8").splitlines() if line]
    return rows, process_log.read_text(encoding="utf-8", errors="replace")


def main() -> None:
    audit = json.loads(AUDIT.read_text(encoding="utf-8"))
    record = next(row for row in audit["results"] if row["id"] == "S18")
    video = Path(audit["source"]) / record["filename"]
    if not video.exists():
        video = ROOT / "records" / Path(audit["source"]).name / record["filename"]
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    surface = config["surfaces"][0]
    rows, process_log = run_replay(video)
    first_alert = next(
        row for row in rows if any(candidate["alert"] for candidate in row["status"]["surfaces"][0]["candidates"])
    )
    status = first_alert["status"]["surfaces"][0]
    candidates = [candidate for candidate in status["candidates"] if candidate["alert"]]
    baseline_path = SCRATCH / status["reference"]
    evidence_path = SCRATCH / candidates[0]["evidence_file"]
    baseline = read_surface_image(baseline_path)
    current = read_surface_image(evidence_path)
    roi, raw, filtered, offset, component_sizes = reconstruct_masks(
        baseline, current, surface["polygon"], int(surface["threshold"]), float(surface["min_area"])
    )

    frame_path = OUT / "original-frame.png"
    subprocess.run([
        str(FFMPEG), "-hide_banner", "-loglevel", "error", "-ss", str(first_alert["video_s"]),
        "-i", str(video), "-frames:v", "1", "-y", str(frame_path),
    ], check=True)
    frame = Image.open(frame_path).convert("RGB")
    draw = ImageDraw.Draw(frame)
    polygon_px = [(round(x * frame.width), round(y * frame.height)) for x, y in surface["polygon"]]
    draw.line(polygon_px + [polygon_px[0]], fill=(0, 255, 80), width=5)
    for index, candidate in enumerate(candidates, 1):
        x1, y1, x2, y2 = candidate["bbox"]
        box = (round(x1 * frame.width), round(y1 * frame.height), round(x2 * frame.width), round(y2 * frame.height))
        draw.rectangle(box, outline=(255, 35, 35), width=6)
        draw.text((box[0], max(0, box[1] - 30)), f"C{index}", fill=(255, 35, 35), font=font(23, True))
    frame.save(OUT / "original-with-roi-and-candidates.png")

    baseline_view = masked_surface(baseline, roi)
    current_view = masked_surface(current, roi)
    raw_view = overlay_mask(current, roi, raw)
    filtered_view = overlay_mask(current, roi, filtered)
    baseline_view.resize((768, 768), Image.Resampling.NEAREST).save(OUT / "baseline-grid.png")
    current_view.resize((768, 768), Image.Resampling.NEAREST).save(OUT / "current-grid.png")
    raw_view.resize((768, 768), Image.Resampling.NEAREST).save(OUT / "threshold-mask.png")
    filtered_view.resize((768, 768), Image.Resampling.NEAREST).save(OUT / "filtered-mask.png")

    panels = [
        panel(frame, "(a) 원본 영상과 감시 영역", f"S18, {first_alert['video_s']:.1f}초 · 빨강: 변화 후보"),
        panel(baseline_view, "(b) 정상 기준 표면", "가림 없는 5회 관측의 평균", square=True),
        panel(raw_view, "(c) 임계값 적용 결과", f"threshold=32 · 밝기 보정값={offset}", square=True),
        panel(filtered_view, "(d) 연결 성분 필터링", f"통과 영역 {len(component_sizes)}개 · 15초 후 알림", square=True),
    ]
    figure = Image.new("RGB", (1600, 900), "white")
    for index, item in enumerate(panels):
        figure.paste(item, ((index % 2) * 800, (index // 2) * 450))
    figure.save(OUT / "s18-surface-mask-pipeline.png", optimize=True)

    metadata = {
        "purpose": "Paper figure generated from a fresh replay; visualization is not ground truth.",
        "scenario": "S18",
        "scenario_title": record["title"],
        "video": str(video),
        "video_sha256": sha256(video),
        "config": str(CONFIG),
        "config_sha256": sha256(CONFIG),
        "person_model_sha256": sha256(PERSON_MODEL),
        "object_model_sha256": sha256(OBJECT_MODEL),
        "video_s": first_alert["video_s"],
        "surface_time": first_alert["status"]["frame_time"],
        "parameters": {
            "threshold": surface["threshold"],
            "min_area": surface["min_area"],
            "confirm_seconds": surface["confirm_seconds"],
        },
        "brightness_offset": offset,
        "roi_cells": sum(roi),
        "threshold_cells": sum(raw),
        "filtered_cells": sum(filtered),
        "accepted_component_sizes": component_sizes,
        "alert_candidates": candidates,
        "process_metrics": process_log.strip().splitlines()[-1],
        "notes": [
            "The mask is reconstructed from the freshly saved 96x96 baseline and alert evidence using the production threshold and connected-component rules.",
            "No person covered the surface at this alert frame, and S18 has no registered fixture, so the visible mask equals the configured surface polygon mask.",
            "Red regions are detector predictions, not manually annotated ground truth.",
        ],
    }
    (OUT / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(OUT), "metadata": metadata}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
