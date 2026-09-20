"""Export a mask-pipeline figure from an existing candidate-17 replay."""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[3]
SOURCE = Path(__file__).with_name("replay_s18_mask.py")
SPEC = importlib.util.spec_from_file_location("mask_tools", SOURCE)
assert SPEC and SPEC.loader
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario")
    args = parser.parse_args()
    scenario = args.scenario.upper()

    audit = json.loads(M.AUDIT.read_text(encoding="utf-8"))
    record = next(row for row in audit["results"] if row["id"] == scenario)
    video = Path(audit["source"]) / record["filename"]
    if not video.exists():
        video = ROOT / "records" / Path(audit["source"]).name / record["filename"]

    run_dir = ROOT / "runs" / "surface-search" / "search-KqZcln" / "candidate-17" / scenario
    predictions = [
        json.loads(line)
        for line in (run_dir / "predictions.jsonl").read_text(encoding="utf-8").splitlines()
        if line
    ]
    first_alert = next(
        row for row in predictions
        if any(candidate["alert"] for candidate in row["status"]["surfaces"][0]["candidates"])
    )
    status = first_alert["status"]["surfaces"][0]
    candidates = [candidate for candidate in status["candidates"] if candidate["alert"]]
    attempt = next(path for path in run_dir.iterdir() if path.is_dir() and path.name.startswith("attempt-"))
    baseline = M.read_surface_image(attempt / status["reference"])
    current = M.read_surface_image(attempt / candidates[0]["evidence_file"])

    config = json.loads(M.CONFIG.read_text(encoding="utf-8"))
    surface = config["surfaces"][0]
    roi, raw, filtered, offset, component_sizes = M.reconstruct_masks(
        baseline, current, surface["polygon"], int(surface["threshold"]), float(surface["min_area"])
    )

    out = ROOT / "docs" / "논문" / "figures" / f"{scenario.lower()}-mask-sample"
    out.mkdir(parents=True, exist_ok=True)
    frame_path = out / "original-frame.png"
    subprocess.run([
        str(M.FFMPEG), "-hide_banner", "-loglevel", "error", "-ss", str(first_alert["video_s"]),
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
        draw.text((box[0], max(0, box[1] - 30)), f"C{index}", fill=(255, 35, 35), font=M.font(23, True))
    frame.save(out / "original-with-roi-and-candidates.png")

    baseline_view = M.masked_surface(baseline, roi)
    raw_view = M.overlay_mask(current, roi, raw)
    filtered_view = M.overlay_mask(current, roi, filtered)
    panels = [
        M.panel(frame, "(a) 원본 영상과 감시 영역", f"{scenario}, {first_alert['video_s']:.1f}초 · 빨강: 변화 후보"),
        M.panel(baseline_view, "(b) 정상 기준 표면", "가림 없는 5회 관측의 평균", square=True),
        M.panel(raw_view, "(c) 임계값 적용 결과", f"threshold=32 · 밝기 보정값={offset}", square=True),
        M.panel(filtered_view, "(d) 연결 성분 필터링", f"통과 영역 {len(component_sizes)}개 · 15초 후 알림", square=True),
    ]
    figure = Image.new("RGB", (1600, 900), "white")
    for index, item in enumerate(panels):
        figure.paste(item, ((index % 2) * 800, (index // 2) * 450))
    figure_path = out / f"{scenario.lower()}-surface-mask-pipeline.png"
    figure.save(figure_path, optimize=True)
    (out / "metadata.json").write_text(json.dumps({
        "scenario": scenario,
        "title": record["title"],
        "video_s": first_alert["video_s"],
        "roi_cells": sum(roi),
        "threshold_cells": sum(raw),
        "filtered_cells": sum(filtered),
        "component_sizes": component_sizes,
        "brightness_offset": offset,
        "figure": str(figure_path),
        "warning": "Predicted change mask, not a human-annotated ground-truth mask.",
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    print(figure_path)


if __name__ == "__main__":
    main()
